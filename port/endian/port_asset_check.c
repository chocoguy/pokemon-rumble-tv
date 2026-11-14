/* rumble --check-assets: runs every file in the content archives through its byte-order
 * fixup, outside the game.
 *
 * It walks each U8 content file, decompresses .cx files (the SDK's CX library, as the
 * game's loader does) and descends into nested archives. Every file of a converted
 * format goes through its fixup twice: the first pass must succeed (each fixup checks
 * its offsets against the file and reports what it cannot place), and the second must
 * recognise the file as converted and leave it byte for byte unchanged. Formats the port
 * does not convert yet are counted by extension.
 *
 * Exit status: 0 when every converted format passed. */
#include <port_endian.h>
#include <port_os.h>
#include <revolution/cx.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef int (*Fixup)(void* file);

typedef struct Format {
    const char* extension;
    Fixup fixup;
    unsigned int passed;
    unsigned int failed;
} Format;

static Format sFormats[] = {
    {".brlyt", PortFixupBRLYT, 0, 0}, {".brlan", PortFixupBRLAN, 0, 0}, {".tpl", PortFixupTPL, 0, 0},
    {".brfnt", PortFixupBRFNT, 0, 0}, {".bmg", PortFixupBMG, 0, 0},     {".breff", PortFixupBREFF, 0, 0},
    {".breft", PortFixupBREFT, 0, 0}, {".brsar", PortFixupBRSAR, 0, 0},
};

typedef struct Other {
    char extension[16];
    unsigned int count;
} Other;

static Other sOthers[64];
static int sOtherCount;
static unsigned int sArchives;
static unsigned int sDecompressed;
static unsigned int sDecompressFailures;

static const char* Extension(const char* name) {
    const char* dot = strrchr(name, '.');
    return dot != NULL ? dot : "(none)";
}

static void CountOther(const char* extension) {
    int i;
    for (i = 0; i < sOtherCount; i++) {
        if (strcmp(sOthers[i].extension, extension) == 0) {
            sOthers[i].count++;
            return;
        }
    }
    if (sOtherCount < (int)(sizeof(sOthers) / sizeof(sOthers[0]))) {
        snprintf(sOthers[sOtherCount].extension, sizeof(sOthers[sOtherCount].extension), "%s", extension);
        sOthers[sOtherCount].count = 1;
        sOtherCount++;
    }
}

static void CheckFile(const char* path, uint8_t* data, uint32_t size);

static void CheckArchive(const char* path, uint8_t* data, uint32_t size) {
    uint32_t fstStart;
    uint32_t count;
    const uint8_t* names;
    char dirs[16][256];
    uint32_t ends[16];
    int depth = 0;
    uint32_t i;

    if (!PortFixupU8(data, size)) {
        PortRed("check-assets: %s: bad U8 archive", path);
        return;
    }
    sArchives++;
    memcpy(&fstStart, data + 4, 4);
    memcpy(&count, data + fstStart + 8, 4);
    names = data + fstStart + count * 12;
    snprintf(dirs[0], sizeof(dirs[0]), "%s", path);
    ends[0] = count;

    for (i = 1; i < count; i++) {
        const uint8_t* entry = data + fstStart + i * 12;
        uint32_t word0, position, length;
        char full[512];
        while (depth > 0 && i >= ends[depth]) {
            depth--;
        }
        memcpy(&word0, entry, 4);
        memcpy(&position, entry + 4, 4);
        memcpy(&length, entry + 8, 4);
        snprintf(full, sizeof(full), "%s/%s", dirs[depth], (const char*)names + (word0 & 0xFFFFFF));
        if (word0 >> 24) {
            if (depth + 1 < 16) {
                depth++;
                snprintf(dirs[depth], sizeof(dirs[depth]), "%s", full);
                ends[depth] = length;
            }
        } else if ((uint64_t)position + length <= size) {
            CheckFile(full, data + position, length);
        } else {
            PortRed("check-assets: %s lies outside its archive", full);
        }
    }
}

static int Decompress(const char* path, const uint8_t* data, uint32_t size, uint8_t** out, uint32_t* outSize) {
    static uint8_t work[0x9000]; /* the game's work area size (CFileDataWorker.h) */
    uint32_t unpacked = CXGetUncompressedSize(data);
    uint8_t* buffer = (uint8_t*)malloc(unpacked + 32);
    int result = -1;
    switch (data[0] & 0xF0) {
    case 0x10:
        result = CXSecureUncompressLZ(data, size, buffer);
        break;
    case 0x20:
        result = CXSecureUncompressHuffman(data, size, buffer);
        break;
    case 0x30:
        result = CXSecureUncompressRL(data, size, buffer);
        break;
    case 0x40:
        memset(work, 0, sizeof(work));
        result = CXSecureUncompressLH(data, size, buffer, work);
        break;
    case 0x50:
        memset(work, 0, sizeof(work));
        result = CXSecureUncompressLRC(data, size, buffer, work);
        break;
    case 0x80:
        result = CXSecureUnfilterDiff(data, size, buffer);
        break;
    }
    if (result != 0) {
        PortRed("check-assets: %s: CX type 0x%02x failed to decompress (%d)", path, data[0], result);
        free(buffer);
        sDecompressFailures++;
        return 0;
    }
    sDecompressed++;
    *out = buffer;
    *outSize = unpacked;
    return 1;
}

static void CheckFile(const char* path, uint8_t* data, uint32_t size) {
    const char* extension = Extension(path);
    size_t i;

    if (strcmp(extension, ".cx") == 0) {
        char inner[512];
        uint8_t* unpacked;
        uint32_t unpackedSize;
        if (Decompress(path, data, size, &unpacked, &unpackedSize)) {
            snprintf(inner, sizeof(inner), "%.*s", (int)(strlen(path) - 3), path);
            CheckFile(inner, unpacked, unpackedSize);
            free(unpacked);
        }
        return;
    }
    {
        /* The Home Button Menu's files name their compression instead: LZ77_x.arc. */
        static const char* const kPrefixes[] = {"LZ77_", "Huf8_", "Huf4_", "RL_"};
        const char* base = strrchr(path, '/');
        base = base != NULL ? base + 1 : path;
        for (i = 0; i < sizeof(kPrefixes) / sizeof(kPrefixes[0]); i++) {
            size_t n = strlen(kPrefixes[i]);
            if (strncmp(base, kPrefixes[i], n) == 0) {
                char inner[512];
                uint8_t* unpacked;
                uint32_t unpackedSize;
                if (Decompress(path, data, size, &unpacked, &unpackedSize)) {
                    snprintf(inner, sizeof(inner), "%.*s%s", (int)(base - path), path, base + n);
                    CheckFile(inner, unpacked, unpackedSize);
                    free(unpacked);
                }
                return;
            }
        }
    }
    if (size >= 4 && PortBE32(data) == 0x55AA382Du) {
        CheckArchive(path, data, size);
        return;
    }
    for (i = 0; i < sizeof(sFormats) / sizeof(sFormats[0]); i++) {
        if (strcmp(extension, sFormats[i].extension) == 0) {
            uint8_t* copy;
            int ok = sFormats[i].fixup(data) == 1;
            /* A second pass must see a converted file and change nothing. */
            copy = (uint8_t*)malloc(size);
            memcpy(copy, data, size);
            if (ok && (sFormats[i].fixup(data) != 1 || memcmp(copy, data, size) != 0)) {
                PortRed("check-assets: %s: a second fixup pass changed the file", path);
                ok = 0;
            }
            free(copy);
            if (ok) {
                sFormats[i].passed++;
            } else {
                sFormats[i].failed++;
                PortRed("check-assets: %s failed its fixup", path);
            }
            return;
        }
    }
    CountOther(extension);
}

int PortCheckAssets(const char* romDir) {
    unsigned int failed = 0;
    int content;
    size_t i;

    for (content = 0; content < 16; content++) {
        char path[1024];
        FILE* file;
        long length;
        uint8_t* data;

        snprintf(path, sizeof(path), "%s/%08x.app", romDir, content);
        file = fopen(path, "rb");
        if (file == NULL) {
            continue;
        }
        fseek(file, 0, SEEK_END);
        length = ftell(file);
        fseek(file, 0, SEEK_SET);
        data = (uint8_t*)malloc((size_t)length);
        if (fread(data, 1, (size_t)length, file) != (size_t)length) {
            PortRed("check-assets: short read of %s", path);
        }
        fclose(file);
        if (length >= 4 && PortBE32(data) == 0x55AA382Du) {
            char name[32];
            snprintf(name, sizeof(name), "%d:", content);
            CheckArchive(name, data, (uint32_t)length);
        }
        free(data);
    }

    printf("check-assets: %u archives, %u files decompressed (%u failed)\n", sArchives, sDecompressed,
           sDecompressFailures);
    for (i = 0; i < sizeof(sFormats) / sizeof(sFormats[0]); i++) {
        printf("  %-7s %5u converted, %u failed\n", sFormats[i].extension, sFormats[i].passed, sFormats[i].failed);
        failed += sFormats[i].failed;
    }
    printf("  not converted yet:");
    for (i = 0; i < (size_t)sOtherCount; i++) {
        printf(" %s %u%s", sOthers[i].extension, sOthers[i].count, i + 1 < (size_t)sOtherCount ? "," : "");
    }
    printf("\n");
    return failed == 0 && sDecompressFailures == 0 ? 0 : 1;
}
