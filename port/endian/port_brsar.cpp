/* Byte-order fixup for NW4R sound archives (.brsar, RSAR): the file header and the SYMB
 * (label strings) and INFO (sound, bank, player, file and group tables) blocks, as
 * nw4r::snd::detail::SoundArchiveFileReader reads them. Offsets come from the decomp's
 * own structs (nw4r/snd/snd_SoundArchiveFile.h).
 *
 * The FILE block's contents (sequences, banks, wave sounds, wave archives, streams) are
 * separate formats converted where nw4r::snd opens them (port_brseq.c, port_brsnd.cpp).
 *
 * Several references may share one table, so every swapped halfword is recorded and
 * never swapped twice. The signature tells whether the file is converted: it reads
 * 'RSAR' big-endian before and natively after. */
#include <nw4r/snd/snd_SoundArchiveFile.h>
#include <port_endian.h>
#include <port_os.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "port_fixer.h"

using namespace nw4r::snd;
using namespace nw4r::snd::detail;

namespace {

typedef SoundArchiveFile F;

void SoundCommon(Fixer& f, u32 at, u32 info) {
    SWAP32(F::SoundCommonInfo, stringId);
    SWAP32(F::SoundCommonInfo, fileId);
    SWAP32(F::SoundCommonInfo, playerId);
    SWAP32(F::SoundCommonInfo, userParam);
    u32 param3d = Ref(f, at + offsetof(F::SoundCommonInfo, param3dRef), info);
    if (param3d != 0) {
        f.S32(param3d + offsetof(F::Sound3DParam, flags));
    }
    u8 type = 0;
    u32 sound = Ref(f, at + offsetof(F::SoundCommonInfo, soundInfoRef), info, &type);
    if (sound == 0) {
        return;
    }
    u32 base = at;
    at = sound;
    switch (type) {
    case 1: /* SeqSoundInfo */
        SWAP32(F::SeqSoundInfo, dataOffset);
        SWAP32(F::SeqSoundInfo, bankId);
        SWAP32(F::SeqSoundInfo, allocTrack);
        SWAP32(F::SeqSoundInfo, reserved);
        break;
    case 2: /* StrmSoundInfo */
        SWAP32(F::StrmSoundInfo, startPosition);
        SWAP16(F::StrmSoundInfo, allocChannelCount);
        SWAP16(F::StrmSoundInfo, allocTrackFlag);
        SWAP32(F::StrmSoundInfo, reserved);
        break;
    case 3: /* WaveSoundInfo */
        SWAP32(F::WaveSoundInfo, subNo);
        SWAP32(F::WaveSoundInfo, allocTrack);
        SWAP32(F::WaveSoundInfo, reserved);
        break;
    default:
        PortRed("brsar fixup: sound info at 0x%x has type %u", base, type);
        break;
    }
}

void Info(Fixer& f, u32 block) {
    u32 info = block + offsetof(F::InfoBlock, info);
    u32 at = info;
    f.S32(block, 2); /* block header */

    RefTable(f, Ref(f, at + offsetof(F::Info, soundTableRef), info), info,
             [&](u32 item) { SoundCommon(f, item, info); });
    RefTable(f, Ref(f, at + offsetof(F::Info, bankTableRef), info), info, [&](u32 item) {
        u32 at = item;
        SWAP32(F::BankInfo, stringId);
        SWAP32(F::BankInfo, fileId);
        SWAP32(F::BankInfo, reserved);
    });
    RefTable(f, Ref(f, at + offsetof(F::Info, playerTableRef), info), info, [&](u32 item) {
        u32 at = item;
        SWAP32(F::PlayerInfo, stringId);
        SWAP16(F::PlayerInfo, padding2);
        SWAP32(F::PlayerInfo, heapSize);
        SWAP32(F::PlayerInfo, reserved);
    });
    RefTable(f, Ref(f, at + offsetof(F::Info, fileTableRef), info), info, [&](u32 item) {
        u32 at = item;
        SWAP32(F::FileInfo, fileSize);
        SWAP32(F::FileInfo, waveDataFileSize);
        SWAP32(F::FileInfo, entryNum);
        Ref(f, at + offsetof(F::FileInfo, extFilePathRef), info);
        RefTable(f, Ref(f, at + offsetof(F::FileInfo, filePosTableRef), info), info, [&](u32 pos) {
            f.S32(pos + offsetof(SoundArchive::FilePos, groupId));
            f.S32(pos + offsetof(SoundArchive::FilePos, index));
        });
    });
    RefTable(f, Ref(f, at + offsetof(F::Info, groupTableRef), info), info, [&](u32 item) {
        u32 at = item;
        SWAP32(F::GroupInfo, stringId);
        SWAP32(F::GroupInfo, entryNum);
        SWAP32(F::GroupInfo, offset);
        SWAP32(F::GroupInfo, size);
        SWAP32(F::GroupInfo, waveDataOffset);
        SWAP32(F::GroupInfo, waveDataSize);
        Ref(f, at + offsetof(F::GroupInfo, extFilePathRef), info);
        RefTable(f, Ref(f, at + offsetof(F::GroupInfo, itemTableRef), info), info, [&](u32 entry) {
            f.S32(entry, sizeof(F::GroupItemInfo) / 4);
        });
    });
    u32 player = Ref(f, at + offsetof(F::Info, soundArchivePlayerInfoRef), info);
    if (player != 0) {
        at = player;
        f.S16(at, (offsetof(F::SoundArchivePlayerInfo, reserved)) / 2);
        SWAP32(F::SoundArchivePlayerInfo, reserved);
    }
}

void StringTree(Fixer& f, u32 at) {
    f.S32(at + offsetof(F::StringTree, rootIdx));
    u32 table = at + offsetof(F::StringTree, nodeTable);
    f.S32(table);
    u32 count = f.N32(table);
    for (u32 i = 0; i < count && f.ok; i++) {
        u32 node = table + 4 + i * sizeof(F::StringTreeNode);
        f.S16(node + offsetof(F::StringTreeNode, flags));
        f.S16(node + offsetof(F::StringTreeNode, bit));
        f.S32(node + offsetof(F::StringTreeNode, leftIdx), 4); /* left, right, strIdx, id */
    }
}

void Symbols(Fixer& f, u32 block) {
    u32 strings = block + offsetof(F::SymbolBlock, stringBlock);
    f.S32(block, 2);
    f.S32(strings, sizeof(F::StringChunk) / 4);
    u32 table = f.N32(strings + offsetof(F::StringChunk, tableOffset));
    if (table != 0) {
        f.S32(strings + table);
        u32 count = f.N32(strings + table);
        f.S32(strings + table + 4, count);
    }
    const size_t trees[] = {offsetof(F::StringChunk, soundTreeOffset), offsetof(F::StringChunk, playerTreeOffset),
                            offsetof(F::StringChunk, groupTreeOffset), offsetof(F::StringChunk, bankTreeOffset)};
    for (int i = 0; i < 4 && f.ok; i++) {
        u32 tree = f.N32(strings + trees[i]);
        if (tree != 0) {
            StringTree(f, strings + tree);
        }
    }
}

}

extern "C" int PortFixupBRSAR(void* file) {
    u8* base = static_cast<u8*>(file);
    u32 native;
    memcpy(&native, base, 4);
    if (native == F::SIGNATURE_FILE) {
        return 1;
    }
    if (PortBE32(base) != F::SIGNATURE_FILE) {
        return 0;
    }
    /* Only the header, SYMB and INFO are converted, so the bounds are their end. */
    u32 symbol = PortBE32(base + offsetof(F::Header, symbolDataOffset));
    u32 symbolSize = PortBE32(base + offsetof(F::Header, symbolDataSize));
    u32 info = PortBE32(base + offsetof(F::Header, infoOffset));
    u32 infoSize = PortBE32(base + offsetof(F::Header, infoSize));
    u32 end = symbol + symbolSize > info + infoSize ? symbol + symbolSize : info + infoSize;
    Fixer f = {base, end, static_cast<u8*>(calloc(end / 16 + 1, 1)), true};

    /* ut::BinaryFileHeader: signature, BOM (left alone), version, fileSize, headerSize,
     * dataBlocks; then the six chunk offsets and sizes. */
    f.S32(0);
    f.S16(6);
    f.S32(8);
    f.S16(12, 2);
    f.S32(offsetof(F::Header, symbolDataOffset), 6);
    Symbols(f, symbol);
    Info(f, info);
    free(f.done);
    return f.ok;
}
