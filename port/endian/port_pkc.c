/* Byte-order fixup for the game's compiled scripts (.pkc in script.arc), as
 * NPpu::CCode::Load reads them (game/script/NPpuVM.h):
 *
 *   header  eight words: magic, stack size, code size, wide-string bytes, wide-string
 *           count, string bytes, string count, reserved
 *   code    32-bit instruction words; operands that are halves of a word (switch tables)
 *           are taken from the word as read, so swapping whole words keeps them right
 *   wide strings  UTF-16, NUL-terminated
 *   strings       Shift-JIS bytes, left alone
 *
 * The magic tells the state: big-endian files read MAGIC_SWAPPED (or the old form's)
 * natively before the fixup and MAGIC after. Hooked in CPpuCode::Load. */
#include <port_endian.h>
#include <port_os.h>
#include <stdint.h>
#include <string.h>

#define PKC_MAGIC 0x706B1A20u
#define PKC_MAGIC_OLD 0x201A706Bu

int PortFixupPKC(void* file, size_t size) {
    uint8_t* b = (uint8_t*)file;
    uint32_t magic, code, wide, strings;
    static int reported;
    if (b == NULL || size < 32) {
        return 0;
    }
    memcpy(&magic, b, 4);
    if (magic == PKC_MAGIC || magic == PKC_MAGIC_OLD) {
        return 1; /* converted already */
    }
    magic = PortBE32(b);
    if (magic != PKC_MAGIC && magic != PKC_MAGIC_OLD) {
        return 0; /* not a script: CCode::Load rejects it */
    }
    code = PortBE32(b + 8);
    wide = PortBE32(b + 12);
    strings = PortBE32(b + 20);
    if ((code & 3) != 0 || (wide & 1) != 0 || code > size - 32 || wide > size - 32 - code ||
        strings > size - 32 - code - wide) {
        PortRedOnce(&reported, "pkc fixup at %p: sizes out of range (code 0x%x, wide 0x%x, strings 0x%x, file 0x%zx)",
                    file, code, wide, strings, size);
        return 0;
    }
    PortSwap32Array(b, 8);
    PortSwap32Array(b + 32, code / 4);
    PortSwap16Array(b + 32 + code, wide / 2);
    return 1;
}
