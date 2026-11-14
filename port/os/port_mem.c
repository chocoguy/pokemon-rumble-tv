/* The game's address window (port_addr.h) and the memory inside it.
 *
 * The window starts at the executable's Mach-O header, so the executable's code and data
 * are in it, and continues into sWindow, a zero-fill array in the executable's own
 * __DATA segment: the loader maps it right after the image and touches no page until the
 * game does. Everything the game can point at is placed in it at the offsets it had on
 * the console where that is cheap:
 *
 *   offset 0x00000000  Mach-O header, code, data (the console's MEM1 low memory and DOL)
 *   0x02000000-        MEM1 arena, 24 MB
 *   0x0C000000-        hardware registers (CP, PE, PI, MEM, DSP, ...): plain memory that
 *                      the compiled SDK code reads and writes
 *   0x0E000000         locked cache, 16 KB
 *   0x10000000-        MEM2 arena, 64 MB (the console's MEM2 is at physical 0x10000000)
 *   0x20000000-        host stacks of game threads
 */
#include <port_os.h>
#include <port_addr.h>
#include <revolution/os.h>
#include <revolution/os/OSArena.h>
#include <mach-o/ldsyms.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WINDOW_ARRAY_SIZE 0x3C000000u /* 1 GB minus room for the image */

#define OFF_MEM1 0x02000000u
#define MEM1_SIZE 0x01800000u
#define OFF_REGS 0x0C000000u
#define REGS_SIZE 0x02000000u
#define OFF_LOCKED_CACHE 0x0E000000u
#define LOCKED_CACHE_SIZE 0x4000u
#define OFF_MEM2 0x10000000u
#define MEM2_SIZE 0x04000000u
#define OFF_STACKS 0x20000000u
#define STACKS_SIZE 0x10000000u

#if __has_feature(address_sanitizer)
#define HOST_STACK_SIZE 0x400000u /* ASan frames are several times larger */
#else
#define HOST_STACK_SIZE 0x200000u
#endif
#define STACK_SLOTS (STACKS_SIZE / HOST_STACK_SIZE)

__attribute__((no_sanitize("address"), aligned(0x4000))) static unsigned char sWindow[WINDOW_ARRAY_SIZE];

static unsigned char sStackUsed[STACK_SLOTS];

static void* sMEM1ArenaLo;
static void* sMEM1ArenaHi;
static void* sMEM2ArenaLo;
static void* sMEM2ArenaHi;

/* Hardware registers the compiled SDK code addresses by name (include/private/flipper.h). */
vu16 __VIRegs[0x3B];
vu32 __PIRegs[0xC];
vu16 __MEMRegs[0x40];
vu16 __DSPRegs[0x20];
vu32 __EXIRegs[0x10];
vu32 __AIRegs[0x8];
vu32 __ACRRegs[0x59];
vu32 __DIRegs[0x10];
vu32 __SIRegs[0x40];

static unsigned char* At(uint32_t offset) {
    return (unsigned char*)(gPortAddrBase + offset);
}

void PortMemInit(void) {
    uintptr_t base = (uintptr_t)&_mh_execute_header;
    uintptr_t arrayStart = (uintptr_t)sWindow;
    uintptr_t arrayEnd = arrayStart + sizeof(sWindow);

    gPortAddrBase = base;
    if (arrayStart - base > OFF_MEM1 || arrayEnd - base < OFF_STACKS + STACKS_SIZE ||
        arrayEnd - base > PORT_ADDR_WINDOW_SIZE) {
        PortFatal("address window layout: image %p, window array at offset 0x%lx..0x%lx", (void*)base,
                  (unsigned long)(arrayStart - base), (unsigned long)(arrayEnd - base));
    }

    sMEM1ArenaLo = At(OFF_MEM1);
    sMEM1ArenaHi = At(OFF_MEM1 + MEM1_SIZE);
    sMEM2ArenaLo = At(OFF_MEM2);
    sMEM2ArenaHi = At(OFF_MEM2 + MEM2_SIZE);
}

/* ------------------------------------------------------------------------------------ */
/* Arenas (OSArena.c)                                                                    */

void* OSGetMEM1ArenaHi(void) {
    return sMEM1ArenaHi;
}
void* OSGetMEM1ArenaLo(void) {
    return sMEM1ArenaLo;
}
void* OSGetMEM2ArenaHi(void) {
    return sMEM2ArenaHi;
}
void* OSGetMEM2ArenaLo(void) {
    return sMEM2ArenaLo;
}
void OSSetMEM1ArenaHi(void* p) {
    sMEM1ArenaHi = p;
}
void OSSetMEM1ArenaLo(void* p) {
    sMEM1ArenaLo = p;
}
void OSSetMEM2ArenaHi(void* p) {
    sMEM2ArenaHi = p;
}
void OSSetMEM2ArenaLo(void* p) {
    sMEM2ArenaLo = p;
}
void* OSGetArenaHi(void) {
    return sMEM1ArenaHi;
}
void* OSGetArenaLo(void) {
    return sMEM1ArenaLo;
}
void OSSetArenaHi(void* p) {
    sMEM1ArenaHi = p;
}
void OSSetArenaLo(void* p) {
    sMEM1ArenaLo = p;
}

#define ROUND_UP(x, a) (((uintptr_t)(x) + (a)-1) & ~(uintptr_t)((a)-1))
#define ROUND_DOWN(x, a) ((uintptr_t)(x) & ~(uintptr_t)((a)-1))

void* OSAllocFromMEM1ArenaLo(u32 size, u32 align) {
    uintptr_t p = ROUND_UP(sMEM1ArenaLo, align);
    sMEM1ArenaLo = (void*)ROUND_UP(p + size, align);
    return (void*)p;
}

void* OSAllocFromMEM1ArenaHi(u32 size, u32 align) {
    uintptr_t hi = ROUND_DOWN(sMEM1ArenaHi, align);
    hi = ROUND_DOWN(hi - size, align);
    sMEM1ArenaHi = (void*)hi;
    return (void*)hi;
}

void* OSAllocFromMEM2ArenaLo(u32 size, u32 align) {
    uintptr_t p = ROUND_UP(sMEM2ArenaLo, align);
    sMEM2ArenaLo = (void*)ROUND_UP(p + size, align);
    return (void*)p;
}

void* OSAllocFromMEM2ArenaHi(u32 size, u32 align) {
    uintptr_t hi = ROUND_DOWN(sMEM2ArenaHi, align);
    hi = ROUND_DOWN(hi - size, align);
    sMEM2ArenaHi = (void*)hi;
    return (void*)hi;
}

/* ------------------------------------------------------------------------------------ */
/* Host stacks for game threads                                                          */

void* PortAllocStack(unsigned int gameSize, unsigned int* outSize) {
    unsigned int i;
    (void)gameSize;
    for (i = 0; i < STACK_SLOTS; i++) {
        if (!sStackUsed[i]) {
            sStackUsed[i] = 1;
            *outSize = HOST_STACK_SIZE;
            return At(OFF_STACKS + i * HOST_STACK_SIZE);
        }
    }
    PortFatal("out of thread stacks (%u in use)", (unsigned)STACK_SLOTS);
}

void PortFreeStack(void* base) {
    uintptr_t offset = (uintptr_t)base - (uintptr_t)At(OFF_STACKS);
    if (base == NULL) {
        return;
    }
    sStackUsed[offset / HOST_STACK_SIZE] = 0;
}

/* ------------------------------------------------------------------------------------ */
/* Caches                                                                                */

/* The locked cache: 16 KB of scratch memory shared by all threads, as the console's was
 * (it belongs to the one CPU, not to a thread). */
void* PORTLockedCacheBase(void) {
    return At(OFF_LOCKED_CACHE);
}

void LCEnable(void) {}
void LCDisable(void) {}
void LCLoadBlocks(void* dst, void* src, u32 blocks) {
    memmove(dst, src, (size_t)blocks * 32);
}
void LCStoreBlocks(void* dst, void* src, u32 blocks) {
    memmove(dst, src, (size_t)blocks * 32);
}
u32 LCStoreData(void* dst, void* src, u32 bytes) {
    memmove(dst, src, bytes);
    return (bytes + 4095) / 4096;
}
u32 LCQueueLength(void) {
    return 0;
}
void LCQueueWait(u32 len) {
    (void)len;
}

/* The host keeps caches coherent; flushing is only a point where the GPU may look. */
void DCFlushRange(void* p, u32 n) {
    (void)p;
    (void)n;
}
void DCFlushRangeNoSync(void* p, u32 n) {
    (void)p;
    (void)n;
}
void DCStoreRange(void* p, u32 n) {
    (void)p;
    (void)n;
}
void DCStoreRangeNoSync(void* p, u32 n) {
    (void)p;
    (void)n;
}
void DCInvalidateRange(void* p, u32 n) {
    (void)p;
    (void)n;
}
void ICInvalidateRange(void* p, u32 n) {
    (void)p;
    (void)n;
}
