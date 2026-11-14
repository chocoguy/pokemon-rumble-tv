/* Byte-order fixups for NW4R effect files: .breff (REFF, emitters) and .breft (REFT,
 * textures), as nw4r::ef reads them (src/nw4r/ef/ef_resource.cpp, ef_animcurve.cpp).
 *
 * Fields ef reads natively are swapped; the name-table entries, which ef_resource.cpp
 * decodes byte by byte as big-endian, are left as stored, and so are texture and palette
 * data (read big-endian by the GX texture decoder) and all byte fields.
 *
 * Offsets come from the decomp's own structs (offsetof), so this follows the headers in
 * include/nw4r/ef. The file header's headersize tells whether a file is converted: it is
 * 0x0010 big-endian before and native after. */
#include <nw4r/ef/ef_resource.h>
#include <nw4r/ef/ef_res_emitter.h>
#include <nw4r/ef/ef_res_texture.h>
#include <nw4r/ef/ef_particle.h>
#include <nw4r/ef/ef_animcurveinternal.h>
#include <nw4r/ef/ef_particlemanagerfield.h>
#include <port_endian.h>
#include <port_os.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

using namespace nw4r::ef;

namespace {

struct Fixer {
    u8* base;
    u32 size;
    const char* what;
    bool ok;

    bool In(u32 offset, u32 length) {
        if (offset > size || length > size - offset) {
            if (ok) {
                PortRed("%s fixup at %p: 0x%x (+0x%x) is outside the 0x%x-byte file", what, base, offset, length, size);
            }
            ok = false;
        }
        return ok;
    }
    void S16(u32 offset, u32 count = 1) {
        if (In(offset, count * 2)) {
            for (u32 i = 0; i < count; i++) {
                PortSwap16(base + offset + i * 2);
            }
        }
    }
    void S32(u32 offset, u32 count = 1) {
        if (In(offset, count * 4)) {
            PortSwap32Array(base + offset, count);
        }
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
    u8 B(u32 offset) {
        return In(offset, 1) ? base[offset] : 0;
    }
};

u32 ChannelCount(u8 mask) {
    return static_cast<u32>(__builtin_popcount(mask));
}

/* ef_resource.cpp's FileHeader: signature[4], byteorder[2], version, filesize,
 * headersize, blocknum. Returns the project's offset (after the block header). */
u32 Header(Fixer& f) {
    f.S16(6);
    f.S32(8);
    f.S16(12, 2);
    u32 block = f.N16(12);
    f.S32(block + 4); /* BlockHeader.headersize */
    return block + 8;
}

/* EffectProject / TextureProject, then the name table they point at. Returns the name
 * table's offset. */
u32 Project(Fixer& f, u32 project) {
    f.S32(project + offsetof(EffectProject, headersize));
    f.S16(project + offsetof(EffectProject, projectnameLen));
    u32 table = project + f.N32(project + offsetof(EffectProject, headersize));
    f.S32(table);     /* tablesize */
    f.S16(table + 4); /* numEntry */
    return table;
}

/* Calls `body` with the offset of each entry's resource. The entries themselves (u16
 * length, name, u32 offset, u32 size) are read big-endian by ef and stay as stored. */
template <typename F> void Entries(Fixer& f, u32 table, F body) {
    u16 count = f.N16(table + 4);
    u32 p = table + 8;
    for (u16 i = 0; i < count && f.ok; i++) {
        if (!f.In(p, 2)) {
            break;
        }
        u32 length = PortBE16(f.base + p);
        p += 2 + length;
        if (!f.In(p, 8)) {
            break;
        }
        u32 offset = PortBE32(f.base + p);
        p += 8;
        body(table + offset);
    }
}

/* --------------------------------------------------------------------------------- */
/* Emitters                                                                          */

#define SWAP16(T, m) f.S16(at + offsetof(T, m), sizeof(((T*)0)->m) / 2)
#define SWAP32(T, m) f.S32(at + offsetof(T, m), sizeof(((T*)0)->m) / 4)

void EmitterDescription(Fixer& f, u32 at) {
    SWAP32(EmitterDesc, commonFlag);
    SWAP32(EmitterDesc, emitFlag);
    SWAP16(EmitterDesc, emitLife);
    SWAP16(EmitterDesc, ptclLife);
    SWAP32(EmitterDesc, emitEmit);
    SWAP16(EmitterDesc, emitEmitStart);
    SWAP16(EmitterDesc, emitEmitPast);
    SWAP16(EmitterDesc, emitEmitInterval);
    SWAP32(EmitterDesc, commonParam);
    SWAP16(EmitterDesc, emitEmitDiv);
    SWAP32(EmitterDesc, velPowerRadiationDir);
    SWAP32(EmitterDesc, velPowerYAxis);
    SWAP32(EmitterDesc, velPowerRandomDir);
    SWAP32(EmitterDesc, velPowerNormalDir);
    SWAP32(EmitterDesc, velDiffusionEmitterNormal);
    SWAP32(EmitterDesc, velPowerSpecDir);
    SWAP32(EmitterDesc, velDiffusionSpecDir);
    SWAP32(EmitterDesc, velSpecDir);
    SWAP32(EmitterDesc, scale);
    SWAP32(EmitterDesc, rotate);
    SWAP32(EmitterDesc, translate);
    SWAP32(EmitterDesc, randomSeed);

    at += offsetof(EmitterDesc, drawSetting);
    SWAP16(EmitterDrawSetting, mFlags);
    SWAP16(EmitterDrawSetting, mAlphaFlickCycle);
    f.S32(at + offsetof(EmitterDrawSetting, mLighting) + offsetof(Lighting, mRadius), 4); /* radius, position */
    SWAP32(EmitterDrawSetting, mIndTexOffsetMtx);
    SWAP32(EmitterDrawSetting, zOffset);
}

void ParticleDescription(Fixer& f, u32 at, u32 size) {
    /* size .. textureTranslate are consecutive floats. */
    f.S32(at + offsetof(ParticleParameterDesc, size),
          (offsetof(ParticleParameterDesc, mTexture) - offsetof(ParticleParameterDesc, size)) / 4);
    SWAP16(ParticleParameterDesc, textureWrap);
    SWAP32(ParticleParameterDesc, rotateOffset);
    /* Texture names: u16 length (read natively by RelocateCommand), then the name. */
    u32 p = at + offsetof(ParticleParameterDesc, textureNames);
    for (int i = 0; i < TEX_LAYER_MAX && f.ok; i++) {
        f.S16(p);
        p += 2 + f.N16(p);
    }
    if (p > at + size) {
        f.In(p, at + size - p);
    }
}

/* --------------------------------------------------------------------------------- */
/* Animation curves                                                                  */

enum CurveData { CURVE_BYTES, CURVE_FLOATS, CURVE_ROTATION };

/* Keys of a value curve: u16 count, pad, then records of `stride` bytes: u16 frame, u8
 * flags, reserved, u8 type[8], then the values, or a u16 random index when flags != 0. */
void ValueKeys(Fixer& f, u32 keys, u32 channels, CurveData data) {
    f.S16(keys);
    u16 count = f.N16(keys);
    u32 stride = data == CURVE_BYTES ? (channels + 0xD) & ~1u : channels * 4 + 0xC;
    for (u16 i = 0; i < count && f.ok; i++) {
        u32 record = keys + 4 + i * stride;
        f.S16(record + offsetof(detail::ValueCurveKey, mFrame));
        if (f.B(record + offsetof(detail::ValueCurveKey, mFlags)) != 0) {
            f.S16(record + 12); /* random index */
        } else if (data != CURVE_BYTES) {
            f.S32(record + 12, channels);
        }
    }
}

/* Range and random tables of a value curve: u16 count, pad, then entries of (base,
 * range) pairs per channel; rotation entries add a 4-byte trailer (a flag byte). */
void ValueRanges(Fixer& f, u32 table, u32 bytes, u32 channels, CurveData data) {
    if (bytes < 4) {
        return;
    }
    f.S16(table);
    if (data == CURVE_BYTES) {
        return;
    }
    u32 pairBytes = channels * 8;
    u32 stride = data == CURVE_ROTATION ? pairBytes + 4 : pairBytes;
    if (stride == 0) {
        return;
    }
    for (u32 entry = table + 4; entry + pairBytes <= table + bytes && f.ok; entry += stride) {
        f.S32(entry, channels * 2);
    }
}

void NameTable(Fixer& f, u32 table) {
    f.S16(table);
    u16 count = f.N16(table);
    f.S32(table + 4, count); /* work slots */
    u32 p = table + 4 + count * 4;
    for (u16 i = 0; i < count && f.ok; i++) {
        f.S16(p);
        p += 2 + f.N16(p);
    }
}

void TextureCurve(Fixer& f, u32 keys, u32 ranges, u32 random) {
    f.S16(keys);
    u16 count = f.N16(keys);
    for (u16 i = 0; i < count && f.ok; i++) {
        u32 record = keys + offsetof(detail::TextureCurveKeys, mEntry) + i * sizeof(detail::TextureCurveRecord);
        f.S16(record + offsetof(detail::TextureCurveRecord, mFrame));
        u32 value = record + offsetof(detail::TextureCurveRecord, mValue);
        if (f.B(record + offsetof(detail::TextureCurveRecord, mFlags)) != 0) {
            f.S16(value); /* random index */
        } else {
            f.S16(value + offsetof(detail::TextureCurveKey, mNameIndex));
        }
    }
    if (ranges != random) { /* the range table is present */
        f.S16(ranges);
        u16 n = f.N16(ranges);
        for (u16 i = 0; i < n && f.ok; i++) {
            f.S16(ranges + offsetof(detail::TextureCurveRandom, mEntry) + i * sizeof(detail::TextureCurveRandomEntry) +
                  offsetof(detail::TextureCurveKey, mNameIndex));
        }
    }
}

void TextureRandomTable(Fixer& f, u32 table, u32 bytes) {
    if (bytes < 4) {
        return;
    }
    f.S16(table);
    u16 n = f.N16(table);
    for (u16 i = 0; i < n && f.ok; i++) {
        f.S16(table + offsetof(detail::TextureCurveRandomTable, mKey) + i * sizeof(detail::TextureCurveKey) +
              offsetof(detail::TextureCurveKey, mNameIndex));
    }
}

void ChildSetting(Fixer& f, u32 at) {
    f.S16(at + offsetof(detail::ChildCurveSetting, mSetting) + offsetof(EmitterInheritSetting, speed));
    f.S16(at + offsetof(detail::ChildCurveSetting, mNameIndex));
}

void ChildCurve(Fixer& f, u32 keys, u32 random, u32 randomBytes) {
    f.S16(keys);
    u16 count = f.N16(keys);
    for (u16 i = 0; i < count && f.ok; i++) {
        u32 record = keys + offsetof(detail::ChildCurveKeys, mEntry) + i * sizeof(detail::ChildCurveKey);
        f.S16(record + offsetof(detail::ChildCurveKey, mFrame));
        u32 data = record + offsetof(detail::ChildCurveKey, mData);
        if (f.B(record + offsetof(detail::ChildCurveKey, mFlags)) != 0) {
            f.S16(data); /* random index */
        } else {
            ChildSetting(f, data);
        }
    }
    if (randomBytes >= 4) {
        f.S16(random);
        u16 n = f.N16(random);
        for (u16 i = 0; i < n && f.ok; i++) {
            ChildSetting(f, random + offsetof(detail::ChildCurveTable, mEntry) + i * sizeof(detail::ChildCurveSetting));
        }
    }
}

/* A field's parameters, after the tables; the file stores only the first `bytes` bytes
 * (infoTable) of ParticleField. */
void Field(Fixer& f, u32 at, u32 bytes, u8 kind) {
    typedef detail::ParticleField PF;
    const u32 params = offsetof(PF, mParams);
    if (bytes >= offsetof(PF, mPower) + 4) {
        f.S32(at + offsetof(PF, mPower));
    }
    if (kind == PF::RANDOM) {
        if (bytes >= params + offsetof(PF::Parameters::Random, mDiffusion) + 4) {
            f.S32(at + params + offsetof(PF::Parameters::Random, mDiffusion));
        }
        if (bytes >= params + offsetof(PF::Parameters::Random, mInterval) + 2) {
            f.S16(at + params + offsetof(PF::Parameters::Random, mInterval));
        }
    } else if (bytes > params) {
        u32 words = (bytes - params) / 4;
        if (words > sizeof(PF::Parameters) / 4) {
            words = sizeof(PF::Parameters) / 4;
        }
        f.S32(at + params, words);
    }
}

/* One curve of `length` bytes at `at`. Emitter tracks are always float curves. */
void Curve(Fixer& f, u32 at, u32 length, bool emitterTrack) {
    if (length < sizeof(AnimCurveHeader) || !f.In(at, length)) {
        return;
    }
    u8 magic = f.B(at + offsetof(AnimCurveHeader, magic));
    if (magic != NW4R_EF_MAGIC_ANIMCURVE && magic != NW4R_EF_MAGIC_ANIMBAKED) {
        return; /* ef skips these too */
    }
    u8 type = f.B(at + offsetof(AnimCurveHeader, curveFlag));
    u8 kind = f.B(at + offsetof(AnimCurveHeader, kindType));
    u32 channels = ChannelCount(f.B(at + offsetof(AnimCurveHeader, kindEnable)));

    f.S16(at + offsetof(AnimCurveHeader, randomSeed), 3); /* randomSeed, frameLength, padding */
    f.S32(at + offsetof(AnimCurveHeader, keyTable), 5);
    u32 keyBytes = f.N32(at + offsetof(AnimCurveHeader, keyTable));
    u32 rangeBytes = f.N32(at + offsetof(AnimCurveHeader, rangeTable));
    u32 randomBytes = f.N32(at + offsetof(AnimCurveHeader, randomTable));
    u32 nameBytes = f.N32(at + offsetof(AnimCurveHeader, nameTable));
    u32 infoBytes = f.N32(at + offsetof(AnimCurveHeader, infoTable));
    u32 keys = at + sizeof(AnimCurveHeader);
    u32 ranges = keys + keyBytes;
    u32 random = ranges + rangeBytes;
    u32 names = random + randomBytes;
    if (!f.In(keys, keyBytes + rangeBytes + randomBytes + nameBytes)) {
        return;
    }

    CurveData data = CURVE_FLOATS;
    if (!emitterTrack) {
        switch (type) {
        case AC_TYPE_PARTICLE_U8:
            data = CURVE_BYTES;
            break;
        case AC_TYPE_PARTICLE_ROTATE:
            data = CURVE_ROTATION;
            break;
        case AC_TYPE_PARTICLE_TEXTURE:
            if (keyBytes != 0) {
                TextureCurve(f, keys, ranges, rangeBytes != 0 ? random : ranges);
            }
            TextureRandomTable(f, random, randomBytes);
            if (nameBytes != 0) {
                NameTable(f, names);
            }
            return;
        case AC_TYPE_CHILD:
        case AC_TYPE_EMITTER_CHILD:
            /* Emitter-child curves may carry no keys, only an info block that nothing in
             * nw4r::ef reads; it stays as stored. */
            if (keyBytes != 0) {
                ChildCurve(f, keys, random, randomBytes);
            }
            if (nameBytes != 0) {
                NameTable(f, names);
            }
            return;
        case detail::ParticleField::CURVE_TYPE:
            if (keyBytes != 0) {
                ValueKeys(f, keys, channels, CURVE_FLOATS);
                ValueRanges(f, ranges, rangeBytes, channels, CURVE_FLOATS);
                ValueRanges(f, random, randomBytes, channels, CURVE_FLOATS);
            }
            if (f.In(names + nameBytes, infoBytes)) {
                Field(f, names + nameBytes, infoBytes, kind);
            }
            return;
        }
    }

    if (magic == NW4R_EF_MAGIC_ANIMBAKED) {
        /* Baked: frameLength frames of one value per channel. */
        if (data != CURVE_BYTES) {
            u32 frames = f.N16(at + offsetof(AnimCurveHeader, frameLength));
            f.S32(keys, frames * channels);
        }
        return;
    }
    if (keyBytes != 0) {
        ValueKeys(f, keys, channels, data);
    }
    ValueRanges(f, ranges, rangeBytes, channels, data);
    ValueRanges(f, random, randomBytes, channels, data);
}

/* Track tables after the particle description: u16 count, u16 init count, then count
 * u32 slots (filled by LocateTracks) and count u32 sizes. Returns the end. */
u32 TrackTable(Fixer& f, u32 at, u32* count, u32* sizes) {
    f.S16(at, 2);
    *count = f.N16(at);
    f.S32(at + 4, *count * 2);
    *sizes = at + 4 + *count * 4;
    return at + 4 + *count * 8;
}

void EmitterResourceData(Fixer& f, u32 resource) {
    /* EmitterResource: name slot (filled by breffRelocate), headersize. */
    f.S32(resource + 4);
    u32 descSize = f.N32(resource + 4);
    u32 desc = resource + 8;
    EmitterDescription(f, desc);

    u32 ptcl = desc + descSize;
    f.S32(ptcl);
    u32 ptclSize = f.N32(ptcl);
    ParticleDescription(f, ptcl + 4, ptclSize);

    u32 ptclCount, ptclSizes, emitCount, emitSizes;
    u32 emitTable = TrackTable(f, ptcl + 4 + ptclSize, &ptclCount, &ptclSizes);
    u32 tracks = TrackTable(f, emitTable, &emitCount, &emitSizes);
    for (u32 i = 0; i < ptclCount && f.ok; i++) {
        u32 length = f.N32(ptclSizes + i * 4);
        Curve(f, tracks, length, false);
        tracks += length;
    }
    for (u32 i = 0; i < emitCount && f.ok; i++) {
        u32 length = f.N32(emitSizes + i * 4);
        Curve(f, tracks, length, true);
        tracks += length;
    }
}

int State(const void* file, const char* signature) {
    u16 native;
    if (memcmp(file, signature, 4) != 0) {
        return 0;
    }
    memcpy(&native, static_cast<const u8*>(file) + 12, 2);
    if (native == 0x10) {
        return 1;
    }
    return PortBE16(static_cast<const u8*>(file) + 12) == 0x10 ? 2 : 0;
}

}

extern "C" int PortFixupBREFF(void* file) {
    int state = State(file, "REFF");
    if (state != 2) {
        return state;
    }
    Fixer f = {static_cast<u8*>(file), PortBE32(static_cast<u8*>(file) + 8), "breff", true};
    u32 table = Project(f, Header(f));
    Entries(f, table, [&](u32 resource) { EmitterResourceData(f, resource); });
    return f.ok;
}

extern "C" int PortFixupBREFT(void* file) {
    int state = State(file, "REFT");
    if (state != 2) {
        return state;
    }
    Fixer f = {static_cast<u8*>(file), PortBE32(static_cast<u8*>(file) + 8), "breft", true};
    u32 table = Project(f, Header(f));
    Entries(f, table, [&](u32 at) {
        SWAP16(TextureData, width);
        SWAP16(TextureData, height);
        SWAP32(TextureData, dataSize);
        SWAP16(TextureData, tlutEntries);
        SWAP32(TextureData, tlutSize);
        SWAP32(TextureData, lod_bias);
    });
    return f.ok;
}
