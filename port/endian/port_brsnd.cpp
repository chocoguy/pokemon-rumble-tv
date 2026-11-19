/* Byte-order fixups for the sound archive's member files, as nw4r::snd reads them:
 *
 *   RWSD  wave sounds (snd_WsdFile.h): DATA lists each sound's info and note table, WAVE
 *         lists the wave infos its notes use
 *   RBNK  instrument banks (snd_BankFile.h): DATA's instrument table of references to
 *         instruments, key-range tables or index tables, nested; WAVE as in RWSD
 *   RWAR  wave archives (snd_WaveArchive.h): a table of the RWAV files in its data block
 *   RWAV  one wave (snd_WaveFile.h): INFO holds a wave info
 *
 * A wave info is the shared piece: format bytes, rate, loop points, then a table of
 * channel infos, each pointing at DSP-ADPCM coefficients and history (halfwords). The
 * samples themselves stay as stored: the mixer (port/ax, phase 5) reads them big-endian,
 * as the DSP does.
 *
 * Each file's signature tells its state ('RWSD' read big-endian before the fixup,
 * natively after). Hooked in the readers' constructors; every swapped halfword is
 * recorded (port_fixer.h) because several references may share one table. */
#include <nw4r/snd/snd_BankFile.h>
#include <nw4r/snd/snd_WaveArchive.h>
#include <nw4r/snd/snd_WaveFile.h>
#include <nw4r/snd/snd_WsdFile.h>
#include <port_endian.h>
#include <port_os.h>
#include <stdlib.h>
#include <string.h>
#include "port_fixer.h"

using namespace nw4r::snd;
using namespace nw4r::snd::detail;

namespace {

typedef char LayoutCheck1[(sizeof(BankFile::InstParam) == 0x30) ? 1 : -1]; /* InstParam must keep the file's layout */
typedef char LayoutCheck2[(sizeof(WsdFile::NoteInfo) == 0x30) ? 1 : -1]; /* NoteInfo must keep the file's layout */
typedef char LayoutCheck3[(sizeof(WsdFile::WsdInfo) == 0x20) ? 1 : -1]; /* WsdInfo must keep the file's layout */
typedef char LayoutCheck4[(sizeof(WaveFile::WaveInfo) == 0x1C) ? 1 : -1]; /* WaveInfo must keep the file's layout */
typedef char LayoutCheck5[(sizeof(AdpcmInfo) == 0x30) ? 1 : -1]; /* AdpcmInfo must keep the file's layout */

const u32 kRWSD = 0x52575344u, kRBNK = 0x52424E4Bu, kRWAR = 0x52574152u, kRWAV = 0x52574156u;

/* State of a file by its signature: 1 converted, 0 needs converting, -1 not this format. */
int State(const void* file, u32 signature) {
    u32 native;
    if (file == NULL) {
        return -1;
    }
    memcpy(&native, file, 4);
    if (native == signature) {
        return 1;
    }
    return PortBE32(static_cast<const u8*>(file)) == signature ? 0 : -1;
}

/* ut::BinaryFileHeader (signature, BOM left alone, version, size, header size, block
 * count), then `blocks` offset/size pairs. */
void FileHeader(Fixer& f, u32 blocks) {
    f.S32(0);
    f.S16(6);
    f.S32(8);
    f.S16(12, 2);
    f.S32(16, blocks * 2);
}

/* A wave info at `at`: everything it refers to is relative to itself. */
void FixWaveInfo(Fixer& f, u32 at) {
    typedef WaveFile::WaveInfo W;
    SWAP16(W, sampleRate);
    SWAP32(W, loopStart);
    SWAP32(W, loopEnd);
    SWAP32(W, channelInfoTableOffset);
    SWAP32(W, dataLocation);
    SWAP32(W, reserved);
    if (!f.In(at, sizeof(W))) {
        return;
    }
    u32 channels = f.base[at + offsetof(W, numChannels)];
    u32 table = at + f.N32(at + offsetof(W, channelInfoTableOffset));
    f.S32(table, channels);
    for (u32 i = 0; i < channels && f.ok; i++) {
        u32 channel = at + f.N32(table + i * 4);
        f.S32(channel, sizeof(WaveFile::WaveChannelInfo) / 4);
        u32 adpcm = f.N32(channel + offsetof(WaveFile::WaveChannelInfo, adpcmOffset));
        if (adpcm != 0) {
            f.S16(at + adpcm, sizeof(AdpcmInfo) / 2);
        }
    }
}

/* A WAVE block of RWSD (v1.1+: count, then offsets from the block) or RBNK (a Table of
 * DataRefs relative to the table). */
void WsdWaves(Fixer& f, u32 block, bool counted) {
    f.S32(block, 2);
    if (!counted) {
        PortRed("rwsd fixup at %p: version 1.0 wave block (no count) not handled", f.base);
        f.ok = false;
        return;
    }
    f.S32(block + 8);
    u32 count = f.N32(block + 8);
    f.S32(block + 12, count);
    for (u32 i = 0; i < count && f.ok; i++) {
        FixWaveInfo(f, block + f.N32(block + 12 + i * 4));
    }
}

void BankWaves(Fixer& f, u32 block) {
    f.S32(block, 2);
    u32 table = block + offsetof(BankFile::WaveBlock, waveInfoTable);
    RefTable(f, table, table, [&](u32 target) { FixWaveInfo(f, target); });
}

void InstParam(Fixer& f, u32 at, u32 base) {
    typedef BankFile::InstParam P;
    f.S32(at); /* waveIndex (the address and callback forms are never in a file) */
    SWAP32(P, tune);
    Ref(f, at + offsetof(P, lfoTableRef), base);
    Ref(f, at + offsetof(P, graphEnvTableRef), base);
    Ref(f, at + offsetof(P, randomizerTableRef), base);
    SWAP32(P, reserved);
}

/* A bank DataRegion: an instrument, or a split by key or velocity whose entries are
 * DataRegions again. `depth` bounds the nesting (key, then velocity). */
void Region(Fixer& f, u32 at, u32 base, int depth) {
    u8 type = 0;
    u32 target = Ref(f, at, base, &type);
    if (target == 0 || depth > 3) {
        return;
    }
    switch (type) {
    case Util::DATATYPE_T1:
        InstParam(f, target, base);
        break;
    case Util::DATATYPE_T2: { /* RangeTable: size, keys, padding, then a region per key */
        if (!f.In(target, 1)) {
            return;
        }
        u32 n = f.base[target];
        u32 regions = target + ((n + 1 + 3) & ~3u);
        for (u32 i = 0; i < n && f.ok; i++) {
            Region(f, regions + i * sizeof(BankFile::DataRegion), base, depth + 1);
        }
        break;
    }
    case Util::DATATYPE_T3: { /* IndexTable: min, max, reserved, then a region per key */
        f.S16(target + offsetof(BankFile::IndexTable, reserved));
        if (!f.In(target, 2)) {
            return;
        }
        u32 lo = f.base[target], hi = f.base[target + 1];
        for (u32 k = lo; k <= hi && f.ok; k++) {
            Region(f, target + offsetof(BankFile::IndexTable, ref) + (k - lo) * sizeof(BankFile::DataRegion), base,
                   depth + 1);
        }
        break;
    }
    default:
        break;
    }
}

Fixer Begin(void* file, const char* what) {
    u8* base = static_cast<u8*>(file);
    u32 size = PortBE32(base + 8);
    Fixer f = {base, size, static_cast<u8*>(calloc(size / 16 + 1, 1)), true, what};
    return f;
}

int End(Fixer& f) {
    free(f.done);
    return f.ok;
}

}

extern "C" int PortFixupRWAV(void* file) {
    int state = State(file, kRWAV);
    if (state != 0) {
        return state > 0;
    }
    Fixer f = Begin(file, "rwav");
    FileHeader(f, 2);
    u32 info = f.N32(offsetof(WaveFile::FileHeader, infoChunkOffset));
    f.S32(info, 2);
    FixWaveInfo(f, info + offsetof(WaveFile::InfoBlock, waveInfo));
    u32 data = f.N32(offsetof(WaveFile::FileHeader, dataChunkOffset));
    if (data != 0) {
        f.S32(data, 2);
    }
    return End(f);
}

extern "C" int PortFixupRWAR(void* file) {
    int state = State(file, kRWAR);
    if (state != 0) {
        return state > 0;
    }
    Fixer f = Begin(file, "rwar");
    FileHeader(f, 2);
    u32 table = f.N32(offsetof(WaveArchive::FileHeader, tableChunkOffset));
    u32 data = f.N32(offsetof(WaveArchive::FileHeader, dataChunkOffset));
    f.S32(table, 2);
    f.S32(data, 2);
    u32 files = table + offsetof(WaveArchive::TableBlock, fileTable);
    f.S32(files);
    u32 count = f.N32(files);
    int ok = 1;
    for (u32 i = 0; i < count && f.ok; i++) {
        u32 item = files + 4 + i * sizeof(WaveArchive::TableItem);
        u32 wave = Ref(f, item + offsetof(WaveArchive::TableItem, waveFileRef), data);
        f.S32(item + offsetof(WaveArchive::TableItem, waveFileSize));
        if (wave != 0 && f.In(wave, 16)) {
            ok &= PortFixupRWAV(f.base + wave);
        }
    }
    return End(f) && ok;
}

extern "C" int PortFixupRWSD(void* file) {
    int state = State(file, kRWSD);
    if (state != 0) {
        return state > 0;
    }
    Fixer f = Begin(file, "rwsd");
    u16 version = PortBE16(f.base + 6);
    FileHeader(f, 2);
    u32 data = f.N32(offsetof(WsdFile::Header, dataBlockOffset));
    u32 wave = f.N32(offsetof(WsdFile::Header, waveBlockOffset));
    if (data != 0) {
        f.S32(data, 2);
        u32 base = data + offsetof(WsdFile::DataBlock, wsdCount);
        RefTable(f, base, base, [&](u32 at) {
            typedef WsdFile::Wsd W;
            u32 info = Ref(f, at + offsetof(W, refWsdInfo), base);
            if (info != 0) {
                typedef WsdFile::WsdInfo I;
                u32 i0 = info;
                f.S32(i0 + offsetof(I, pitch));
                Ref(f, i0 + offsetof(I, graphEnvTablevRef), base);
                Ref(f, i0 + offsetof(I, randomizerTableRef), base);
                f.S32(i0 + offsetof(I, reserved));
            }
            /* The track table is not read by this version of nw4r::snd; only its references. */
            u32 tracks = Ref(f, at + offsetof(W, refTrackTable), base);
            if (tracks != 0) {
                RefTable(f, tracks, base, [](u32) {});
            }
            u32 notes = Ref(f, at + offsetof(W, refNoteTable), base);
            RefTable(f, notes, base, [&](u32 note) {
                typedef WsdFile::NoteInfo N;
                f.S32(note + offsetof(N, waveIndex));
                f.S32(note + offsetof(N, pitch));
                Ref(f, note + offsetof(N, lfoTableRef), base);
                Ref(f, note + offsetof(N, graphEnvTablevRef), base);
                Ref(f, note + offsetof(N, randomizerTableRef), base);
                f.S32(note + offsetof(N, reserved));
            });
        });
    }
    if (wave != 0) {
        WsdWaves(f, wave, version >= NW4R_VERSION(1, 1));
    }
    return End(f);
}

extern "C" int PortFixupRBNK(void* file) {
    int state = State(file, kRBNK);
    if (state != 0) {
        return state > 0;
    }
    Fixer f = Begin(file, "rbnk");
    FileHeader(f, 2);
    u32 data = f.N32(offsetof(BankFile::Header, dataBlockOffset));
    u32 wave = f.N32(offsetof(BankFile::Header, waveBlockOffset));
    if (data != 0) {
        f.S32(data, 2);
        u32 table = data + offsetof(BankFile::DataBlock, instTable);
        f.S32(table);
        u32 count = f.N32(table);
        for (u32 i = 0; i < count && f.ok; i++) {
            Region(f, table + 4 + i * sizeof(BankFile::DataRegion), table, 0);
        }
    }
    if (wave != 0) {
        BankWaves(f, wave);
    }
    return End(f);
}
