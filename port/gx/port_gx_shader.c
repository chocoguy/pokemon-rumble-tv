/* Shader generation: the GPU's transform unit (XF: matrices, lighting, texture coordinate
 * generation) and its pixel pipeline (TEV stages, alpha test, fog) written out as Metal
 * source for the state of a batch.
 *
 * The TEV arithmetic is integer, as on the console: inputs are 8-bit, registers signed
 * 11-bit, and each stage computes
 *     ((d + bias) << s) +/- ((((a << 8) + (b - a) * (c + (c >> 7))) << s) + round) >> 8
 * then shifts right for "divide by 2" and clamps.
 *
 * Not yet drawn: indirect texture stages, Z textures, emboss texgens, fog range
 * adjustment. Each reports a [RED] line once when a batch uses it. */
#include "port_gx_internal.h"
#include <port_os.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------------------------ */
/* Key and uniforms                                                                      */

static uint64_t HashBytes(const void* data, size_t n) {
    const uint8_t* p = (const uint8_t*)data;
    uint64_t h = 0xCBF29CE484222325ull;
    size_t i;
    for (i = 0; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    return h;
}

static float FogFloat(uint32_t reg) {
    uint32_t mant = reg & 0x7FF;
    uint32_t expn = (reg >> 11) & 0xFF;
    uint32_t sign = (reg >> 19) & 1;
    uint32_t bits = sign << 31 | expn << 23 | mant << 12;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static int32_t Sign11(uint32_t v) {
    return ((int32_t)(v << 21)) >> 21;
}

void PortGXBuildShader(PortGXShaderKey* key, PortGXUniforms* u, uint64_t* hash) {
    const uint32_t* bp = gPortGX.bp;
    const uint32_t* xf = gPortGX.xf;
    uint32_t genMode = bp[0x00];
    uint32_t i;
    static int ztex, emboss;

    memset(key, 0, sizeof(*key));
    key->numStages = ((genMode >> 10) & 15) + 1;
    key->numTexGens = xf[XFREG(0x103F)] & 15;
    if (key->numTexGens > 8) {
        key->numTexGens = 8;
    }
    key->numChans = xf[XFREG(0x1009)] & 3;
    if (key->numChans > 2) {
        key->numChans = 2;
    }
    key->numIndStages = (genMode >> 16) & 7;

    for (i = 0; i < key->numStages; i++) {
        uint32_t tref = bp[0x28 + i / 2] >> ((i & 1) ? 12 : 0);
        uint32_t ksel = bp[0xF6 + i / 2];
        key->colorEnv[i] = bp[0xC0 + 2 * i] & 0xFFFFFF;
        key->alphaEnv[i] = bp[0xC1 + 2 * i] & 0xFFFFFF;
        key->tref[i] = tref & 0x3FF;
        key->kcsel[i] = (ksel >> ((i & 1) ? 14 : 4)) & 31;
        key->kasel[i] = (ksel >> ((i & 1) ? 19 : 9)) & 31;
        if ((tref >> 6) & 1) {
            key->texMask |= 1u << (tref & 7);
        }
        key->indCmd[i] = bp[0x10 + i] & 0x1FFFFF;
    }
    if (key->numIndStages > 4) {
        key->numIndStages = 4;
    }
    if (key->numIndStages != 0) {
        key->indRef = bp[0x27] & ((1u << (6 * key->numIndStages)) - 1);
        key->indScale[0] = bp[0x25] & 0xFFFF;
        key->indScale[1] = bp[0x26] & 0xFFFF;
        for (i = 0; i < key->numIndStages; i++) {
            key->texMask |= 1u << ((key->indRef >> (6 * i)) & 7);
        }
    } else {
        for (i = 0; i < key->numStages; i++) {
            /* Without indirect stages a command can only wrap coordinates; keep it. */
            if ((key->indCmd[i] & 0x7FE00) == 0) {
                key->indCmd[i] = 0;
            }
        }
    }
    for (i = 0; i < 4; i++) {
        key->swap[i] = (bp[0xF6 + 2 * i] & 15) | (bp[0xF7 + 2 * i] & 15) << 4;
    }
    key->alphaFunc = (bp[0xF3] >> 16) & 0xFF;
    key->fogType = ((bp[0xF1] >> 21) & 7) | ((bp[0xF1] >> 20) & 1) << 3;
    if ((key->fogType & 7) == 0) {
        key->fogType = 0;
    }
    key->dstAlpha = (bp[0x42] >> 8) & 1;
    key->efbFormat = bp[0x43] & 7;
    if (key->efbFormat > 2) {
        key->efbFormat = 0;
    }
    if ((bp[0xF5] & 3) != 0) {
        PortRedOnce(&ztex, "GX: Z textures are not drawn yet");
    }

    for (i = 0; i < key->numChans; i++) {
        key->chanCtrl[i] = xf[XFREG(0x100E + i)] & 0x7FFF;
        key->chanCtrl[2 + i] = xf[XFREG(0x1010 + i)] & 0x7FFF;
    }
    key->dualTex = xf[XFREG(0x1012)] & 1;
    for (i = 0; i < key->numTexGens; i++) {
        key->texGen[i] = xf[XFREG(0x1040 + i)] & 0x3FFFF;
        if (((key->texGen[i] >> 4) & 7) == 1) {
            PortRedOnce(&emboss, "GX: emboss texture coordinates are not generated yet");
        }
        if (key->dualTex && (xf[XFREG(0x1050 + i)] >> 8) & 1) {
            key->postNormalize |= 1u << i;
        }
    }

    memcpy(u->tevReg, gPortGX.tevReg, sizeof(u->tevReg));
    memcpy(u->konst, gPortGX.tevKonst, sizeof(u->konst));
    u->alphaRef[0] = (int32_t)(bp[0xF3] & 0xFF);
    u->alphaRef[1] = (int32_t)((bp[0xF3] >> 8) & 0xFF);
    u->alphaRef[2] = u->alphaRef[3] = 0;
    u->fog[0] = FogFloat(bp[0xEE]);
    u->fog[1] = FogFloat(bp[0xF1]);
    u->fog[2] = (float)(bp[0xEF] & 0xFFFFFF);
    u->fog[3] = (float)(bp[0xF0] & 31);
    u->fogColor[0] = (int32_t)((bp[0xF2] >> 16) & 0xFF);
    u->fogColor[1] = (int32_t)((bp[0xF2] >> 8) & 0xFF);
    u->fogColor[2] = (int32_t)(bp[0xF2] & 0xFF);
    u->fogColor[3] = 0;
    for (i = 0; i < 8; i++) {
        uint32_t mode0 = bp[(i < 4 ? 0x80 : 0xA0) + (i & 3)];
        int8_t bias = (int8_t)((mode0 >> 9) & 0xFF);
        u->lodBias[i][0] = (float)bias / 32.0f;
        u->lodBias[i][1] = u->lodBias[i][2] = u->lodBias[i][3] = 0.0f;
    }
    for (i = 0; i < 3; i++) {
        uint32_t a = bp[0x06 + 3 * i], b = bp[0x07 + 3 * i], c = bp[0x08 + 3 * i];
        int32_t shift = (int32_t)(((a >> 22) & 3) | ((b >> 22) & 3) << 2 | ((c >> 22) & 3) << 4) - 17;
        u->indMtx[2 * i][0] = Sign11(a & 0x7FF);
        u->indMtx[2 * i][1] = Sign11(b & 0x7FF);
        u->indMtx[2 * i][2] = Sign11(c & 0x7FF);
        u->indMtx[2 * i][3] = shift;
        u->indMtx[2 * i + 1][0] = Sign11((a >> 11) & 0x7FF);
        u->indMtx[2 * i + 1][1] = Sign11((b >> 11) & 0x7FF);
        u->indMtx[2 * i + 1][2] = Sign11((c >> 11) & 0x7FF);
        u->indMtx[2 * i + 1][3] = shift;
    }
    for (i = 0; i < 8; i++) {
        uint32_t image0 = bp[(i < 4 ? 0x88 : 0xA8) + (i & 3)];
        u->texSize[i][0] = (float)((image0 & 0x3FF) + 1);
        u->texSize[i][1] = (float)(((image0 >> 10) & 0x3FF) + 1);
        u->texSize[i][2] = u->texSize[i][3] = 0.0f;
    }
    u->efb[0] = PORT_EFB_WIDTH;
    u->efb[1] = PORT_EFB_HEIGHT;
    u->efb[2] = (float)((bp[0x59] & 0x3FF) * 2);
    u->efb[3] = (float)(((bp[0x59] >> 10) & 0x3FF) * 2);
    /* The constant destination alpha is stored like any other: 6 bits in RGBA6. */
    u->misc[0] = key->efbFormat == 1 ? (float)((bp[0x42] & 0xFF) >> 2) / 63.0f : (float)(bp[0x42] & 0xFF) / 255.0f;
    /* SU_LPSIZE: line width and point size in sixths of a pixel. */
    u->misc[1] = (float)(bp[0x22] & 0xFF) / 6.0f;
    u->misc[2] = (float)((bp[0x22] >> 8) & 0xFF) / 6.0f;
    u->misc[3] = 0.0f;

    *hash = HashBytes(key, sizeof(*key));
}

/* ------------------------------------------------------------------------------------ */
/* Source text                                                                           */

typedef struct Str {
    char* p;
    size_t len, cap;
} Str;

static void Add(Str* s, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
static void Add(Str* s, const char* fmt, ...) {
    va_list args;
    int n;
    for (;;) {
        size_t room = s->cap - s->len;
        va_start(args, fmt);
        n = vsnprintf(s->p + s->len, room, fmt, args);
        va_end(args);
        if (n >= 0 && (size_t)n < room) {
            s->len += (size_t)n;
            return;
        }
        s->cap = s->cap * 2 + (size_t)n + 256;
        s->p = (char*)realloc(s->p, s->cap);
    }
}

static const char* kPrelude =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct Vtx { packed_float3 pos; packed_float3 nrm; packed_float3 bin; packed_float3 tan;\n"
    "             float4 clr[2]; float2 tex[8]; uint4 mtx; };\n"
    "struct U { int4 tevReg[4]; int4 konst[4]; int4 alphaRef; float4 fog; int4 fogColor;\n"
    "           float4 lodBias[8]; int4 indMtx[6]; float4 texSize[8]; float4 efb; float4 misc; };\n"
    "static inline float F(constant uint* xf, uint a) { return as_type<float>(xf[a]); }\n"
    "static inline float4 R4(constant uint* xf, uint a) { return float4(F(xf, a), F(xf, a + 1), F(xf, a + 2), F(xf, a + 3)); }\n"
    "static inline float3 R3(constant uint* xf, uint a) { return float3(F(xf, a), F(xf, a + 1), F(xf, a + 2)); }\n"
    "static inline float4 C8(uint c) { return float4((c >> 24) & 255u, (c >> 16) & 255u, (c >> 8) & 255u, c & 255u) / 255.0; }\n"
    "static inline float3 SafeNormalize(float3 v) { float l = length(v); return l > 0.0 ? v / l : v; }\n";

static void VertexOut(Str* s, const PortGXShaderKey* key) {
    uint32_t i;
    Add(s, "struct VOut {\n  float4 pos [[position]];\n  float4 c0;\n  float4 c1;\n");
    for (i = 0; i < key->numTexGens; i++) {
        Add(s, "  float3 t%u;\n", i);
    }
    Add(s, "};\n");
}

/* One light's contribution to `lit` (a float4 accumulating colour and alpha). */
static void Light(Str* s, uint32_t ctrl, int light, const char* comp) {
    uint32_t diffuse = (ctrl >> 7) & 3;
    int attnEnable = (ctrl >> 9) & 1;
    int spot = (ctrl >> 10) & 1;
    Add(s, "    { uint la = 0x%xu;\n", 0x600 + light * 16);
    Add(s, "      float3 lpos = R3(xf, la + 10), ldir = R3(xf, la + 13);\n");
    Add(s, "      float3 cosa = R3(xf, la + 4), dista = R3(xf, la + 7);\n");
    Add(s, "      float4 lcol = C8(xf[la + 3]);\n");
    Add(s, "      float3 ld = lpos - ep; float dist2 = dot(ld, ld); float dist = sqrt(dist2);\n");
    Add(s, "      ld = dist > 0.0 ? ld / dist : en;\n");
    if (!attnEnable) {
        Add(s, "      float attn = 1.0;\n");
    } else if (spot) {
        Add(s, "      float attn = max(0.0, dot(ld, ldir));\n");
        Add(s, "      attn = max(0.0, cosa.x + cosa.y * attn + cosa.z * attn * attn) / dot(dista, float3(1.0, dist, dist2));\n");
    } else {
        Add(s, "      float attn = dot(en, ld) >= 0.0 ? max(0.0, dot(en, ldir)) : 0.0;\n");
        Add(s, "      float3 da = %s;\n", diffuse == 0 ? "dista" : "SafeNormalize(dista)");
        Add(s, "      attn = max(0.0, dot(cosa, float3(1.0, attn, attn * attn))) / dot(da, float3(1.0, attn, attn * attn));\n");
    }
    switch (diffuse) {
    case 0:
        Add(s, "      lit.%s += attn * lcol.%s; }\n", comp, comp);
        break;
    case 1:
        Add(s, "      lit.%s += attn * dot(ld, en) * lcol.%s; }\n", comp, comp);
        break;
    default:
        Add(s, "      lit.%s += attn * max(0.0, dot(ld, en)) * lcol.%s; }\n", comp, comp);
        break;
    }
}

static void Channel(Str* s, uint32_t ctrl, int chan, const char* comp) {
    uint32_t lights = ((ctrl >> 2) & 15) | ((ctrl >> 11) & 15) << 4;
    int l;
    Add(s, "  { float4 mat = %s;\n", (ctrl & 1) ? (chan ? "v.clr[1]" : "v.clr[0]") : (chan ? "C8(xf[0x80D])" : "C8(xf[0x80C])"));
    if ((ctrl >> 1) & 1) {
        Add(s, "    float4 lit = %s;\n", ((ctrl >> 6) & 1) ? (chan ? "v.clr[1]" : "v.clr[0]") : (chan ? "C8(xf[0x80B])" : "C8(xf[0x80A])"));
        for (l = 0; l < 8; l++) {
            if (lights & (1u << l)) {
                Light(s, ctrl, l, comp);
            }
        }
        Add(s, "    mat.%s *= clamp(lit.%s, 0.0, 1.0);\n", comp, comp);
    }
    Add(s, "    o.c%d.%s = mat.%s; }\n", chan, comp, comp);
}

static const char* TexMatrixIndex(uint32_t i) {
    static const char* kIndex[8] = {
        "((v.mtx.x >> 8) & 63u)",  "((v.mtx.x >> 16) & 63u)", "((v.mtx.x >> 24) & 63u)", "(v.mtx.y & 63u)",
        "((v.mtx.y >> 8) & 63u)", "((v.mtx.y >> 16) & 63u)", "((v.mtx.y >> 24) & 63u)", "(v.mtx.z & 63u)",
    };
    return kIndex[i & 7];
}

static void TexGen(Str* s, const PortGXShaderKey* key, uint32_t i) {
    uint32_t info = key->texGen[i];
    int stq = (info >> 1) & 1;
    int abc1 = (info >> 2) & 1;
    uint32_t type = (info >> 4) & 7;
    uint32_t row = (info >> 7) & 31;
    uint32_t embossSource = (info >> 12) & 7;

    if (type == 2 || type == 3) {
        Add(s, "  o.t%u = float3(o.c%u.rg, 1.0);\n", i, type - 2);
        return;
    }
    if (type == 1) {
        /* Emboss: the source coordinates, without the light offset. */
        if (embossSource < i) {
            Add(s, "  o.t%u = o.t%u;\n", i, embossSource);
        } else {
            Add(s, "  o.t%u = float3(0.0, 0.0, 1.0);\n", i);
        }
        return;
    }
    Add(s, "  { float4 src = ");
    switch (row) {
    case 0:
        Add(s, "float4(float3(v.pos), 1.0);\n");
        break;
    case 1:
        Add(s, "float4(float3(v.nrm), 1.0);\n");
        break;
    case 3:
        Add(s, "float4(float3(v.tan), 1.0);\n");
        break;
    case 4:
        Add(s, "float4(float3(v.bin), 1.0);\n");
        break;
    default:
        if (row >= 5 && row <= 12) {
            Add(s, "float4(v.tex[%u], 1.0, 1.0);\n", row - 5);
        } else {
            Add(s, "float4(0.0, 0.0, 1.0, 1.0);\n");
        }
        break;
    }
    if (!abc1) {
        Add(s, "    src.z = 1.0;\n");
    }
    Add(s, "    uint ta = %s * 4u;\n", TexMatrixIndex(i));
    if (stq) {
        Add(s, "    float3 t = float3(dot(R4(xf, ta), src), dot(R4(xf, ta + 4), src), dot(R4(xf, ta + 8), src));\n");
    } else {
        Add(s, "    float3 t = float3(dot(R4(xf, ta), src), dot(R4(xf, ta + 4), src), 1.0);\n");
    }
    if (key->dualTex) {
        if (key->postNormalize & (1u << i)) {
            Add(s, "    t = SafeNormalize(t);\n");
        }
        Add(s, "    uint pa = 0x500u + (xf[0x%x] & 63u) * 4u;\n", XFREG(0x1050 + i));
        Add(s, "    float4 t4 = float4(t, 1.0);\n");
        Add(s, "    t = float3(dot(R4(xf, pa), t4), dot(R4(xf, pa + 4), t4), dot(R4(xf, pa + 8), t4));\n");
    }
    Add(s, "    o.t%u = t; }\n", i);
}

/* Eye-space position to Metal clip space: the projection (XF 0x1020-0x1026), then the
 * viewport to EFB pixels (the scissor offset moves the EFB's origin), then Metal's clip
 * space over the whole EFB; written linearly in w so clipping stays correct. */
static void PositionHelper(Str* s) {
    Add(s, "static float4 GXPos(float3 ep, constant uint* xf, constant U& u) {\n"
           "  float4 clip;\n"
           "  if (xf[0x826] == 0u) {\n"
           "    clip = float4(F(xf, 0x820) * ep.x + F(xf, 0x821) * ep.z, F(xf, 0x822) * ep.y + F(xf, 0x823) * ep.z,\n"
           "                  F(xf, 0x824) * ep.z + F(xf, 0x825), -ep.z);\n"
           "  } else {\n"
           "    clip = float4(F(xf, 0x820) * ep.x + F(xf, 0x821), F(xf, 0x822) * ep.y + F(xf, 0x823),\n"
           "                  F(xf, 0x824) * ep.z + F(xf, 0x825), 1.0);\n"
           "  }\n"
           "  float w = clip.w;\n"
           "  float ox = F(xf, 0x81D) - u.efb.z, oy = F(xf, 0x81E) - u.efb.w;\n"
           "  return float4(2.0 * (ox * w + F(xf, 0x81A) * clip.x) / u.efb.x - w,\n"
           "                w - 2.0 * (oy * w + F(xf, 0x81B) * clip.y) / u.efb.y,\n"
           "                (F(xf, 0x81F) * w + F(xf, 0x81C) * clip.z) / 16777215.0, w);\n"
           "}\n\n");
}

static void VertexShader(Str* s, const PortGXShaderKey* key) {
    uint32_t i;
    PositionHelper(s);
    Add(s, "vertex VOut vs(uint vid [[vertex_id]], device const Vtx* vb [[buffer(0)]], constant uint* xf [[buffer(1)]],\n"
           "              constant U& u [[buffer(2)]]) {\n");
    Add(s, "  Vtx v = vb[vid];\n  VOut o;\n");
    Add(s, "  uint pm = v.mtx.x & 63u;\n");
    Add(s, "  float4 p4 = float4(float3(v.pos), 1.0);\n");
    Add(s, "  float3 ep = float3(dot(R4(xf, pm * 4u), p4), dot(R4(xf, pm * 4u + 4u), p4), dot(R4(xf, pm * 4u + 8u), p4));\n");
    Add(s, "  uint na = 0x400u + (pm & 31u) * 3u;\n");
    Add(s, "  float3 n0 = float3(v.nrm);\n");
    Add(s, "  float3 en = SafeNormalize(float3(dot(R3(xf, na), n0), dot(R3(xf, na + 3u), n0), dot(R3(xf, na + 6u), n0)));\n");
    Add(s, "  o.pos = GXPos(ep, xf, u);\n");
    /* Lines and points (port_gx.c DrawLinesPoints): move this corner across the line, or
     * out to the point's corner, by half the width in EFB pixels. */
    Add(s, "  if (v.mtx.w != 0u) {\n"
           "    float2 px = u.efb.xy;\n"
           "    if ((v.mtx.w >> 28) == 8u) {\n"
           "      Vtx ov = vb[int(vid) + int(short(v.mtx.w & 0xFFFFu))];\n"
           "      uint om = ov.mtx.x & 63u;\n"
           "      float4 op4 = float4(float3(ov.pos), 1.0);\n"
           "      float4 other = GXPos(float3(dot(R4(xf, om * 4u), op4), dot(R4(xf, om * 4u + 4u), op4),\n"
           "                                  dot(R4(xf, om * 4u + 8u), op4)), xf, u);\n"
           "      float2 d = (other.xy / other.w - o.pos.xy / o.pos.w) * px;\n"
           "      float len = length(d);\n"
           "      float2 n = len > 0.0 ? float2(-d.y, d.x) / len : float2(0.0, 1.0);\n"
           "      float side = (v.mtx.w & 0x10000u) != 0u ? 1.0 : -1.0;\n"
           "      o.pos.xy += n * side * u.misc.y / px * o.pos.w;\n"
           "    } else {\n"
           "      float2 c = float2((v.mtx.w & 1u) != 0u ? 1.0 : -1.0, (v.mtx.w & 2u) != 0u ? 1.0 : -1.0);\n"
           "      o.pos.xy += c * u.misc.z / px * o.pos.w;\n"
           "    }\n"
           "  }\n");
    Add(s, "  o.c0 = float4(0.0);\n  o.c1 = float4(0.0);\n");
    for (i = 0; i < key->numChans; i++) {
        Channel(s, key->chanCtrl[i], (int)i, "rgb");
        Channel(s, key->chanCtrl[2 + i], (int)i, "a");
    }
    for (i = 0; i < key->numTexGens; i++) {
        TexGen(s, key, i);
    }
    {
        const char* show = (key->numChans > 0 && (key->chanCtrl[0] >> 1) & 1) ? getenv("PORT_GX_SHOW") : NULL;
        if (show != NULL && strcmp(show, "nrm") == 0) {
            Add(s, "  o.c0 = float4(en * 0.5 + 0.5, 1.0);\n");
        } else if (show != NULL && strcmp(show, "mat") == 0) {
            Add(s, "  o.c0 = C8(xf[0x80C]);\n");
        } else if (show != NULL && strcmp(show, "amb") == 0) {
            Add(s, "  o.c0 = C8(xf[0x80A]);\n");
        } else if (show != NULL && strcmp(show, "l1") == 0) {
            Add(s, "  o.c0 = float4(abs(R3(xf, 0x61A)) / 3e10, 1.0);\n");
        }
    }
    Add(s, "  return o;\n}\n");
}

/* --- Pixel ----------------------------------------------------------------------------- */

static const char kSwz[4] = {'r', 'g', 'b', 'a'};

static void Swizzle(char out[5], uint32_t table) {
    out[0] = kSwz[table & 3];
    out[1] = kSwz[(table >> 2) & 3];
    out[2] = kSwz[(table >> 4) & 3];
    out[3] = kSwz[(table >> 6) & 3];
    out[4] = '\0';
}

static const char* KonstColor(uint32_t sel, char* buf) {
    static const char* kFrac[8] = {"int3(255)", "int3(223)", "int3(191)", "int3(159)",
                                   "int3(128)", "int3(96)",  "int3(64)",  "int3(32)"};
    if (sel < 8) {
        return kFrac[sel];
    }
    if (sel >= 12 && sel < 16) {
        sprintf(buf, "u.konst[%u].rgb", sel - 12);
    } else if (sel >= 16) {
        sprintf(buf, "u.konst[%u].%c%c%c", sel & 3, kSwz[(sel - 16) / 4], kSwz[(sel - 16) / 4], kSwz[(sel - 16) / 4]);
    } else {
        return "int3(0)";
    }
    return buf;
}

static const char* KonstAlpha(uint32_t sel, char* buf) {
    static const char* kFrac[8] = {"255", "223", "191", "159", "128", "96", "64", "32"};
    if (sel < 8) {
        return kFrac[sel];
    }
    if (sel >= 16) {
        sprintf(buf, "u.konst[%u].%c", sel & 3, kSwz[(sel - 16) / 4]);
        return buf;
    }
    return "0";
}

static const char* ColorInput(uint32_t sel) {
    static const char* kIn[16] = {"prev.rgb", "prev.aaa", "r0.rgb",    "r0.aaa",    "r1.rgb",    "r1.aaa",
                                  "r2.rgb",   "r2.aaa",   "tx.rgb",    "tx.aaa",    "rs.rgb",    "rs.aaa",
                                  "int3(255)", "int3(128)", "ks.rgb", "int3(0)"};
    return kIn[sel & 15];
}

static const char* AlphaInput(uint32_t sel) {
    static const char* kIn[8] = {"prev.a", "r0.a", "r1.a", "r2.a", "tx.a", "rs.a", "ks.a", "0"};
    return kIn[sel & 7];
}

static const char* kDest[4] = {"prev", "r0", "r1", "r2"};

static void StageMath(Str* s, uint32_t env, int alpha) {
    uint32_t bias = (env >> 16) & 3;
    uint32_t sub = (env >> 18) & 1;
    uint32_t clampOn = (env >> 19) & 1;
    uint32_t shift = (env >> 20) & 3;
    uint32_t dest = (env >> 22) & 3;
    const char* c = alpha ? "a" : "rgb";
    const char* type = alpha ? "int" : "int3";

    if (bias == 3) {
        /* Compare: d + (a OP b ? c : 0). */
        uint32_t op = shift << 1 | sub;
        const char* cmp = (op & 1) ? "==" : ">";
        switch (op >> 1) {
        case 0:
            Add(s, "    %s res = D.%s + ((A.r %s B.r) ? C.%s : %s(0));\n", type, c, cmp, c, type);
            break;
        case 1:
            Add(s, "    %s res = D.%s + (((A.g << 8) | A.r) %s ((B.g << 8) | B.r) ? C.%s : %s(0));\n", type, c, cmp, c,
                type);
            break;
        case 2:
            Add(s, "    %s res = D.%s + (((A.b << 16) | (A.g << 8) | A.r) %s ((B.b << 16) | (B.g << 8) | B.r) ? C.%s : %s(0));\n",
                type, c, cmp, c, type);
            break;
        default:
            if (alpha) {
                Add(s, "    int res = D.a + ((A.a %s B.a) ? C.a : 0);\n", cmp);
            } else {
                Add(s, "    int3 res = D.rgb + select(int3(0), C.rgb, A.rgb %s B.rgb);\n", cmp);
            }
            break;
        }
    } else {
        static const char* kBias[4] = {"", " + 128", " - 128", ""};
        static const char* kLeft[4] = {"", " << 1", " << 2", ""};
        static const char* kLerpBias[4] = {"", " + 128", "", " + 127"};
        Add(s, "    %s cc = C.%s + (C.%s >> 7);\n", type, c, c);
        Add(s, "    %s res = ((D.%s%s)%s) %s (((((A.%s << 8) + (B.%s - A.%s) * cc)%s)%s) >> 8);\n", type, c, kBias[bias],
            kLeft[shift], sub ? "-" : "+", c, c, c, kLeft[shift], kLerpBias[2 * sub + ((shift == 3) == (alpha != 0))]);
        if (shift == 3) {
            Add(s, "    res = res >> 1;\n");
        }
    }
    if (clampOn) {
        Add(s, "    res = clamp(res, %s(0), %s(255));\n", type, type);
    } else {
        Add(s, "    res = clamp(res, %s(-1024), %s(1023));\n", type, type);
    }
    Add(s, "    %s.%s = res;\n", kDest[dest], c);
}

static void AlphaCompare(Str* s, uint32_t op, const char* ref, char* out) {
    static const char* kOp[8] = {"false", "<", "==", "<=", ">", "!=", ">=", "true"};
    (void)s;
    if (op == 0 || op == 7) {
        strcpy(out, kOp[op]);
    } else {
        sprintf(out, "(outc.a %s %s)", kOp[op], ref);
    }
}

/* An indirect stage: offsets this stage's texture coordinate (fixed point, 7 fraction
 * bits, in texels) by a matrix times the indirect lookup, after wrapping it; `uv` is
 * replaced by the result. */
static void Indirect(Str* s, const PortGXShaderKey* key, uint32_t cmd, uint32_t map) {
    static const int kMask[4] = {255, 31, 15, 7};
    static const int kWrap[8] = {0, 256, 128, 64, 32, 16, 0, 0};
    uint32_t bt = cmd & 3;
    uint32_t fmt = (cmd >> 2) & 3;
    uint32_t bias = (cmd >> 4) & 7;
    uint32_t bs = (cmd >> 7) & 3;
    uint32_t mid = (cmd >> 9) & 15;
    uint32_t sw = (cmd >> 13) & 7;
    uint32_t tw = (cmd >> 16) & 7;
    uint32_t fb = (cmd >> 20) & 1;
    int add = fmt == 0 ? -128 : 1;

    Add(s, "    int2 fpuv = int2(uv * u.texSize[%u].xy * 128.0);\n", map);
    Add(s, "    int3 ic = %s;\n", bt < key->numIndStages ? (bt == 0 ? "iind0" : bt == 1 ? "iind1" : bt == 2 ? "iind2" : "iind3")
                                                       : "int3(0)");
    if (bs != 0) {
        static const char* kComp[4] = {"", "x", "y", "z"};
        static const char* kBump[4] = {"(ic.%s & 248)", "((ic.%s & 31) << 3)", "((ic.%s & 15) << 4)", "((ic.%s & 7) << 5)"};
        Add(s, "    ab = ");
        Add(s, kBump[fmt], kComp[bs]);
        Add(s, ";\n");
    }
    Add(s, "    ic &= %d;\n", kMask[fmt]);
    if (bias & 1) Add(s, "    ic.x += %d;\n", add);
    if (bias & 2) Add(s, "    ic.y += %d;\n", add);
    if (bias & 4) Add(s, "    ic.z += %d;\n", add);
    if (mid >= 1 && mid <= 3) {
        uint32_t m = mid - 1;
        Add(s, "    int2 trans = int2(u.indMtx[%u].x * ic.x + u.indMtx[%u].y * ic.y + u.indMtx[%u].z * ic.z,\n"
               "                      u.indMtx[%u].x * ic.x + u.indMtx[%u].y * ic.y + u.indMtx[%u].z * ic.z) >> 3;\n",
            2 * m, 2 * m, 2 * m, 2 * m + 1, 2 * m + 1, 2 * m + 1);
        Add(s, "    int sh = u.indMtx[%u].w;\n", 2 * m);
    } else if ((mid >= 5 && mid <= 7) || (mid >= 9 && mid <= 11)) {
        uint32_t m = (mid & 3) - 1;
        Add(s, "    int2 trans = (fpuv * ic.%s) >> 8;\n", mid < 8 ? "xx" : "yy");
        Add(s, "    int sh = u.indMtx[%u].w;\n", 2 * m);
    } else {
        Add(s, "    int2 trans = int2(0); int sh = 0;\n");
    }
    Add(s, "    trans = sh >= 0 ? (trans << sh) : (trans >> (-sh));\n");
    if (sw == 6) {
        Add(s, "    fpuv.x = 0;\n");
    } else if (sw != 0) {
        Add(s, "    fpuv.x &= %d;\n", (kWrap[sw] << 7) - 1);
    }
    if (tw == 6) {
        Add(s, "    fpuv.y = 0;\n");
    } else if (tw != 0) {
        Add(s, "    fpuv.y &= %d;\n", (kWrap[tw] << 7) - 1);
    }
    Add(s, "    tevcoord = %sfpuv + trans;\n", fb ? "tevcoord + " : "");
    Add(s, "    uv = float2(tevcoord) / 128.0 / u.texSize[%u].xy;\n", map);
}

static void FragmentShader(Str* s, const PortGXShaderKey* key) {
    uint32_t i;
    char k0[40], k1[40];
    uint32_t lastColorDest = 0, lastAlphaDest = 0;

    if (key->dstAlpha) {
        Add(s, "struct FOut { float4 c [[color(0), index(0)]]; float4 src [[color(0), index(1)]]; };\n");
    } else {
        Add(s, "struct FOut { float4 c [[color(0)]]; };\n");
    }
    Add(s, "fragment FOut fs(VOut in [[stage_in]], constant U& u [[buffer(0)]]");
    for (i = 0; i < 8; i++) {
        if (key->texMask & (1u << i)) {
            Add(s, ",\n  texture2d<float> tex%u [[texture(%u)]], sampler smp%u [[sampler(%u)]]", i, i, i, i);
        }
    }
    Add(s, ") {\n");
    Add(s, "  int4 prev = u.tevReg[0], r0 = u.tevReg[1], r1 = u.tevReg[2], r2 = u.tevReg[3];\n");
    Add(s, "  int4 ras0 = int4(round(clamp(in.c0, 0.0, 1.0) * 255.0)), ras1 = int4(round(clamp(in.c1, 0.0, 1.0) * 255.0));\n");
    Add(s, "  int2 tevcoord = int2(0);\n  int ab = 0;\n  int4 stx[16];\n");
    /* Indirect lookups: S, T, U from the texture's alpha, blue and green. */
    for (i = 0; i < key->numIndStages; i++) {
        uint32_t ref = key->indRef >> (6 * i);
        uint32_t imap = ref & 7;
        uint32_t icoord = (ref >> 3) & 7;
        uint32_t ss = key->indScale[i / 2] >> (8 * (i & 1));
        float scaleS = 1.0f / (float)(1u << (ss & 15));
        float scaleT = 1.0f / (float)(1u << ((ss >> 4) & 15));
        if (icoord < key->numTexGens) {
            Add(s, "  int3 iind%u = int3(round(tex%u.sample(smp%u, (in.t%u.z != 0.0 ? in.t%u.xy / in.t%u.z : in.t%u.xy) * float2(%.9g, %.9g)) * 255.0).abg);\n",
                i, imap, imap, icoord, icoord, icoord, icoord, scaleS, scaleT);
        } else {
            Add(s, "  int3 iind%u = int3(0);\n", i);
        }
    }

    for (i = 0; i < key->numStages; i++) {
        uint32_t tref = key->tref[i];
        uint32_t map = tref & 7;
        uint32_t coord = (tref >> 3) & 7;
        uint32_t chan = (tref >> 7) & 7;
        uint32_t cenv = key->colorEnv[i];
        uint32_t aenv = key->alphaEnv[i];
        char rswap[5], tswap[5];
        Swizzle(rswap, key->swap[aenv & 3]);
        Swizzle(tswap, key->swap[(aenv >> 2) & 3]);

        Add(s, "  { // stage %u\n", i);
        if (coord < key->numTexGens) {
            Add(s, "    float2 uv = in.t%u.z != 0.0 ? in.t%u.xy / in.t%u.z : in.t%u.xy;\n", coord, coord, coord, coord);
        } else {
            Add(s, "    float2 uv = float2(0.0);\n");
        }
        if (key->indCmd[i] != 0) {
            Indirect(s, key, key->indCmd[i], map);
        } else if (key->numIndStages != 0) {
            Add(s, "    tevcoord = int2(uv * u.texSize[%u].xy * 128.0);\n", map);
        }
        if ((tref >> 6) & 1) {
            Add(s, "    int4 tx = int4(round(tex%u.sample(smp%u, uv, bias(u.lodBias[%u].x)) * 255.0)).%s;\n", map, map,
                map, tswap);
        } else {
            Add(s, "    int4 tx = int4(0);\n");
        }
        Add(s, "    stx[%u] = tx;\n", i);
        if (chan == 0) {
            Add(s, "    int4 rs = ras0.%s;\n", rswap);
        } else if (chan == 1) {
            Add(s, "    int4 rs = ras1.%s;\n", rswap);
        } else if (chan == 5) {
            Add(s, "    int4 rs = int4(ab);\n");
        } else if (chan == 6) {
            Add(s, "    int4 rs = int4(ab | (ab >> 5));\n");
        } else {
            Add(s, "    int4 rs = int4(0);\n");
        }
        Add(s, "    int4 ks = int4(%s, %s);\n", KonstColor(key->kcsel[i], k0), KonstAlpha(key->kasel[i], k1));
        Add(s, "    int4 A = int4(%s, %s) & 255;\n", ColorInput((cenv >> 12) & 15), AlphaInput((aenv >> 13) & 7));
        Add(s, "    int4 B = int4(%s, %s) & 255;\n", ColorInput((cenv >> 8) & 15), AlphaInput((aenv >> 10) & 7));
        Add(s, "    int4 C = int4(%s, %s) & 255;\n", ColorInput((cenv >> 4) & 15), AlphaInput((aenv >> 7) & 7));
        Add(s, "    int4 D = int4(%s, %s);\n", ColorInput(cenv & 15), AlphaInput((aenv >> 4) & 7));
        Add(s, "    {\n");
        StageMath(s, cenv, 0);
        Add(s, "    }\n    {\n");
        StageMath(s, aenv, 1);
        Add(s, "    }\n  }\n");
        lastColorDest = (cenv >> 22) & 3;
        lastAlphaDest = (aenv >> 22) & 3;
    }
    /* The last stage's result leaves the TEV as its low 8 bits: an unclamped 256 is 0
     * (effects rely on it, e.g. an alpha that wraps to fail the alpha test). */
    Add(s, "  int4 outc = int4(%s.rgb, %s.a) & 255;\n", kDest[lastColorDest], kDest[lastAlphaDest]);

    {
        uint32_t op0 = key->alphaFunc & 7;
        uint32_t op1 = (key->alphaFunc >> 3) & 7;
        uint32_t logic = (key->alphaFunc >> 6) & 3;
        char a0[48], a1[48];
        static const char* kLogic[4] = {"&&", "||", "!=", "=="};
        AlphaCompare(s, op0, "u.alphaRef.x", a0);
        AlphaCompare(s, op1, "u.alphaRef.y", a1);
        if (!(logic == 0 && op0 == 7 && op1 == 7) && !(logic == 1 && (op0 == 7 || op1 == 7))) {
            Add(s, "  if (!(%s %s %s)) discard_fragment();\n", a0, kLogic[logic], a1);
        }
    }

    if (key->fogType != 0) {
        uint32_t fsel = key->fogType & 7;
        int ortho = (key->fogType >> 3) & 1;
        Add(s, "  { int zc = int(in.pos.z * 16777215.0);\n");
        if (ortho) {
            Add(s, "    float ze = u.fog.x * float(zc) / 16777216.0;\n");
        } else {
            Add(s, "    float ze = (u.fog.x * 16777216.0) / float(int(u.fog.z) - (zc >> int(u.fog.w)));\n");
        }
        Add(s, "    float fog = clamp(ze - u.fog.y, 0.0, 1.0);\n");
        switch (fsel) {
        case 4:
            Add(s, "    fog = 1.0 - exp2(-8.0 * fog);\n");
            break;
        case 5:
            Add(s, "    fog = 1.0 - exp2(-8.0 * fog * fog);\n");
            break;
        case 6:
            Add(s, "    fog = exp2(-8.0 * (1.0 - fog));\n");
            break;
        case 7:
            Add(s, "    fog = 1.0 - fog; fog = exp2(-8.0 * fog * fog);\n");
            break;
        default:
            break;
        }
        Add(s, "    int ifog = int(round(fog * 256.0));\n");
        Add(s, "    outc.rgb = (outc.rgb * (256 - ifog) + u.fogColor.rgb * ifog) >> 8; }\n");
    }

    {
        /* PORT_GX_SHOW=ras0|ras1|tx<stage>: draw one input of the TEV instead of its result (debugging). */
        const char* show = (key->numChans > 0 && (key->chanCtrl[0] >> 1) & 1) ? getenv("PORT_GX_SHOW") : NULL;
        if (show != NULL) {
            if (strcmp(show, "ras0") == 0 || strcmp(show, "nrm") == 0 || strcmp(show, "mat") == 0 || strcmp(show, "amb") == 0 || strcmp(show, "l1") == 0) {
                Add(s, "  outc = int4(ras0.rgb, 255);\n");
            } else if (strcmp(show, "ras1") == 0) {
                Add(s, "  outc = int4(ras1.rgb, 255);\n");
            } else if (strncmp(show, "tx", 2) == 0 && (uint32_t)atoi(show + 2) < key->numStages) {
                Add(s, "  outc = int4(clamp(stx[%d].rgb, 0, 255), 255);\n", atoi(show + 2));
            }
        }
    }
    /* The EFB keeps 6 bits per channel in RGBA6 and 5/6/5 in RGB565; what is read back
     * (copies, blending) is those bits expanded, so 128 comes back as 130. */
    Add(s, "  FOut o;\n");
    switch (key->efbFormat) {
    case 1:
        Add(s, "  float4 c = float4(outc >> 2) / 63.0;\n");
        break;
    case 2:
        Add(s, "  float4 c = float4(float(outc.r >> 3) / 31.0, float(outc.g >> 2) / 63.0, float(outc.b >> 3) / 31.0, 1.0);\n");
        break;
    default:
        Add(s, "  float4 c = float4(outc) / 255.0;\n");
        break;
    }
    if (key->dstAlpha) {
        Add(s, "  o.c = float4(c.rgb, u.misc.x);\n  o.src = c;\n");
    } else {
        Add(s, "  o.c = c;\n");
    }
    Add(s, "  return o;\n}\n");
}

char* PortGXShaderSource(const void* shaderKey) {
    const PortGXShaderKey* key = (const PortGXShaderKey*)shaderKey;
    Str s = {NULL, 0, 0};
    s.cap = 16384;
    s.p = (char*)malloc(s.cap);
    s.p[0] = '\0';
    Add(&s, "%s", kPrelude);
    VertexOut(&s, key);
    VertexShader(&s, key);
    FragmentShader(&s, key);
    {
        /* PORT_GX_SHADER_DUMP=FILE: appends every generated shader to FILE (debugging). */
        const char* dump = getenv("PORT_GX_SHADER_DUMP");
        FILE* f = dump != NULL ? fopen(dump, "a") : NULL;
        if (f != NULL) {
            fprintf(f, "// ==== shader\n%s\n", s.p);
            fclose(f);
        }
    }
    return s.p;
}
