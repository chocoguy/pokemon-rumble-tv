/* Byte-order fixups for the NW4R layout family: .brlyt (RLYT), .brlan (RLAN), .tpl and
 * .brfnt (RFNT). Written against the decomp's structs (nw4r/lyt/lyt_resources.h,
 * lyt_materialTypes.h, nw4r/ut/ut_ResFontBase.h, revolution/tpl.h).
 *
 * Every multi-byte field is swapped to native order in place; byte fields, strings and
 * texture/palette data are left alone (the GX texture decoder reads them big-endian).
 * Each file's signature tells whether it has been converted: its first word reads as the
 * signature big-endian before the fixup and natively after it.
 *
 * The NW4R byte-order mark is left as stored: bytes FE FF read natively on a
 * little-endian host as 0xFFFE, which is NW4R_BYTEORDER_NATIVE on the port.
 *
 * A fixup that meets something out of bounds reports it with [RED] and stops, leaving
 * the file unconverted so the game's own checks reject it. */
#include <port_endian.h>
#include <port_os.h>
#include <stdio.h>
#include <string.h>

#define FOURCC(a, b, c, d) ((uint32_t)(a) << 24 | (uint32_t)(b) << 16 | (uint32_t)(c) << 8 | (uint32_t)(d))

typedef struct Span {
    uint8_t* base;
    size_t size;
    const char* what;
    int ok;
} Span;

static int Fail(Span* s, const char* fmt, uint32_t a, uint32_t b) {
    if (s->ok) {
        char msg[160];
        snprintf(msg, sizeof(msg), fmt, a, b);
        PortRed("%s fixup at %p: %s", s->what, (void*)s->base, msg);
    }
    s->ok = 0;
    return 0;
}

/* Whether [offset, offset + length) lies in the file. */
static int In(Span* s, size_t offset, size_t length) {
    if (offset > s->size || length > s->size - offset) {
        return Fail(s, "0x%x (+0x%x) is outside the file", (uint32_t)offset, (uint32_t)length);
    }
    return 1;
}

static void Sw16(Span* s, size_t offset, size_t count) {
    size_t i;
    if (s->ok && In(s, offset, count * 2)) {
        for (i = 0; i < count; i++) {
            PortSwap16(s->base + offset + i * 2);
        }
    }
}

static void Sw32(Span* s, size_t offset, size_t count) {
    if (s->ok && In(s, offset, count * 4)) {
        PortSwap32Array(s->base + offset, count);
    }
}

static uint16_t N16(Span* s, size_t offset) {
    uint16_t v = 0;
    if (In(s, offset, 2)) {
        memcpy(&v, s->base + offset, 2);
    }
    return v;
}

static uint32_t N32(Span* s, size_t offset) {
    uint32_t v = 0;
    if (In(s, offset, 4)) {
        memcpy(&v, s->base + offset, 4);
    }
    return v;
}

/* 0: not this format; 1: already native; 2: big-endian, to convert. */
static int State(const void* file, uint32_t signature) {
    uint32_t native;
    memcpy(&native, file, 4);
    if (native == signature) {
        return 1;
    }
    return PortBE32(file) == signature ? 2 : 0;
}

/* NW4R binary file header: signature, BOM (left alone), version, fileSize, headerSize,
 * dataBlocks. Sets s->size from the file and returns the first block's offset. */
static size_t FileHeader(Span* s) {
    size_t headerSize;
    PortSwap32(s->base);
    PortSwap16(s->base + 6);
    PortSwap32(s->base + 8);
    PortSwap16(s->base + 12);
    PortSwap16(s->base + 14);
    s->size = N32(s, 8) ? N32(s, 8) : s->size;
    headerSize = N16(s, 12);
    return headerSize;
}

/* ------------------------------------------------------------------------------------ */
/* .brlyt                                                                                */

/* res::Pane after its block header: flag, basePosition, alpha, padding, name[16],
 * userData[8], then translate, rotate (VEC3 each), scale (VEC2), size (2 floats). */
#define PANE_SIZE 0x4C
static void Pane(Span* s, size_t b) {
    Sw32(s, b + 0x24, 10);
}

static void TexCoords(Span* s, size_t offset, unsigned count) {
    Sw32(s, offset, (size_t)count * 8); /* 4 corners, VEC2 each */
}

/* res::Material; returns nothing, swaps every multi-byte field it contains. */
static void Material(Span* s, size_t m) {
    uint32_t bits;
    unsigned texMap, texSRT, texCoordGen, indSRT, indStage, tevStage;
    size_t p;

    Sw16(s, m + 20, 12); /* tevCols: 3 GXColorS10 */
    Sw32(s, m + 0x3C, 1); /* resNum (after name[20], tevCols, tevKCols) */
    bits = N32(s, m + 0x3C);
    texMap = bits & 0xF;
    texSRT = (bits >> 4) & 0xF;
    texCoordGen = (bits >> 8) & 0xF;
    indSRT = (bits >> 13) & 0x3;
    indStage = (bits >> 15) & 0x7;
    tevStage = (bits >> 18) & 0x1F;

    p = m + 0x40; /* sizeof(res::Material) */
    {
        unsigned i;
        for (i = 0; i < texMap; i++) {
            Sw16(s, p + i * 4, 1); /* TexMap.texIdx */
        }
    }
    p += texMap * 4;
    Sw32(s, p, texSRT * 5);
    p += texSRT * 20;
    p += texCoordGen * 4;
    p += ((bits >> 25) & 1) * 4; /* ChanCtrl */
    p += ((bits >> 27) & 1) * 4; /* material colour */
    p += ((bits >> 12) & 1) * 16; /* TevSwapMode x4 */
    Sw32(s, p, indSRT * 5);
    p += indSRT * 20;
    p += indStage * 4;
    p += tevStage * 16;
    /* AlphaCompare and BlendMode are bytes. */
    (void)p;
}

static void LayoutBlock(Span* s, size_t b, uint32_t kind, uint32_t size) {
    switch (kind) {
    case FOURCC('l', 'y', 't', '1'):
        Sw32(s, b + 12, 2); /* layoutSize */
        break;
    case FOURCC('t', 'x', 'l', '1'):
    case FOURCC('f', 'n', 'l', '1'): {
        uint16_t n;
        uint16_t i;
        Sw16(s, b + 8, 1);
        n = N16(s, b + 8);
        for (i = 0; i < n; i++) {
            Sw32(s, b + 12 + (size_t)i * 8, 1); /* nameStrOffset */
        }
        break;
    }
    case FOURCC('m', 'a', 't', '1'): {
        uint16_t n;
        uint16_t i;
        Sw16(s, b + 8, 1);
        n = N16(s, b + 8);
        Sw32(s, b + 12, n);
        for (i = 0; i < n && s->ok; i++) {
            uint32_t offset = N32(s, b + 12 + (size_t)i * 4);
            if (offset >= size) {
                Fail(s, "material offset 0x%x is outside its 0x%x-byte block", offset, size);
                break;
            }
            Material(s, b + offset);
        }
        break;
    }
    case FOURCC('p', 'a', 'n', '1'):
    case FOURCC('b', 'n', 'd', '1'):
        Pane(s, b);
        break;
    case FOURCC('p', 'i', 'c', '1'): {
        uint8_t texCoordNum;
        Pane(s, b);
        Sw32(s, b + PANE_SIZE, 4); /* vtxCols */
        Sw16(s, b + PANE_SIZE + 16, 1); /* materialIdx */
        texCoordNum = In(s, b + PANE_SIZE + 18, 1) ? s->base[b + PANE_SIZE + 18] : 0;
        TexCoords(s, b + PANE_SIZE + 20, texCoordNum);
        break;
    }
    case FOURCC('t', 'x', 't', '1'): {
        uint16_t strBytes;
        uint32_t strOffset;
        Pane(s, b);
        Sw16(s, b + PANE_SIZE, 4); /* textBufBytes, textStrBytes, materialIdx, fontIdx */
        Sw32(s, b + PANE_SIZE + 12, 1); /* textStrOffset */
        Sw32(s, b + PANE_SIZE + 16, 2); /* textCols */
        Sw32(s, b + PANE_SIZE + 24, 4); /* fontSize, charSpace, lineSpace */
        strBytes = N16(s, b + PANE_SIZE + 2);
        strOffset = N32(s, b + PANE_SIZE + 12);
        if (strBytes != 0) {
            Sw16(s, b + strOffset, strBytes / 2); /* UTF-16 text */
        }
        break;
    }
    case FOURCC('w', 'n', 'd', '1'): {
        uint8_t frameNum;
        uint32_t content;
        uint32_t frames;
        uint8_t texCoordNum;
        unsigned i;
        Pane(s, b);
        Sw32(s, b + PANE_SIZE, 4); /* inflation */
        frameNum = In(s, b + PANE_SIZE + 16, 1) ? s->base[b + PANE_SIZE + 16] : 0;
        Sw32(s, b + PANE_SIZE + 20, 2); /* contentOffset, frameOffsetTableOffset */
        content = N32(s, b + PANE_SIZE + 20);
        frames = N32(s, b + PANE_SIZE + 24);
        Sw32(s, b + content, 4); /* WindowContent.vtxCols */
        Sw16(s, b + content + 16, 1);
        texCoordNum = In(s, b + content + 18, 1) ? s->base[b + content + 18] : 0;
        TexCoords(s, b + content + 20, texCoordNum);
        Sw32(s, b + frames, frameNum);
        for (i = 0; i < frameNum && s->ok; i++) {
            Sw16(s, b + N32(s, b + frames + i * 4), 1); /* WindowFrame.materialIdx */
        }
        break;
    }
    case FOURCC('g', 'r', 'p', '1'):
        Sw16(s, b + 24, 1); /* paneNum; the pane names are chars */
        break;
    case FOURCC('u', 's', 'd', '1'): {
        uint16_t n;
        uint16_t i;
        Sw16(s, b + 8, 1);
        n = N16(s, b + 8);
        for (i = 0; i < n && s->ok; i++) {
            size_t e = b + 12 + (size_t)i * 12;
            uint32_t dataOffset;
            uint16_t dataNum;
            uint8_t type;
            Sw32(s, e, 2);
            Sw16(s, e + 8, 1);
            dataOffset = N32(s, e + 4);
            dataNum = N16(s, e + 8);
            type = In(s, e + 10, 1) ? s->base[e + 10] : 0;
            if (type == 1 || type == 2) { /* s32 or f32 values; 0 is a string */
                Sw32(s, e + dataOffset, dataNum);
            }
        }
        break;
    }
    case FOURCC('p', 'a', 's', '1'):
    case FOURCC('p', 'a', 'e', '1'):
    case FOURCC('g', 'r', 's', '1'):
    case FOURCC('g', 'r', 'e', '1'):
        break;
    default:
        Fail(s, "unknown block 0x%08x (size 0x%x)", kind, size);
        break;
    }
}

/* Walks the blocks after the file header, swapping each block header and calling
 * `body` with the block's offset. */
static void Blocks(Span* s, size_t first, void (*body)(Span*, size_t, uint32_t, uint32_t)) {
    uint16_t count = N16(s, 14);
    size_t b = first;
    uint16_t i;
    for (i = 0; i < count && s->ok; i++) {
        uint32_t kind;
        uint32_t size;
        if (!In(s, b, 8)) {
            break;
        }
        Sw32(s, b, 2);
        kind = N32(s, b);
        size = N32(s, b + 4);
        if (size < 8 || !In(s, b, size)) {
            Fail(s, "block 0x%08x has size 0x%x", kind, size);
            break;
        }
        body(s, b, kind, size);
        b += size;
    }
}

int PortFixupBRLYT(void* file) {
    Span s = {(uint8_t*)file, 0x7FFFFFFF, "brlyt", 1};
    int state = State(file, FOURCC('R', 'L', 'Y', 'T'));
    if (state != 2) {
        return state;
    }
    Blocks(&s, FileHeader(&s), LayoutBlock);
    return s.ok;
}

/* ------------------------------------------------------------------------------------ */
/* .brlan                                                                                */

static void AnimTargets(Span* s, size_t info, uint32_t kind) {
    uint8_t num = In(s, info + 4, 1) ? s->base[info + 4] : 0;
    int step = kind == FOURCC('R', 'L', 'V', 'I') || kind == FOURCC('R', 'L', 'T', 'P');
    uint8_t i;
    Sw32(s, info + 8, num);
    for (i = 0; i < num && s->ok; i++) {
        size_t t = info + N32(s, info + 8 + (size_t)i * 4);
        uint16_t keyNum;
        uint32_t keys;
        uint16_t k;
        Sw16(s, t + 4, 1);
        Sw32(s, t + 8, 1);
        keyNum = N16(s, t + 4);
        keys = N32(s, t + 8);
        for (k = 0; k < keyNum && s->ok; k++) {
            if (step) {
                Sw32(s, t + keys + (size_t)k * 8, 1); /* frame */
                Sw16(s, t + keys + (size_t)k * 8 + 4, 1); /* value */
            } else {
                Sw32(s, t + keys + (size_t)k * 12, 3); /* frame, value, slope */
            }
        }
    }
}

static void AnimBlock(Span* s, size_t b, uint32_t kind, uint32_t size) {
    (void)size;
    switch (kind) {
    case FOURCC('p', 'a', 't', '1'):
        Sw16(s, b + 8, 2); /* tagOrder, groupNum */
        Sw32(s, b + 12, 2); /* nameOffset, groupsOffset */
        Sw16(s, b + 20, 2); /* startFrame, endFrame */
        break;
    case FOURCC('p', 'a', 'h', '1'):
        Sw32(s, b + 8, 1);
        Sw16(s, b + 12, 1);
        break;
    case FOURCC('p', 'a', 'i', '1'): {
        uint16_t fileNum;
        uint16_t contNum;
        uint32_t contOffsets;
        uint16_t i;
        Sw16(s, b + 8, 1); /* frameSize */
        Sw16(s, b + 12, 2); /* fileNum, animContNum */
        Sw32(s, b + 16, 1); /* animContOffsetsOffset */
        fileNum = N16(s, b + 12);
        contNum = N16(s, b + 14);
        contOffsets = N32(s, b + 16);
        Sw32(s, b + 20, fileNum); /* file name offsets */
        Sw32(s, b + contOffsets, contNum);
        for (i = 0; i < contNum && s->ok; i++) {
            size_t c = b + N32(s, b + contOffsets + (size_t)i * 4);
            uint8_t infoNum = In(s, c + 20, 1) ? s->base[c + 20] : 0;
            uint8_t j;
            Sw32(s, c + 24, infoNum);
            for (j = 0; j < infoNum && s->ok; j++) {
                size_t info = c + N32(s, c + 24 + (size_t)j * 4);
                Sw32(s, info, 1); /* kind */
                AnimTargets(s, info, N32(s, info));
            }
        }
        break;
    }
    default:
        Fail(s, "unknown block 0x%08x (size 0x%x)", kind, size);
        break;
    }
}

int PortFixupBRLAN(void* file) {
    Span s = {(uint8_t*)file, 0x7FFFFFFF, "brlan", 1};
    int state = State(file, FOURCC('R', 'L', 'A', 'N'));
    if (state != 2) {
        return state;
    }
    Blocks(&s, FileHeader(&s), AnimBlock);
    return s.ok;
}

/* ------------------------------------------------------------------------------------ */
/* .tpl                                                                                  */

#define TPL_VERSION 2142000u

/* Swaps the headers only; offsets stay offsets (TPLBind resolves them). Headers that
 * several descriptors share are converted once. */
int PortFixupTPL(void* file) {
    Span s = {(uint8_t*)file, 0x7FFFFFFF, "tpl", 1};
    uint32_t count;
    uint32_t table;
    uint32_t done[64];
    unsigned doneCount = 0;
    uint32_t i;
    int state = State(file, TPL_VERSION);

    if (state != 2) {
        return state;
    }
    Sw32(&s, 0, 3);
    count = N32(&s, 4);
    table = N32(&s, 8);
    if (count > 4096) {
        return Fail(&s, "%u descriptors at 0x%x", count, table);
    }
    Sw32(&s, table, count * 2);
    for (i = 0; i < count && s.ok; i++) {
        uint32_t header = N32(&s, table + i * 8);
        uint32_t clut = N32(&s, table + i * 8 + 4);
        unsigned k;
        int seenHeader = 0;
        int seenClut = 0;
        for (k = 0; k < doneCount; k++) {
            seenHeader |= done[k] == header;
            seenClut |= done[k] == clut;
        }
        if (header != 0 && !seenHeader) {
            Sw16(&s, header, 2); /* height, width */
            Sw32(&s, header + 4, 8); /* format, data, wrapS, wrapT, minFilter, magFilter, LODBias */
            if (doneCount < 64) {
                done[doneCount++] = header;
            }
        }
        if (clut != 0 && !seenClut) {
            Sw16(&s, clut, 1); /* numEntries */
            Sw32(&s, clut + 4, 2); /* format, data */
            if (doneCount < 64) {
                done[doneCount++] = clut;
            }
        }
    }
    return s.ok;
}

/* ------------------------------------------------------------------------------------ */
/* .brfnt                                                                                */

static void FontBlock(Span* s, size_t b, uint32_t kind, uint32_t size) {
    size_t p = b + 8;
    switch (kind) {
    case FOURCC('F', 'I', 'N', 'F'):
        Sw16(s, p + 2, 1); /* alterCharIndex */
        Sw32(s, p + 8, 3); /* pGlyph, pWidth, pMap */
        break;
    case FOURCC('T', 'G', 'L', 'P'):
        Sw32(s, p + 4, 1); /* sheetSize */
        Sw16(s, p + 8, 6); /* sheetNum .. sheetHeight */
        Sw32(s, p + 20, 1); /* sheetImage */
        break;
    case FOURCC('C', 'W', 'D', 'H'):
        Sw16(s, p, 2); /* indexBegin, indexEnd */
        Sw32(s, p + 4, 1); /* pNext; the widths are bytes */
        break;
    case FOURCC('C', 'M', 'A', 'P'): {
        uint16_t begin;
        uint16_t end;
        uint16_t method;
        Sw16(s, p, 4);
        Sw32(s, p + 8, 1);
        begin = N16(s, p);
        end = N16(s, p + 2);
        method = N16(s, p + 4);
        if (method == 0) { /* direct: the first glyph index */
            Sw16(s, p + 12, 1);
        } else if (method == 1) { /* table: one index per code */
            Sw16(s, p + 12, (size_t)(end - begin) + 1);
        } else if (method == 2) { /* scan: count, then (code, index) pairs */
            uint16_t n;
            Sw16(s, p + 12, 1);
            n = N16(s, p + 12);
            Sw16(s, p + 14, (size_t)n * 2);
        } else {
            Fail(s, "character map method %u in a 0x%x-byte block", method, size);
        }
        break;
    }
    case FOURCC('G', 'L', 'G', 'R'):
        /* Glyph groups belong to archive fonts (.brfna), which ArchiveFontBase reads
         * from a stream; they never come through here. */
        Fail(s, "glyph-group block (archive font) of 0x%x bytes at 0x%x", size, (uint32_t)b);
        break;
    default:
        Fail(s, "unknown block 0x%08x (size 0x%x)", kind, size);
        break;
    }
}

int PortFixupBRFNT(void* file) {
    Span s = {(uint8_t*)file, 0x7FFFFFFF, "brfnt", 1};
    int state = State(file, FOURCC('R', 'F', 'N', 'T'));
    if (state != 2) {
        return state;
    }
    Blocks(&s, FileHeader(&s), FontBlock);
    return s.ok;
}
