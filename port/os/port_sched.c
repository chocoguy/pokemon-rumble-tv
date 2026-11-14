/* The Wii's thread scheduler, interrupts, alarms and time base on host pthreads.
 *
 * The scheduling logic (run queues, priorities, priority inheritance through mutexes,
 * suspend counts, join) is the SDK's own, adapted from src/RVL_SDK/os/OSThread.c and
 * OSAlarm.c. What changes is the context switch: instead of loading another register
 * context, the running pthread hands the CPU to the next thread's pthread and waits on
 * its own condition variable until it is chosen again. See port_os.h for the model.
 *
 * Every game thread holds sCPU while it runs, so game code never runs on two host
 * threads at once and needs no host-level locking.
 */
#include <port_os.h>
#include <revolution/os/OSThread.h>
#include <revolution/os/OSMutex.h>
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ------------------------------------------------------------------------------------ */
/* Host side of a game thread                                                            */

typedef struct PortHost {
    pthread_t pthread;
    pthread_cond_t cond;
    OSThread* os;
    void* (*func)(void*);
    void* arg;
    void* stack;
    int started; /* the pthread exists */
    volatile int killed; /* cancelled while waiting: exit as soon as it wakes */
    volatile int finished; /* the pthread has left game code and released sCPU */
    struct PortHost* next; /* all records, for reuse */
} PortHost;

static pthread_mutex_t sCPU = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t sIdleCond = PTHREAD_COND_INITIALIZER; /* host threads posting work */
static __thread PortHost* tSelf;
static PortHost* volatile sRunningHost; /* the record allowed to run game code */
static PortHost* sHosts;

/* The host record lives in the OSThread's register context, which the port never uses. */
static inline PortHost* HostOf(OSThread* thread) {
    PortHost* host;
    memcpy(&host, &thread->context.fpr[0], sizeof(host));
    return host;
}
static inline void SetHostOf(OSThread* thread, PortHost* host) {
    memcpy(&thread->context.fpr[0], &host, sizeof(host));
}
/* For an OSThread the game may never have passed to OSCreateThread (fresh heap memory). */
static PortHost* ValidHostOf(OSThread* thread) {
    PortHost* candidate = HostOf(thread);
    PortHost* host;
    for (host = sHosts; host; host = host->next) {
        if (host == candidate) {
            return host->os == thread ? host : NULL;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------ */
/* SDK scheduler state (OSThread.c)                                                      */

static volatile u32 RunQueueBits;
static OSThreadQueue RunQueue[32];
static volatile BOOL RunQueueHint;
static volatile s32 Reschedule;
static OSThread DefaultThread;
OSThread* __OSCurrentThread;
OSThreadQueue __OSActiveThreadQueue;
static OSContext sInterruptContext;

static void DefaultSwitchThreadCallback(OSThread* from, OSThread* to) {
    (void)from;
    (void)to;
}
static OSSwitchThreadCallback SwitchThreadCallback = DefaultSwitchThreadCallback;

#define EnqueueTail(queue, thread, link)                                                                               \
    do {                                                                                                               \
        OSThread* __prev;                                                                                              \
        __prev = (queue)->tail;                                                                                        \
        if (__prev == NULL)                                                                                            \
            (queue)->head = (thread);                                                                                  \
        else                                                                                                           \
            __prev->link.next = (thread);                                                                              \
        (thread)->link.prev = __prev;                                                                                  \
        (thread)->link.next = NULL;                                                                                    \
        (queue)->tail = (thread);                                                                                      \
    } while (0)

#define EnqueuePrio(queue, thread, link)                                                                               \
    do {                                                                                                               \
        OSThread* __prev;                                                                                              \
        OSThread* __next;                                                                                              \
        for (__next = (queue)->head; __next && __next->priority <= thread->priority; __next = __next->link.next)       \
            ;                                                                                                          \
        if (__next == NULL)                                                                                            \
            EnqueueTail(queue, thread, link);                                                                          \
        else {                                                                                                         \
            (thread)->link.next = __next;                                                                              \
            __prev = __next->link.prev;                                                                                \
            __next->link.prev = (thread);                                                                              \
            (thread)->link.prev = __prev;                                                                              \
            if (__prev == NULL)                                                                                        \
                (queue)->head = (thread);                                                                              \
            else                                                                                                       \
                __prev->link.next = (thread);                                                                          \
        }                                                                                                              \
    } while (0)

#define DequeueItem(queue, thread, link)                                                                               \
    do {                                                                                                               \
        OSThread* __next;                                                                                              \
        OSThread* __prev;                                                                                              \
        __next = (thread)->link.next;                                                                                  \
        __prev = (thread)->link.prev;                                                                                  \
        if (__next == NULL)                                                                                            \
            (queue)->tail = __prev;                                                                                    \
        else                                                                                                           \
            __next->link.prev = __prev;                                                                                \
        if (__prev == NULL)                                                                                            \
            (queue)->head = __next;                                                                                    \
        else                                                                                                           \
            __prev->link.next = __next;                                                                                \
    } while (0)

#define DequeueHead(queue, thread, link)                                                                               \
    do {                                                                                                               \
        OSThread* __next;                                                                                              \
        (thread) = (queue)->head;                                                                                      \
        __next = (thread)->link.next;                                                                                  \
        if (__next == NULL)                                                                                            \
            (queue)->tail = NULL;                                                                                      \
        else                                                                                                           \
            __next->link.prev = NULL;                                                                                  \
        (queue)->head = __next;                                                                                        \
    } while (0)

#define IsSuspended(suspendCount) (0 < (suspendCount))

/* ------------------------------------------------------------------------------------ */
/* Time base                                                                             */

static uint64_t sHostStart;
static mach_timebase_info_data_t sTimebase;
static volatile OSTime sSkipped; /* ticks skipped by the idle loop in fast mode */

static uint64_t HostNanos(void) {
    uint64_t t = mach_absolute_time() - sHostStart;
    return t * sTimebase.numer / sTimebase.denom;
}

OSTime PortNow(void) {
    /* OS_TIMER_CLOCK is 60.75 MHz: 243 ticks every 4000 ns. */
    return (OSTime)(HostNanos() * 243 / 4000) + sSkipped;
}

static uint64_t TicksToNanos(OSTime ticks) {
    return ticks <= 0 ? 0 : (uint64_t)ticks * 4000 / 243;
}

OSTime OSGetTime(void) {
    PortSafePoint();
    return PortNow();
}

OSTick OSGetTick(void) {
    PortSafePoint();
    return (OSTick)PortNow();
}

OSTime __OSGetSystemTime(void) {
    return PortNow();
}

OSTime __OSTimeToSystemTime(OSTime time) {
    return time;
}

/* ------------------------------------------------------------------------------------ */
/* Interrupts                                                                            */

static volatile int sIntrEnabled;
static int sInService;
static __OSInterruptHandler sHandlers[32];
static volatile u32 sUnmasked;
static volatile u32 sPosted; /* OS_INTERRUPTMASK bits */

typedef struct PortTimer {
    PortTimerFunc func;
    OSTime fire;
    OSTime period;
} PortTimer;
static PortTimer sTimers[8];
static int sTimerCount;
static OSTime sDecrementerFire = PORT_NO_EVENT; /* the next alarm */

typedef struct PortCallback {
    void (*func)(void*);
    void* arg;
} PortCallback;
static PortCallback sCallbacks[256];
static unsigned int sCallbackHead, sCallbackTail;

static void DecrementerCallback(void);

__OSInterruptHandler __OSSetInterruptHandler(__OSInterrupt interrupt, __OSInterruptHandler handler) {
    __OSInterruptHandler old = sHandlers[interrupt];
    sHandlers[interrupt] = handler;
    return old;
}

__OSInterruptHandler __OSGetInterruptHandler(__OSInterrupt interrupt) {
    return sHandlers[interrupt];
}

OSInterruptMask __OSMaskInterrupts(OSInterruptMask mask) {
    u32 old = ~sUnmasked;
    sUnmasked &= ~mask;
    return old;
}

OSInterruptMask __OSUnmaskInterrupts(OSInterruptMask mask) {
    u32 old = ~sUnmasked;
    sUnmasked |= mask;
    return old;
}

void PortPostInterrupt(int interrupt) {
    __atomic_fetch_or(&sPosted, OS_INTERRUPTMASK(interrupt), __ATOMIC_SEQ_CST);
}

void PortPostCallback(void (*func)(void*), void* arg) {
    if (sCallbackTail - sCallbackHead >= sizeof(sCallbacks) / sizeof(sCallbacks[0])) {
        PortFatal("PortPostCallback: queue full");
    }
    sCallbacks[sCallbackTail % 256].func = func;
    sCallbacks[sCallbackTail % 256].arg = arg;
    sCallbackTail++;
}

int PortAddTimer(PortTimerFunc func, OSTime firstFire, OSTime period) {
    if (sTimerCount == (int)(sizeof(sTimers) / sizeof(sTimers[0]))) {
        PortFatal("PortAddTimer: too many timers");
    }
    sTimers[sTimerCount].func = func;
    sTimers[sTimerCount].fire = firstFire;
    sTimers[sTimerCount].period = period;
    return sTimerCount++;
}

static OSTime NextEventTime(void) {
    OSTime next = sDecrementerFire;
    int i;
    for (i = 0; i < sTimerCount; i++) {
        if (sTimers[i].fire < next) {
            next = sTimers[i].fire;
        }
    }
    return next;
}

static int AnythingPending(OSTime now) {
    return (sPosted & sUnmasked) != 0 || sCallbackHead != sCallbackTail || NextEventTime() <= now;
}

/* Runs every due interrupt once. Interrupts are disabled and the scheduler held off, as
 * in an exception handler; the caller reschedules afterwards. */
static void Dispatch(void) {
    int round;
    sInService = 1;
    Reschedule++;
    for (round = 0; round < 64; round++) {
        OSTime now = PortNow();
        u32 posted = __atomic_fetch_and(&sPosted, ~sUnmasked, __ATOMIC_SEQ_CST) & sUnmasked;
        int progress = 0;
        int i;

        for (i = 0; i < 32; i++) {
            if (posted & OS_INTERRUPTMASK(i)) {
                if (sHandlers[i] != NULL) {
                    sHandlers[i]((__OSInterrupt)i, &sInterruptContext);
                }
                progress = 1;
            }
        }
        while (sCallbackHead != sCallbackTail) {
            PortCallback cb = sCallbacks[sCallbackHead % 256];
            sCallbackHead++;
            cb.func(cb.arg);
            progress = 1;
        }
        for (i = 0; i < sTimerCount; i++) {
            if (sTimers[i].fire <= now) {
                sTimers[i].fire += sTimers[i].period;
                if (sTimers[i].fire <= now && !gPortConfig.fastTime) {
                    /* Fell behind the host clock: drop the missed events. */
                    sTimers[i].fire = now + sTimers[i].period;
                }
                sTimers[i].func();
                progress = 1;
            }
        }
        if (sDecrementerFire <= now) {
            sDecrementerFire = PORT_NO_EVENT;
            DecrementerCallback();
            progress = 1;
        }
        if (!progress) {
            break;
        }
    }
    Reschedule--;
    sInService = 0;
}

static void __OSRescheduleInternal(void);

static void ServiceInterrupts(void) {
    sIntrEnabled = 0;
    Dispatch();
    __OSRescheduleInternal();
    sIntrEnabled = 1;
}

void PortSafePoint(void) {
    if (sIntrEnabled && !sInService && AnythingPending(PortNow())) {
        ServiceInterrupts();
    }
}

BOOL OSDisableInterrupts(void) {
    BOOL old = sIntrEnabled;
    sIntrEnabled = 0;
    return old;
}

BOOL OSEnableInterrupts(void) {
    BOOL old = sIntrEnabled;
    sIntrEnabled = 1;
    PortSafePoint();
    return old;
}

BOOL OSRestoreInterrupts(BOOL level) {
    BOOL old = sIntrEnabled;
    sIntrEnabled = level ? 1 : 0;
    if (level) {
        PortSafePoint();
    }
    return old;
}

/* ------------------------------------------------------------------------------------ */
/* Context switch                                                                        */

static void HostFinish(PortHost* me) __attribute__((noreturn));
static void HostFinish(PortHost* me) {
    me->finished = 1;
    pthread_mutex_unlock(&sCPU);
    pthread_exit(NULL);
}

static void WaitUntilChosen(PortHost* me) {
    while (sRunningHost != me) {
        pthread_cond_wait(&me->cond, &sCPU);
        if (me->killed) {
            HostFinish(me);
        }
    }
}

static inline void OSSetCurrentThread(OSThread* thread) {
    SwitchThreadCallback(__OSCurrentThread, thread);
    __OSCurrentThread = thread;
}

static void SwitchTo(OSThread* next) {
    PortHost* me = tSelf;
    PortHost* host = HostOf(next);

    OSSetCurrentThread(next);
    sRunningHost = host;
    if (host == me) {
        return;
    }
    pthread_cond_signal(&host->cond);
    if (me->os == NULL || me->killed) {
        /* This thread exited or was cancelled: its pthread ends here. */
        HostFinish(me);
    }
    WaitUntilChosen(me);
}

/* No thread is ready: wait for (or, in fast mode, skip to) the next interrupt. Runs on
 * the pthread of the thread that gave up the CPU, with interrupts disabled. */
static void Idle(void) {
    unsigned int quietRounds = 0;
    while (RunQueueBits == 0) {
        OSTime now = PortNow();
        if (!AnythingPending(now)) {
            OSTime next = NextEventTime();
            if (next == PORT_NO_EVENT) {
                if (++quietRounds == 1) {
                    PortRed("deadlock: no thread is ready and no interrupt is pending");
                    PortDumpThreads();
                    PortExit(3);
                }
            } else if (gPortConfig.fastTime) {
                sSkipped += next - now;
            } else {
                uint64_t waitNs = TicksToNanos(next - now);
                struct timespec ts;
                struct timespec nowTs;
                clock_gettime(CLOCK_REALTIME, &nowTs);
                ts.tv_sec = nowTs.tv_sec + (time_t)(waitNs / 1000000000u);
                ts.tv_nsec = nowTs.tv_nsec + (long)(waitNs % 1000000000u);
                if (ts.tv_nsec >= 1000000000L) {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000L;
                }
                pthread_cond_timedwait(&sIdleCond, &sCPU, &ts);
            }
        }
        Dispatch();
    }
}

static void SetRun(OSThread* thread) {
    thread->queue = &RunQueue[thread->priority];
    EnqueueTail(thread->queue, thread, link);
    RunQueueBits |= 1u << (31 - thread->priority);
    RunQueueHint = TRUE;
}

static void UnsetRun(OSThread* thread) {
    OSThreadQueue* queue = thread->queue;
    DequeueItem(queue, thread, link);
    if (queue->head == 0) {
        RunQueueBits &= ~(1u << (31 - thread->priority));
    }
    thread->queue = 0;
}

OSPriority __OSGetEffectivePriority(OSThread* thread) {
    OSPriority priority = thread->base;
    OSMutex* mutex;
    for (mutex = thread->queueMutex.head; mutex; mutex = mutex->link.next) {
        OSThread* blocked = mutex->queue.head;
        if (blocked != 0 && blocked->priority < priority) {
            priority = blocked->priority;
        }
    }
    return priority;
}

static OSThread* SetEffectivePriority(OSThread* thread, OSPriority priority) {
    switch (thread->state) {
    case OS_THREAD_STATE_READY:
        UnsetRun(thread);
        thread->priority = priority;
        SetRun(thread);
        break;
    case OS_THREAD_STATE_WAITING:
        DequeueItem(thread->queue, thread, link);
        thread->priority = priority;
        EnqueuePrio(thread->queue, thread, link);
        if (thread->mutex != 0) {
            return thread->mutex->thread;
        }
        break;
    case OS_THREAD_STATE_RUNNING:
        RunQueueHint = TRUE;
        thread->priority = priority;
        break;
    }
    return NULL;
}

static void UpdatePriority(OSThread* thread) {
    do {
        OSPriority priority;
        if (IsSuspended(thread->suspend)) {
            break;
        }
        priority = __OSGetEffectivePriority(thread);
        if (thread->priority == priority) {
            break;
        }
        thread = SetEffectivePriority(thread, priority);
    } while (thread);
}

void __OSPromoteThread(OSThread* thread, OSPriority priority) {
    do {
        if (thread->suspend > 0 || thread->priority <= priority) {
            break;
        }
        thread = SetEffectivePriority(thread, priority);
    } while (thread);
}

static OSThread* SelectThread(BOOL yield) {
    OSThread* currentThread;
    OSThread* nextThread;
    OSPriority priority;
    OSThreadQueue* queue;

    if (Reschedule > 0) {
        return 0;
    }

    currentThread = __OSCurrentThread;
    if (currentThread != 0 && currentThread->state == OS_THREAD_STATE_RUNNING) {
        if (!yield) {
            priority = __cntlzw(RunQueueBits);
            if (currentThread->priority <= priority) {
                return 0;
            }
        }
        currentThread->state = OS_THREAD_STATE_READY;
        SetRun(currentThread);
    }

    if (RunQueueBits == 0) {
        OSSetCurrentThread(0);
        Idle();
    }

    RunQueueHint = FALSE;
    priority = __cntlzw(RunQueueBits);
    queue = &RunQueue[priority];
    DequeueHead(queue, nextThread, link);
    if (queue->head == 0) {
        RunQueueBits &= ~(1u << (31 - priority));
    }
    nextThread->queue = 0;
    nextThread->state = OS_THREAD_STATE_RUNNING;
    SwitchTo(nextThread);
    return nextThread;
}

static void __OSRescheduleInternal(void) {
    if (RunQueueHint) {
        SelectThread(FALSE);
    }
}

void __OSReschedule(void) {
    __OSRescheduleInternal();
}

/* ------------------------------------------------------------------------------------ */
/* Thread API (OSThread.c)                                                               */

OSSwitchThreadCallback OSSetSwitchThreadCallback(OSSwitchThreadCallback callback) {
    OSSwitchThreadCallback old = SwitchThreadCallback;
    SwitchThreadCallback = callback ? callback : DefaultSwitchThreadCallback;
    return old == DefaultSwitchThreadCallback ? NULL : old;
}

void OSInitThreadQueue(OSThreadQueue* queue) {
    queue->head = queue->tail = 0;
}

OSThread* OSGetCurrentThread(void) {
    return __OSCurrentThread;
}

BOOL OSIsThreadSuspended(OSThread* thread) {
    return thread->suspend > 0 ? TRUE : FALSE;
}

BOOL OSIsThreadTerminated(OSThread* thread) {
    return (thread->state == OS_THREAD_STATE_MORIBUND || thread->state == 0) ? TRUE : FALSE;
}

static BOOL __OSIsThreadActive(OSThread* thread) {
    OSThread* active;
    if (thread->state == 0) {
        return FALSE;
    }
    for (active = __OSActiveThreadQueue.head; active; active = active->linkActive.next) {
        if (thread == active) {
            return TRUE;
        }
    }
    return FALSE;
}

s32 OSDisableScheduler(void) {
    BOOL enabled = OSDisableInterrupts();
    s32 count = Reschedule++;
    OSRestoreInterrupts(enabled);
    return count;
}

s32 OSEnableScheduler(void) {
    BOOL enabled = OSDisableInterrupts();
    s32 count = Reschedule--;
    OSRestoreInterrupts(enabled);
    return count;
}

void OSYieldThread(void) {
    BOOL enabled = OSDisableInterrupts();
    SelectThread(TRUE);
    OSRestoreInterrupts(enabled);
}

extern void __OSUnlockAllMutex(OSThread* thread);
extern void __OSCancelAlarmsForUserData(void* userData);

static void* HostThreadMain(void* param) {
    PortHost* me = (PortHost*)param;
    void* result;
    tSelf = me;
    pthread_mutex_lock(&sCPU);
    WaitUntilChosen(me);
    /* A new thread starts with interrupts enabled (its MSR has EE set). */
    sIntrEnabled = 1;
    PortSafePoint();
    result = me->func(me->arg);
    OSExitThread(result);
    PortFatal("OSExitThread returned");
}

/* Waits for finished pthreads and makes their records reusable. */
static PortHost* GetHostRecord(void) {
    PortHost* host;
    for (host = sHosts; host; host = host->next) {
        if (host->started && host->finished) {
            pthread_join(host->pthread, NULL);
            PortFreeStack(host->stack);
            host->started = 0;
        }
    }
    for (host = sHosts; host; host = host->next) {
        if (!host->started && host->os == NULL) {
            return host;
        }
    }
    host = (PortHost*)calloc(1, sizeof(*host));
    pthread_cond_init(&host->cond, NULL);
    host->next = sHosts;
    sHosts = host;
    return host;
}

static void ReleaseHost(PortHost* host) {
    /* The record stays bound to its OSThread until the pthread is gone. */
    host->os = NULL;
}

static void StartHost(PortHost* host, OSThread* thread, void* (*func)(void*), void* param, u32 gameStackSize) {
    pthread_attr_t attr;
    unsigned int size;
    host->os = thread;
    host->func = func;
    host->arg = param;
    host->killed = 0;
    host->finished = 0;
    host->stack = PortAllocStack(gameStackSize, &size);
    pthread_attr_init(&attr);
    pthread_attr_setstack(&attr, host->stack, size);
    if (pthread_create(&host->pthread, &attr, HostThreadMain, host) != 0) {
        PortFatal("pthread_create failed");
    }
    pthread_attr_destroy(&attr);
    host->started = 1;
    SetHostOf(thread, host);
}

BOOL OSCreateThread(OSThread* thread, void* (*func)(void*), void* param, void* stack, u32 stackSize, OSPriority priority,
                    u16 attr) {
    BOOL enabled;
    PortHost* old;
    int i;

    if (priority < 0 || 31 < priority) {
        return FALSE;
    }

    enabled = OSDisableInterrupts();
    /* A struct reused before its previous thread ever ran: retire the old pthread. */
    old = ValidHostOf(thread);
    if (old != NULL && old->started && !old->finished && old != tSelf) {
        old->killed = 1;
        pthread_cond_signal(&old->cond);
        ReleaseHost(old);
    }

    thread->state = OS_THREAD_STATE_READY;
    thread->attr = (u16)(attr & 1);
    thread->priority = thread->base = priority;
    thread->suspend = 1;
    thread->value = (void*)-1;
    thread->mutex = NULL;
    OSInitThreadQueue(&thread->queueJoin);
    thread->queueMutex.head = thread->queueMutex.tail = NULL;
    thread->stackBase = (u8*)stack;
    thread->stackEnd = (u32*)((u8*)stack - stackSize);
    *(thread->stackEnd) = 0xDEADBABE;
    thread->error = 0;
    for (i = 0; i < 2; ++i) {
        thread->specific[i] = 0;
    }
    StartHost(GetHostRecord(), thread, func, param, stackSize);
    EnqueueTail(&__OSActiveThreadQueue, thread, linkActive);
    OSRestoreInterrupts(enabled);
    return TRUE;
}

void OSExitThread(void* val) {
    BOOL enabled = OSDisableInterrupts();
    OSThread* currentThread = OSGetCurrentThread();

    if (currentThread->attr & 1) {
        DequeueItem(&__OSActiveThreadQueue, currentThread, linkActive);
        currentThread->state = 0;
    } else {
        currentThread->state = OS_THREAD_STATE_MORIBUND;
        currentThread->value = val;
    }
    __OSUnlockAllMutex(currentThread);
    OSWakeupThread(&currentThread->queueJoin);
    ReleaseHost(tSelf);
    RunQueueHint = TRUE;
    __OSRescheduleInternal();
    OSRestoreInterrupts(enabled);
    PortFatal("exited thread was scheduled again");
}

void OSCancelThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();
    PortHost* host = ValidHostOf(thread);

    __OSCancelAlarmsForUserData(thread);

    switch (thread->state) {
    case OS_THREAD_STATE_READY:
        if (!IsSuspended(thread->suspend)) {
            UnsetRun(thread);
        }
        break;
    case OS_THREAD_STATE_RUNNING:
        RunQueueHint = TRUE;
        break;
    case OS_THREAD_STATE_WAITING:
        DequeueItem(thread->queue, thread, link);
        thread->queue = NULL;
        if (!IsSuspended(thread->suspend) && thread->mutex) {
            UpdatePriority(thread->mutex->thread);
        }
        break;
    default:
        OSRestoreInterrupts(enabled);
        return;
    }

    if (thread->attr & 1) {
        DequeueItem(&__OSActiveThreadQueue, thread, linkActive);
        thread->state = 0;
    } else {
        thread->state = OS_THREAD_STATE_MORIBUND;
    }
    __OSUnlockAllMutex(thread);
    OSWakeupThread(&thread->queueJoin);
    if (host != NULL && host != tSelf) {
        host->killed = 1;
        pthread_cond_signal(&host->cond);
        ReleaseHost(host);
    } else if (host == tSelf) {
        ReleaseHost(host);
    }
    __OSRescheduleInternal();
    OSRestoreInterrupts(enabled);
}

BOOL OSJoinThread(OSThread* thread, void** val) {
    BOOL enabled = OSDisableInterrupts();

    if (!(thread->attr & 1) && thread->state != OS_THREAD_STATE_MORIBUND && thread->queueJoin.head == NULL) {
        OSSleepThread(&thread->queueJoin);
        if (!__OSIsThreadActive(thread)) {
            OSRestoreInterrupts(enabled);
            return FALSE;
        }
    }
    if (((volatile OSThread*)thread)->state == OS_THREAD_STATE_MORIBUND) {
        if (val) {
            *val = thread->value;
        }
        DequeueItem(&__OSActiveThreadQueue, thread, linkActive);
        thread->state = 0;
        OSRestoreInterrupts(enabled);
        return TRUE;
    }
    OSRestoreInterrupts(enabled);
    return FALSE;
}

void OSDetachThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();
    thread->attr |= 1;
    if (thread->state == OS_THREAD_STATE_MORIBUND) {
        DequeueItem(&__OSActiveThreadQueue, thread, linkActive);
        thread->state = 0;
    }
    OSWakeupThread(&thread->queueJoin);
    OSRestoreInterrupts(enabled);
}

s32 OSResumeThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();
    s32 suspendCount = thread->suspend--;

    if (thread->suspend < 0) {
        thread->suspend = 0;
    } else if (thread->suspend == 0) {
        switch (thread->state) {
        case OS_THREAD_STATE_READY:
            thread->priority = __OSGetEffectivePriority(thread);
            SetRun(thread);
            break;
        case OS_THREAD_STATE_WAITING:
            DequeueItem(thread->queue, thread, link);
            thread->priority = __OSGetEffectivePriority(thread);
            EnqueuePrio(thread->queue, thread, link);
            if (thread->mutex) {
                UpdatePriority(thread->mutex->thread);
            }
            break;
        }
        __OSRescheduleInternal();
    }
    OSRestoreInterrupts(enabled);
    return suspendCount;
}

s32 OSSuspendThread(OSThread* thread) {
    BOOL enabled = OSDisableInterrupts();
    s32 suspendCount = thread->suspend++;

    if (suspendCount == 0) {
        switch (thread->state) {
        case OS_THREAD_STATE_RUNNING:
            RunQueueHint = TRUE;
            thread->state = OS_THREAD_STATE_READY;
            break;
        case OS_THREAD_STATE_READY:
            UnsetRun(thread);
            break;
        case OS_THREAD_STATE_WAITING:
            DequeueItem(thread->queue, thread, link);
            thread->priority = 32;
            EnqueueTail(thread->queue, thread, link);
            if (thread->mutex) {
                UpdatePriority(thread->mutex->thread);
            }
            break;
        }
        __OSRescheduleInternal();
    }
    OSRestoreInterrupts(enabled);
    return suspendCount;
}

void OSSleepThread(OSThreadQueue* queue) {
    BOOL enabled = OSDisableInterrupts();
    OSThread* currentThread = OSGetCurrentThread();

    if (currentThread == NULL) {
        PortFatal("OSSleepThread called from interrupt context");
    }
    currentThread->state = OS_THREAD_STATE_WAITING;
    currentThread->queue = queue;
    EnqueuePrio(queue, currentThread, link);
    RunQueueHint = TRUE;
    __OSRescheduleInternal();
    OSRestoreInterrupts(enabled);
}

void OSWakeupThread(OSThreadQueue* queue) {
    BOOL enabled = OSDisableInterrupts();
    while (queue->head) {
        OSThread* thread;
        DequeueHead(queue, thread, link);
        thread->state = OS_THREAD_STATE_READY;
        if (!IsSuspended(thread->suspend)) {
            SetRun(thread);
        }
    }
    __OSRescheduleInternal();
    OSRestoreInterrupts(enabled);
}

BOOL OSSetThreadPriority(OSThread* thread, OSPriority priority) {
    BOOL enabled;
    if (priority < 0 || 31 < priority) {
        return FALSE;
    }
    enabled = OSDisableInterrupts();
    if (thread->base != priority) {
        thread->base = priority;
        UpdatePriority(thread);
        __OSRescheduleInternal();
    }
    OSRestoreInterrupts(enabled);
    return TRUE;
}

OSPriority OSGetThreadPriority(OSThread* thread) {
    return thread->base;
}

static void SleepAlarmHandler(OSAlarm* alarm, OSContext* context) {
    (void)context;
    OSResumeThread((OSThread*)OSGetAlarmUserData(alarm));
}

void OSSleepTicks(OSTime ticks) {
    BOOL enabled = OSDisableInterrupts();
    OSThread* current = OSGetCurrentThread();
    OSAlarm alarm;
    OSCreateAlarm(&alarm);
    OSSetAlarmUserData(&alarm, current);
    OSSetAlarm(&alarm, ticks, SleepAlarmHandler);
    OSSuspendThread(current);
    OSCancelAlarm(&alarm);
    OSRestoreInterrupts(enabled);
}

/* Contexts are the host's business; the SDK's context calls keep their bookkeeping only. */
void OSClearContext(OSContext* context) {
    context->mode = 0;
    context->state = 0;
}

void OSSetCurrentContext(OSContext* context) {
    (void)context;
}

OSContext* OSGetCurrentContext(void) {
    return __OSCurrentThread ? &__OSCurrentThread->context : &sInterruptContext;
}

/* ------------------------------------------------------------------------------------ */
/* Alarms (OSAlarm.c); the decrementer is sDecrementerFire                               */

static struct OSAlarmQueue {
    OSAlarm* head;
    OSAlarm* tail;
} AlarmQueue;

static void SetTimer(OSAlarm* alarm) {
    sDecrementerFire = alarm->fire;
}

static void InsertAlarm(OSAlarm* alarm, OSTime fire, OSAlarmHandler handler) {
    OSAlarm* next;
    OSAlarm* prev;

    if (0 < alarm->period) {
        OSTime time = __OSGetSystemTime();
        fire = alarm->start;
        if (alarm->start < time) {
            fire += alarm->period * ((time - alarm->start) / alarm->period + 1);
        }
    }

    alarm->handler = handler;
    alarm->fire = fire;

    for (next = AlarmQueue.head; next; next = next->next) {
        if (next->fire <= fire) {
            continue;
        }
        alarm->prev = next->prev;
        next->prev = alarm;
        alarm->next = next;
        prev = alarm->prev;
        if (prev != NULL) {
            prev->next = alarm;
        } else {
            AlarmQueue.head = alarm;
            SetTimer(alarm);
        }
        return;
    }

    alarm->next = NULL;
    prev = AlarmQueue.tail;
    AlarmQueue.tail = alarm;
    alarm->prev = prev;
    if (prev != NULL) {
        prev->next = alarm;
    } else {
        AlarmQueue.head = AlarmQueue.tail = alarm;
        SetTimer(alarm);
    }
}

void OSCreateAlarm(OSAlarm* alarm) {
    alarm->handler = NULL;
    alarm->tag = 0;
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler) {
    BOOL enabled = OSDisableInterrupts();
    alarm->period = 0;
    InsertAlarm(alarm, __OSGetSystemTime() + tick, handler);
    OSRestoreInterrupts(enabled);
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period, OSAlarmHandler handler) {
    BOOL enabled = OSDisableInterrupts();
    alarm->period = period;
    alarm->start = __OSTimeToSystemTime(start);
    InsertAlarm(alarm, 0, handler);
    OSRestoreInterrupts(enabled);
}

void OSCancelAlarm(OSAlarm* alarm) {
    OSAlarm* next;
    BOOL enabled = OSDisableInterrupts();

    if (alarm->handler == NULL) {
        OSRestoreInterrupts(enabled);
        return;
    }
    next = alarm->next;
    if (next == NULL) {
        AlarmQueue.tail = alarm->prev;
    } else {
        next->prev = alarm->prev;
    }
    if (alarm->prev != NULL) {
        alarm->prev->next = next;
    } else {
        AlarmQueue.head = next;
        if (next != NULL) {
            SetTimer(next);
        } else {
            sDecrementerFire = PORT_NO_EVENT;
        }
    }
    alarm->handler = NULL;
    OSRestoreInterrupts(enabled);
}

static void DecrementerCallback(void) {
    OSAlarm* alarm;
    OSAlarm* next;
    OSAlarmHandler handler;
    OSTime time = __OSGetSystemTime();

    alarm = AlarmQueue.head;
    if (alarm == NULL) {
        return;
    }
    if (time < alarm->fire) {
        SetTimer(alarm);
        return;
    }
    next = alarm->next;
    AlarmQueue.head = next;
    if (next == NULL) {
        AlarmQueue.tail = NULL;
    } else {
        next->prev = NULL;
    }
    handler = alarm->handler;
    alarm->handler = NULL;
    if (0 < alarm->period) {
        InsertAlarm(alarm, 0, handler);
    }
    if (AlarmQueue.head != NULL) {
        SetTimer(AlarmQueue.head);
    }
    handler(alarm, &sInterruptContext);
}

void OSSetAlarmUserData(OSAlarm* alarm, void* userData) {
    alarm->userData = userData;
}

void* OSGetAlarmUserData(const OSAlarm* alarm) {
    return alarm->userData;
}

void OSSetAlarmTag(OSAlarm* alarm, u32 tag) {
    alarm->tag = tag;
}

void __OSCancelAlarmsForUserData(void* userData) {
    BOOL enabled = OSDisableInterrupts();
    OSAlarm* alarm;
    OSAlarm* next;
    for (alarm = AlarmQueue.head, next = alarm ? alarm->next : NULL; alarm;
         alarm = next, next = alarm ? alarm->next : NULL) {
        if (alarm->tag == 0xFFFFFFFF && alarm->userData == userData) {
            OSCancelAlarm(alarm);
        }
    }
    OSRestoreInterrupts(enabled);
}

/* ------------------------------------------------------------------------------------ */
/* Start-up and diagnostics                                                              */

static void (*sEntry)(void);

static void* DefaultThreadMain(void* param) {
    (void)param;
    sEntry();
    PortLog("GameMain returned");
    PortExit(0);
}

void PortOSStart(void (*entry)(void)) {
    PortHost* host;
    int prio;

    mach_timebase_info(&sTimebase);
    sHostStart = mach_absolute_time();
    PortMemInit();

    for (prio = 0; prio <= 31; prio++) {
        OSInitThreadQueue(&RunQueue[prio]);
    }
    OSInitThreadQueue(&__OSActiveThreadQueue);
    AlarmQueue.head = AlarmQueue.tail = NULL;

    /* The default thread: what main() ran on, priority 16, already running. */
    DefaultThread.state = OS_THREAD_STATE_RUNNING;
    DefaultThread.attr = 1;
    DefaultThread.priority = DefaultThread.base = 16;
    DefaultThread.suspend = 0;
    DefaultThread.value = (void*)-1;
    DefaultThread.mutex = NULL;
    OSInitThreadQueue(&DefaultThread.queueJoin);
    DefaultThread.queueMutex.head = DefaultThread.queueMutex.tail = NULL;
    EnqueueTail(&__OSActiveThreadQueue, &DefaultThread, linkActive);
    __OSCurrentThread = &DefaultThread;

    sEntry = entry;
    sIntrEnabled = 1;
    pthread_mutex_lock(&sCPU);
    host = GetHostRecord();
    sRunningHost = host;
    StartHost(host, &DefaultThread, DefaultThreadMain, NULL, 0x10000);
    pthread_mutex_unlock(&sCPU);
}

static const char* StateName(u16 state) {
    switch (state) {
    case 0:
        return "free";
    case OS_THREAD_STATE_READY:
        return "ready";
    case OS_THREAD_STATE_RUNNING:
        return "running";
    case OS_THREAD_STATE_WAITING:
        return "waiting";
    case OS_THREAD_STATE_MORIBUND:
        return "exited";
    }
    return "?";
}

void PortDumpThreads(void) {
    OSThread* thread;
    fprintf(stderr, "threads (current %p):\n", (void*)__OSCurrentThread);
    for (thread = __OSActiveThreadQueue.head; thread; thread = thread->linkActive.next) {
        fprintf(stderr, "  %p prio %d/%d %-8s suspend %d queue %p mutex %p\n", (void*)thread, thread->priority,
                thread->base, StateName(thread->state), thread->suspend, (void*)thread->queue, (void*)thread->mutex);
    }
    fprintf(stderr, "  next alarm in %lld ticks, interrupts posted %08x unmasked %08x\n",
            sDecrementerFire == PORT_NO_EVENT ? -1LL : (long long)(sDecrementerFire - PortNow()), sPosted, sUnmasked);
}
