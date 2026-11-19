/* The vertex loader: turns the vertices of a draw command, described by the vertex
 * descriptor (CP 0x50/0x60) and the vertex attribute format (CP 0x70-0x9F), into
 * PortGXVertex. Direct attributes come from the command stream; indexed ones from the
 * arrays the CP registers 0xA0-0xBF point at. Both are big-endian, as the GPU read them.
 *
 * Attributes a vertex does not have get the values the shaders expect: matrix indices
 * from CP MATINDEX_A/B, a +Z normal, white colours, zero texture coordinates. */
#include "port_gx_internal.h"
#include <port_addr.h>
#include <port_endian.h>
#include <port_os.h>
#include <string.h>

static const int kComponentSize[8] = {1, 1, 2, 2, 4, 0, 0, 0};
static const int kColorSize[8] = {2, 3, 4, 2, 3, 4, 0, 0};

static float BEFloat(const uint8_t* p) {
    uint32_t u = PortBE32(p);
    float f;
    memcpy(&f, &u, 4);
    return f;
}

static float ReadComponent(const uint8_t* p, int fmt, float scale) {
    switch (fmt) {
    case 0:
        return (float)p[0] * scale;
    case 1:
        return (float)(int8_t)p[0] * scale;
    case 2:
        return (float)PortBE16(p) * scale;
    case 3:
        return (float)(int16_t)PortBE16(p) * scale;
    default:
        return BEFloat(p);
    }
}

static void ReadColor(const uint8_t* p, int fmt, float out[4]) {
    uint32_t v;
    float r, g, b, a = 255.0f;
    switch (fmt) {
    case 0: /* RGB565 */
        v = PortBE16(p);
        r = (float)(((v >> 11) & 31) << 3 | ((v >> 13) & 7));
        g = (float)(((v >> 5) & 63) << 2 | ((v >> 9) & 3));
        b = (float)((v & 31) << 3 | ((v >> 2) & 7));
        break;
    case 1: /* RGB888 */
    case 2: /* RGB888x */
        r = p[0];
        g = p[1];
        b = p[2];
        break;
    case 3: /* RGBA4444 */
        v = PortBE16(p);
        r = (float)(((v >> 12) & 15) * 17);
        g = (float)(((v >> 8) & 15) * 17);
        b = (float)(((v >> 4) & 15) * 17);
        a = (float)((v & 15) * 17);
        break;
    case 4: /* RGBA6666 */
        v = (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
        r = (float)((((v >> 18) & 63) << 2) | (((v >> 18) & 63) >> 4));
        g = (float)((((v >> 12) & 63) << 2) | (((v >> 12) & 63) >> 4));
        b = (float)((((v >> 6) & 63) << 2) | (((v >> 6) & 63) >> 4));
        a = (float)(((v & 63) << 2) | ((v & 63) >> 4));
        break;
    default: /* RGBA8888 */
        r = p[0];
        g = p[1];
        b = p[2];
        a = p[3];
        break;
    }
    out[0] = r / 255.0f;
    out[1] = g / 255.0f;
    out[2] = b / 255.0f;
    out[3] = a / 255.0f;
}

/* Address of element `index` of array `array` (CP 0xA0+n base, 0xB0+n stride). */
static const uint8_t* ArrayElement(int array, uint32_t index, uint32_t bytes) {
    uint32_t base = gPortGX.cp[0xA0 + array] & 0x3FFFFFFF;
    uint32_t stride = gPortGX.cp[0xB0 + array] & 0xFF;
    uint32_t addr = base + index * stride;
    static int reported;
    if (base == 0 || addr + bytes > PORT_ADDR_WINDOW_SIZE) {
        PortRedOnce(&reported, "GX: indexed vertex read outside memory (array %d, base 0x%08x, index %u)", array, base,
                    index);
        return NULL;
    }
    return (const uint8_t*)PortPtrRaw(addr);
}

typedef struct AttrFormat {
    int type;   /* 0 none, 1 direct, 2 index8, 3 index16 */
    int count;  /* the VAT's component-count bit */
    int fmt;
    int shift;
} AttrFormat;

typedef struct VertexFormat {
    uint32_t mtxMask; /* bit 0 position matrix, bits 1-8 texture matrices */
    AttrFormat pos, nrm, clr[2], tex[8];
    int nbt, nrmIndex3;
    float posScale, texScale[8];
} VertexFormat;

static int Decode(int vat, VertexFormat* f) {
    uint32_t lo = gPortGX.cp[0x50];
    uint32_t hi = gPortGX.cp[0x60];
    uint32_t a = gPortGX.cp[0x70 + vat];
    uint32_t b = gPortGX.cp[0x80 + vat];
    uint32_t c = gPortGX.cp[0x90 + vat];
    int i;

    memset(f, 0, sizeof(*f));
    f->mtxMask = lo & 0x1FF;
    f->pos.type = (lo >> 9) & 3;
    f->pos.count = a & 1;
    f->pos.fmt = (a >> 1) & 7;
    f->pos.shift = (a >> 4) & 31;
    f->nrm.type = (lo >> 11) & 3;
    f->nbt = (a >> 9) & 1;
    f->nrm.fmt = (a >> 10) & 7;
    f->nrmIndex3 = (a >> 31) & 1;
    f->clr[0].type = (lo >> 13) & 3;
    f->clr[0].fmt = (a >> 14) & 7;
    f->clr[1].type = (lo >> 15) & 3;
    f->clr[1].fmt = (a >> 18) & 7;
    for (i = 0; i < 8; i++) {
        f->tex[i].type = (hi >> (2 * i)) & 3;
    }
    f->tex[0].count = (a >> 21) & 1, f->tex[0].fmt = (a >> 22) & 7, f->tex[0].shift = (a >> 25) & 31;
    f->tex[1].count = b & 1, f->tex[1].fmt = (b >> 1) & 7, f->tex[1].shift = (b >> 4) & 31;
    f->tex[2].count = (b >> 9) & 1, f->tex[2].fmt = (b >> 10) & 7, f->tex[2].shift = (b >> 13) & 31;
    f->tex[3].count = (b >> 18) & 1, f->tex[3].fmt = (b >> 19) & 7, f->tex[3].shift = (b >> 22) & 31;
    f->tex[4].count = (b >> 27) & 1, f->tex[4].fmt = (b >> 28) & 7, f->tex[4].shift = c & 31;
    f->tex[5].count = (c >> 5) & 1, f->tex[5].fmt = (c >> 6) & 7, f->tex[5].shift = (c >> 9) & 31;
    f->tex[6].count = (c >> 14) & 1, f->tex[6].fmt = (c >> 15) & 7, f->tex[6].shift = (c >> 18) & 31;
    f->tex[7].count = (c >> 23) & 1, f->tex[7].fmt = (c >> 24) & 7, f->tex[7].shift = (c >> 27) & 31;

    if (f->pos.type == 0 || kComponentSize[f->pos.fmt] == 0) {
        return 0;
    }
    if (f->nrm.type != 0 && kComponentSize[f->nrm.fmt] == 0) {
        return 0;
    }
    for (i = 0; i < 2; i++) {
        if (f->clr[i].type != 0 && kColorSize[f->clr[i].fmt] == 0) {
            return 0;
        }
    }
    f->posScale = 1.0f / (float)(1u << f->pos.shift);
    for (i = 0; i < 8; i++) {
        if (f->tex[i].type != 0 && kComponentSize[f->tex[i].fmt] == 0) {
            return 0;
        }
        f->texScale[i] = 1.0f / (float)(1u << f->tex[i].shift);
    }
    return 1;
}

/* Reads an index of `type` at p; advances p. */
static uint32_t ReadIndex(const uint8_t** p, int type) {
    uint32_t v;
    if (type == 2) {
        v = (*p)[0];
        *p += 1;
    } else {
        v = PortBE16(*p);
        *p += 2;
    }
    return v;
}

uint32_t PortGXLoadVertices(int vat, const uint8_t* data, uint32_t count, PortGXVertex* out) {
    VertexFormat f;
    const uint8_t* p = data;
    /* Matrices a vertex does not name come from the transform unit's own copy of the
     * matrix index registers (XF 0x1018/0x1019), not the command processor's (CP
     * 0x30/0x40): g3d's display lists can change only the XF copy, e.g. pointing a
     * projection-mapped texgen at the position matrix. */
    uint32_t defA = gPortGX.xf[XFREG(0x1018)];
    uint32_t defB = gPortGX.xf[XFREG(0x1019)];
    uint8_t defaults[9];
    uint32_t v;
    int i;

    if (!Decode(vat, &f)) {
        return 0;
    }
    defaults[0] = (uint8_t)(defA & 63);
    defaults[1] = (uint8_t)((defA >> 6) & 63);
    defaults[2] = (uint8_t)((defA >> 12) & 63);
    defaults[3] = (uint8_t)((defA >> 18) & 63);
    defaults[4] = (uint8_t)((defA >> 24) & 63);
    defaults[5] = (uint8_t)(defB & 63);
    defaults[6] = (uint8_t)((defB >> 6) & 63);
    defaults[7] = (uint8_t)((defB >> 12) & 63);
    defaults[8] = (uint8_t)((defB >> 18) & 63);

    for (v = 0; v < count; v++) {
        PortGXVertex* o = &out[v];
        uint8_t mtx[9];
        const uint8_t* src;
        int cs, comps;

        memcpy(mtx, defaults, 9);
        for (i = 0; i < 9; i++) {
            if (f.mtxMask & (1u << i)) {
                mtx[i] = *p++ & 63;
            }
        }
        o->mtx[0] = mtx[0] | (uint32_t)mtx[1] << 8 | (uint32_t)mtx[2] << 16 | (uint32_t)mtx[3] << 24;
        o->mtx[1] = mtx[4] | (uint32_t)mtx[5] << 8 | (uint32_t)mtx[6] << 16 | (uint32_t)mtx[7] << 24;
        o->mtx[2] = mtx[8];
        o->mtx[3] = 0;

        /* Position. */
        cs = kComponentSize[f.pos.fmt];
        comps = f.pos.count ? 3 : 2;
        if (f.pos.type == 1) {
            src = p;
            p += cs * comps;
        } else {
            src = ArrayElement(0, ReadIndex(&p, f.pos.type), (uint32_t)(cs * comps));
        }
        if (src != NULL) {
            o->pos[0] = ReadComponent(src, f.pos.fmt, f.posScale);
            o->pos[1] = ReadComponent(src + cs, f.pos.fmt, f.posScale);
            o->pos[2] = comps == 3 ? ReadComponent(src + 2 * cs, f.pos.fmt, f.posScale) : 0.0f;
        } else {
            o->pos[0] = o->pos[1] = o->pos[2] = 0.0f;
        }

        /* Normal (and binormal, tangent). */
        o->nrm[0] = o->nrm[1] = 0.0f;
        o->nrm[2] = 1.0f;
        memset(o->bin, 0, sizeof(o->bin));
        memset(o->tan, 0, sizeof(o->tan));
        if (f.nrm.type != 0) {
            float nscale = f.nrm.fmt == 1 ? 1.0f / 64.0f : f.nrm.fmt == 3 ? 1.0f / 16384.0f : f.nrm.fmt == 0 ? 1.0f / 128.0f
                                                                            : f.nrm.fmt == 2 ? 1.0f / 32768.0f : 1.0f;
            int vectors = f.nbt ? 3 : 1;
            float* dst[3];
            int k;
            dst[0] = o->nrm;
            dst[1] = o->bin;
            dst[2] = o->tan;
            cs = kComponentSize[f.nrm.fmt];
            if (f.nrm.type == 1) {
                for (k = 0; k < vectors; k++) {
                    dst[k][0] = ReadComponent(p, f.nrm.fmt, nscale);
                    dst[k][1] = ReadComponent(p + cs, f.nrm.fmt, nscale);
                    dst[k][2] = ReadComponent(p + 2 * cs, f.nrm.fmt, nscale);
                    p += 3 * cs;
                }
            } else if (vectors == 3 && f.nrmIndex3) {
                for (k = 0; k < 3; k++) {
                    src = ArrayElement(1, ReadIndex(&p, f.nrm.type), (uint32_t)(9 * cs));
                    if (src != NULL) {
                        src += k * 3 * cs;
                        dst[k][0] = ReadComponent(src, f.nrm.fmt, nscale);
                        dst[k][1] = ReadComponent(src + cs, f.nrm.fmt, nscale);
                        dst[k][2] = ReadComponent(src + 2 * cs, f.nrm.fmt, nscale);
                    }
                }
            } else {
                src = ArrayElement(1, ReadIndex(&p, f.nrm.type), (uint32_t)(vectors * 3 * cs));
                for (k = 0; src != NULL && k < vectors; k++) {
                    dst[k][0] = ReadComponent(src, f.nrm.fmt, nscale);
                    dst[k][1] = ReadComponent(src + cs, f.nrm.fmt, nscale);
                    dst[k][2] = ReadComponent(src + 2 * cs, f.nrm.fmt, nscale);
                    src += 3 * cs;
                }
            }
        }

        /* Colours. */
        for (i = 0; i < 2; i++) {
            if (f.clr[i].type == 0) {
                o->clr[i][0] = o->clr[i][1] = o->clr[i][2] = o->clr[i][3] = 1.0f;
                continue;
            }
            if (f.clr[i].type == 1) {
                src = p;
                p += kColorSize[f.clr[i].fmt];
            } else {
                src = ArrayElement(2 + i, ReadIndex(&p, f.clr[i].type), (uint32_t)kColorSize[f.clr[i].fmt]);
            }
            if (src != NULL) {
                ReadColor(src, f.clr[i].fmt, o->clr[i]);
            } else {
                o->clr[i][0] = o->clr[i][1] = o->clr[i][2] = o->clr[i][3] = 1.0f;
            }
        }

        /* Texture coordinates. */
        for (i = 0; i < 8; i++) {
            o->tex[i][0] = o->tex[i][1] = 0.0f;
            if (f.tex[i].type == 0) {
                continue;
            }
            cs = kComponentSize[f.tex[i].fmt];
            comps = f.tex[i].count ? 2 : 1;
            if (f.tex[i].type == 1) {
                src = p;
                p += cs * comps;
            } else {
                src = ArrayElement(4 + i, ReadIndex(&p, f.tex[i].type), (uint32_t)(cs * comps));
            }
            if (src != NULL) {
                o->tex[i][0] = ReadComponent(src, f.tex[i].fmt, f.texScale[i]);
                if (comps == 2) {
                    o->tex[i][1] = ReadComponent(src + cs, f.tex[i].fmt, f.texScale[i]);
                }
            }
        }
    }
    return (uint32_t)(p - data);
}

uint32_t PortGXPrimitiveIndices(int prim, uint32_t base, uint32_t count, uint32_t* out) {
    uint32_t n = 0;
    uint32_t i;
    switch (prim) {
    case 0: /* quads */
    case 1: /* quads (the second opcode) */
        for (i = 0; i + 3 < count; i += 4) {
            out[n++] = base + i;
            out[n++] = base + i + 1;
            out[n++] = base + i + 2;
            out[n++] = base + i;
            out[n++] = base + i + 2;
            out[n++] = base + i + 3;
        }
        break;
    case 2: /* triangles */
        for (i = 0; i + 2 < count; i += 3) {
            out[n++] = base + i;
            out[n++] = base + i + 1;
            out[n++] = base + i + 2;
        }
        break;
    case 3: /* triangle strip */
        for (i = 2; i < count; i++) {
            if (i & 1) {
                out[n++] = base + i - 1;
                out[n++] = base + i - 2;
            } else {
                out[n++] = base + i - 2;
                out[n++] = base + i - 1;
            }
            out[n++] = base + i;
        }
        break;
    case 4: /* triangle fan */
        for (i = 2; i < count; i++) {
            out[n++] = base;
            out[n++] = base + i - 1;
            out[n++] = base + i;
        }
        break;
    default: /* lines, line strips, points: drawn by the caller */
        break;
    }
    return n;
}
