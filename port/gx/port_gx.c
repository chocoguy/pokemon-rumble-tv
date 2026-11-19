/* The GX command pipe and the command-stream decoder.
 *
 * The compiled GX library writes the console's command bytes into gPORTGXPipe (see
 * port_gx_pipe.h). This file owns that buffer and plays the GPU's command processor:
 * whenever the game flushes (GXFlush ends in PPCSync) or the buffer fills, everything
 * written so far is decoded. Display lists the stream calls are decoded where they lie in
 * game memory.
 *
 * The decoder checks that every command is well formed (known opcode, XF writes inside XF
 * memory, display lists inside the address window, vertex data sized from the current
 * VCD/VAT), keeps the CP, XF and BP registers, and does what the GPU's side effects
 * require: the draw-done and token interrupts, the PE token register, TLUT loads into
 * TMEM, EFB copies. Draws are gathered into batches that share all state; a batch goes
 * to the backend (port_gfx.h) when a register it depends on changes. Without a backend
 * (PortGfxInit not called or failed) nothing is drawn and the stream is only checked.
 */
#include "port_gx_internal.h"
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
PortGXRegs gPortGX;

static uint8_t* sPipeStart;
static size_t sPipeCapacity;
static int sGfx; /* a backend is drawing */

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
    unsigned long long batches;
    unsigned int errors;
} GXStats;
static GXStats sStats;

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
    uint32_t lo = gPortGX.cp[0x50];
    uint32_t hi = gPortGX.cp[0x60];
    uint32_t a = gPortGX.cp[0x70 + vat];
    uint32_t b = gPortGX.cp[0x80 + vat];
    uint32_t c = gPortGX.cp[0x90 + vat];
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
/* Batches                                                                               */

static struct {
    int active;
    PortGXVertex* vertices;
    uint32_t vertexCount, vertexCapacity;
    uint32_t* indices;
    uint32_t indexCount, indexCapacity;
    PortGfxDraw draw;
    PortGXShaderKey key;
    PortGXUniforms uniforms;
    int lines; /* lines and points, expanded to quads: a batch of their own, never culled */
} sBatch;

/* PORT_GX_TRACE=<retrace>: logs every batch drawn during that frame. */
static unsigned int sTraceRetrace;
static unsigned int sCurrentRetrace;

static void TraceBatch(void) {
    const PortGfxDraw* d = &sBatch.draw;
    const PortGXShaderKey* k = &sBatch.key;
    const PortGfxRaster* r = &d->raster;
    uint32_t i;
    fprintf(stderr, "[trace] batch %llu: %u verts %u idx, %u stages %u texgens %u chans, tex 0x%x, blend %d %d/%d sub %d logic %d/%d upd %d%d, z %d/%d/%d, cull %d, scissor %d,%d-%d,%d, afunc 0x%02x\n",
            sStats.batches, d->vertexCount, d->indexCount, k->numStages, k->numTexGens, k->numChans, k->texMask,
            r->blendEnable, r->srcFactor, r->dstFactor, r->blendSubtract, r->logicEnable, r->logicOp, r->colorUpdate,
            r->alphaUpdate, r->zEnable, r->zFunc, r->zUpdate, r->cull, r->scissor[0], r->scissor[1], r->scissor[2],
            r->scissor[3], k->alphaFunc);
    for (i = 0; i < k->numStages; i++) {
        fprintf(stderr, "[trace]   stage %u: color %06x alpha %06x tref %03x k %u/%u ind %06x\n", i, k->colorEnv[i],
                k->alphaEnv[i], k->tref[i], k->kcsel[i], k->kasel[i], k->indCmd[i]);
    }
    if (k->numIndStages != 0) {
        fprintf(stderr, "[trace]   %u indirect stages, iref %06x, scale %04x %04x\n", k->numIndStages, k->indRef,
                k->indScale[0], k->indScale[1]);
    }
    for (i = 0; i < 8; i++) {
        if (k->texMask & (1u << i)) {
            uint32_t image0 = gPortGX.bp[(i < 4 ? 0x88 : 0xA8) + (i & 3)];
            fprintf(stderr, "[trace]   tex%u: %ux%u fmt %u at 0x%08x\n", i, (image0 & 0x3FF) + 1, ((image0 >> 10) & 0x3FF) + 1,
                    (image0 >> 20) & 15, (gPortGX.bp[(i < 4 ? 0x94 : 0xB4) + (i & 3)] & 0xFFFFFF) << 5);
        }
    }
    if (d->vertexCount > 0) {
        const PortGXVertex* v = &d->vertices[0];
        fprintf(stderr, "[trace]   v0 pos %.2f %.2f %.2f clr %.2f %.2f %.2f %.2f tex %.3f %.3f mtx %u\n", v->pos[0], v->pos[1],
                v->pos[2], v->clr[0][0], v->clr[0][1], v->clr[0][2], v->clr[0][3], v->tex[0][0], v->tex[0][1], v->mtx[0] & 63);
        {
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            uint32_t k, j, mtxs = 0;
            for (k = 0; k < d->vertexCount; k++) {
                for (j = 0; j < 3; j++) {
                    float p = d->vertices[k].pos[j];
                    lo[j] = p < lo[j] ? p : lo[j];
                    hi[j] = p > hi[j] ? p : hi[j];
                }
                mtxs |= 1u << ((d->vertices[k].mtx[0] & 63) / 3 % 32);
            }
            fprintf(stderr, "[trace]   pos range %.2f..%.2f %.2f..%.2f %.2f..%.2f, matrices 0x%x\n", lo[0], hi[0], lo[1], hi[1],
                    lo[2], hi[2], mtxs);
            for (j = 0; j < 3; j++) {
                lo[j] = 1e30f;
                hi[j] = -1e30f;
            }
            for (k = 0; k < d->vertexCount; k++) {
                const PortGXVertex* q = &d->vertices[k];
                uint32_t r = (q->mtx[0] & 63) * 4;
                for (j = 0; j < 3; j++) {
                    float m4[4], e;
                    memcpy(m4, &gPortGX.xf[r + j * 4], 16);
                    e = m4[0] * q->pos[0] + m4[1] * q->pos[1] + m4[2] * q->pos[2] + m4[3];
                    lo[j] = e < lo[j] ? e : lo[j];
                    hi[j] = e > hi[j] ? e : hi[j];
                }
            }
            fprintf(stderr, "[trace]   eye range %.1f..%.1f %.1f..%.1f %.1f..%.1f\n", lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]);
            if (getenv("PORT_GX_TRACE_VERTS") != NULL) {
                for (k = 0; k < d->vertexCount && k < 24; k++) {
                    fprintf(stderr, "[trace]     v%u %.3f %.3f %.3f n %.2f %.2f %.2f t %.3f %.3f\n", k, d->vertices[k].pos[0],
                            d->vertices[k].pos[1], d->vertices[k].pos[2], d->vertices[k].nrm[0], d->vertices[k].nrm[1],
                            d->vertices[k].nrm[2], d->vertices[k].tex[0][0], d->vertices[k].tex[0][1]);
                }
                fprintf(stderr, "[trace]     idx");
                for (k = 0; k < d->indexCount && k < 36; k++) {
                    fprintf(stderr, " %u", d->indices[k]);
                }
                fprintf(stderr, "\n");
            }
        }
        {
            /* Where v0 lands: model-view rows from XF memory, then the projection. */
            float m[12], e[3], pr[6], c[4];
            uint32_t row = (v->mtx[0] & 63) * 4, i;
            uint32_t type = gPortGX.xf[XFREG(0x1026)];
            for (i = 0; i < 12; i++) {
                memcpy(&m[i], &gPortGX.xf[row + i], 4);
            }
            for (i = 0; i < 6; i++) {
                memcpy(&pr[i], &gPortGX.xf[XFREG(0x1020) + i], 4);
            }
            for (i = 0; i < 3; i++) {
                e[i] = m[i * 4] * v->pos[0] + m[i * 4 + 1] * v->pos[1] + m[i * 4 + 2] * v->pos[2] + m[i * 4 + 3];
            }
            if (type == 0) {
                c[0] = pr[0] * e[0] + pr[1] * e[2];
                c[1] = pr[2] * e[1] + pr[3] * e[2];
                c[2] = pr[4] * e[2] + pr[5];
                c[3] = -e[2];
            } else {
                c[0] = pr[0] * e[0] + pr[1];
                c[1] = pr[2] * e[1] + pr[3];
                c[2] = pr[4] * e[2] + pr[5];
                c[3] = 1.0f;
            }
            fprintf(stderr, "[trace]   mtx row0 %.3f %.3f %.3f %.3f eye %.2f %.2f %.2f clip %.2f %.2f %.2f w %.2f (%s)\n", m[0], m[1],
                    m[2], m[3], e[0], e[1], e[2], c[0], c[1], c[2], c[3], type == 0 ? "persp" : "ortho");
        }
    }
    for (i = 0; i < k->numChans; i++) {
        uint32_t cc = k->chanCtrl[i], ac = k->chanCtrl[2 + i], l;
        uint32_t lights = (((cc >> 2) & 15) | ((cc >> 11) & 15) << 4) | (((ac >> 2) & 15) | ((ac >> 11) & 15) << 4);
        fprintf(stderr, "[trace]   chan%u: ctrl %04x/%04x mat %08x amb %08x\n", i, cc, ac, gPortGX.xf[XFREG(0x100C + i)],
                gPortGX.xf[XFREG(0x100A + i)]);
        for (l = 0; l < 8; l++) {
            if (lights & (1u << l)) {
                float f[13];
                uint32_t la = 0x600 + l * 16, j;
                for (j = 0; j < 12; j++) {
                    memcpy(&f[j], &gPortGX.xf[la + 4 + j], 4);
                }
                fprintf(stderr, "[trace]     light%u: color %08x cos %.3g %.3g %.3g dist %.3g %.3g %.3g pos %.3g %.3g %.3g dir %.3g %.3g %.3g\n",
                        l, gPortGX.xf[la + 3], f[0], f[1], f[2], f[3], f[4], f[5], f[6], f[7], f[8], f[9], f[10], f[11]);
            }
        }
    }
    fprintf(stderr, "[trace]   matrix index CP %08x %08x XF %08x %08x\n", gPortGX.cp[0x30], gPortGX.cp[0x40],
            gPortGX.xf[XFREG(0x1018)], gPortGX.xf[XFREG(0x1019)]);
    for (i = 0; i < k->numTexGens; i++) {
        fprintf(stderr, "[trace]   texgen%u: %05x post %08x\n", i, k->texGen[i], gPortGX.xf[XFREG(0x1050 + i)]);
        if (d->vertexCount > 0) {
            static const int kShift[8] = {8, 16, 24, 0, 8, 16, 24, 0};
            const uint32_t* mt = d->vertices[0].mtx;
            uint32_t word = i < 3 ? mt[0] : i < 7 ? mt[1] : mt[2];
            uint32_t row = ((word >> kShift[i]) & 63) * 4, j;
            uint32_t post = 0x500 + (gPortGX.xf[XFREG(0x1050 + i)] & 63) * 4;
            float m[12], p[12];
            for (j = 0; j < 12; j++) {
                memcpy(&m[j], &gPortGX.xf[row + j], 4);
                memcpy(&p[j], &gPortGX.xf[post + j], 4);
            }
            fprintf(stderr, "[trace]     texmtx @%03x: %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g\n", row,
                    m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11]);
            fprintf(stderr, "[trace]     post @%03x: %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g\n", post,
                    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8], p[9], p[10], p[11]);
        }
    }
    if (d->vertexCount > 0) {
        float n[9];
        uint32_t j, na = 0x400 + (d->vertices[0].mtx[0] & 31) * 3;
        for (j = 0; j < 9; j++) {
            memcpy(&n[j], &gPortGX.xf[na + j], 4);
        }
        fprintf(stderr, "[trace]   nrm mtx @%03x: %.3g %.3g %.3g / %.3g %.3g %.3g / %.3g %.3g %.3g\n", na, n[0], n[1], n[2], n[3],
                n[4], n[5], n[6], n[7], n[8]);
    }
    fprintf(stderr, "[trace]   tevreg prev %d %d %d %d c0 %d %d %d %d c1 %d %d %d %d\n", gPortGX.tevReg[0][0], gPortGX.tevReg[0][1],
            gPortGX.tevReg[0][2], gPortGX.tevReg[0][3], gPortGX.tevReg[1][0], gPortGX.tevReg[1][1], gPortGX.tevReg[1][2],
            gPortGX.tevReg[1][3], gPortGX.tevReg[2][0], gPortGX.tevReg[2][1], gPortGX.tevReg[2][2], gPortGX.tevReg[2][3]);
    fprintf(stderr, "[trace]   tevreg c2 %d %d %d %d konst %d %d %d %d / %d %d %d %d / %d %d %d %d / %d %d %d %d, alpha ref %u %u, cmode1 %06x\n",
            gPortGX.tevReg[3][0], gPortGX.tevReg[3][1], gPortGX.tevReg[3][2], gPortGX.tevReg[3][3], gPortGX.tevKonst[0][0],
            gPortGX.tevKonst[0][1], gPortGX.tevKonst[0][2], gPortGX.tevKonst[0][3], gPortGX.tevKonst[1][0], gPortGX.tevKonst[1][1],
            gPortGX.tevKonst[1][2], gPortGX.tevKonst[1][3], gPortGX.tevKonst[2][0], gPortGX.tevKonst[2][1], gPortGX.tevKonst[2][2],
            gPortGX.tevKonst[2][3], gPortGX.tevKonst[3][0], gPortGX.tevKonst[3][1], gPortGX.tevKonst[3][2], gPortGX.tevKonst[3][3],
            gPortGX.bp[0xF3] & 0xFF, (gPortGX.bp[0xF3] >> 8) & 0xFF, gPortGX.bp[0x42] & 0xFFFFFF);
}

static unsigned long long sDrawFirst, sDrawLast;

static void FlushBatch(void) {
    if (!sBatch.active) {
        return;
    }
    sBatch.active = 0;
    if (sBatch.indexCount == 0) {
        sBatch.vertexCount = 0;
        return;
    }
    sBatch.draw.vertices = sBatch.vertices;
    sBatch.draw.vertexCount = sBatch.vertexCount;
    sBatch.draw.indices = sBatch.indices;
    sBatch.draw.indexCount = sBatch.indexCount;
    if (sTraceRetrace != 0 && sCurrentRetrace == sTraceRetrace) {
        TraceBatch();
    }
    /* PORT_GX_DRAW=first-last: draw only those batches (numbers from the trace). */
    if (sDrawFirst == 0 || (sStats.batches >= sDrawFirst && sStats.batches <= sDrawLast)) {
        PortGfxDrawBatch(&sBatch.draw);
    }
    sStats.batches++;
    sBatch.vertexCount = 0;
    sBatch.indexCount = 0;
}

/* A register the current batch depends on is about to change. */
static inline void Touch(void) {
    if (sBatch.active) {
        FlushBatch();
    }
}

static void RasterState(PortGfxRaster* r) {
    const uint32_t* bp = gPortGX.bp;
    uint32_t cmode0 = bp[0x41];
    uint32_t zmode = bp[0x40];
    uint32_t pix = bp[0x43] & 7;
    int offX = (int)((bp[0x59] & 0x3FF) * 2);
    int offY = (int)(((bp[0x59] >> 10) & 0x3FF) * 2);
    int x0 = (int)((bp[0x20] >> 12) & 0x7FF) - offX;
    int y0 = (int)(bp[0x20] & 0x7FF) - offY;
    int x1 = (int)((bp[0x21] >> 12) & 0x7FF) - offX + 1;
    int y1 = (int)(bp[0x21] & 0x7FF) - offY + 1;

    memset(r, 0, sizeof(*r));
    r->blendEnable = cmode0 & 1;
    r->logicEnable = (cmode0 >> 1) & 1;
    r->colorUpdate = (cmode0 >> 3) & 1;
    r->alphaUpdate = (cmode0 >> 4) & 1;
    r->dstFactor = (cmode0 >> 5) & 7;
    r->srcFactor = (cmode0 >> 8) & 7;
    r->blendSubtract = (cmode0 >> 11) & 1;
    r->logicOp = (cmode0 >> 12) & 15;
    r->dstAlphaEnable = (bp[0x42] >> 8) & 1;
    r->efbHasAlpha = pix == 1;
    r->zEnable = zmode & 1;
    r->zFunc = (zmode >> 1) & 7;
    r->zUpdate = (zmode >> 4) & 1;
    r->cull = (bp[0x00] >> 14) & 3;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 > PORT_EFB_WIDTH ? PORT_EFB_WIDTH : x1;
    y1 = y1 > PORT_EFB_HEIGHT ? PORT_EFB_HEIGHT : y1;
    r->scissor[0] = (int16_t)x0;
    r->scissor[1] = (int16_t)y0;
    r->scissor[2] = (int16_t)(x1 > x0 ? x1 : x0);
    r->scissor[3] = (int16_t)(y1 > y0 ? y1 : y0);
}

static void BeginBatch(void) {
    PortGfxDraw* d = &sBatch.draw;
    int i;
    memset(d, 0, sizeof(*d));
    PortGXBuildShader(&sBatch.key, &sBatch.uniforms, &d->shaderHash);
    d->shaderKey = &sBatch.key;
    d->shaderKeyBytes = sizeof(sBatch.key);
    d->uniforms = &sBatch.uniforms;
    d->uniformBytes = sizeof(sBatch.uniforms);
    d->xf = gPortGX.xf;
    d->xfWords = PORT_XF_WORDS;
    d->xfVersion = gPortGX.xfVersion;
    for (i = 0; i < 8; i++) {
        if (sBatch.key.texMask & (1u << i)) {
            d->textures[i] = PortGXTexture(i);
            d->samplerMode0[i] = gPortGX.bp[(i < 4 ? 0x80 : 0xA0) + (i & 3)];
            d->samplerMode1[i] = gPortGX.bp[(i < 4 ? 0x84 : 0xA4) + (i & 3)];
        }
    }
    RasterState(&d->raster);
    sBatch.active = 1;
}

static void Reserve(uint32_t vertices, uint32_t indices) {
    if (sBatch.vertexCount + vertices > sBatch.vertexCapacity) {
        uint32_t cap = sBatch.vertexCapacity ? sBatch.vertexCapacity : 4096;
        while (cap < sBatch.vertexCount + vertices) {
            cap *= 2;
        }
        sBatch.vertices = (PortGXVertex*)realloc(sBatch.vertices, cap * sizeof(PortGXVertex));
        sBatch.vertexCapacity = cap;
    }
    if (sBatch.indexCount + indices > sBatch.indexCapacity) {
        uint32_t cap = sBatch.indexCapacity ? sBatch.indexCapacity : 8192;
        while (cap < sBatch.indexCount + indices) {
            cap *= 2;
        }
        sBatch.indices = (uint32_t*)realloc(sBatch.indices, cap * sizeof(uint32_t));
        sBatch.indexCapacity = cap;
    }
}

/* Lines, line strips and points become quads the vertex shader sizes on screen: each
 * corner is a copy of its vertex whose mtx[3] says how to move it (PORT_GX_LINE: the
 * side and the offset to the other end's copy; PORT_GX_POINT: the corner). */
#define PORT_GX_LINE 0x80000000u
#define PORT_GX_POINT 0x90000000u

static void DrawLinesPoints(int op, int prim, const uint8_t* data, uint32_t count) {
    static PortGXVertex* sScratch;
    static uint32_t sScratchCap;
    uint32_t segments, i, k;
    if (count > sScratchCap) {
        sScratchCap = count * 2;
        sScratch = (PortGXVertex*)realloc(sScratch, sScratchCap * sizeof(PortGXVertex));
    }
    if (PortGXLoadVertices(op & 7, data, count, sScratch) == 0) {
        return;
    }
    if (sBatch.active && !sBatch.lines) {
        FlushBatch();
    }
    if (!sBatch.active) {
        BeginBatch();
        sBatch.lines = 1;
        sBatch.draw.raster.cull = 0;
    }
    if (prim == 7) {
        Reserve(count * 4, count * 6);
        for (i = 0; i < count; i++) {
            uint32_t base = sBatch.vertexCount;
            for (k = 0; k < 4; k++) {
                sBatch.vertices[base + k] = sScratch[i];
                sBatch.vertices[base + k].mtx[3] = PORT_GX_POINT | k;
            }
            sBatch.indices[sBatch.indexCount++] = base;
            sBatch.indices[sBatch.indexCount++] = base + 1;
            sBatch.indices[sBatch.indexCount++] = base + 3;
            sBatch.indices[sBatch.indexCount++] = base;
            sBatch.indices[sBatch.indexCount++] = base + 3;
            sBatch.indices[sBatch.indexCount++] = base + 2;
            sBatch.vertexCount += 4;
        }
        return;
    }
    segments = prim == 5 ? count / 2 : count - 1;
    Reserve(segments * 4, segments * 6);
    for (i = 0; i < segments; i++) {
        const PortGXVertex* a = &sScratch[prim == 5 ? 2 * i : i];
        const PortGXVertex* b = &sScratch[prim == 5 ? 2 * i + 1 : i + 1];
        uint32_t base = sBatch.vertexCount;
        /* Corners 0, 1 at a (sides -, +), 2, 3 at b; each points 2 copies ahead or back. */
        sBatch.vertices[base] = *a;
        sBatch.vertices[base].mtx[3] = PORT_GX_LINE | 0x0002u;
        sBatch.vertices[base + 1] = *a;
        sBatch.vertices[base + 1].mtx[3] = PORT_GX_LINE | 0x10000u | 0x0002u;
        sBatch.vertices[base + 2] = *b;
        sBatch.vertices[base + 2].mtx[3] = PORT_GX_LINE | 0xFFFEu;
        sBatch.vertices[base + 3] = *b;
        sBatch.vertices[base + 3].mtx[3] = PORT_GX_LINE | 0x10000u | 0xFFFEu;
        sBatch.indices[sBatch.indexCount++] = base;
        sBatch.indices[sBatch.indexCount++] = base + 1;
        sBatch.indices[sBatch.indexCount++] = base + 3;
        sBatch.indices[sBatch.indexCount++] = base;
        sBatch.indices[sBatch.indexCount++] = base + 3;
        sBatch.indices[sBatch.indexCount++] = base + 2;
        sBatch.vertexCount += 4;
    }
}

static void Draw(int op, const uint8_t* data, uint32_t count) {
    int prim = (op >> 3) & 7;
    if (!sGfx || count == 0) {
        return;
    }
    if (prim >= 5) {
        DrawLinesPoints(op, prim, data, count); /* never culled */
        return;
    }
    if (((gPortGX.bp[0x00] >> 14) & 3) == 3) {
        return; /* everything culled */
    }
    if (sBatch.active && sBatch.lines) {
        FlushBatch();
    }
    if (!sBatch.active) {
        BeginBatch();
        sBatch.lines = 0;
    }
    Reserve(count, 3 * count);
    if (sTraceRetrace != 0 && sCurrentRetrace == sTraceRetrace && getenv("PORT_GX_TRACE_VERTS") != NULL) {
        int a, vat = op & 7, n = VertexSize(vat), k;
        fprintf(stderr, "[trace] draw prim %d vat %d count %u: vcd %08x %08x vat %08x %08x %08x, %d bytes/vertex:", prim, vat,
                count, gPortGX.cp[0x50], gPortGX.cp[0x60], gPortGX.cp[0x70 + vat], gPortGX.cp[0x80 + vat],
                gPortGX.cp[0x90 + vat], n);
        for (k = 0; k < n * 3 && k < 48; k++) {
            fprintf(stderr, " %02x", data[k]);
        }
        fprintf(stderr, "\n[trace]   arrays");
        for (a = 0; a < 6; a++) {
            fprintf(stderr, " %d:%08x/%u", a, gPortGX.cp[0xA0 + a], gPortGX.cp[0xB0 + a] & 0xFF);
        }
        fprintf(stderr, "\n");
    }
    if (PortGXLoadVertices(op & 7, data, count, sBatch.vertices + sBatch.vertexCount) == 0) {
        return;
    }
    sBatch.indexCount += PortGXPrimitiveIndices(prim, sBatch.vertexCount, count, sBatch.indices + sBatch.indexCount);
    sBatch.vertexCount += count;
}

/* ------------------------------------------------------------------------------------ */
/* EFB copies and the display                                                            */

static unsigned int sDumpRetrace[16];
static char* sDumpPath[16];
static int sDumpCount;

void PortGXRequestDump(unsigned int retrace, const char* path) {
    if (sDumpCount < 16) {
        sDumpRetrace[sDumpCount] = retrace;
        sDumpPath[sDumpCount] = strdup(path);
        sDumpCount++;
    }
}

static unsigned int sDumpEvery;
static char* sDumpDir;

void PortGXRequestDumpEvery(unsigned int every, const char* dir) {
    sDumpEvery = every;
    sDumpDir = strdup(dir);
}

static int CopyKind(uint32_t cmd, int depth) {
    uint32_t fmt = ((cmd >> 4) & 7) | ((cmd >> 3) & 1) << 3;
    int intensity = ((cmd >> 15) & 3) == 3;
    if (depth) {
        return PORT_COPY_Z;
    }
    switch (fmt) {
    case 0:
    case 1:
        return intensity ? PORT_COPY_I : PORT_COPY_R;
    case 2:
    case 3:
        return intensity ? PORT_COPY_IA : PORT_COPY_RGBA;
    case 4:
        return PORT_COPY_RGB;
    case 7:
        return PORT_COPY_A;
    case 8:
        return PORT_COPY_R;
    case 9:
        return PORT_COPY_G;
    case 10:
        return PORT_COPY_B;
    case 11:
        return PORT_COPY_RG;
    case 12:
        return PORT_COPY_GB;
    default:
        return PORT_COPY_RGBA;
    }
}

static void CopyEFB(uint32_t cmd) {
    const uint32_t* bp = gPortGX.bp;
    PortGfxCopy c;
    PortGfxTexture* t;
    uint32_t addr = (bp[0x4B] & 0xFFFFFF) << 5;
    int toDisplay = (cmd >> 14) & 1;
    uint32_t cmode0 = bp[0x41];

    memset(&c, 0, sizeof(c));
    c.x = (int)(bp[0x49] & 0x3FF);
    c.y = (int)((bp[0x49] >> 10) & 0x3FF);
    c.width = (int)(bp[0x4A] & 0x3FF) + 1;
    c.height = (int)((bp[0x4A] >> 10) & 0x3FF) + 1;
    c.efbHasAlpha = (bp[0x43] & 7) == 1;
    c.clear = (cmd >> 11) & 1;
    c.clearColor[0] = (uint8_t)bp[0x4F];
    c.clearColor[1] = (uint8_t)(bp[0x50] >> 8);
    c.clearColor[2] = (uint8_t)bp[0x50];
    c.clearColor[3] = (uint8_t)(bp[0x4F] >> 8);
    c.clearZ = bp[0x51] & 0xFFFFFF;
    c.clearColorMask = ((cmode0 >> 3) & 1) | (((cmode0 >> 4) & 1) << 1);
    c.clearZUpdate = (bp[0x40] >> 4) & 1;
    if (toDisplay) {
        uint32_t yscale = bp[0x4E] & 0x1FF;
        sStats.displayCopies++;
        c.kind = PORT_COPY_RGB;
        c.dstWidth = c.width;
        c.dstHeight = c.height;
        if (((cmd >> 10) & 1) && yscale != 0) {
            c.dstHeight = (int)((c.height * 256 + yscale / 2) / yscale);
        }
    } else {
        sStats.textureCopies++;
        c.kind = CopyKind(cmd, (bp[0x43] & 7) == 3);
        c.halfScale = (cmd >> 9) & 1;
        c.dstWidth = c.halfScale ? (c.width + 1) / 2 : c.width;
        c.dstHeight = c.halfScale ? (c.height + 1) / 2 : c.height;
    }
    if (!sGfx) {
        return;
    }
    Touch();
    if (sTraceRetrace != 0 && sCurrentRetrace == sTraceRetrace) {
        fprintf(stderr, "[trace] copy after batch %llu: cmd %06x pixfmt %u %s fmt %u kind %d, %d,%d %dx%d -> %dx%d at 0x%08x, clear %d z %06x\n",
                sStats.batches, cmd, bp[0x43] & 7, toDisplay ? "display" : "texture",
                ((cmd >> 4) & 7) | ((cmd >> 3) & 1) << 3, c.kind, c.x, c.y, c.width, c.height, c.dstWidth, c.dstHeight, addr,
                c.clear, c.clearZ);
    }
    t = PortGfxCopyEFB(&c);
    if (t == NULL) {
        return;
    }
    if (toDisplay) {
        PortGXFrameBufferCopied(addr, t);
        PortGXTextureFrame();
    } else {
        PortGXTextureCopied(addr, t, c.dstWidth, c.dstHeight);
    }
}

void PortGXRetrace(unsigned int retrace, void* frameBuffer, int black) {
    PortGfxTexture* t = NULL;
    int i;
    sCurrentRetrace = retrace;
    if (!sGfx) {
        return;
    }
    if (!black && frameBuffer != NULL) {
        t = PortGXFrameBufferTexture(PortPhys(frameBuffer));
    }
    PortGfxPresent(t);
    for (i = 0; i < sDumpCount; i++) {
        if (sDumpRetrace[i] == retrace) {
            if (t == NULL) {
                PortLog("dump at retrace %u: the display is black", retrace);
            } else if (PortGfxWritePNG(t, sDumpPath[i]) == 0) {
                PortLog("dump at retrace %u: %s", retrace, sDumpPath[i]);
            } else {
                PortRed("dump at retrace %u: could not write %s", retrace, sDumpPath[i]);
            }
        }
    }
    if (sDumpEvery != 0 && retrace % sDumpEvery == 0 && t != NULL) {
        char path[1024];
        snprintf(path, sizeof(path), "%s/frame-%05u.png", sDumpDir, retrace);
        PortGfxWritePNG(t, path);
    }
}

int PortGXStart(int scale) {
    const char* trace = getenv("PORT_GX_TRACE");
    const char* draw = getenv("PORT_GX_DRAW");
    if (draw != NULL && sscanf(draw, "%llu-%llu", &sDrawFirst, &sDrawLast) != 2) {
        sDrawFirst = sDrawLast = 0;
    }
    if (trace != NULL) {
        sTraceRetrace = (unsigned int)strtoul(trace, NULL, 10);
    }
    sGfx = PortGfxInit(scale);
    return sGfx;
}

/* ------------------------------------------------------------------------------------ */
/* Register side effects                                                                 */

static int32_t Sign11(uint32_t v) {
    return ((int32_t)(v << 21)) >> 21;
}

static void LoadTlut(uint32_t data) {
    uint32_t src = (gPortGX.bp[0x64] & 0xFFFFFF) << 5;
    uint32_t tmem = (data & 0x3FF) << 9;
    uint32_t bytes = ((data >> 10) & 0x7FF) * 32;
    static int reported;
    if (src + bytes > PORT_ADDR_WINDOW_SIZE || tmem + bytes > sizeof(gPortTMEM)) {
        PortRedOnce(&reported, "GX: TLUT load from 0x%08x (%u bytes) to TMEM 0x%x is out of range", src, bytes, tmem);
        return;
    }
    memcpy(gPortTMEM + tmem, PortPtrRaw(src), bytes);
    PortGXTextureInvalidate();
}

static void WriteBP(uint32_t value) {
    uint32_t reg = value >> 24;
    uint32_t data = value & 0xFFFFFF;
    uint32_t newValue;

    sStats.bpWrites++;
    if (reg == 0xFE) {
        sBPMask = data;
        return;
    }
    newValue = (gPortGX.bp[reg] & ~sBPMask) | (data & sBPMask);
    sBPMask = 0xFFFFFF;
    if (newValue != gPortGX.bp[reg]) {
        Touch();
    }
    gPortGX.bp[reg] = newValue;
    data = newValue;

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
        CopyEFB(data);
        break;
    case 0x65: /* TLUT load */
        Touch();
        LoadTlut(data);
        break;
    case 0x66: /* texture cache invalidate */
        Touch();
        PortGXTextureInvalidate();
        break;
    case 0xE0:
    case 0xE2:
    case 0xE4:
    case 0xE6: {
        int n = (int)(reg - 0xE0) / 2;
        if (data & (1u << 23)) {
            gPortGX.tevKonst[n][0] = (int32_t)(data & 0xFF);
            gPortGX.tevKonst[n][3] = (int32_t)((data >> 12) & 0xFF);
        } else {
            gPortGX.tevReg[n][0] = Sign11(data & 0x7FF);
            gPortGX.tevReg[n][3] = Sign11((data >> 12) & 0x7FF);
        }
        break;
    }
    case 0xE1:
    case 0xE3:
    case 0xE5:
    case 0xE7: {
        int n = (int)(reg - 0xE1) / 2;
        if (data & (1u << 23)) {
            gPortGX.tevKonst[n][2] = (int32_t)(data & 0xFF);
            gPortGX.tevKonst[n][1] = (int32_t)((data >> 12) & 0xFF);
        } else {
            gPortGX.tevReg[n][2] = Sign11(data & 0x7FF);
            gPortGX.tevReg[n][1] = Sign11((data >> 12) & 0x7FF);
        }
        break;
    }
    }
}

static void WriteCP(uint32_t reg, uint32_t value) {
    sStats.cpWrites++;
    if (gPortGX.cp[reg] != value) {
        Touch();
        gPortGX.cp[reg] = value;
    }
}

static int ValidXF(uint32_t addr, uint32_t count) {
    uint32_t end = addr + count;
    return end <= 0x0800 || (addr >= 0x1000 && end <= 0x1058);
}

/* `native`: the words are in host order (memory the CPU filled) rather than the command
 * stream's big-endian order. */
static void WriteXFWords(uint32_t addr, const uint8_t* words, uint32_t count, int native) {
    uint32_t i;
    uint32_t base = addr < 0x1000 ? addr : XFREG(addr);
    int changed = 0;
    for (i = 0; i < count; i++) {
        uint32_t w;
        if (native) {
            memcpy(&w, words + 4 * i, 4);
        } else {
            w = PortBE32(words + 4 * i);
        }
        if (changed || gPortGX.xf[base + i] != w) {
            if (!changed) {
                Touch();
                changed = 1;
            }
            gPortGX.xf[base + i] = w;
        }
    }
    if (changed) {
        gPortGX.xfVersion++;
    }
}

static void WriteXF(uint32_t addr, const uint8_t* words, uint32_t count) {
    WriteXFWords(addr, words, count, 0);
}

/* GXLoadPosMtxIndx and friends: XF memory loaded from array 12-15. */
static void IndexedXF(uint8_t op, uint32_t header, const uint8_t* at, const uint8_t* start) {
    int array = 12 + ((op >> 3) & 3);
    uint32_t index = header >> 16;
    uint32_t count = ((header >> 12) & 15) + 1;
    uint32_t addr = header & 0xFFF;
    uint32_t base = gPortGX.cp[0xA0 + array] & 0x3FFFFFFF;
    uint32_t stride = gPortGX.cp[0xB0 + array] & 0xFF;
    uint32_t src = base + index * stride;
    if (addr + count > 0x800 || src + count * 4 > PORT_ADDR_WINDOW_SIZE) {
        StreamError(at, start, "indexed XF load of %u words from 0x%08x to 0x%03x", count, src, addr);
        return;
    }
    /* The source is always matrices or light objects the CPU computed (g3d's view
     * matrix array, GXLightObj), so it is in host order on the port. */
    WriteXFWords(addr, (const uint8_t*)PortPtrRaw(src), count, 1);
}

/* ------------------------------------------------------------------------------------ */
/* Decoder                                                                               */

static size_t Decode(const uint8_t* start, size_t size, int inDisplayList);

static int CallDisplayList(uint32_t addr, uint32_t size, const uint8_t* at, const uint8_t* start) {
    const uint8_t* list;
    const char* outer = sWhere;
    /* The command processor ignores the address's top bits: a cached or uncached
     * address (0x8.../0xC...) names the same memory. */
    addr &= PORT_ADDR_WINDOW_SIZE - 1;
    if (addr == 0 || size > PORT_ADDR_WINDOW_SIZE - addr) {
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
                            gPortGX.cp[0x50], gPortGX.cp[0x60], gPortGX.cp[0x70 + vat], gPortGX.cp[0x80 + vat],
                            gPortGX.cp[0x90 + vat]);
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
            Draw(op, p + 3, count);
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
                    WriteCP(p[1], PortBE32(p + 2));
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
                    } else {
                        WriteXF(header & 0xFFFF, p + 5, count);
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
                if (left >= length) {
                    IndexedXF(op, PortBE32(p + 1), p, start);
                }
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
            "%llu display copies, %llu texture copies, %llu draw-done, %llu tokens, %llu batches drawn, %u stream errors\n",
            sStats.commands, sStats.bytes, sStats.primitives, sStats.vertices, sStats.displayLists,
            sStats.displayCopies, sStats.textureCopies, sStats.drawDones, sStats.tokens, sStats.batches, sStats.errors);
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
