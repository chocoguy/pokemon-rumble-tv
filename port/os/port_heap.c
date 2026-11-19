/* The game's C heap: malloc/free in the game's own code and its operator new/delete
 * (src/game/core/new.cpp). On the console these came out of a game heap, so the game
 * keeps their addresses in 32-bit slots (the script VM's values, for one); on the port
 * they must therefore live in the address window (port_mem.c, offset 0x30000000).
 *
 * A size-class allocator: requests up to 1 KB round up to a multiple of 16, larger ones
 * to a power of two. Each class keeps a list of freed blocks; a new block is cut from
 * the region's unused end. Freed memory is never returned to the system or merged, which
 * suits the game's steady pattern (it allocates the same objects every stage). A 16-byte
 * header before each block holds its class.
 *
 * If the region runs out, the host heap takes over (with a [RED] line, since anything
 * the game then stores as a 32-bit address will stop the game); free tells the two apart
 * by address. Under ASan, free blocks are poisoned, and so is a block's slack past the
 * size asked for. */
#include <port_os.h>
#include <os/lock.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
#define POISON(p, n) ASAN_POISON_MEMORY_REGION((p), (n))
#define UNPOISON(p, n) ASAN_UNPOISON_MEMORY_REGION((p), (n))
#else
#define POISON(p, n) ((void)(p), (void)(n))
#define UNPOISON(p, n) ((void)(p), (void)(n))
#endif

#define HEADER 16u
#define SMALL_MAX 1024u
#define SMALL_CLASSES (SMALL_MAX / 16u)  /* 16, 32, ..., 1024 */
#define LARGE_CLASSES 20u                /* 2 KB .. 1 GB */
#define MAGIC 0x52484541u                /* 'RHEA' */

typedef struct {
    uint32_t magic;
    uint32_t cls;
    uint64_t size; /* the size asked for */
} Header;

typedef struct FreeBlock {
    struct FreeBlock* next;
} FreeBlock;

static os_unfair_lock sLock = OS_UNFAIR_LOCK_INIT;
static unsigned char* sStart;
static unsigned char* sNext;
static unsigned char* sEnd;
static FreeBlock* sFree[SMALL_CLASSES + LARGE_CLASSES];
static size_t sInUse, sPeak;

static uint32_t ClassOf(size_t size, size_t* blockSize) {
    uint32_t cls;
    size_t block;
    if (size <= SMALL_MAX) {
        cls = size == 0 ? 0 : (uint32_t)((size - 1) / 16u);
        *blockSize = (cls + 1) * 16u;
        return cls;
    }
    block = 2048;
    cls = SMALL_CLASSES;
    while (block < size) {
        block <<= 1;
        cls++;
    }
    *blockSize = block;
    return cls;
}

static size_t BlockSize(uint32_t cls) {
    return cls < SMALL_CLASSES ? (cls + 1) * 16u : (size_t)2048 << (cls - SMALL_CLASSES);
}

static void Summary(void) {
    PortLog("game heap: %zu KB in use, peak %zu KB, %zu KB of the region cut", sInUse >> 10, sPeak >> 10,
            (size_t)(sNext - sStart) >> 10);
}

void* PortGameAlloc(size_t size) {
    size_t block;
    uint32_t cls;
    unsigned char* p = NULL;
    static int reported;

    if (size > ((size_t)1 << 30)) {
        return NULL;
    }
    cls = ClassOf(size, &block);
    os_unfair_lock_lock(&sLock);
    if (sStart == NULL) {
        PortGameHeapRegion(&sStart, &sEnd);
        sNext = sStart;
        PortAddSummary(Summary);
    }
    if (sFree[cls] != NULL) {
        FreeBlock* f = sFree[cls];
        UNPOISON(f, sizeof(*f));
        sFree[cls] = f->next;
        p = (unsigned char*)f;
    } else if ((size_t)(sEnd - sNext) >= HEADER + block) {
        p = sNext + HEADER;
        sNext += HEADER + block;
    }
    if (p != NULL) {
        Header* h = (Header*)(p - HEADER);
        UNPOISON(h, HEADER);
        h->magic = MAGIC;
        h->cls = cls;
        h->size = size;
        sInUse += block;
        if (sInUse > sPeak) {
            sPeak = sInUse;
        }
    }
    os_unfair_lock_unlock(&sLock);

    if (p == NULL) {
        PortRedOnce(&reported, "game heap: the %u MB region is full; using the host heap (%zu bytes asked)",
                    (unsigned)((sEnd - sStart) >> 20), size);
        return malloc(size);
    }
    UNPOISON(p, size);
    POISON(p + size, block - size);
    return p;
}

void PortGameFree(void* ptr) {
    unsigned char* p = (unsigned char*)ptr;
    Header* h;
    uint32_t cls;
    size_t block;
    if (p == NULL) {
        return;
    }
    if (sStart == NULL || p < sStart || p >= sEnd) {
        free(p); /* from the host heap (before the region existed, or after it filled) */
        return;
    }
    h = (Header*)(p - HEADER);
    if (h->magic != MAGIC || h->cls >= SMALL_CLASSES + LARGE_CLASSES) {
        PortFatal("game heap: free of %p, which is not a block (header %08x %08x)", ptr, h->magic, h->cls);
    }
    cls = h->cls;
    block = BlockSize(cls);
    h->magic = 0; /* a second free of the same block is caught above */
    UNPOISON(p, block);
    os_unfair_lock_lock(&sLock);
    ((FreeBlock*)p)->next = sFree[cls];
    sFree[cls] = (FreeBlock*)p;
    sInUse -= block;
    POISON(p, block);
    POISON(h, HEADER);
    os_unfair_lock_unlock(&sLock);
}
