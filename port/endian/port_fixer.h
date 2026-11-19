/* Shared by the C++ fixups (port_brsar.cpp, port_brsnd.cpp, port_brres.cpp): a
 * bounds-checked swapper over one file that records every swapped halfword, so data
 * several references share is converted once; and the NW4R sound DataRef / Table walkers.
 * Everything is in an anonymous namespace, private to each user. */
#ifndef PORT_FIXER_H
#define PORT_FIXER_H

#include <nw4r/snd/snd_Util.h>
#include <port_endian.h>
#include <port_os.h>
#include <stddef.h>
#include <string.h>

namespace {

using nw4r::snd::detail::Util;

struct Fixer {
    u8* base;
    u32 size;
    u8* done; /* one bit per halfword already swapped */
    bool ok;
    const char* what; /* the format, for messages (NULL: brsar) */

    bool In(u32 offset, u32 length) {
        if (offset > size || length > size - offset) {
            if (ok) {
                PortRed("%s fixup at %p: 0x%x (+0x%x) is outside the 0x%x-byte file", what ? what : "brsar", base, offset,
                        length, size);
            }
            ok = false;
        }
        return ok;
    }
    bool First(u32 offset) {
        u32 bit = offset / 2;
        if (done[bit / 8] & (1 << (bit % 8))) {
            return false;
        }
        done[bit / 8] |= static_cast<u8>(1 << (bit % 8));
        return true;
    }
    void S16(u32 offset, u32 count = 1) {
        if (!In(offset, count * 2)) {
            return;
        }
        for (u32 i = 0; i < count; i++) {
            if (First(offset + i * 2)) {
                PortSwap16(base + offset + i * 2);
            }
        }
    }
    void S32(u32 offset, u32 count = 1) {
        if (!In(offset, count * 4)) {
            return;
        }
        for (u32 i = 0; i < count; i++) {
            u32 at = offset + i * 4;
            if (First(at)) {
                First(at + 2);
                PortSwap32(base + at);
            }
        }
    }
    bool Done(u32 offset) const {
        u32 bit = offset / 2;
        return (done[bit / 8] & (1 << (bit % 8))) != 0;
    }
    u16 N16(u32 offset) {
        u16 v = 0;
        if (In(offset, 2)) {
            memcpy(&v, base + offset, 2);
        }
        return v;
    }
    u32 N32(u32 offset) {
        u32 v = 0;
        if (In(offset, 4)) {
            memcpy(&v, base + offset, 4);
        }
        return v;
    }
};

#define SWAP16(T, m) f.S16(at + offsetof(T, m), sizeof(((T*)0)->m) / 2)
#define SWAP32(T, m) f.S32(at + offsetof(T, m), sizeof(((T*)0)->m) / 4)

/* A DataRef (refType, dataType, reserved u16, value u32); returns the offset it refers
 * to (relative to `info`), or 0 for none. */
u32 Ref(Fixer& f, u32 at, u32 info, u8* dataType = NULL) {
    typedef Util::DataRef<void> R;
    f.S16(at + offsetof(R, reserved));
    f.S32(at + offsetof(R, value));
    if (!f.In(at, sizeof(R))) {
        return 0;
    }
    u8 refType = f.base[at + offsetof(R, refType)];
    if (dataType != NULL) {
        *dataType = f.base[at + offsetof(R, dataType)];
    }
    u32 value = f.N32(at + offsetof(R, value));
    if (refType != Util::REFTYPE_OFFSET || value == 0) {
        return 0;
    }
    return info + value;
}

/* Util::Table<DataRef<T>>: count, then the references; `item` handles each target. */
template <typename Item> void RefTable(Fixer& f, u32 table, u32 info, Item item) {
    if (table == 0) {
        return;
    }
    f.S32(table);
    u32 count = f.N32(table);
    for (u32 i = 0; i < count && f.ok; i++) {
        u32 target = Ref(f, table + 4 + i * sizeof(Util::DataRef<void>), info);
        if (target != 0) {
            item(target);
        }
    }
}

}

#endif
