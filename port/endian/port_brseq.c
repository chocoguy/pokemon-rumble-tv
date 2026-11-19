/* Byte-order fixup for NW4R sequence files (.brseq, RSEQ), as
 * nw4r::snd::detail::SeqFileReader reads them (nw4r/snd/snd_SeqFile.h):
 *
 *   header  ut::BinaryFileHeader (16 bytes), then DATA offset/size and LABL offset/size
 *   DATA    block header (kind, size), base offset; then the sequence's byte stream
 *   LABL    block header, label count, offsets (from the block + 8) to
 *           { offset, name length, name }
 *
 * The sequence commands are bytes; MmlParser composes its 16- and 24-bit operands itself,
 * so only the structures above are swapped. The signature tells the state: 'RSEQ' read
 * big-endian before, natively after. The byte-order mark stays as stored (see
 * port_nw4r.c). Hooked in SeqFileReader's constructor. */
#include <port_endian.h>
#include <port_os.h>
#include <stdint.h>
#include <string.h>

#define RSEQ 0x52534551u

static uint32_t Native32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

int PortFixupBRSEQ(void* file) {
    uint8_t* b = (uint8_t*)file;
    uint32_t size, data, label, count, i;
    static int reported;

    if (b == NULL) {
        return 0;
    }
    if (Native32(b) == RSEQ) {
        return 1; /* converted already */
    }
    if (PortBE32(b) != RSEQ) {
        return 0; /* not a sequence: SeqFileReader rejects it */
    }
    size = PortBE32(b + 8);
    data = PortBE32(b + 16);
    label = PortBE32(b + 24);
    if (size < 32 || data > size - 12 || (label != 0 && label > size - 12)) {
        PortRedOnce(&reported, "brseq fixup at %p: header offsets out of range (size 0x%x)", file, size);
        return 0;
    }

    PortSwap32(b);          /* signature */
    PortSwap16(b + 6);      /* version */
    PortSwap32(b + 8);      /* file size */
    PortSwap16(b + 12);     /* header size */
    PortSwap16(b + 14);     /* block count */
    PortSwap32Array(b + 16, 4);

    /* DATA: kind, size, base offset. */
    PortSwap32Array(b + data, 3);

    if (label != 0) {
        uint8_t* l = b + label;
        PortSwap32Array(l, 2);
        count = PortBE32(l + 8);
        if (count > (size - label - 12) / 4) {
            PortRedOnce(&reported, "brseq fixup at %p: %u labels do not fit", file, count);
            PortSwap32(l + 8);
            return 1;
        }
        PortSwap32(l + 8);
        for (i = 0; i < count; i++) {
            uint32_t at = PortBE32(l + 12 + 4 * i);
            PortSwap32(l + 12 + 4 * i);
            if (at + 8 + 8 > size - label) {
                PortRedOnce(&reported, "brseq fixup at %p: label %u at 0x%x is outside the file", file, i, at);
                continue;
            }
            PortSwap32Array(l + 8 + at, 2);
        }
    }
    return 1;
}
