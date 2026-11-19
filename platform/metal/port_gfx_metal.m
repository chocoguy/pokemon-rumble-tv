/* The Metal backend of the renderer (port_gfx.h), shared by the headless runner on the Mac
 * and the tvOS app.
 *
 * The EFB is one RGBA8 colour texture and one 32-bit float depth texture at `scale` times
 * 640x528. Batches are drawn into it in one render pass, which ends only for an EFB copy:
 * the copy is a draw into a new texture (at the same scale) that samples the EFB, followed
 * by the copy's clear as a scissored draw. Pipelines are compiled from the generated
 * source on first use and cached by shader hash and blend state.
 *
 * Per-batch data (vertices, indices, XF memory, uniforms) goes into shared buffers taken
 * from a pool; a buffer returns to the pool when the command buffer using it completes. */
#import <Foundation/Foundation.h>
#include <time.h>
#import <Metal/Metal.h>
#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <QuartzCore/CAMetalLayer.h>
#include <os/lock.h>
#include <port_gfx.h>
#include "port_gfx_metal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void PortLog(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void PortRed(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void PortRedOnce(int* reported, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void PortAddSummary(void (*func)(void));

/* ------------------------------------------------------------------------------------ */

@interface PortTex : NSObject {
@public
    id<MTLTexture> tex;
    int width, height; /* console pixels */
}
@end
@implementation PortTex
@end

@interface PortChunk : NSObject {
@public
    id<MTLBuffer> buf;
    size_t used;
}
@end
@implementation PortChunk
@end

static id<MTLDevice> sDevice;
static id<MTLCommandQueue> sQueue;
static id<MTLLibrary> sFixedLib;
static int sScale = 1;
static id<MTLTexture> sEFBColor;
static id<MTLTexture> sEFBDepth;
static int sEFBInitialized;

static id<MTLCommandBuffer> sCmd;
static id<MTLRenderCommandEncoder> sEnc;
static dispatch_semaphore_t sInFlight;

static NSMutableArray<PortChunk*>* sFreeChunks;
static NSMutableArray<PortChunk*>* sUsedChunks;
static PortChunk* sChunk;
#define CHUNK_SIZE (8u << 20)

static NSMutableDictionary<NSNumber*, id<MTLLibrary>>* sLibraries;
static NSMutableDictionary<NSNumber*, id<MTLRenderPipelineState>>* sPipelines;
static NSMutableDictionary<NSNumber*, id<MTLSamplerState>>* sSamplers;
static id<MTLDepthStencilState> sDepthStates[2][8][2];
static double sCompileSeconds, sCompileWorst; /* shader and pipeline compiles */
static id<MTLRenderPipelineState> sCopyPipeline;
static id<MTLRenderPipelineState> sClearPipelines[4];
static id<MTLDepthStencilState> sClearDepth[2];
static id<MTLSamplerState> sLinearClamp, sNearestClamp;

/* What the encoder has bound, so a batch only sets what changed. */
static uint32_t sBoundXFVersion;
static id<MTLBuffer> sBoundXFBuffer;
static NSUInteger sBoundXFOffset;
static int sXFBoundThisPass;

static id<MTLTexture> sPresented; /* the frame on the display (sPresentLock) */
static float sDisplayAspect = 16.0f / 9.0f;

static unsigned long long sStatDraws, sStatPipelines, sStatCommandBuffers;

/* ------------------------------------------------------------------------------------ */
/* Upload buffers                                                                        */

static void* Upload(size_t bytes, id<MTLBuffer>* buffer, NSUInteger* offset) {
    size_t aligned = (bytes + 255) & ~(size_t)255;
    if (sChunk == nil || sChunk->used + aligned > sChunk->buf.length) {
        PortChunk* c = nil;
        @synchronized(sFreeChunks) {
            for (PortChunk* f in sFreeChunks) {
                if (f->buf.length >= aligned) {
                    c = f;
                    break;
                }
            }
            if (c != nil) {
                [sFreeChunks removeObject:c];
            }
        }
        if (c == nil) {
            c = [PortChunk new];
            c->buf = [sDevice newBufferWithLength:(aligned > CHUNK_SIZE ? aligned : CHUNK_SIZE)
                                          options:MTLResourceStorageModeShared];
        }
        c->used = 0;
        [sUsedChunks addObject:c];
        sChunk = c;
    }
    *buffer = sChunk->buf;
    *offset = sChunk->used;
    sChunk->used += aligned;
    return (uint8_t*)sChunk->buf.contents + *offset;
}

/* ------------------------------------------------------------------------------------ */
/* Command buffers and the EFB pass                                                      */

static void EnsureCommandBuffer(void) {
    if (sCmd != nil) {
        return;
    }
    dispatch_semaphore_wait(sInFlight, DISPATCH_TIME_FOREVER);
    sCmd = [sQueue commandBuffer];
    sStatCommandBuffers++;
}

static void EndPass(void) {
    if (sEnc != nil) {
        [sEnc endEncoding];
        sEnc = nil;
    }
}

static void Commit(void) {
    NSArray<PortChunk*>* chunks;
    EndPass();
    if (sCmd == nil) {
        return;
    }
    chunks = [sUsedChunks copy];
    [sUsedChunks removeAllObjects];
    sChunk = nil;
    [sCmd addCompletedHandler:^(id<MTLCommandBuffer> cb) {
      static int errorReported;
      if (cb.error != nil && !errorReported) {
          errorReported = 1;
          fprintf(stderr, "[port] Metal: command buffer failed: %s\n", cb.error.localizedDescription.UTF8String);
      }
      @synchronized(sFreeChunks) {
          [sFreeChunks addObjectsFromArray:chunks];
      }
      dispatch_semaphore_signal(sInFlight);
    }];
    [sCmd commit];
    sCmd = nil;
}

static void BeginPass(void) {
    MTLRenderPassDescriptor* rp;
    if (sEnc != nil) {
        return;
    }
    EnsureCommandBuffer();
    rp = [MTLRenderPassDescriptor renderPassDescriptor];
    rp.colorAttachments[0].texture = sEFBColor;
    rp.colorAttachments[0].loadAction = sEFBInitialized ? MTLLoadActionLoad : MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.depthAttachment.texture = sEFBDepth;
    rp.depthAttachment.loadAction = sEFBInitialized ? MTLLoadActionLoad : MTLLoadActionClear;
    rp.depthAttachment.clearDepth = 1.0;
    rp.depthAttachment.storeAction = MTLStoreActionStore;
    sEFBInitialized = 1;
    sEnc = [sCmd renderCommandEncoderWithDescriptor:rp];
    /* GX's front faces are counter-clockwise on screen (y down), which Metal, judging
     * winding with y up, sees as clockwise. (Getting this backwards culls every floor and
     * draws back faces, e.g. the outline pass's full-screen quad, over the scene.) */
    [sEnc setFrontFacingWinding:MTLWindingClockwise];
    sXFBoundThisPass = 0;
}

/* ------------------------------------------------------------------------------------ */
/* Fixed shaders: EFB copies and clears                                                  */

static const char* kFixedSource =
    "#include <metal_stdlib>\n"
    "using namespace metal;\n"
    "struct CopyOut { float4 pos [[position]]; float2 uv; };\n"
    "struct CopyU { float4 rect; int4 kind; float4 clearColor; float4 clearZ; };\n"
    "vertex CopyOut copyVS(uint vid [[vertex_id]], constant CopyU& u [[buffer(0)]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  CopyOut o;\n"
    "  o.pos = float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "  o.uv = u.rect.xy + p * u.rect.zw;\n"
    "  return o;\n"
    "}\n"
    "fragment float4 copyFS(CopyOut in [[stage_in]], constant CopyU& u [[buffer(0)]],\n"
    "                       texture2d<float> color [[texture(0)]], depth2d<float> depth [[texture(1)]],\n"
    "                       sampler s [[sampler(0)]]) {\n"
    "  float4 c = color.sample(s, in.uv);\n"
    "  if (u.kind.y == 0) c.a = 1.0;\n"
    "  float y = clamp(dot(c.rgb, float3(0.257, 0.504, 0.098)) + 16.0 / 255.0, 0.0, 1.0);\n"
    "  switch (u.kind.x) {\n"
    "  case 1: return float4(y);\n"
    "  case 2: return float4(y, y, y, c.a);\n"
    "  case 3: return float4(c.r);\n"
    "  case 4: return float4(c.g);\n"
    "  case 5: return float4(c.b);\n"
    "  case 6: return float4(c.a);\n"
    "  case 7: return float4(c.r, c.r, c.r, c.g);\n"
    "  case 8: return float4(c.g, c.g, c.g, c.b);\n"
    "  case 9: { float z = depth.sample(s, in.uv); return float4(z); }\n"
    "  case 10: return float4(c.rgb, 1.0);\n"
    "  default: return c;\n"
    "  }\n"
    "}\n"
    "struct ClearOut { float4 c [[color(0)]]; float z [[depth(any)]]; };\n"
    "vertex float4 clearVS(uint vid [[vertex_id]]) {\n"
    "  float2 p = float2((vid << 1) & 2, vid & 2);\n"
    "  return float4(p * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);\n"
    "}\n"
    "fragment ClearOut clearFS(float4 pos [[position]], constant CopyU& u [[buffer(0)]]) {\n"
    "  ClearOut o; o.c = u.clearColor; o.z = u.clearZ.x; return o;\n"
    "}\n";

typedef struct CopyUniforms {
    float rect[4];
    int32_t kind[4];
    float clearColor[4];
    float clearZ[4];
} CopyUniforms;

static int MakeFixed(void) {
    NSError* err = nil;
    MTLRenderPipelineDescriptor* d;
    MTLSamplerDescriptor* sd;
    MTLDepthStencilDescriptor* dd;
    int i;
    sFixedLib = [sDevice newLibraryWithSource:[NSString stringWithUTF8String:kFixedSource] options:nil error:&err];
    if (sFixedLib == nil) {
        PortRed("Metal: fixed shaders: %s", err.localizedDescription.UTF8String);
        return 0;
    }
    d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [sFixedLib newFunctionWithName:@"copyVS"];
    d.fragmentFunction = [sFixedLib newFunctionWithName:@"copyFS"];
    d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
    sCopyPipeline = [sDevice newRenderPipelineStateWithDescriptor:d error:&err];
    if (sCopyPipeline == nil) {
        PortRed("Metal: copy pipeline: %s", err.localizedDescription.UTF8String);
        return 0;
    }
    for (i = 0; i < 4; i++) {
        d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [sFixedLib newFunctionWithName:@"clearVS"];
        d.fragmentFunction = [sFixedLib newFunctionWithName:@"clearFS"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        d.colorAttachments[0].writeMask = ((i & 1) ? (MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue) : 0) |
                                          ((i & 2) ? MTLColorWriteMaskAlpha : 0);
        d.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        sClearPipelines[i] = [sDevice newRenderPipelineStateWithDescriptor:d error:&err];
        if (sClearPipelines[i] == nil) {
            PortRed("Metal: clear pipeline: %s", err.localizedDescription.UTF8String);
            return 0;
        }
    }
    for (i = 0; i < 2; i++) {
        dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = MTLCompareFunctionAlways;
        dd.depthWriteEnabled = i != 0;
        sClearDepth[i] = [sDevice newDepthStencilStateWithDescriptor:dd];
    }
    sd = [MTLSamplerDescriptor new];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
    sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge;
    sLinearClamp = [sDevice newSamplerStateWithDescriptor:sd];
    sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterNearest;
    sNearestClamp = [sDevice newSamplerStateWithDescriptor:sd];
    return 1;
}

/* ------------------------------------------------------------------------------------ */

static void PrintSummary(void) {
    fprintf(stderr, "[port] Metal: shader and pipeline compiles took %.0f ms (worst %.0f ms)\n", sCompileSeconds * 1000.0,
            sCompileWorst * 1000.0);
    fprintf(stderr, "[port] Metal: %llu draws, %llu pipelines, %llu command buffers, EFB %dx%d\n", sStatDraws,
            sStatPipelines, sStatCommandBuffers, PORT_EFB_WIDTH * sScale, PORT_EFB_HEIGHT * sScale);
}

int PortGfxInit(int scale) {
    @autoreleasepool {
        MTLTextureDescriptor* td;
        sDevice = MTLCreateSystemDefaultDevice();
        if (sDevice == nil) {
            PortRed("Metal: no GPU; nothing will be drawn");
            return 0;
        }
        sScale = scale < 1 ? 1 : scale > 6 ? 6 : scale;
        sQueue = [sDevice newCommandQueue];
        sInFlight = dispatch_semaphore_create(3);
        sFreeChunks = [NSMutableArray new];
        sUsedChunks = [NSMutableArray new];
        sLibraries = [NSMutableDictionary new];
        sPipelines = [NSMutableDictionary new];
        sSamplers = [NSMutableDictionary new];
        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                width:PORT_EFB_WIDTH * sScale
                                                               height:PORT_EFB_HEIGHT * sScale
                                                            mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        sEFBColor = [sDevice newTextureWithDescriptor:td];
        td.pixelFormat = MTLPixelFormatDepth32Float;
        sEFBDepth = [sDevice newTextureWithDescriptor:td];
        if (!MakeFixed()) {
            return 0;
        }
        PortAddSummary(PrintSummary);
        PortLog("Metal: %s, EFB %dx%d", sDevice.name.UTF8String, PORT_EFB_WIDTH * sScale, PORT_EFB_HEIGHT * sScale);
        return 1;
    }
}

int PortGfxScale(void) {
    return sScale;
}

/* ------------------------------------------------------------------------------------ */
/* Textures                                                                              */

PortGfxTexture* PortGfxTextureCreate(int width, int height, int levels, const uint8_t* const* rgba) {
    @autoreleasepool {
        PortTex* t = [PortTex new];
        MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                      width:(NSUInteger)width
                                                                                     height:(NSUInteger)height
                                                                                  mipmapped:levels > 1];
        int l;
        td.mipmapLevelCount = (NSUInteger)levels;
        td.usage = MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModeShared;
        t->tex = [sDevice newTextureWithDescriptor:td];
        t->width = width;
        t->height = height;
        for (l = 0; l < levels; l++) {
            int w = width >> l, h = height >> l;
            w = w < 1 ? 1 : w;
            h = h < 1 ? 1 : h;
            [t->tex replaceRegion:MTLRegionMake2D(0, 0, (NSUInteger)w, (NSUInteger)h)
                      mipmapLevel:(NSUInteger)l
                        withBytes:rgba[l]
                      bytesPerRow:(NSUInteger)w * 4];
        }
        return (__bridge_retained PortGfxTexture*)t;
    }
}

void PortGfxTextureRelease(PortGfxTexture* texture) {
    if (texture != NULL) {
        PortTex* t = (__bridge_transfer PortTex*)texture;
        (void)t;
    }
}

static PortTex* Tex(PortGfxTexture* t) {
    return (__bridge PortTex*)t;
}

/* ------------------------------------------------------------------------------------ */
/* Pipelines                                                                             */

static MTLBlendFactor SrcFactor(int f, const PortGfxRaster* r) {
    switch (f) {
    case 0: return MTLBlendFactorZero;
    case 1: return MTLBlendFactorOne;
    case 2: return MTLBlendFactorDestinationColor;
    case 3: return MTLBlendFactorOneMinusDestinationColor;
    case 4: return r->dstAlphaEnable ? MTLBlendFactorSource1Alpha : MTLBlendFactorSourceAlpha;
    case 5: return r->dstAlphaEnable ? MTLBlendFactorOneMinusSource1Alpha : MTLBlendFactorOneMinusSourceAlpha;
    case 6: return r->efbHasAlpha ? MTLBlendFactorDestinationAlpha : MTLBlendFactorOne;
    default: return r->efbHasAlpha ? MTLBlendFactorOneMinusDestinationAlpha : MTLBlendFactorZero;
    }
}

static MTLBlendFactor DstFactor(int f, const PortGfxRaster* r) {
    switch (f) {
    case 0: return MTLBlendFactorZero;
    case 1: return MTLBlendFactorOne;
    case 2: return MTLBlendFactorSourceColor;
    case 3: return MTLBlendFactorOneMinusSourceColor;
    case 4: return r->dstAlphaEnable ? MTLBlendFactorSource1Alpha : MTLBlendFactorSourceAlpha;
    case 5: return r->dstAlphaEnable ? MTLBlendFactorOneMinusSource1Alpha : MTLBlendFactorOneMinusSourceAlpha;
    case 6: return r->efbHasAlpha ? MTLBlendFactorDestinationAlpha : MTLBlendFactorOne;
    default: return r->efbHasAlpha ? MTLBlendFactorOneMinusDestinationAlpha : MTLBlendFactorZero;
    }
}

/* The blend-related bits of the raster state, as one number. */
static uint32_t BlendKey(const PortGfxRaster* r) {
    return (uint32_t)r->blendEnable | (uint32_t)r->blendSubtract << 1 | (uint32_t)r->srcFactor << 2 |
           (uint32_t)r->dstFactor << 5 | (uint32_t)r->logicEnable << 8 | (uint32_t)r->logicOp << 9 |
           (uint32_t)r->colorUpdate << 13 | (uint32_t)r->alphaUpdate << 14 | (uint32_t)r->dstAlphaEnable << 15 |
           (uint32_t)r->efbHasAlpha << 16;
}

/* Time spent compiling shaders and pipelines (on the game's thread). */
static double NowSeconds(void) {
    return (double)clock_gettime_nsec_np(CLOCK_UPTIME_RAW) / 1e9;
}


static void NoteCompile(double start) {
    double t = NowSeconds() - start;
    sCompileSeconds += t;
    if (t > sCompileWorst) {
        sCompileWorst = t;
    }
}

static id<MTLLibrary> Library(const PortGfxDraw* d) {
    NSNumber* key = @(d->shaderHash);
    id<MTLLibrary> lib = sLibraries[key];
    if (lib == nil) {
        double start = NowSeconds();
        char* src = PortGXShaderSource(d->shaderKey);
        NSError* err = nil;
        MTLCompileOptions* opts = [MTLCompileOptions new];
        opts.mathMode = MTLMathModeSafe;
        lib = [sDevice newLibraryWithSource:[NSString stringWithUTF8String:src] options:opts error:&err];
        NoteCompile(start);
        if (lib == nil) {
            NSString* path = [NSTemporaryDirectory() stringByAppendingFormat:@"rumble_shader_%016llx.metal", d->shaderHash];
            [[NSString stringWithUTF8String:src] writeToFile:path atomically:NO encoding:NSUTF8StringEncoding error:nil];
            PortRed("Metal: shader %016llx does not compile (source in %s): %.600s", d->shaderHash, path.UTF8String,
                    err.localizedDescription.UTF8String);
            free(src);
            lib = (id<MTLLibrary>)[NSNull null];
            sLibraries[key] = lib;
            return nil;
        }
        free(src);
        sLibraries[key] = lib;
    }
    if ((id)lib == [NSNull null]) {
        return nil;
    }
    return lib;
}

static id<MTLRenderPipelineState> Pipeline(const PortGfxDraw* d) {
    const PortGfxRaster* r = &d->raster;
    uint32_t blend = BlendKey(r);
    NSNumber* key = @(d->shaderHash ^ ((uint64_t)blend * 0x9E3779B97F4A7C15ull));
    id<MTLRenderPipelineState> ps = sPipelines[key];
    if (ps == nil) {
        id<MTLLibrary> lib = Library(d);
        MTLRenderPipelineDescriptor* pd;
        MTLRenderPipelineColorAttachmentDescriptor* ca;
        NSError* err = nil;
        MTLColorWriteMask mask = 0;
        static int logicReported;
        if (lib == nil) {
            return nil;
        }
        pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"];
        pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
        ca = pd.colorAttachments[0];
        ca.pixelFormat = MTLPixelFormatRGBA8Unorm;
        if (r->colorUpdate) {
            mask |= MTLColorWriteMaskRed | MTLColorWriteMaskGreen | MTLColorWriteMaskBlue;
        }
        if (r->alphaUpdate && r->efbHasAlpha) {
            mask |= MTLColorWriteMaskAlpha;
        }
        if (r->blendEnable) {
            ca.blendingEnabled = YES;
            if (r->blendSubtract) {
                ca.rgbBlendOperation = ca.alphaBlendOperation = MTLBlendOperationReverseSubtract;
                ca.sourceRGBBlendFactor = ca.sourceAlphaBlendFactor = MTLBlendFactorOne;
                ca.destinationRGBBlendFactor = ca.destinationAlphaBlendFactor = MTLBlendFactorOne;
            } else {
                ca.sourceRGBBlendFactor = ca.sourceAlphaBlendFactor = SrcFactor(r->srcFactor, r);
                ca.destinationRGBBlendFactor = ca.destinationAlphaBlendFactor = DstFactor(r->dstFactor, r);
            }
        } else if (r->logicEnable) {
            switch (r->logicOp) {
            case 3: /* copy */
                break;
            case 0: /* clear */
                ca.blendingEnabled = YES;
                ca.sourceRGBBlendFactor = ca.sourceAlphaBlendFactor = MTLBlendFactorZero;
                ca.destinationRGBBlendFactor = ca.destinationAlphaBlendFactor = MTLBlendFactorZero;
                break;
            case 5: /* no-op */
                mask = 0;
                break;
            default:
                PortRedOnce(&logicReported, "GX: logic op %d is drawn as copy", r->logicOp);
                break;
            }
        }
        ca.writeMask = mask;
        {
            double start = NowSeconds();
            ps = [sDevice newRenderPipelineStateWithDescriptor:pd error:&err];
            NoteCompile(start);
        }
        if (ps == nil) {
            PortRed("Metal: pipeline for shader %016llx: %s", d->shaderHash, err.localizedDescription.UTF8String);
            return nil;
        }
        sPipelines[key] = ps;
        sStatPipelines++;
    }
    return ps;
}

static id<MTLDepthStencilState> DepthState(const PortGfxRaster* r) {
    int enable = r->zEnable ? 1 : 0;
    int func = r->zFunc & 7;
    int update = r->zUpdate ? 1 : 0;
    int fi = enable ? func : 0, ui = enable ? update : 0;
    if (sDepthStates[enable][fi][ui] == nil) {
        static const MTLCompareFunction kFunc[8] = {
            MTLCompareFunctionNever,   MTLCompareFunctionLess,     MTLCompareFunctionEqual,
            MTLCompareFunctionLessEqual, MTLCompareFunctionGreater, MTLCompareFunctionNotEqual,
            MTLCompareFunctionGreaterEqual, MTLCompareFunctionAlways,
        };
        MTLDepthStencilDescriptor* dd = [MTLDepthStencilDescriptor new];
        dd.depthCompareFunction = enable ? kFunc[func] : MTLCompareFunctionAlways;
        dd.depthWriteEnabled = enable && update;
        sDepthStates[enable][fi][ui] = [sDevice newDepthStencilStateWithDescriptor:dd];
    }
    return sDepthStates[enable][fi][ui];
}

static MTLSamplerAddressMode Wrap(uint32_t w) {
    return w == 1 ? MTLSamplerAddressModeRepeat : w == 2 ? MTLSamplerAddressModeMirrorRepeat : MTLSamplerAddressModeClampToEdge;
}

static id<MTLSamplerState> Sampler(uint32_t mode0, uint32_t mode1) {
    uint64_t k = (uint64_t)(mode0 & 0x1800FF) | (uint64_t)(mode1 & 0xFFFF) << 32;
    NSNumber* key = @(k);
    id<MTLSamplerState> s = sSamplers[key];
    if (s == nil) {
        MTLSamplerDescriptor* sd = [MTLSamplerDescriptor new];
        uint32_t minf = (mode0 >> 5) & 7;
        uint32_t aniso = (mode0 >> 19) & 3;
        sd.sAddressMode = Wrap(mode0 & 3);
        sd.tAddressMode = Wrap((mode0 >> 2) & 3);
        sd.magFilter = ((mode0 >> 4) & 1) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.minFilter = (minf & 4) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
        sd.mipFilter = (minf & 3) == 1 ? MTLSamplerMipFilterNearest
                       : (minf & 3) == 2 ? MTLSamplerMipFilterLinear
                                         : MTLSamplerMipFilterNotMipmapped;
        sd.lodMinClamp = (float)(mode1 & 0xFF) / 16.0f;
        sd.lodMaxClamp = (float)((mode1 >> 8) & 0xFF) / 16.0f;
        sd.maxAnisotropy = aniso == 0 ? 1 : aniso == 1 ? 2 : 4;
        s = [sDevice newSamplerStateWithDescriptor:sd];
        sSamplers[key] = s;
    }
    return s;
}

/* ------------------------------------------------------------------------------------ */
/* Draws                                                                                 */

void PortGfxDrawBatch(const PortGfxDraw* d) {
    @autoreleasepool {
        const PortGfxRaster* r = &d->raster;
        id<MTLRenderPipelineState> ps;
        id<MTLBuffer> buf;
        NSUInteger off, ioff;
        id<MTLBuffer> ibuf;
        void* p;
        int i;
        MTLScissorRect sc;

        if (r->scissor[2] <= r->scissor[0] || r->scissor[3] <= r->scissor[1]) {
            return;
        }
        ps = Pipeline(d);
        if (ps == nil) {
            return;
        }
        BeginPass();
        [sEnc setRenderPipelineState:ps];
        [sEnc setDepthStencilState:DepthState(r)];
        [sEnc setCullMode:r->cull == 1 ? MTLCullModeBack : r->cull == 2 ? MTLCullModeFront : MTLCullModeNone];
        sc.x = (NSUInteger)(r->scissor[0] * sScale);
        sc.y = (NSUInteger)(r->scissor[1] * sScale);
        sc.width = (NSUInteger)((r->scissor[2] - r->scissor[0]) * sScale);
        sc.height = (NSUInteger)((r->scissor[3] - r->scissor[1]) * sScale);
        [sEnc setScissorRect:sc];

        p = Upload(d->vertexCount * sizeof(PortGXVertex), &buf, &off);
        memcpy(p, d->vertices, d->vertexCount * sizeof(PortGXVertex));
        [sEnc setVertexBuffer:buf offset:off atIndex:0];

        if (!sXFBoundThisPass || d->xfVersion != sBoundXFVersion || sBoundXFBuffer == nil) {
            p = Upload(d->xfWords * 4, &buf, &off);
            memcpy(p, d->xf, d->xfWords * 4);
            sBoundXFBuffer = buf;
            sBoundXFOffset = off;
            sBoundXFVersion = d->xfVersion;
            sXFBoundThisPass = 1;
            [sEnc setVertexBuffer:buf offset:off atIndex:1];
        }

        p = Upload(d->uniformBytes, &buf, &off);
        memcpy(p, d->uniforms, d->uniformBytes);
        [sEnc setVertexBuffer:buf offset:off atIndex:2];
        [sEnc setFragmentBuffer:buf offset:off atIndex:0];

        for (i = 0; i < 8; i++) {
            if (d->textures[i] != NULL) {
                [sEnc setFragmentTexture:Tex(d->textures[i])->tex atIndex:(NSUInteger)i];
                [sEnc setFragmentSamplerState:Sampler(d->samplerMode0[i], d->samplerMode1[i]) atIndex:(NSUInteger)i];
            }
        }

        p = Upload(d->indexCount * 4, &ibuf, &ioff);
        memcpy(p, d->indices, d->indexCount * 4);
        [sEnc drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                         indexCount:d->indexCount
                          indexType:MTLIndexTypeUInt32
                        indexBuffer:ibuf
                  indexBufferOffset:ioff];
        sStatDraws++;
    }
}

/* ------------------------------------------------------------------------------------ */
/* Copies                                                                                */

PortGfxTexture* PortGfxCopyEFB(const PortGfxCopy* c) {
    @autoreleasepool {
        PortTex* t = [PortTex new];
        MTLTextureDescriptor* td;
        MTLRenderPassDescriptor* rp;
        id<MTLRenderCommandEncoder> enc;
        CopyUniforms u;

        if (c->width <= 0 || c->height <= 0 || c->dstWidth <= 0 || c->dstHeight <= 0) {
            return NULL;
        }
        EndPass();
        EnsureCommandBuffer();
        if (!sEFBInitialized) {
            BeginPass();
            EndPass();
        }

        td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                width:(NSUInteger)(c->dstWidth * sScale)
                                                               height:(NSUInteger)(c->dstHeight * sScale)
                                                            mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
        td.storageMode = MTLStorageModePrivate;
        t->tex = [sDevice newTextureWithDescriptor:td];
        t->width = c->dstWidth;
        t->height = c->dstHeight;

        memset(&u, 0, sizeof(u));
        u.rect[0] = (float)c->x / PORT_EFB_WIDTH;
        u.rect[1] = (float)c->y / PORT_EFB_HEIGHT;
        u.rect[2] = (float)c->width / PORT_EFB_WIDTH;
        u.rect[3] = (float)c->height / PORT_EFB_HEIGHT;
        u.kind[0] = c->kind;
        u.kind[1] = c->efbHasAlpha;

        rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = t->tex;
        rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        enc = [sCmd renderCommandEncoderWithDescriptor:rp];
        [enc setRenderPipelineState:sCopyPipeline];
        [enc setVertexBytes:&u length:sizeof(u) atIndex:0];
        [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
        [enc setFragmentTexture:sEFBColor atIndex:0];
        [enc setFragmentTexture:sEFBDepth atIndex:1];
        /* Same size: one EFB texel per destination texel. Half size or a stretched
         * display copy: the bilinear filter averages the source texels. */
        [enc setFragmentSamplerState:(c->halfScale || c->dstHeight != c->height) ? sLinearClamp : sNearestClamp atIndex:0];
        [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        [enc endEncoding];

        if (c->clear && (c->clearColorMask != 0 || c->clearZUpdate)) {
            MTLScissorRect sc;
            int mask = c->clearColorMask & (c->efbHasAlpha ? 3 : 1);
            u.clearColor[0] = c->clearColor[0] / 255.0f;
            u.clearColor[1] = c->clearColor[1] / 255.0f;
            u.clearColor[2] = c->clearColor[2] / 255.0f;
            u.clearColor[3] = c->clearColor[3] / 255.0f;
            u.clearZ[0] = (float)c->clearZ / 16777215.0f;
            BeginPass();
            sc.x = (NSUInteger)(c->x * sScale);
            sc.y = (NSUInteger)(c->y * sScale);
            sc.width = (NSUInteger)(c->width * sScale);
            sc.height = (NSUInteger)(c->height * sScale);
            if (sc.x + sc.width > sEFBColor.width) {
                sc.width = sEFBColor.width - sc.x;
            }
            if (sc.y + sc.height > sEFBColor.height) {
                sc.height = sEFBColor.height - sc.y;
            }
            [sEnc setRenderPipelineState:sClearPipelines[mask]];
            [sEnc setDepthStencilState:sClearDepth[c->clearZUpdate ? 1 : 0]];
            [sEnc setCullMode:MTLCullModeNone];
            [sEnc setScissorRect:sc];
            [sEnc setFragmentBytes:&u length:sizeof(u) atIndex:0];
            [sEnc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        return (__bridge_retained PortGfxTexture*)t;
    }
}

/* ------------------------------------------------------------------------------------ */
/* Display                                                                               */

static os_unfair_lock sPresentLock = OS_UNFAIR_LOCK_INIT;
static id<MTLRenderPipelineState> sScreenPipeline;
static MTLPixelFormat sScreenFormat;

void PortGfxPresent(PortGfxTexture* frame) {
    @autoreleasepool {
        id<MTLTexture> tex = frame != NULL ? Tex(frame)->tex : nil;
        /* A retrace is where the console would have shown the frame: send the work first,
         * so a screen pass the app's thread commits later reads the finished frame. */
        Commit();
        os_unfair_lock_lock(&sPresentLock);
        sPresented = tex;
        os_unfair_lock_unlock(&sPresentLock);
    }
}

void PortGfxPresentToLayer(CAMetalLayer* layer) {
    @autoreleasepool {
        id<MTLTexture> tex;
        id<CAMetalDrawable> drawable;
        id<MTLCommandBuffer> cb;
        id<MTLRenderCommandEncoder> enc;
        MTLRenderPassDescriptor* rp;
        if (sDevice == nil) {
            return;
        }
        if (layer.device == nil) {
            layer.device = sDevice;
            layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
            layer.framebufferOnly = YES;
        }
        if (sScreenPipeline == nil || sScreenFormat != layer.pixelFormat) {
            MTLRenderPipelineDescriptor* d = [MTLRenderPipelineDescriptor new];
            NSError* err = nil;
            d.vertexFunction = [sFixedLib newFunctionWithName:@"copyVS"];
            d.fragmentFunction = [sFixedLib newFunctionWithName:@"copyFS"];
            d.colorAttachments[0].pixelFormat = layer.pixelFormat;
            sScreenPipeline = [sDevice newRenderPipelineStateWithDescriptor:d error:&err];
            sScreenFormat = layer.pixelFormat;
            if (sScreenPipeline == nil) {
                PortRed("Metal: screen pipeline: %s", err.localizedDescription.UTF8String);
                return;
            }
        }
        os_unfair_lock_lock(&sPresentLock);
        tex = sPresented;
        os_unfair_lock_unlock(&sPresentLock);

        drawable = [layer nextDrawable];
        if (drawable == nil) {
            return;
        }
        cb = [sQueue commandBuffer];
        rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = drawable.texture;
        rp.colorAttachments[0].loadAction = MTLLoadActionClear;
        rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
        rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        enc = [cb renderCommandEncoderWithDescriptor:rp];
        if (tex != nil) {
            double dw = (double)drawable.texture.width, dh = (double)drawable.texture.height;
            double aspect = sDisplayAspect > 0.0f ? sDisplayAspect : (double)tex.width / (double)tex.height;
            double w = dw, h = dw / aspect;
            CopyUniforms u;
            MTLViewport vp;
            if (h > dh) {
                h = dh;
                w = dh * aspect;
            }
            vp.originX = (dw - w) / 2.0;
            vp.originY = (dh - h) / 2.0;
            vp.width = w;
            vp.height = h;
            vp.znear = 0.0;
            vp.zfar = 1.0;
            memset(&u, 0, sizeof(u));
            u.rect[2] = u.rect[3] = 1.0f;
            u.kind[0] = PORT_COPY_RGB;
            [enc setViewport:vp];
            [enc setRenderPipelineState:sScreenPipeline];
            [enc setVertexBytes:&u length:sizeof(u) atIndex:0];
            [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
            [enc setFragmentTexture:tex atIndex:0];
            [enc setFragmentTexture:sEFBDepth atIndex:1];
            [enc setFragmentSamplerState:sLinearClamp atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
        }
        [enc endEncoding];
        [cb presentDrawable:drawable];
        [cb commit];
    }
}

int PortGfxWritePNG(PortGfxTexture* frame, const char* path) {
    @autoreleasepool {
        PortTex* t = Tex(frame);
        NSUInteger w, h, row;
        id<MTLBuffer> out;
        id<MTLCommandBuffer> cb;
        id<MTLBlitCommandEncoder> blit;
        CGColorSpaceRef cs;
        CGDataProviderRef dp;
        CGImageRef img;
        CFURLRef url;
        CGImageDestinationRef dst;
        int ok;

        id<MTLTexture> src;
        if (t == nil) {
            return -1;
        }
        Commit();
        src = t->tex;
        h = src.height;
        w = sDisplayAspect > 0.0f ? (NSUInteger)lroundf((float)h * sDisplayAspect) : src.width;
        row = w * 4;
        out = [sDevice newBufferWithLength:row * h options:MTLResourceStorageModeShared];
        cb = [sQueue commandBuffer];
        if (w != src.width) {
            /* Stretch to the display's shape, as the TV would. */
            MTLTextureDescriptor* td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                                                                          width:w
                                                                                         height:h
                                                                                      mipmapped:NO];
            MTLRenderPassDescriptor* rp = [MTLRenderPassDescriptor renderPassDescriptor];
            id<MTLRenderCommandEncoder> enc;
            CopyUniforms u;
            memset(&u, 0, sizeof(u));
            u.rect[2] = u.rect[3] = 1.0f;
            u.kind[0] = PORT_COPY_RGB;
            td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
            td.storageMode = MTLStorageModePrivate;
            src = [sDevice newTextureWithDescriptor:td];
            rp.colorAttachments[0].texture = src;
            rp.colorAttachments[0].loadAction = MTLLoadActionDontCare;
            rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            enc = [cb renderCommandEncoderWithDescriptor:rp];
            [enc setRenderPipelineState:sCopyPipeline];
            [enc setVertexBytes:&u length:sizeof(u) atIndex:0];
            [enc setFragmentBytes:&u length:sizeof(u) atIndex:0];
            [enc setFragmentTexture:t->tex atIndex:0];
            [enc setFragmentTexture:sEFBDepth atIndex:1];
            [enc setFragmentSamplerState:sLinearClamp atIndex:0];
            [enc drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            [enc endEncoding];
        }
        blit = [cb blitCommandEncoder];
        [blit copyFromTexture:src
                         sourceSlice:0
                         sourceLevel:0
                        sourceOrigin:MTLOriginMake(0, 0, 0)
                          sourceSize:MTLSizeMake(w, h, 1)
                            toBuffer:out
                   destinationOffset:0
              destinationBytesPerRow:row
            destinationBytesPerImage:row * h];
        [blit endEncoding];
        [cb commit];
        [cb waitUntilCompleted];

        cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
        dp = CGDataProviderCreateWithData(NULL, out.contents, row * h, NULL);
        img = CGImageCreate(w, h, 8, 32, row, cs, kCGImageAlphaNoneSkipLast | kCGBitmapByteOrderDefault, dp, NULL, false,
                            kCGRenderingIntentDefault);
        url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8*)path, (CFIndex)strlen(path), false);
        dst = CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL);
        ok = 0;
        if (dst != NULL) {
            CGImageDestinationAddImage(dst, img, NULL);
            ok = CGImageDestinationFinalize(dst);
            CFRelease(dst);
        }
        CFRelease(url);
        CGImageRelease(img);
        CGDataProviderRelease(dp);
        CGColorSpaceRelease(cs);
        return ok ? 0 : -1;
    }
}

void PortGfxSetDisplayAspect(float aspect) {
    sDisplayAspect = aspect;
}

float PortGfxDisplayAspect(void) {
    return sDisplayAspect;
}

id<MTLDevice> PortGfxDevice(void) {
    return sDevice;
}
