/* Internal interface of port/os, shared by the rest of port/ and the platform shells.
 *
 * The Wii ran one CPU. port/os keeps that: every game thread is a host pthread, but only
 * the one the SDK scheduler (port_sched.c, adapted from the SDK's OSThread.c) has chosen
 * runs; the others wait on their own condition variable. A thread gives up the CPU only
 * where it would have on the console: when it blocks, yields, exits, or when an
 * interrupt makes a higher-priority thread ready.
 *
 * Interrupts are delivered at safe points: whenever the running thread enables
 * interrupts (OSRestoreInterrupts, OSEnableInterrupts), calls OSGetTime/OSGetTick/
 * VIGetRetraceCount with interrupts enabled, and whenever no thread is ready (the idle
 * loop). The handlers run on the current thread's host stack, as they ran on the current
 * thread's stack on the console.
 *
 * Time is the console's time base (OS_TIMER_CLOCK ticks). It follows the host clock; in
 * fast mode (headless runs) the idle loop skips straight to the next event instead of
 * sleeping, so a headless run takes as long as the CPU work it does.
 */
#ifndef PORT_OS_H
#define PORT_OS_H

#include <revolution/types.h>
#include <revolution/os.h>
#include <stdarg.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PortOSConfig {
    int fastTime;            /* skip idle time instead of sleeping */
    const char* romDir;      /* folder holding the content files (rom/content) */
    const char* nandDir;     /* folder standing in for the console's NAND */
    unsigned int stopAfterRetraces; /* 0: run until the game exits */
} PortOSConfig;

extern PortOSConfig gPortConfig;

/* Reserves the address window and arenas, makes the default thread (priority 16) and
 * starts `entry` on it. Returns at once; the game runs on its own threads. */
void PortOSStart(void (*entry)(void));
/* Blocks the calling host thread until the game calls PortExit. Returns the exit code. */
int PortOSWaitForExit(void);
/* Ends the process after printing the run summary. Callable from game code. */
void PortExit(int code) __attribute__((noreturn));

/* Logging. PortRed counts and prints a "[RED]" line (something the port does not handle). */
void PortLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void PortRed(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void PortRedOnce(int* reported, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
unsigned int PortRedCount(void);
void PortFatal(const char* fmt, ...) __attribute__((format(printf, 1, 2), noreturn));
void PortDumpThreads(void);

/* Time base. */
OSTime PortNow(void);

/* Interrupt sources. */
#define PORT_NO_EVENT ((OSTime)0x7FFFFFFFFFFFFFFFLL)
typedef void (*PortTimerFunc)(void);
/* A periodic source serviced like an interrupt (VI retrace, audio DMA). */
int PortAddTimer(PortTimerFunc func, OSTime firstFire, OSTime period);
/* Raises an SDK interrupt (__OS_INTERRUPT_*); its handler runs at the next safe point. */
void PortPostInterrupt(int interrupt);
/* Runs `func(arg)` in interrupt context at the next safe point (async I/O completions). */
void PortPostCallback(void (*func)(void*), void* arg);
/* Services pending interrupts now if interrupts are enabled. */
void PortSafePoint(void);

/* Memory window (port_mem.c). */
void PortMemInit(void);

/* The game's C heap (port_heap.c): what the game's malloc/free and operator new/delete
 * use, so everything the game allocates has a 32-bit game address. Blocks are 16-byte
 * aligned. Thread-safe from any thread, usable before PortOSStart. */
void* PortGameAlloc(size_t size);
void PortGameFree(void* p);
void PortGameHeapRegion(unsigned char** start, unsigned char** end);
void* PortAllocStack(unsigned int size, unsigned int* outSize);
void PortFreeStack(void* base);

/* Called from the VI retrace (port_vi.c) for the run summary and watchdog. */
void PortNoteRetrace(unsigned int retraceCount);
void PortNoteFramePresented(void);

/* Subsystem hooks for the summary printed at exit. */
typedef void (*PortSummaryFunc)(void);
void PortAddSummary(PortSummaryFunc func);

/* The renderer (port/gx). PortGXStart starts the GPU backend at an EFB scale (call before
 * PortOSStart; without it the command stream is only checked). PortGXRetrace shows the
 * display copy at `frameBuffer` (VI's current frame buffer) and writes any frame dump
 * requested for this retrace with PortGXRequestDump. */
int PortGXStart(int scale);
void PortGXRetrace(unsigned int retrace, void* frameBuffer, int black);
void PortGXRequestDump(unsigned int retrace, const char* path);
/* Also writes every Nth retrace's display to DIR/frame-NNNNN.png (exploring a script). */
void PortGXRequestDumpEvery(unsigned int every, const char* dir);

/* Wii Remote buttons (port/pad/port_pad.c). The script is read once before PortOSStart;
 * PortPadSetButtons holds WPAD_BUTTON_* bits until changed (the app shell). */
int PortPadLoadScript(const char* path);
void PortPadSetButtons(unsigned int buttons);

#ifdef __cplusplus
}
#endif

#endif
