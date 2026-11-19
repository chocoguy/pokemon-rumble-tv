/* The GPU backend under port/gx (Metal: platform/metal/port_gfx_metal.m).
 *
 * port/gx plays the Wii's GPU front end: it decodes the command stream, converts vertices
 * to one fixed layout (PortGXVertex), decodes textures to RGBA8 and writes the shaders for
 * the current TEV and transform state (port/gx/port_gx_shader.c). The backend owns the
 * embedded frame buffer (EFB) as a render target at an integer multiple of the console's
 * resolution, draws batches into it, and copies it out to textures or to the display.
 *
 * Every call comes from the thread that holds the emulated CPU, one at a time. */
#ifndef PORT_GFX_H
#define PORT_GFX_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The EFB the backend renders into, in console pixels (the largest the console has). */
#define PORT_EFB_WIDTH 640
#define PORT_EFB_HEIGHT 528

/* One vertex as the vertex shader reads it. The layout matches `Vtx` in the generated
 * Metal source: packed float3s, then 16-byte aligned colours and 8-byte texcoords. */
typedef struct PortGXVertex {
    float pos[3];
    float nrm[3];
    float bin[3];
    float tan[3];
    float clr[2][4];  /* 0..1 */
    float tex[8][2];
    uint32_t mtx[4];  /* byte k of the 9: position, then texture 0-7 matrix row index */
} PortGXVertex;

typedef struct PortGfxTexture PortGfxTexture;

/* Starts the backend; `scale` is the EFB's multiple of 640x528. Returns 0 if there is no
 * usable GPU. */
int PortGfxInit(int scale);
int PortGfxScale(void);

/* A texture from RGBA8 levels (byte order R, G, B, A), level 0 first. */
PortGfxTexture* PortGfxTextureCreate(int width, int height, int levels, const uint8_t* const* rgba);
void PortGfxTextureRelease(PortGfxTexture* texture);

/* Fixed-function state of a batch, decoded from the BP registers. */
typedef struct PortGfxRaster {
    uint8_t blendEnable;
    uint8_t blendSubtract;
    uint8_t srcFactor;  /* GX hardware encoding (PE_CMODE0) */
    uint8_t dstFactor;
    uint8_t logicEnable;
    uint8_t logicOp;
    uint8_t colorUpdate;
    uint8_t alphaUpdate;
    uint8_t dstAlphaEnable; /* PE_CMODE1 constant alpha: the shader writes it, blending uses source 1 */
    uint8_t efbHasAlpha;
    uint8_t zEnable;
    uint8_t zFunc;
    uint8_t zUpdate;
    uint8_t cull;   /* 0 none, 1 back (counter-clockwise on screen), 2 front, 3 all */
    uint8_t pad[2];
    int16_t scissor[4]; /* x0, y0, x1, y1 in EFB pixels, end exclusive */
} PortGfxRaster;

typedef struct PortGfxDraw {
    const PortGXVertex* vertices;
    uint32_t vertexCount;
    const uint32_t* indices;   /* triangle list */
    uint32_t indexCount;
    const uint32_t* xf;        /* XF memory (0x800 words) then registers 0x1000-0x1057 */
    uint32_t xfWords;
    uint32_t xfVersion;        /* changes whenever `xf` does */
    const void* uniforms;      /* PortGXUniforms (port_gx_shader.h) */
    uint32_t uniformBytes;
    const void* shaderKey;     /* PortGXShaderKey */
    uint32_t shaderKeyBytes;
    uint64_t shaderHash;
    PortGfxTexture* textures[8];
    uint32_t samplerMode0[8];  /* TX_SETMODE0/1 of each bound texture */
    uint32_t samplerMode1[8];
    PortGfxRaster raster;
} PortGfxDraw;

/* Called by the backend when it has no pipeline for a shader key: the Metal source of
 * the vertex function `vs` and fragment function `fs`. Returns a malloc'd string. */
char* PortGXShaderSource(const void* shaderKey);

void PortGfxDrawBatch(const PortGfxDraw* draw);

/* EFB copy. The result texture holds what sampling the destination format would read
 * (intensity formats replicated, missing channels as the decoder fills them). */
typedef enum PortGfxCopyKind {
    PORT_COPY_RGBA = 0,  /* RGBA8, RGB565, RGB5A3 and friends */
    PORT_COPY_I,         /* intensity: Y of the YUV conversion in all four channels */
    PORT_COPY_IA,        /* I in RGB, alpha in A */
    PORT_COPY_R, PORT_COPY_G, PORT_COPY_B, PORT_COPY_A,
    PORT_COPY_RG, PORT_COPY_GB,
    PORT_COPY_Z,         /* depth (24 bits) as intensity */
    PORT_COPY_RGB,       /* colour with alpha 1 (RGB565) */
} PortGfxCopyKind;

typedef struct PortGfxCopy {
    int x, y, width, height;   /* source in EFB pixels */
    int dstWidth, dstHeight;   /* destination (half size for a mipmap copy, or the display's height) */
    int kind;                  /* PortGfxCopyKind */
    int halfScale;             /* box filter 2x2 */
    int efbHasAlpha;
    int clear;                 /* clear the source rectangle afterwards */
    uint8_t clearColor[4];     /* R, G, B, A */
    uint32_t clearZ;           /* 24 bits */
    int clearColorMask;        /* bit 0 RGB, bit 1 alpha */
    int clearZUpdate;
} PortGfxCopy;

/* Returns a new texture holding the copy (texture copies and display copies alike). */
PortGfxTexture* PortGfxCopyEFB(const PortGfxCopy* copy);

/* The display shows `frame` (a display copy) from this retrace on; NULL shows black. */
void PortGfxPresent(PortGfxTexture* frame);

/* The shape of the TV the frame is shown on (width / height): the game draws anamorphic
 * 16:9 when the console's settings say so (port_sc.c does), and the TV stretches the
 * 640-pixel-wide frame. 0 shows the frame's own pixels. Default 16:9. */
void PortGfxSetDisplayAspect(float aspect);
float PortGfxDisplayAspect(void);

/* Writes `frame` as a PNG at the display's shape (waits for the GPU). Returns 0 on success. */
int PortGfxWritePNG(PortGfxTexture* frame, const char* path);

#ifdef __cplusplus
}
#endif

#endif
