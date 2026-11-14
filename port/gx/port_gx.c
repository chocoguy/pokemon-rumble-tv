/* The GX command pipe and the command-stream decoder.
 *
 * The compiled GX library writes the console's command bytes into gPORTGXPipe (see
 * port_gx_pipe.h). This file owns that buffer and plays the GPU's command processor:
 * whenever the game flushes (GXFlush ends in PPCSync) or the buffer fills, everything
 * written so far is decoded. Display lists the stream calls are decoded where they lie in
 * game memory.
 *
 * Phase 2 draws nothing. The decoder checks that every command is well formed (known
 * opcode, XF writes inside XF memory, display lists inside the address window, vertex
 * data sized from the current VCD/VAT) and does what the GPU's side effects require:
 * the draw-done and token interrupts, and the PE token register. The phase 3 renderer
 * hooks in where the commands are recognised.
 */
#include <port_os.h>
#include <port_gx_pipe.h>
#include <port_endian.h>
#include <revolution/gx.h>
#include <revolution/gx/GXFifo.h>
#include <revolution/gx/GXRegs.h>
#include <revolution/gx/GXState.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PORTGXPipe gPORTGXPipe;

static uint8_t* sPipeStart;
static size_t sPipeCapacity;

typedef struct GXStats {
    unsigned long long commands;
    unsigned long long bytes;
    unsigned long long primitives;
    unsigned long long vertices;
    unsigned long long xfWrites;
    unsigned long long bpWrites;
    unsigned long long cpWrites;
    unsigned long long displayLists;
    unsigned long long displayCopies;
    unsigned long long textureCopies;
    unsigned long long drawDones;
    unsigned long long tokens;
    unsigned int errors;
} GXStats;
static GXStats sStats;

/* Registers the GPU keeps that the decoder needs. */
static uint32_t sCP[256];
static uint32_t sBP[256];
static uint32_t sBPMask = 0xFFFFFF;

static const char* sWhere = "fifo";

static void StreamError(const uint8_t* at, const uint8_t* start, const char* fmt, ...) __attribute__((format(printf, 3, 4)));
static void StreamError(const uint8_t* at, const uint8_t* start, const char* fmt, ...) {
    char msg[256];
    va_list args;
    sStats.errors++;
    if (sStats.errors > 20) {
        return;
    }
    va_start(args, fmt);
    vsnprintf(msg, sizeof(msg), fmt, args);
    va_end(args);
    PortRed("GX stream (%s, offset %ld): %s; bytes %02x %02x %02x %02x %02x %02x %02x %02x", sWhere,
            (long)(at - start), msg, at[0], at[1], at[2], at[3], at[4], at[5], at[6], at[7]);
}

/* ------------------------------------------------------------------------------------ */
/* Vertex size from VCD and VAT                                                          */

static const int kComponentSize[8] = {1, 1, 2, 2, 4, 0, 0, 0};
static const int kColorSize[8] = {2, 3, 4, 2, 3, 4, 0, 0};

static int IndexSize(uint32_t type) {
    return type == 2 ? 1 : type == 3 ? 2 : 0;
}

/* Bytes per vertex for format `vat`, or -1 if the format is invalid. */
static int VertexSize(int vat) {
    uint32_t lo = sCP[0x50];
    uint32_t hi = sCP[0x60];
    uint32_t a = sCP[0x70 + vat];
    uint32_t b = sCP[0x80 + vat];
    uint32_t c = sCP[0x90 + vat];
    int size = 0;
    int i;
    uint32_t type;

    /* Matrix indices: one byte each. */
    for (i = 0; i < 9; i++) {
        if (lo & (1u << i)) {
            size++;
        }
    }
    /* Position. */
    type = (lo >> 9) & 3;
    if (type == 1) {
        int comps = (a & 1) ? 3 : 2;
        int cs = kComponentSize[(a >> 1) & 7];
        if (cs == 0) {
            return -1;
        }
        size += comps * cs;
    } else if (type != 0) {
        size += IndexSize(type);
    } else {
        return -1; /* every vertex has a position */
    }
    /* Normal. */
    type = (lo >> 11) & 3;
    if (type != 0) {
        int nbt = (a >> 9) & 1;
        if (type == 1) {
            int cs = kComponentSize[(a >> 10) & 7];
            if (cs == 0) {
                return -1;
            }
            size += (nbt ? 9 : 3) * cs;
        } else {
            int index3 = (a >> 31) & 1;
            size += IndexSize(type) * (nbt && index3 ? 3 : 1);
        }
    }
    /* Colours. */
    for (i = 0; i < 2; i++) {
        uint32_t fmt = i == 0 ? (a >> 14) & 7 : (a >> 18) & 7;
        type = (lo >> (13 + 2 * i)) & 3;
        if (type == 1) {
            if (kColorSize[fmt] == 0) {
                return -1;
            }
            size += kColorSize[fmt];
        } else if (type != 0) {
            size += IndexSize(type);
        }
    }
    /* Texture coordinates. */
    for (i = 0; i < 8; i++) {
        uint32_t cnt;
        uint32_t fmt;
        type = (hi >> (2 * i)) & 3;
        if (type == 0) {
            continue;
        }
        switch (i) {
        case 0:
            cnt = (a >> 21) & 1;
            fmt = (a >> 22) & 7;
            break;
        case 1:
            cnt = b & 1;
            fmt = (b >> 1) & 7;
            break;
        case 2:
            cnt = (b >> 9) & 1;
            fmt = (b >> 10) & 7;
            break;
        case 3:
            cnt = (b >> 18) & 1;
            fmt = (b >> 19) & 7;
            break;
        case 4:
            cnt = (b >> 27) & 1;
            fmt = (b >> 28) & 7;
            break;
        case 5:
            cnt = (c >> 5) & 1;
            fmt = (c >> 6) & 7;
            break;
        case 6:
            cnt = (c >> 14) & 1;
            fmt = (c >> 15) & 7;
            break;
        default:
            cnt = (c >> 23) & 1;
            fmt = (c >> 24) & 7;
            break;
        }
        if (type == 1) {
            if (kComponentSize[fmt] == 0) {
                return -1;
            }
            size += (cnt ? 2 : 1) * kComponentSize[fmt];
        } else {
            size += IndexSize(type);
        }
    }
    return size;
}

/* ------------------------------------------------------------------------------------ */
/* Register side effects                                                                 */

static void WriteBP(uint32_t value) {
    uint32_t reg = value >> 24;
    uint32_t data = value & 0xFFFFFF;

    sStats.bpWrites++;
    if (reg == 0xFE) {
        sBPMask = data;
        return;
    }
    sBP[reg] = (sBP[reg] & ~sBPMask) | (data & sBPMask);
    sBPMask = 0xFFFFFF;

    switch (reg) {
    case 0x45: /* PE_DONE: the draw-done interrupt */
        sStats.drawDones++;
        PortPostInterrupt(__OS_INTERRUPT_PI_PE_FINISH);
        break;
    case 0x47: /* PE_TOKEN */
        GX_PE_REG_WRITE_U16(PE_REG_TOKEN, data & 0xFFFF);
        break;
    case 0x48: /* PE_TOKEN_INT */
        sStats.tokens++;
        GX_PE_REG_WRITE_U16(PE_REG_TOKEN, data & 0xFFFF);
        PortPostInterrupt(__OS_INTERRUPT_PI_PE_TOKEN);
        break;
    case 0x52: /* PE copy execute */
        if (data & (1u << 14)) {
            sStats.displayCopies++;
        } else {
            sStats.textureCopies++;
        }
        break;
    }
}

static int ValidXF(uint32_t addr, uint32_t count) {
    uint32_t end = addr + count;
    return end <= 0x0800 || (addr >= 0x1000 && end <= 0x1058);
}

/* ------------------------------------------------------------------------------------ */
/* Decoder                                                                               */

static size_t Decode(const uint8_t* start, size_t size, int inDisplayList);

static int CallDisplayList(uint32_t addr, uint32_t size, const uint8_t* at, const uint8_t* start) {
    const uint8_t* list;
    const char* outer = sWhere;
    if (addr == 0 || addr >= PORT_ADDR_WINDOW_SIZE || size > PORT_ADDR_WINDOW_SIZE - addr) {
        StreamError(at, start, "display list 0x%08x (+%u) is outside the address window", addr, size);
        return 0;
    }
    if (addr & 31 || size & 31) {
        StreamError(at, start, "display list 0x%08x (+%u) is not 32-byte aligned", addr, size);
    }
    list = (const uint8_t*)PortPtrRaw(addr);
    sStats.displayLists++;
    sWhere = "display list";
    if (Decode(list, size, 1) != size) {
        StreamError(list, list, "display list at 0x%08x ends inside a command", addr);
    }
    sWhere = outer;
    return 1;
}

/* Decodes whole commands; returns the bytes consumed. In the FIFO a command may straddle
 * the end of what has been written so far; it is left for the next drain. */
static size_t Decode(const uint8_t* start, size_t size, int inDisplayList) {
    const uint8_t* p = start;
    const uint8_t* end = start + size;

    while (p < end) {
        size_t left = (size_t)(end - p);
        uint8_t op = p[0];
        size_t length;

        if (op >= 0x80 && op < 0xC0) {
            int vat = op & 7;
            int vsize;
            uint16_t count;
            if (left < 3) {
                break;
            }
            count = PortBE16(p + 1);
            vsize = VertexSize(vat);
            if (vsize < 0) {
                StreamError(p, start, "draw with invalid vertex format %d (VCD %06x/%04x, VAT %08x %08x %08x)", vat,
                            sCP[0x50], sCP[0x60], sCP[0x70 + vat], sCP[0x80 + vat], sCP[0x90 + vat]);
                return size;
            }
            length = 3 + (size_t)count * (size_t)vsize;
            if (left < length) {
                if (inDisplayList) {
                    StreamError(p, start, "draw of %u vertices runs past the end of the list", count);
                    return size;
                }
                break;
            }
            sStats.primitives++;
            sStats.vertices += count;
        } else {
            switch (op) {
            case 0x00: /* NOP */
            case 0x44: /* metrics */
            case 0x48: /* invalidate vertex cache */
                length = 1;
                break;
            case 0x08: /* load CP register */
                length = 6;
                if (left >= length) {
                    sCP[p[1]] = PortBE32(p + 2);
                    sStats.cpWrites++;
                }
                break;
            case 0x10: { /* load XF registers or memory */
                uint32_t header;
                uint32_t count;
                if (left < 5) {
                    length = 5;
                    break;
                }
                header = PortBE32(p + 1);
                count = ((header >> 16) & 0xF) + 1;
                length = 5 + count * 4;
                if (left >= length) {
                    if (!ValidXF(header & 0xFFFF, count)) {
                        StreamError(p, start, "XF write of %u words at 0x%04x", count, header & 0xFFFF);
                    }
                    sStats.xfWrites++;
                }
                break;
            }
            case 0x20:
            case 0x28:
            case 0x30:
            case 0x38: /* indexed XF loads */
                length = 5;
                break;
            case 0x40: /* call display list */
                length = 9;
                if (left >= length) {
                    if (inDisplayList) {
                        StreamError(p, start, "display list calls another display list");
                    } else {
                        CallDisplayList(PortBE32(p + 1), PortBE32(p + 5), p, start);
                    }
                }
                break;
            case 0x61: /* load BP register */
                length = 5;
                if (left >= length) {
                    WriteBP(PortBE32(p + 1));
                }
                break;
            default:
                StreamError(p, start, "unknown command 0x%02x", op);
                return size; /* the rest of this buffer cannot be parsed */
            }
            if (left < length) {
                if (inDisplayList) {
                    StreamError(p, start, "command 0x%02x runs past the end of the list", op);
                    return size;
                }
                break;
            }
        }
        sStats.commands++;
        p += length;
    }
    sStats.bytes += (size_t)(p - start);
    return (size_t)(p - start);
}

/* ------------------------------------------------------------------------------------ */
/* The pipe                                                                              */

static void Drain(void) {
    size_t written;
    size_t used;
    if (sPipeStart == NULL) {
        return;
    }
    written = (size_t)(gPORTGXPipe.cur - sPipeStart);
    sWhere = "fifo";
    used = Decode(sPipeStart, written, 0);
    memmove(sPipeStart, sPipeStart + used, written - used);
    gPORTGXPipe.cur = sPipeStart + (written - used);
}

static void PrintSummary(void) {
    fprintf(stderr,
            "[port] GX: %llu commands (%llu bytes), %llu primitives, %llu vertices, %llu display lists, "
            "%llu display copies, %llu texture copies, %llu draw-done, %llu tokens, %u stream errors\n",
            sStats.commands, sStats.bytes, sStats.primitives, sStats.vertices, sStats.displayLists,
            sStats.displayCopies, sStats.textureCopies, sStats.drawDones, sStats.tokens, sStats.errors);
}

static void InitPipe(void) {
    if (sPipeStart != NULL) {
        return;
    }
    sPipeCapacity = 1u << 20;
    sPipeStart = (uint8_t*)malloc(sPipeCapacity);
    gPORTGXPipe.cur = sPipeStart;
    gPORTGXPipe.end = sPipeStart + sPipeCapacity;
    PortAddSummary(PrintSummary);
}

void PORTGXPipeReserve(uint32_t bytes) {
    size_t used;
    InitPipe();
    Drain();
    used = (size_t)(gPORTGXPipe.cur - sPipeStart);
    if (sPipeCapacity - used < bytes) {
        size_t capacity = sPipeCapacity * 2;
        while (capacity - used < bytes) {
            capacity *= 2;
        }
        sPipeStart = (uint8_t*)realloc(sPipeStart, capacity);
        sPipeCapacity = capacity;
        gPORTGXPipe.cur = sPipeStart + used;
    }
    gPORTGXPipe.end = sPipeStart + sPipeCapacity;
}

/* GXFlush ends with a sync: the point where the GPU sees what was written. */
void PPCSync(void) {
    Drain();
}

unsigned int PortGXStreamErrors(void) {
    return sStats.errors;
}

/* ------------------------------------------------------------------------------------ */
/* FIFO objects (GXFifo.c). The game's FIFO memory is not used for commands.             */

static GXBreakPtCallback sBreakPtCallback;
static const GXFifoObj* sCPUFifo;
static const GXFifoObj* sGPFifo;

void __GXFifoInit(void) {
    InitPipe();
}

void GXInitFifoBase(GXFifoObj* fifo, void* base, u32 size) {
    __GXFifoObj* realFifo = (__GXFifoObj*)fifo;
    realFifo->base = (u8*)base;
    realFifo->top = (u8*)base + size - 4;
    realFifo->size = size;
    realFifo->count = 0;
    realFifo->hiWatermark = size - 0x4000;
    realFifo->loWatermark = (size >> 1) & ~31u;
    realFifo->rdPtr = base;
    realFifo->wrPtr = base;
    realFifo->wrap = GX_FALSE;
    realFifo->bind_cpu = GX_FALSE;
    realFifo->bind_gp = GX_FALSE;
}

void GXInitFifoPtrs(GXFifoObj* fifo, void* readPtr, void* writePtr) {
    __GXFifoObj* realFifo = (__GXFifoObj*)fifo;
    realFifo->rdPtr = readPtr;
    realFifo->wrPtr = writePtr;
    realFifo->count = 0;
}

void GXInitFifoLimits(GXFifoObj* fifo, u32 hi, u32 lo) {
    __GXFifoObj* realFifo = (__GXFifoObj*)fifo;
    realFifo->hiWatermark = hi;
    realFifo->loWatermark = lo;
}

void GXSetCPUFifo(const GXFifoObj* fifo) {
    InitPipe();
    sCPUFifo = fifo;
    if (fifo != NULL) {
        ((__GXFifoObj*)fifo)->bind_cpu = GX_TRUE;
    }
}

void GXSetGPFifo(const GXFifoObj* fifo) {
    sGPFifo = fifo;
    if (fifo != NULL) {
        ((__GXFifoObj*)fifo)->bind_gp = GX_TRUE;
    }
}

GXBool GXGetCPUFifo(GXFifoObj* fifo) {
    if (sCPUFifo == NULL) {
        return GX_FALSE;
    }
    memcpy(fifo, sCPUFifo, sizeof(*fifo));
    return GX_TRUE;
}

GXBool GXGetGPFifo(GXFifoObj* fifo) {
    if (sGPFifo == NULL) {
        return GX_FALSE;
    }
    memcpy(fifo, sGPFifo, sizeof(*fifo));
    return GX_TRUE;
}

void GXGetFifoPtrs(const GXFifoObj* fifo, void** readPtr, void** writePtr) {
    const __GXFifoObj* realFifo = (const __GXFifoObj*)fifo;
    *readPtr = realFifo->rdPtr;
    *writePtr = realFifo->wrPtr;
}

GXBool GXGetFifoWrap(const GXFifoObj* fifo) {
    (void)fifo;
    return GX_FALSE;
}

GXBool __GXIsGPFifoReady(void) {
    return GX_TRUE;
}

void __GXCleanGPFifo(void) {
    /* An aborted frame: whatever the GPU had not read is dropped. */
    if (sPipeStart != NULL) {
        gPORTGXPipe.cur = sPipeStart;
    }
}

GXBreakPtCallback GXSetBreakPtCallback(GXBreakPtCallback cb) {
    GXBreakPtCallback old = sBreakPtCallback;
    sBreakPtCallback = cb;
    return old;
}

void GXEnableBreakPt(void* breakPoint) {
    (void)breakPoint;
}

void GXDisableBreakPt(void) {}

void GXGetGPStatus(GXBool* overhi, GXBool* underlow, GXBool* readIdle, GXBool* cmdIdle, GXBool* brkpt) {
    Drain();
    *overhi = GX_FALSE;
    *underlow = GX_TRUE;
    *readIdle = GX_TRUE;
    *cmdIdle = GX_TRUE;
    *brkpt = GX_FALSE;
}

/* ------------------------------------------------------------------------------------ */
/* EFB peeks and projection                                                              */

u32 PORTGXPeekARGB(u16 x, u16 y) {
    (void)x;
    (void)y;
    return 0;
}

u32 PORTGXPeekZ(u16 x, u16 y) {
    (void)x;
    (void)y;
    return 0xFFFFFF;
}

void GXProject(f32 x, f32 y, f32 z, const f32 mtx[3][4], const f32* pm, const f32* vp, f32* sx, f32* sy, f32* sz) {
    f32 peye[3];
    f32 xc, yc, zc, wc;

    peye[0] = mtx[0][0] * x + mtx[0][1] * y + mtx[0][2] * z + mtx[0][3];
    peye[1] = mtx[1][0] * x + mtx[1][1] * y + mtx[1][2] * z + mtx[1][3];
    peye[2] = mtx[2][0] * x + mtx[2][1] * y + mtx[2][2] * z + mtx[2][3];

    if (pm[0] == (f32)GX_PERSPECTIVE) {
        xc = peye[0] * pm[1] + peye[2] * pm[2];
        yc = peye[1] * pm[3] + peye[2] * pm[4];
        zc = pm[6] + peye[2] * pm[5];
        wc = 1.0f / -peye[2];
    } else {
        xc = peye[0] * pm[1] + pm[2];
        yc = peye[1] * pm[3] + pm[4];
        zc = pm[6] + peye[2] * pm[5];
        wc = 1.0f;
    }
    *sx = vp[2] / 2.0f + vp[0] + wc * xc * vp[2] / 2.0f;
    *sy = vp[3] / 2.0f + vp[1] - wc * yc * vp[3] / 2.0f;
    *sz = vp[5] + wc * zc * (vp[5] - vp[4]);
}
