/* BMG message files (MESGbmg1), read by the game's CMsgRes.
 *
 * Header: signature[8], fileSize u32, blockNum u32, encoding u8, padding. Blocks: kind
 * char[4] (compared as text, so left as stored), size u32. INF1: entryNum, entrySize,
 * groupID u16, then entries of entrySize bytes whose first words are offset and
 * attribute (u32). DAT1: the strings, UTF-16 when the encoding says so. STR1: chars.
 *
 * The block count tells the byte order: a file has a handful of blocks, so a count that
 * is small read natively means the file is already native (the system messages that
 * tools/import_wad.py generates are written that way). */
#include <port_endian.h>
#include <port_os.h>
#include <string.h>

#define BMG_ENCODING_UTF16 2

int PortFixupBMG(void* file) {
    uint8_t* base = (uint8_t*)file;
    uint32_t nativeBlocks;
    uint32_t fileSize;
    uint32_t blocks;
    uint32_t offset;
    uint32_t i;
    int utf16;

    if (memcmp(base, "MESGbmg1", 8) != 0) {
        return 0;
    }
    memcpy(&nativeBlocks, base + 12, 4);
    if (nativeBlocks <= 64) {
        return 1;
    }
    fileSize = PortBE32(base + 8);
    blocks = PortBE32(base + 12);
    if (blocks > 64 || fileSize < 32) {
        PortRed("BMG at %p: %u blocks in %u bytes", file, blocks, fileSize);
        return 0;
    }
    utf16 = base[16] == BMG_ENCODING_UTF16;
    PortSwap32(base + 8);
    PortSwap32(base + 12);

    offset = 32;
    for (i = 0; i < blocks; i++) {
        uint8_t* block = base + offset;
        uint32_t size;
        if (offset + 8 > fileSize) {
            PortRed("BMG at %p: block %u at 0x%x is past the end (0x%x)", file, i, offset, fileSize);
            return 0;
        }
        PortSwap32(block + 4);
        memcpy(&size, block + 4, 4);
        if (size < 8 || offset + size > fileSize) {
            PortRed("BMG at %p: block %.4s has size 0x%x", file, (const char*)block, size);
            return 0;
        }
        if (memcmp(block, "INF1", 4) == 0) {
            uint16_t count;
            uint16_t entrySize;
            uint16_t e;
            PortSwap16(block + 8);
            PortSwap16(block + 10);
            PortSwap16(block + 12);
            memcpy(&count, block + 8, 2);
            memcpy(&entrySize, block + 10, 2);
            if (16 + (uint32_t)count * entrySize > size || entrySize < 4) {
                PortRed("BMG at %p: %u entries of %u bytes in a 0x%x-byte INF1", file, count, entrySize, size);
                return 0;
            }
            for (e = 0; e < count; e++) {
                PortSwap32Array(block + 16 + (size_t)e * entrySize, entrySize >= 8 ? 2 : 1);
            }
        } else if (memcmp(block, "DAT1", 4) == 0 && utf16) {
            uint32_t k;
            for (k = 8; k + 2 <= size; k += 2) {
                PortSwap16(block + k);
            }
        }
        offset += size;
    }
    return 1;
}
