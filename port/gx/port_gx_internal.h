/* Shared by the files of port/gx: the GPU registers the command stream sets, the vertex
 * loader, the texture cache and the shader generator. */
#ifndef PORT_GX_INTERNAL_H
#define PORT_GX_INTERNAL_H

#include <port_gfx.h>
#include <stdint.h>

/* XF memory: 0x000-0x0FF position/texture matrices, 0x400-0x45F normal matrices,
 * 0x500-0x5FF post-transform matrices, 0x600-0x67F lights. The registers 0x1000-0x1057
 * follow at word 0x800 of the same array, which is what the vertex shader reads. */
#define PORT_XF_MEM_WORDS 0x800
#define PORT_XF_REG_WORDS 0x58
#define PORT_XF_WORDS (PORT_XF_MEM_WORDS + PORT_XF_REG_WORDS)
#define XFREG(addr) (PORT_XF_MEM_WORDS + ((addr) - 0x1000))

typedef struct PortGXRegs {
    uint32_t cp[256];
    uint32_t bp[256];
    uint32_t xf[PORT_XF_WORDS];
    uint32_t xfVersion;
    /* TEV colour registers and konst colours: the BP pair 0xE0+2n holds either, chosen
     * by bit 23. Components R, G, B, A, signed 11 bits (konst: 8 bits). */
    int32_t tevReg[4][4];
    int32_t tevKonst[4][4];
} PortGXRegs;

extern PortGXRegs gPortGX;

/* TMEM's TLUT half (the 512 KB at 0x80000), as GXLoadTlut fills it: big-endian u16s. */
extern uint8_t gPortTMEM[0x80000];

/* --- Shaders (port_gx_shader.c) ------------------------------------------------------ */

/* Everything the generated shaders depend on; values that change per draw without
 * changing the code (colours, references, matrices) are uniforms or XF memory. Unused
 * stages and texgens are zeroed so equal programs share a key. */
typedef struct PortGXShaderKey {
    /* pixel */
    uint32_t numStages;
    uint32_t numTexGens;
    uint32_t numChans;
    uint32_t numIndStages;
    uint32_t colorEnv[16];
    uint32_t alphaEnv[16];
    uint32_t tref[16];       /* per stage: texmap | texcoord<<3 | enable<<6 | channel<<7 */
    uint32_t kcsel[16];
    uint32_t kasel[16];
    uint32_t swap[4];        /* swap tables: 2 bits per channel R, G, B, A */
    uint32_t alphaFunc;      /* ops and logic (references are uniforms) */
    uint32_t fogType;        /* fsel | proj<<3 */
    uint32_t dstAlpha;       /* constant alpha written instead of the TEV's */
    uint32_t efbFormat;      /* PE pixel format: 0 RGB8, 1 RGBA6, 2 RGB565 (others drawn as RGB8) */
    uint32_t texMask;        /* textures sampled */
    uint32_t indCmd[16];     /* IND_CMD per stage (BP 0x10+n) */
    uint32_t indRef;         /* RAS1_IREF: texmap, texcoord per indirect stage */
    uint32_t indScale[2];    /* RAS1_SS0/SS1 */
    /* vertex */
    uint32_t chanCtrl[4];    /* colour 0, colour 1, alpha 0, alpha 1 */
    uint32_t texGen[8];
    uint32_t dualTex;
    uint32_t postNormalize;  /* bit i: texgen i normalises before the post matrix */
} PortGXShaderKey;

/* Uniforms both stages read (buffer 2 of the vertex stage, buffer 0 of the fragment
 * stage); the layout matches `U` in the generated source. */
typedef struct PortGXUniforms {
    int32_t tevReg[4][4];
    int32_t konst[4][4];
    int32_t alphaRef[4];
    float fog[4];          /* A, C, B magnitude, B shift */
    int32_t fogColor[4];
    float lodBias[8][4];
    int32_t indMtx[6][4];  /* indirect matrices: rows (a, c, e, shift), (b, d, f, shift) */
    float texSize[8][4];   /* width, height of each bound texture */
    float efb[4];          /* EFB width, height (console pixels), scissor offset x, y in screen space */
    float misc[4];         /* x: PE constant alpha (0..1) */
} PortGXUniforms;

void PortGXBuildShader(PortGXShaderKey* key, PortGXUniforms* uniforms, uint64_t* hash);

/* --- Vertices (port_gx_vertex.c) ----------------------------------------------------- */

/* Decodes `count` vertices of format `vat` from `data` (the command stream). Returns the
 * bytes consumed, or 0 if the format is invalid. */
uint32_t PortGXLoadVertices(int vat, const uint8_t* data, uint32_t count, PortGXVertex* out);

/* Appends the triangle-list indices of primitive `prim` (opcode bits 3-6) over vertices
 * base..base+count-1; returns the number written (at most 3 * count). */
uint32_t PortGXPrimitiveIndices(int prim, uint32_t base, uint32_t count, uint32_t* out);

/* --- Textures (port_gx_texture.c) ---------------------------------------------------- */

/* The texture bound to map `map`, decoded or from the cache; NULL if it cannot be read. */
PortGfxTexture* PortGXTexture(int map);
/* Memory may have changed: textures are rehashed when next used. */
void PortGXTextureInvalidate(void);
/* An EFB copy now lives at `addr`. */
void PortGXTextureCopied(uint32_t addr, PortGfxTexture* texture, int width, int height);
/* Called once per presented frame: frees textures unused for a while. */
void PortGXTextureFrame(void);
/* The display copy at physical address `addr`, if there is one. */
PortGfxTexture* PortGXFrameBufferTexture(uint32_t addr);
void PortGXFrameBufferCopied(uint32_t addr, PortGfxTexture* texture);

/* Texture decode to RGBA8 (exposed for tests). Returns the bytes `format` occupies. */
uint32_t PortGXTextureSize(int format, int width, int height);
int PortGXDecodeTexture(uint8_t* out, const uint8_t* src, int format, int width, int height, const uint8_t* tlut,
                        int tlutFormat);

#endif
