/* U8 archives: a 32-byte header followed by the file system table (12-byte entries, then
 * the name strings). The header and the entries are big-endian u32s; names and file data
 * are left as stored. After the fixup the SDK's arc.c reads the table natively.
 *
 * The magic marks the state: 55 AA 38 2D in memory means big-endian (not yet converted);
 * after the swap the same word reads 0x55AA382D natively. */
#include <port_endian.h>
#include <port_os.h>
#include <string.h>

#define U8_MAGIC 0x55AA382Du

int PortFixupU8(void* archive, size_t size) {
    uint8_t* base = (uint8_t*)archive;
    uint32_t magic;
    uint32_t fstStart;
    uint32_t fstSize;
    uint32_t entryCount;

    if (size < 32) {
        return 0;
    }
    memcpy(&magic, base, 4);
    if (magic == U8_MAGIC) {
        return 1; /* already native */
    }
    if (PortBE32(base) != U8_MAGIC) {
        return 0;
    }
    fstStart = PortBE32(base + 4);
    fstSize = PortBE32(base + 8);
    if (fstStart < 32 || (size_t)fstStart + 12 > size) {
        PortRed("U8 archive at %p: table at 0x%x is outside the %zu bytes loaded", archive, fstStart, size);
        return 0;
    }
    entryCount = PortBE32(base + fstStart + 8); /* the root entry's "next" is the count */
    if ((size_t)entryCount * 12 > fstSize || (size_t)fstStart + fstSize > size) {
        PortRed("U8 archive at %p: %u entries do not fit its %u-byte table", archive, entryCount, fstSize);
        return 0;
    }
    PortSwap32Array(base, 8);
    PortSwap32Array(base + fstStart, (size_t)entryCount * 3);
    return 1;
}
