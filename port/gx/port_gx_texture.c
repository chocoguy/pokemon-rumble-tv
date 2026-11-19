/* Textures: decode from the GX tiled formats to RGBA8, and the cache that keeps the
 * decoded copies on the GPU.
 *
 * A texture is identified by its address and its TX_SETIMAGE0 word (size and format),
 * plus the palette for the colour-index formats. Its content is hashed when it is first
 * used after an invalidation (GXInvalidateTexAll, a new frame): if the hash changed, the
 * game rewrote it and it is decoded again. EFB copies are entries of their own that hold
 * the GPU's copy; they stand until the CPU writes the memory under them. */
#include "port_gx_internal.h"
#include <port_addr.h>
#include <port_endian.h>
#include <port_os.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

uint8_t gPortTMEM[0x80000];

/* ------------------------------------------------------------------------------------ */
/* Decode                                                                                */

static void BlockSize(int format, int* bw, int* bh, int* bytes) {
    *bytes = 32;
    switch (format) {
    case 0:  /* I4 */
    case 8:  /* C4 */
    case 14: /* CMPR */
        *bw = 8, *bh = 8;
        break;
    case 1: /* I8 */
    case 2: /* IA4 */
    case 9: /* C8 */
        *bw = 8, *bh = 4;
        break;
    case 6: /* RGBA8: two 32-byte halves */
        *bw = 4, *bh = 4, *bytes = 64;
        break;
    default: /* IA8, RGB565, RGB5A3, C14X2 */
        *bw = 4, *bh = 4;
        break;
    }
}

static int KnownFormat(int format) {
    return format <= 6 || format == 8 || format == 9 || format == 10 || format == 14;
}

uint32_t PortGXTextureSize(int format, int width, int height) {
    int bw, bh, bytes;
    BlockSize(format, &bw, &bh, &bytes);
    return (uint32_t)(((width + bw - 1) / bw) * ((height + bh - 1) / bh) * bytes);
}

static inline uint8_t Expand5(uint32_t v) {
    return (uint8_t)((v << 3) | (v >> 2));
}
static inline uint8_t Expand6(uint32_t v) {
    return (uint8_t)((v << 2) | (v >> 4));
}
static inline uint8_t Expand4(uint32_t v) {
    return (uint8_t)(v * 17);
}
static inline uint8_t Expand3(uint32_t v) {
    return (uint8_t)((v << 5) | (v << 2) | (v >> 1));
}

static inline void Put(uint8_t* p, uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    p[0] = r, p[1] = g, p[2] = b, p[3] = a;
}

static void Decode565(uint8_t* p, uint32_t v) {
    Put(p, Expand5((v >> 11) & 31), Expand6((v >> 5) & 63), Expand5(v & 31), 255);
}

static void Decode5A3(uint8_t* p, uint32_t v) {
    if (v & 0x8000) {
        Put(p, Expand5((v >> 10) & 31), Expand5((v >> 5) & 31), Expand5(v & 31), 255);
    } else {
        Put(p, Expand4((v >> 8) & 15), Expand4((v >> 4) & 15), Expand4(v & 15), Expand3((v >> 12) & 7));
    }
}

static void DecodeTlutEntry(uint8_t* p, const uint8_t* tlut, uint32_t index, int tlutFormat) {
    uint32_t v = PortBE16(tlut + 2 * index);
    switch (tlutFormat) {
    case 0: /* IA8: alpha in the high byte */
        Put(p, (uint8_t)v, (uint8_t)v, (uint8_t)v, (uint8_t)(v >> 8));
        break;
    case 1:
        Decode565(p, v);
        break;
    default:
        Decode5A3(p, v);
        break;
    }
}

/* DXT1 with the GPU's 3/8 and 5/8 blend. */
static void DecodeCMPRBlock(uint8_t* out, int width, int height, int x0, int y0, const uint8_t* src) {
    uint8_t colors[4][4];
    uint32_t c0 = PortBE16(src);
    uint32_t c1 = PortBE16(src + 2);
    int x, y, k;
    Decode565(colors[0], c0);
    Decode565(colors[1], c1);
    if (c0 > c1) {
        for (k = 0; k < 3; k++) {
            colors[2][k] = (uint8_t)((colors[0][k] * 5 + colors[1][k] * 3) >> 3);
            colors[3][k] = (uint8_t)((colors[0][k] * 3 + colors[1][k] * 5) >> 3);
        }
        colors[2][3] = colors[3][3] = 255;
    } else {
        for (k = 0; k < 3; k++) {
            colors[2][k] = (uint8_t)((colors[0][k] + colors[1][k]) >> 1);
            colors[3][k] = 0;
        }
        colors[2][3] = 255;
        colors[3][3] = 0;
    }
    for (y = 0; y < 4; y++) {
        uint8_t bits = src[4 + y];
        for (x = 0; x < 4; x++) {
            int px = x0 + x, py = y0 + y;
            int idx = (bits >> (6 - 2 * x)) & 3;
            if (px < width && py < height) {
                memcpy(out + 4 * ((size_t)py * width + px), colors[idx], 4);
            }
        }
    }
}

int PortGXDecodeTexture(uint8_t* out, const uint8_t* src, int format, int width, int height, const uint8_t* tlut,
                        int tlutFormat) {
    int bw, bh, bytes;
    int bx, by, x, y;
    if (!KnownFormat(format)) {
        return 0;
    }
    BlockSize(format, &bw, &bh, &bytes);
    for (by = 0; by < height; by += bh) {
        for (bx = 0; bx < width; bx += bw) {
            const uint8_t* b = src;
            src += bytes;
            if (format == 14) {
                DecodeCMPRBlock(out, width, height, bx, by, b);
                DecodeCMPRBlock(out, width, height, bx + 4, by, b + 8);
                DecodeCMPRBlock(out, width, height, bx, by + 4, b + 16);
                DecodeCMPRBlock(out, width, height, bx + 4, by + 4, b + 24);
                continue;
            }
            for (y = 0; y < bh; y++) {
                for (x = 0; x < bw; x++) {
                    int i = y * bw + x;
                    int px = bx + x, py = by + y;
                    uint8_t texel[4];
                    uint32_t v;
                    switch (format) {
                    case 0: /* I4 */
                        v = (b[i >> 1] >> ((i & 1) ? 0 : 4)) & 15;
                        v = Expand4(v);
                        Put(texel, (uint8_t)v, (uint8_t)v, (uint8_t)v, (uint8_t)v);
                        break;
                    case 1: /* I8 */
                        v = b[i];
                        Put(texel, (uint8_t)v, (uint8_t)v, (uint8_t)v, (uint8_t)v);
                        break;
                    case 2: /* IA4 */
                        v = b[i];
                        Put(texel, Expand4(v & 15), Expand4(v & 15), Expand4(v & 15), Expand4(v >> 4));
                        break;
                    case 3: /* IA8 */
                        Put(texel, b[2 * i + 1], b[2 * i + 1], b[2 * i + 1], b[2 * i]);
                        break;
                    case 4:
                        Decode565(texel, PortBE16(b + 2 * i));
                        break;
                    case 5:
                        Decode5A3(texel, PortBE16(b + 2 * i));
                        break;
                    case 6: /* RGBA8: AR pairs, then GB pairs */
                        Put(texel, b[2 * i + 1], b[32 + 2 * i], b[32 + 2 * i + 1], b[2 * i]);
                        break;
                    case 8: /* C4 */
                        DecodeTlutEntry(texel, tlut, (b[i >> 1] >> ((i & 1) ? 0 : 4)) & 15, tlutFormat);
                        break;
                    case 9: /* C8 */
                        DecodeTlutEntry(texel, tlut, b[i], tlutFormat);
                        break;
                    default: /* C14X2 */
                        DecodeTlutEntry(texel, tlut, PortBE16(b + 2 * i) & 0x3FFF, tlutFormat);
                        break;
                    }
                    if (px < width && py < height) {
                        memcpy(out + 4 * ((size_t)py * width + px), texel, 4);
                    }
                }
            }
        }
    }
    return 1;
}

/* ------------------------------------------------------------------------------------ */
/* Cache                                                                                 */

typedef struct TexEntry {
    struct TexEntry* next;
    uint32_t addr;
    uint32_t image0;
    uint32_t levels;
    uint32_t tlut;        /* TX_SETTLUT for palette formats, else 0 */
    uint32_t bytes;       /* memory the texture (or, for a copy, its destination) covers */
    uint64_t dataHash;
    uint64_t tlutHash;
    uint32_t epoch;
    uint32_t lastFrame;
    int isCopy;
    int copyWidth, copyHeight;
    PortGfxTexture* texture;
} TexEntry;

#define TEX_BUCKETS 4096
static TexEntry* sBuckets[TEX_BUCKETS];
static uint32_t sEpoch = 1;
static uint32_t sFrame;
static uint8_t* sScratch;
static size_t sScratchSize;
static unsigned long long sDecoded, sCopiesUsed;

static uint64_t Hash(const uint8_t* p, size_t n) {
    uint64_t h = 0x9E3779B97F4A7C15ull ^ n;
    size_t i = 0;
    for (; i + 8 <= n; i += 8) {
        uint64_t v;
        memcpy(&v, p + i, 8);
        h ^= v * 0xC2B2AE3D27D4EB4Full;
        h = ((h << 31) | (h >> 33)) * 0x9E3779B185EBCA87ull;
    }
    for (; i < n; i++) {
        h ^= p[i];
        h *= 0x100000001B3ull;
    }
    h ^= h >> 29;
    h *= 0xBF58476D1CE4E5B9ull;
    return h ^ (h >> 32);
}

static TexEntry** Bucket(uint32_t addr) {
    return &sBuckets[(addr >> 5) % TEX_BUCKETS];
}

static void FreeEntry(TexEntry* e) {
    if (e->texture != NULL) {
        PortGfxTextureRelease(e->texture);
    }
    free(e);
}

static void Unlink(TexEntry* e) {
    TexEntry** link = Bucket(e->addr);
    while (*link != NULL && *link != e) {
        link = &(*link)->next;
    }
    if (*link == e) {
        *link = e->next;
    }
}

void PortGXTextureInvalidate(void) {
    sEpoch++;
}

static int TlutBytes(int format) {
    return format == 8 ? 32 : format == 9 ? 512 : 0x8000;
}

static int IsPalette(int format) {
    return format == 8 || format == 9 || format == 10;
}

static int LevelCount(uint32_t mode0, uint32_t mode1, int width, int height) {
    int levels = 1;
    int maxLevels = 1;
    int size = width > height ? width : height;
    while (size > 1) {
        size >>= 1;
        maxLevels++;
    }
    if (((mode0 >> 5) & 3) != 0) {
        levels = (int)((mode1 >> 8) & 0xFF) / 16 + 1;
    }
    return levels < maxLevels ? levels : maxLevels;
}

static uint32_t TotalBytes(int format, int width, int height, int levels) {
    uint32_t total = 0;
    int l;
    for (l = 0; l < levels; l++) {
        int w = width >> l, h = height >> l;
        total += PortGXTextureSize(format, w < 1 ? 1 : w, h < 1 ? 1 : h);
    }
    return total;
}

static PortGfxTexture* Decode(uint32_t addr, int format, int width, int height, int levels, const uint8_t* tlut,
                              int tlutFormat) {
    const uint8_t* src = (const uint8_t*)PortPtrRaw(addr);
    const uint8_t* level[16];
    size_t need = 0;
    size_t offset = 0;
    int l;
    for (l = 0; l < levels; l++) {
        int w = width >> l, h = height >> l;
        need += (size_t)(w < 1 ? 1 : w) * (h < 1 ? 1 : h) * 4;
    }
    if (need > sScratchSize) {
        free(sScratch);
        sScratch = (uint8_t*)malloc(need);
        sScratchSize = need;
    }
    for (l = 0; l < levels; l++) {
        int w = width >> l, h = height >> l;
        w = w < 1 ? 1 : w;
        h = h < 1 ? 1 : h;
        PortGXDecodeTexture(sScratch + offset, src, format, w, h, tlut, tlutFormat);
        level[l] = sScratch + offset;
        offset += (size_t)w * h * 4;
        src += PortGXTextureSize(format, w, h);
    }
    sDecoded++;
    return PortGfxTextureCreate(width, height, levels, level);
}

PortGfxTexture* PortGXTexture(int map) {
    int hi = map >= 4;
    int n = map & 3;
    uint32_t mode0 = gPortGX.bp[(hi ? 0xA0 : 0x80) + n];
    uint32_t mode1 = gPortGX.bp[(hi ? 0xA4 : 0x84) + n];
    uint32_t image0 = gPortGX.bp[(hi ? 0xA8 : 0x88) + n] & 0xFFFFFF;
    uint32_t image3 = gPortGX.bp[(hi ? 0xB4 : 0x94) + n];
    uint32_t tlutReg = gPortGX.bp[(hi ? 0xB8 : 0x98) + n] & 0xFFFFFF;
    uint32_t addr = (image3 & 0xFFFFFF) << 5;
    int width = (int)(image0 & 0x3FF) + 1;
    int height = (int)((image0 >> 10) & 0x3FF) + 1;
    int format = (int)((image0 >> 20) & 15);
    int levels;
    uint32_t bytes;
    const uint8_t* tlut = NULL;
    uint64_t tlutHash = 0;
    TexEntry* e;
    static int badFormat, badAddr;

    if (!KnownFormat(format)) {
        PortRedOnce(&badFormat, "GX: texture format %d", format);
        return NULL;
    }
    levels = LevelCount(mode0, mode1, width, height);
    bytes = TotalBytes(format, width, height, levels);
    if (addr == 0 || addr + bytes > PORT_ADDR_WINDOW_SIZE) {
        PortRedOnce(&badAddr, "GX: texture at 0x%08x (%dx%d format %d) is outside memory", addr, width, height, format);
        return NULL;
    }
    if (!IsPalette(format)) {
        tlutReg = 0;
    }

    for (e = *Bucket(addr); e != NULL; e = e->next) {
        if (e->addr != addr) {
            continue;
        }
        if (e->isCopy) {
            break;
        }
        if (e->image0 == image0 && e->levels == (uint32_t)levels && e->tlut == tlutReg) {
            break;
        }
    }

    if (tlutReg != 0 || IsPalette(format)) {
        tlut = gPortTMEM + ((tlutReg & 0x3FF) << 9);
        if ((size_t)((tlutReg & 0x3FF) << 9) + TlutBytes(format) > sizeof(gPortTMEM)) {
            tlut = gPortTMEM;
        }
    }

    if (e != NULL && e->isCopy) {
        if (e->epoch != sEpoch) {
            uint64_t h = Hash((const uint8_t*)PortPtrRaw(addr), e->bytes);
            e->epoch = sEpoch;
            if (h != e->dataHash) {
                /* The CPU wrote over the copy: it is an ordinary texture now. */
                Unlink(e);
                FreeEntry(e);
                e = NULL;
                return PortGXTexture(map);
            }
        }
        e->lastFrame = sFrame;
        sCopiesUsed++;
        return e->texture;
    }

    if (e != NULL) {
        if (e->epoch != sEpoch) {
            uint64_t h = Hash((const uint8_t*)PortPtrRaw(addr), bytes);
            uint64_t th = tlut != NULL ? Hash(tlut, (size_t)TlutBytes(format)) : 0;
            e->epoch = sEpoch;
            if (h != e->dataHash || th != e->tlutHash) {
                PortGfxTextureRelease(e->texture);
                e->texture = Decode(addr, format, width, height, levels, tlut, (int)((tlutReg >> 10) & 3));
                e->dataHash = h;
                e->tlutHash = th;
            }
        }
        e->lastFrame = sFrame;
        return e->texture;
    }

    if (tlut != NULL) {
        tlutHash = Hash(tlut, (size_t)TlutBytes(format));
    }
    e = (TexEntry*)calloc(1, sizeof(TexEntry));
    e->addr = addr;
    e->image0 = image0;
    e->levels = (uint32_t)levels;
    e->tlut = tlutReg;
    e->bytes = bytes;
    e->dataHash = Hash((const uint8_t*)PortPtrRaw(addr), bytes);
    e->tlutHash = tlutHash;
    e->epoch = sEpoch;
    e->lastFrame = sFrame;
    e->texture = Decode(addr, format, width, height, levels, tlut, (int)((tlutReg >> 10) & 3));
    e->next = *Bucket(addr);
    *Bucket(addr) = e;
    return e->texture;
}

void PortGXTextureCopied(uint32_t addr, PortGfxTexture* texture, int width, int height) {
    TexEntry** link = Bucket(addr);
    TexEntry* e;
    /* Anything at this address is superseded. */
    while (*link != NULL) {
        e = *link;
        if (e->addr == addr) {
            *link = e->next;
            FreeEntry(e);
        } else {
            link = &e->next;
        }
    }
    e = (TexEntry*)calloc(1, sizeof(TexEntry));
    e->addr = addr;
    e->isCopy = 1;
    e->copyWidth = width;
    e->copyHeight = height;
    e->bytes = (uint32_t)(width * height * 4) < 0x10000 ? (uint32_t)(width * height * 4) : 0x10000;
    if (addr + e->bytes > PORT_ADDR_WINDOW_SIZE) {
        e->bytes = 0;
    }
    e->dataHash = Hash((const uint8_t*)PortPtrRaw(addr), e->bytes);
    e->epoch = sEpoch;
    e->lastFrame = sFrame;
    e->texture = texture;
    e->next = *Bucket(addr);
    *Bucket(addr) = e;
}

/* ------------------------------------------------------------------------------------ */
/* Display copies                                                                        */

#define XFB_SLOTS 4
static struct {
    uint32_t addr;
    PortGfxTexture* texture;
} sXFB[XFB_SLOTS];
static int sXFBNext;

void PortGXFrameBufferCopied(uint32_t addr, PortGfxTexture* texture) {
    int i;
    for (i = 0; i < XFB_SLOTS; i++) {
        if (sXFB[i].texture != NULL && sXFB[i].addr == addr) {
            PortGfxTextureRelease(sXFB[i].texture);
            sXFB[i].texture = texture;
            return;
        }
    }
    i = sXFBNext;
    sXFBNext = (sXFBNext + 1) % XFB_SLOTS;
    if (sXFB[i].texture != NULL) {
        PortGfxTextureRelease(sXFB[i].texture);
    }
    sXFB[i].addr = addr;
    sXFB[i].texture = texture;
}

PortGfxTexture* PortGXFrameBufferTexture(uint32_t addr) {
    int i;
    for (i = 0; i < XFB_SLOTS; i++) {
        if (sXFB[i].texture != NULL && sXFB[i].addr == addr) {
            return sXFB[i].texture;
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------------------------ */

static void PrintSummary(void) {
    fprintf(stderr, "[port] GX textures: %llu decoded, %llu uses of EFB copies\n", sDecoded, sCopiesUsed);
}

void PortGXTextureFrame(void) {
    int b;
    static int summary;
    if (!summary) {
        summary = 1;
        PortAddSummary(PrintSummary);
    }
    sFrame++;
    sEpoch++;
    if (sFrame % 60 != 0) {
        return;
    }
    for (b = 0; b < TEX_BUCKETS; b++) {
        TexEntry** link = &sBuckets[b];
        while (*link != NULL) {
            TexEntry* e = *link;
            if (sFrame - e->lastFrame > 600) {
                *link = e->next;
                FreeEntry(e);
            } else {
                link = &e->next;
            }
        }
    }
}
