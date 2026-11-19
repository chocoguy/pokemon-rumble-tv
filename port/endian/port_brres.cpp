/* Byte-order fixup for NW4R g3d resource files (.brres, 'bres'), as nw4r::g3d reads them.
 * Written against the decomp's structs (nw4r/g3d/res/*.h) and the versions this game's
 * 475 files use: MDL0 v11, TEX0 v3, PLT0 v3, CHR0 v5, CLR0 v4, SRT0 v5, VIS0 v4,
 * SCN0 v5. Another kind or version is reported with [RED] and the file is left alone.
 *
 * What stays as stored: display lists (materials, TEV, shapes), vertex arrays, texture
 * and palette data, which the GPU reads big-endian (port/gx); the model byte code; colours
 * stored as GXColor bytes; strings. Everything else multi-byte is swapped in place.
 *
 * Every name is a string preceded by its 32-bit length. Before a length is swapped it
 * must match the string, which also checks that the offset leading to it was read from
 * the right base. Shared data (names, dictionaries, TEV blocks) is converted once: the
 * fixer records each swapped halfword (port_fixer.h).
 *
 * The file's signature tells its state: 'bres' read natively means converted (it reads
 * 'serb' natively before). The byte-order mark is left as stored, as for the other NW4R
 * formats (port_nw4r.c). Hooked in ResFile's constructor. */
#include <nw4r/g3d/res/g3d_resanmamblight.h>
#include <nw4r/g3d/res/g3d_resanmcamera.h>
#include <nw4r/g3d/res/g3d_resanmchr.h>
#include <nw4r/g3d/res/g3d_resanmclr.h>
#include <nw4r/g3d/res/g3d_resanmfog.h>
#include <nw4r/g3d/res/g3d_resanmlight.h>
#include <nw4r/g3d/res/g3d_resanmscn.h>
#include <nw4r/g3d/res/g3d_resanmtexsrt.h>
#include <nw4r/g3d/res/g3d_resanmvis.h>
#include <nw4r/g3d/res/g3d_resfile.h>
#include <nw4r/g3d/res/g3d_reslightset.h>
#include <nw4r/g3d/res/g3d_resmat.h>
#include <nw4r/g3d/res/g3d_resmdl.h>
#include <nw4r/g3d/res/g3d_resnode.h>
#include <nw4r/g3d/res/g3d_respltt.h>
#include <nw4r/g3d/res/g3d_resshp.h>
#include <nw4r/g3d/res/g3d_restev.h>
#include <nw4r/g3d/res/g3d_restex.h>
#include <nw4r/g3d/res/g3d_resuser.h>
#include <nw4r/g3d/res/g3d_resvtx.h>
#include <port_endian.h>
#include <port_os.h>
#include <stdlib.h>
#include <string.h>
#include "port_fixer.h"

using namespace nw4r::g3d;

namespace {

/* The file layout these offsets assume; a struct that grew on the host would break it. */
typedef char LayoutNode[(sizeof(ResNodeData) == 0xD0) ? 1 : -1];
typedef char LayoutTexPlttInfo[(sizeof(ResTexPlttInfoData) == 0x34) ? 1 : -1];
typedef char LayoutDicNode[(sizeof(ResDicNodeData) == 0x10) ? 1 : -1];
typedef char LayoutTexSrt[(sizeof(TexSrt) == 0x14) ? 1 : -1];
typedef char LayoutTexMtxEffect[(sizeof(TexMtxEffect) == 0x34) ? 1 : -1];

const u32 kBres = 0x62726573u; /* 'bres' */

/* The 32-bit length before the string at `str` (an offset in the file). */
void Name(Fixer& f, u32 str) {
    static int reported;
    if (str < 4 || !f.In(str - 4, 4) || f.Done(str - 4)) {
        return;
    }
    u32 len = PortBE32(f.base + str - 4);
    if (len > 0x400 || !f.In(str, len + 1) || f.base[str + len] != 0 || memchr(f.base + str, 0, len) != NULL) {
        PortRedOnce(&reported, "brres fixup at %p: no name at 0x%x (length word 0x%x)", f.base, str, len);
        return;
    }
    f.S32(str - 4);
}

/* A name field: an offset from `base` to the string, 0 for none. */
void NameField(Fixer& f, u32 base, u32 field) {
    f.S32(field);
    s32 ofs = static_cast<s32>(f.N32(field));
    if (ofs != 0) {
        Name(f, base + ofs);
    }
}

/* A ResDic: size, count, then count + 1 nodes (the first is the root). Names and data
 * are relative to the dictionary. `entry` gets each data offset. */
template <typename Entry> void Dict(Fixer& f, u32 at, Entry entry) {
    if (at == 0) {
        return;
    }
    f.S32(at, 2);
    u32 count = f.N32(at + 4);
    for (u32 i = 0; i <= count && f.ok; i++) {
        u32 node = at + 8 + i * sizeof(ResDicNodeData);
        f.S16(node, 4);
        f.S32(node + 8, 2);
        s32 str = static_cast<s32>(f.N32(node + offsetof(ResDicNodeData, ofsString)));
        s32 data = static_cast<s32>(f.N32(node + offsetof(ResDicNodeData, ofsData)));
        if (str != 0) {
            Name(f, at + str);
        }
        if (i != 0 && data != 0) {
            entry(at + data);
        }
    }
}

/* The target of an offset field at `base + field`, relative to `base` (0 for none). */
u32 Target(Fixer& f, u32 base, size_t field) {
    s32 ofs = static_cast<s32>(f.N32(base + static_cast<u32>(field)));
    return ofs != 0 ? base + ofs : 0;
}

/* An offset field relative to `base`, swapped; returns the target (0 for none). */
u32 Ofs(Fixer& f, u32 base, u32 field) {
    f.S32(field);
    s32 ofs = static_cast<s32>(f.N32(field));
    return ofs != 0 ? base + ofs : 0;
}

void UserData(Fixer& f, u32 at) {
    if (at == 0) {
        return;
    }
    f.S32(at);
    Dict(f, at + offsetof(ResUserDataData, dict), [&](u32 e) {
        f.S32(e, 4); /* size, toData, numData, type */
        u32 type = f.N32(e + offsetof(ResUserDataEntryData, type));
        u32 n = f.N32(e + offsetof(ResUserDataEntryData, numData));
        s32 to = static_cast<s32>(f.N32(e + offsetof(ResUserDataEntryData, toData)));
        if (to != 0 && type != ResUserDataEntry::TYPE_STRING) {
            f.S32(e + to, n);
        }
    });
}

/* ResAnmData: a constant float, or an offset from the field to key frames. */
void AnmValue(Fixer& f, u32 at, bool constant) {
    f.S32(at);
    if (constant) {
        return;
    }
    s32 ofs = static_cast<s32>(f.N32(at));
    if (ofs == 0) {
        return;
    }
    u32 k = at + ofs;
    f.S16(k);
    f.S32(k + offsetof(ResKeyFrameAnmData, invKeyFrameRange));
    u32 n = f.N16(k);
    f.S32(k + offsetof(ResKeyFrameAnmData, keyFrames), n * 3);
}

/* ResColorAnmData: a constant colour, or an offset from the field to one per frame. */
void AnmColor(Fixer& f, u32 at, bool constant, u32 numFrame) {
    f.S32(at);
    if (constant) {
        return;
    }
    s32 ofs = static_cast<s32>(f.N32(at));
    if (ofs != 0) {
        f.S32(at + ofs, numFrame + 1);
    }
}

/* ResBoolAnmData: an offset from the field to one bit per frame. */
void AnmBool(Fixer& f, u32 at, bool constant, u32 numFrame) {
    f.S32(at);
    if (constant) {
        return;
    }
    s32 ofs = static_cast<s32>(f.N32(at));
    if (ofs != 0) {
        f.S32(at + ofs, (numFrame + 1 + 31) / 32);
    }
}

/* The common head of a sub-file: kind (chars), size, then `words` 32-bit fields. */
void SubHeader(Fixer& f, u32 at, u32 words) {
    f.S32(at + offsetof(ResBlockHeaderData, size));
    f.S32(at + 8, words);
}

/* ---------------------------------------------------------------------------------- */
/* MDL0                                                                                 */

void Node(Fixer& f, u32 at) {
    f.S32(at, sizeof(ResNodeData) / 4);
    Name(f, at + static_cast<s32>(f.N32(at + offsetof(ResNodeData, name))));
    s32 user = static_cast<s32>(f.N32(at + offsetof(ResNodeData, toResUserData)));
    if (user != 0) {
        UserData(f, at + user);
    }
}

/* The vertex array headers: seven words, then two bytes and a count; some have more. */
void VtxHeader(Fixer& f, u32 at) {
    f.S32(at, 7);
    Name(f, at + static_cast<s32>(f.N32(at + offsetof(ResVtxPosData, name))));
    f.S16(at + 0x1E);
}

void VtxPos(Fixer& f, u32 at) {
    VtxHeader(f, at);
    f.S32(at + offsetof(ResVtxPosData, min), 6);
}

void VtxTexCoord(Fixer& f, u32 at) {
    VtxHeader(f, at);
    f.S32(at + offsetof(ResVtxTexCoordData, min), 4);
}

void VtxFurVec(Fixer& f, u32 at) {
    f.S32(at, 5);
    Name(f, at + static_cast<s32>(f.N32(at + offsetof(ResVtxFurVecData, name))));
    f.S16(at + offsetof(ResVtxFurVecData, numFurVec));
}

void VtxFurPos(Fixer& f, u32 at) {
    VtxHeader(f, at);
    f.S32(at + offsetof(ResVtxFurPosData, numLayer), 2);
}

void Tev(Fixer& f, u32 at) {
    f.S32(at, 3); /* size, toResMdlData, id; the rest is bytes */
}

void TexPlttInfo(Fixer& f, u32 at) {
    f.S32(at, 12); /* names, two pointer slots, map, tlut, wraps, filters, bias, aniso */
    NameField(f, at, at + offsetof(ResTexPlttInfoData, nameTex));
    NameField(f, at, at + offsetof(ResTexPlttInfoData, namePltt));
}

void Mat(Fixer& f, u32 at) {
    typedef ResMatData M;
    f.S32(at, 5); /* size, toResMdlData, name, id, flag */
    Name(f, at + static_cast<s32>(f.N32(at + offsetof(M, name))));
    f.S32(at + offsetof(M, genMode) + offsetof(ResGenModeData, cullMode));
    f.S32(at + offsetof(M, toResTevData), 6);
    f.S32(at + offsetof(M, texObjData) + offsetof(ResTexObjData, flagUsedTexMapID));
    f.S32(at + offsetof(M, tlutObjData) + offsetof(ResTlutObjData, flagUsedTlutID));

    u32 srt = at + offsetof(M, texSrtData);
    f.S32(srt + offsetof(ResTexSrtData, flag), 2);
    f.S32(srt + offsetof(ResTexSrtData, texSrt), ResTexSrtData::NUM_OF_TEXTURE * 5);
    for (int i = 0; i < ResTexSrtData::NUM_OF_TEXTURE; i++) {
        f.S32(srt + offsetof(ResTexSrtData, effect) + i * sizeof(TexMtxEffect) + offsetof(TexMtxEffect, effectMtx), 12);
    }
    for (int i = 0; i < 2; i++) {
        u32 chan = at + offsetof(M, chan) + i * sizeof(Chan);
        f.S32(chan + offsetof(Chan, flag));
        f.S32(chan + offsetof(Chan, paramChanCtrlC), 2);
    }

    u32 tev = Ofs(f, at, at + offsetof(M, toResTevData));
    if (tev != 0) {
        Tev(f, tev);
    }
    u32 n = f.N32(at + offsetof(M, numResTexPlttInfo));
    u32 infos = Ofs(f, at, at + offsetof(M, toResTexPlttInfo));
    for (u32 i = 0; infos != 0 && i < n && f.ok; i++) {
        TexPlttInfo(f, infos + i * sizeof(ResTexPlttInfoData));
    }
    u32 fur = Ofs(f, at, at + offsetof(M, toResMatFurData));
    if (fur != 0) {
        f.S32(fur, sizeof(ResMatFurData) / 4);
    }
    UserData(f, Ofs(f, at, at + offsetof(M, toResUserData)));
}

void Shp(Fixer& f, u32 at) {
    typedef ResShpData S;
    f.S32(at, 3); /* size, toResMdlData, curMtxIdx; then the vertex descriptor cache (bytes) */
    f.S32(at + offsetof(S, tagPrePrimDL), 3);
    f.S32(at + offsetof(S, tagPrimDL), 3);
    f.S32(at + offsetof(S, vcdBitmap), 6); /* vcdBitmap, flag, name, id, numVtx, numPolygon */
    Name(f, at + static_cast<s32>(f.N32(at + offsetof(S, name))));
    f.S16(at + offsetof(S, idVtxPosition), (offsetof(S, toMtxSetUsed) - offsetof(S, idVtxPosition)) / 2);
    u32 msu = Ofs(f, at, at + offsetof(S, toMtxSetUsed));
    if (msu != 0) {
        f.S32(msu);
        f.S16(msu + offsetof(ResMtxSetUsed, vecMtxID), f.N32(msu));
    }
}

/* ResTexPlttInfoOffsetData: count, four unknown bytes, then {offset, four bytes}. */
void TexPlttLinks(Fixer& f, u32 at) {
    f.S32(at);
    u32 n = f.N32(at);
    for (u32 i = 0; i < n && f.ok; i++) {
        u32 entry = at + 8 + i * 8;
        f.S32(entry);
        u32 info = at + static_cast<s32>(f.N32(entry));
        TexPlttInfo(f, info);
    }
}

void Mdl(Fixer& f, u32 at) {
    typedef ResMdlData M;
    SubHeader(f, at, (offsetof(M, info) - offsetof(M, revision)) / 4);
    Name(f, at + static_cast<s32>(f.N32(at + offsetof(M, name))));

    u32 info = at + offsetof(M, info);
    f.S32(info, 8); /* size, toResMdlData, scaling, tex mtx mode, vertex/triangle sizes, path, numViewMtx */
    f.S32(info + offsetof(ResMdlInfoData, toMtxIDToNodeID), 7);
    s32 path = static_cast<s32>(f.N32(info + offsetof(ResMdlInfoData, original_path)));
    if (path != 0) {
        Name(f, info + path);
    }
    s32 ids = static_cast<s32>(f.N32(info + offsetof(ResMdlInfoData, toMtxIDToNodeID)));
    if (ids != 0) {
        f.S32(info + ids);
        f.S32(info + ids + 4, f.N32(info + ids));
    }

    Dict(f, Target(f, at, offsetof(M, toResByteCodeDic)), [](u32) {});
    Dict(f, Target(f, at, offsetof(M, toResNodeDic)), [&](u32 e) { Node(f, e); });
    Dict(f, Target(f, at, offsetof(M, toResVtxPosDic)), [&](u32 e) { VtxPos(f, e); });
    Dict(f, Target(f, at, offsetof(M, toResVtxNrmDic)), [&](u32 e) { VtxHeader(f, e); });
    Dict(f, Target(f, at, offsetof(M, toResVtxClrDic)), [&](u32 e) { VtxHeader(f, e); });
    Dict(f, Target(f, at, offsetof(M, toResVtxTexCoordDic)), [&](u32 e) { VtxTexCoord(f, e); });
    Dict(f, Target(f, at, offsetof(M, toResVtxFurVecDic)), [&](u32 e) { VtxFurVec(f, e); });
    Dict(f, Target(f, at, offsetof(M, toResVtxFurPosDic)), [&](u32 e) { VtxFurPos(f, e); });
    Dict(f, Target(f, at, offsetof(M, toResMatDic)), [&](u32 e) { Mat(f, e); });
    Dict(f, Target(f, at, offsetof(M, toResTevDic)), [&](u32 e) { Tev(f, e); });
    Dict(f, Target(f, at, offsetof(M, toResShpDic)), [&](u32 e) { Shp(f, e); });
    Dict(f, Target(f, at, offsetof(M, toResTexNameToTexPlttInfoDic)),
         [&](u32 e) { TexPlttLinks(f, e); });
    Dict(f, Target(f, at, offsetof(M, toResPlttNameToTexPlttInfoDic)),
         [&](u32 e) { TexPlttLinks(f, e); });
    s32 user = static_cast<s32>(f.N32(at + offsetof(M, toResUserData)));
    if (user != 0) {
        UserData(f, at + user);
    }
}

/* ---------------------------------------------------------------------------------- */
/* TEX0, PLT0                                                                           */

void Tex(Fixer& f, u32 at) {
    typedef ResTexData T;
    SubHeader(f, at, 5); /* revision, toResFileData, toTexData, name, flag */
    f.S16(at + offsetof(T, width), 2);
    f.S32(at + offsetof(T, fmt), 6); /* fmt, mipmap_level, min_lod, max_lod, path, user */
    Name(f, at + static_cast<s32>(f.N32(at + offsetof(T, name))));
    s32 path = static_cast<s32>(f.N32(at + offsetof(T, original_path)));
    if (path != 0) {
        Name(f, at + path);
    }
    s32 user = static_cast<s32>(f.N32(at + offsetof(T, toResUserData)));
    if (user != 0) {
        UserData(f, at + user);
    }
}

void Pltt(Fixer& f, u32 at) {
    typedef ResPlttData P;
    SubHeader(f, at, 5); /* revision, toResFileData, toPlttData, name, fmt */
    f.S16(at + offsetof(P, numEntries), 2);
    f.S32(at + offsetof(P, original_path), 2);
    Name(f, at + static_cast<s32>(f.N32(at + offsetof(P, name))));
    s32 path = static_cast<s32>(f.N32(at + offsetof(P, original_path)));
    if (path != 0) {
        Name(f, at + path);
    }
    s32 user = static_cast<s32>(f.N32(at + offsetof(P, toResUserData)));
    if (user != 0) {
        UserData(f, at + user);
    }
}

/* The animation sub-files share: header, revision, toResFileData, toDataDic,
 * [extra offsets], toResUserData, name, original_path. Returns the data dictionary. */
u32 AnmHeader(Fixer& f, u32 at, u32 words, size_t nameField, size_t pathField, size_t dicField, size_t userField) {
    SubHeader(f, at, words);
    Name(f, at + static_cast<s32>(f.N32(at + nameField)));
    s32 path = static_cast<s32>(f.N32(at + pathField));
    if (path != 0) {
        Name(f, at + path);
    }
    s32 user = static_cast<s32>(f.N32(at + userField));
    if (user != 0) {
        UserData(f, at + user);
    }
    s32 dic = static_cast<s32>(f.N32(at + dicField));
    return dic != 0 ? at + dic : 0;
}

/* ---------------------------------------------------------------------------------- */
/* CHR0                                                                                 */

enum ChrFmt { FMT_CONST, FMT_FVS32, FMT_FVS48, FMT_FVS96, FMT_CV8, FMT_CV16, FMT_CV32 };

void ChrAnm(Fixer& f, u32 node, u32 slot, ChrFmt fmt, bool constant, u32 numFrame) {
    f.S32(slot);
    if (fmt == FMT_CONST || constant) {
        return;
    }
    s32 ofs = static_cast<s32>(f.N32(slot));
    if (ofs == 0) {
        return;
    }
    u32 d = node + ofs;
    switch (fmt) {
    case FMT_FVS32:
    case FMT_FVS48:
    case FMT_FVS96: {
        f.S16(d);
        f.S32(d + offsetof(ResAnmChrFVSData, invKeyFrameRange));
        u32 n = f.N16(d);
        u32 v = d + offsetof(ResAnmChrFVSData, fvs32);
        if (fmt == FMT_FVS32) {
            f.S32(v, 2 + n); /* scale, offset, packed frame/value/slope words */
        } else if (fmt == FMT_FVS48) {
            f.S32(v, 2);
            f.S16(v + 8, n * 3);
        } else {
            f.S32(v, n * 3);
        }
        break;
    }
    case FMT_CV8:
        f.S32(d, 2); /* scale, offset; then bytes */
        break;
    case FMT_CV16:
        f.S32(d, 2);
        f.S16(d + 8, numFrame + 1);
        break;
    case FMT_CV32:
        f.S32(d, numFrame + 1);
        break;
    default:
        break;
    }
}

ChrFmt FvsFmt(u32 bits) {
    return bits == 1 ? FMT_FVS32 : bits == 2 ? FMT_FVS48 : bits == 3 ? FMT_FVS96 : FMT_CONST;
}

void ChrNode(Fixer& f, u32 at, u32 numFrame) {
    typedef ResAnmChrNodeData N;
    f.S32(at, 2); /* name, flags */
    Name(f, at + static_cast<s32>(f.N32(at)));
    u32 flags = f.N32(at + offsetof(N, flags));
    u32 slot = at + offsetof(N, anms);
    if (flags & N::FLAG_HAS_SCALE) {
        ChrFmt fmt = FvsFmt((flags >> 25) & 3);
        ChrAnm(f, at, slot, fmt, flags & N::FLAG_SCALE_X_CONST, numFrame);
        slot += 4;
        if (!(flags & N::FLAG_SCALE_UNIFORM)) {
            ChrAnm(f, at, slot, fmt, flags & N::FLAG_SCALE_Y_CONST, numFrame);
            ChrAnm(f, at, slot + 4, fmt, flags & N::FLAG_SCALE_Z_CONST, numFrame);
            slot += 8;
        }
    }
    if (flags & N::FLAG_HAS_ROT) {
        ChrFmt fmt;
        switch (flags & N::FLAG_ROT_FMT_MASK) {
        case N::FLAG_ROT_FVS32_FMT: fmt = FMT_FVS32; break;
        case N::FLAG_ROT_FVS48_FMT: fmt = FMT_FVS48; break;
        case N::FLAG_ROT_FVS96_FMT: fmt = FMT_FVS96; break;
        case N::FLAG_ROT_CV8_FMT: fmt = FMT_CV8; break;
        case N::FLAG_ROT_CV16_FMT: fmt = FMT_CV16; break;
        case N::FLAG_ROT_CV32_FMT: fmt = FMT_CV32; break;
        default: fmt = FMT_CONST; break;
        }
        ChrAnm(f, at, slot, fmt, flags & N::FLAG_ROT_X_CONST, numFrame);
        ChrAnm(f, at, slot + 4, fmt, flags & N::FLAG_ROT_Y_CONST, numFrame);
        ChrAnm(f, at, slot + 8, fmt, flags & N::FLAG_ROT_Z_CONST, numFrame);
        slot += 12;
    }
    if (flags & N::FLAG_HAS_TRANS) {
        ChrFmt fmt = FvsFmt((flags >> 30) & 3);
        ChrAnm(f, at, slot, fmt, flags & N::FLAG_TRANS_X_CONST, numFrame);
        ChrAnm(f, at, slot + 4, fmt, flags & N::FLAG_TRANS_Y_CONST, numFrame);
        ChrAnm(f, at, slot + 8, fmt, flags & N::FLAG_TRANS_Z_CONST, numFrame);
    }
}

void Chr(Fixer& f, u32 at) {
    typedef ResAnmChrData A;
    u32 dic = AnmHeader(f, at, (offsetof(A, info) - offsetof(A, revision)) / 4, offsetof(A, name),
                        offsetof(A, original_path), offsetof(A, toChrDataDic), offsetof(A, toResUserData));
    u32 info = at + offsetof(A, info);
    f.S16(info, 2);
    f.S32(info + offsetof(ResAnmChrInfoData, policy), 2);
    u32 numFrame = f.N16(info);
    Dict(f, dic, [&](u32 e) { ChrNode(f, e, numFrame); });
}

/* ---------------------------------------------------------------------------------- */
/* CLR0, SRT0, VIS0                                                                     */

void Clr(Fixer& f, u32 at) {
    typedef ResAnmClrData A;
    u32 dic = AnmHeader(f, at, (offsetof(A, info) - offsetof(A, revision)) / 4, offsetof(A, name),
                        offsetof(A, original_path), offsetof(A, toClrDataDic), offsetof(A, toResUserData));
    u32 info = at + offsetof(A, info);
    f.S16(info, 2);
    f.S32(info + offsetof(ResAnmClrInfoData, policy));
    u32 numFrame = f.N16(info);
    Dict(f, dic, [&](u32 m) {
        f.S32(m, 2); /* name, flags */
        Name(f, m + static_cast<s32>(f.N32(m)));
        u32 flags = f.N32(m + offsetof(ResAnmClrMatData, flags));
        u32 anm = m + offsetof(ResAnmClrMatData, anms);
        for (int i = 0; i < ClrAnmResult::CLA_MAX; i++, flags >>= ResAnmClrMatData::NUM_OF_FLAGS) {
            if (!(flags & ResAnmClrMatData::FLAG_ANM_EXISTS)) {
                continue;
            }
            f.S32(anm + offsetof(ResAnmClrAnmData, mask));
            AnmColor(f, anm + offsetof(ResAnmClrAnmData, color), flags & ResAnmClrMatData::FLAG_ANM_CONSTANT, numFrame);
            anm += sizeof(ResAnmClrAnmData);
        }
    });
}

void SrtTex(Fixer& f, u32 at) {
    typedef ResAnmTexSrtTexData T;
    f.S32(at);
    u32 flags = f.N32(at);
    u32 slot = at + offsetof(T, anms);
    if (!(flags & T::FLAG_SCALE_ONE)) {
        AnmValue(f, slot, flags & T::FLAG_SCALE_U_CONST);
        slot += 4;
        if (!(flags & T::FLAG_SCALE_UNIFORM)) {
            AnmValue(f, slot, flags & T::FLAG_SCALE_V_CONST);
            slot += 4;
        }
    }
    if (!(flags & T::FLAG_ROT_ZERO)) {
        AnmValue(f, slot, flags & T::FLAG_ROT_CONST);
        slot += 4;
    }
    if (!(flags & T::FLAG_TRANS_ZERO)) {
        AnmValue(f, slot, flags & T::FLAG_TRANS_U_CONST);
        AnmValue(f, slot + 4, flags & T::FLAG_TRANS_V_CONST);
    }
}

void Srt(Fixer& f, u32 at) {
    typedef ResAnmTexSrtData A;
    u32 dic = AnmHeader(f, at, (offsetof(A, info) - offsetof(A, revision)) / 4, offsetof(A, name),
                        offsetof(A, original_path), offsetof(A, toTexSrtDataDic), offsetof(A, toResUserData));
    u32 info = at + offsetof(A, info);
    f.S16(info, 2);
    f.S32(info + offsetof(ResAnmTexSrtInfoData, texMtxMode), 2);
    Dict(f, dic, [&](u32 m) {
        typedef ResAnmTexSrtMatData M;
        f.S32(m, 3); /* name, flags, indFlags */
        Name(f, m + static_cast<s32>(f.N32(m)));
        u32 n = __builtin_popcount(f.N32(m + offsetof(M, flags))) + __builtin_popcount(f.N32(m + offsetof(M, indFlags)));
        u32 list = m + offsetof(M, toResAnmTexSrtTexData);
        for (u32 i = 0; i < n && f.ok; i++) {
            u32 tex = Ofs(f, m, list + i * 4);
            if (tex != 0) {
                SrtTex(f, tex);
            }
        }
    });
}

void Vis(Fixer& f, u32 at) {
    typedef ResAnmVisData A;
    u32 dic = AnmHeader(f, at, (offsetof(A, info) - offsetof(A, revision)) / 4, offsetof(A, name),
                        offsetof(A, original_path), offsetof(A, toVisDataDic), offsetof(A, toResUserData));
    u32 info = at + offsetof(A, info);
    f.S16(info, 2);
    f.S32(info + offsetof(ResAnmVisInfoData, policy));
    u32 numFrame = f.N16(info);
    Dict(f, dic, [&](u32 n) {
        f.S32(n, 2); /* name, flags */
        Name(f, n + static_cast<s32>(f.N32(n)));
        if (!(f.N32(n + offsetof(ResAnmVisAnmData, flags)) & ResAnmVisAnmData::FLAG_CONST)) {
            f.S32(n + offsetof(ResAnmVisAnmData, visibility), (numFrame + 1 + 31) / 32);
        }
    });
}

/* ---------------------------------------------------------------------------------- */
/* SCN0                                                                                 */

void ScnLightSet(Fixer& f, u32 at) {
    typedef ResLightSetData L;
    f.S32(at, 6); /* size, toResAnmScnData, name, id, refNumber, ambLightName */
    f.S16(at + offsetof(L, ambLightId));
    f.S32(at + offsetof(L, lightNames), G3DState::NUM_LIGHT_IN_LIGHT_SET);
    f.S16(at + offsetof(L, lightId), G3DState::NUM_LIGHT_IN_LIGHT_SET);
    Name(f, at + static_cast<s32>(f.N32(at + offsetof(L, name))));
    s32 amb = static_cast<s32>(f.N32(at + offsetof(L, ambLightName)));
    if (amb != 0) {
        Name(f, at + amb);
    }
    for (int i = 0; i < G3DState::NUM_LIGHT_IN_LIGHT_SET; i++) {
        s32 light = static_cast<s32>(f.N32(at + offsetof(L, lightNames) + i * 4));
        if (light != 0) {
            Name(f, at + offsetof(L, lightNames) + light); /* relative to the array */
        }
    }
}

void ScnAmbLight(Fixer& f, u32 at, u32 numFrame) {
    typedef ResAnmAmbLightData L;
    f.S32(at, 6); /* size .. flags */
    Name(f, at + static_cast<s32>(f.N32(at + offsetof(L, name))));
    AnmColor(f, at + offsetof(L, color), f.N32(at + offsetof(L, flags)) & L::FLAG_CONST, numFrame);
}

void ScnLight(Fixer& f, u32 at, u32 numFrame) {
    typedef ResAnmLightData L;
    f.S32(at, 8); /* size .. flags */
    Name(f, at + static_cast<s32>(f.N32(at + offsetof(L, name))));
    u32 flags = f.N32(at + offsetof(L, flags));
    AnmBool(f, at + offsetof(L, enable), flags & L::FLAG_ENABLE_CONST, numFrame);
    AnmValue(f, at + offsetof(L, posX), flags & L::FLAG_POS_X_CONST);
    AnmValue(f, at + offsetof(L, posY), flags & L::FLAG_POS_Y_CONST);
    AnmValue(f, at + offsetof(L, posZ), flags & L::FLAG_POS_Z_CONST);
    AnmColor(f, at + offsetof(L, color), flags & L::FLAG_COLOR_CONST, numFrame);
    AnmValue(f, at + offsetof(L, aimX), flags & L::FLAG_AIM_X_CONST);
    AnmValue(f, at + offsetof(L, aimY), flags & L::FLAG_AIM_Y_CONST);
    AnmValue(f, at + offsetof(L, aimZ), flags & L::FLAG_AIM_Z_CONST);
    f.S32(at + offsetof(L, distFunc));
    AnmValue(f, at + offsetof(L, refDistance), flags & L::FLAG_REF_DISTANCE_CONST);
    AnmValue(f, at + offsetof(L, refBrightness), flags & L::FLAG_REF_BRIGHTNESS_CONST);
    f.S32(at + offsetof(L, spotFunc));
    AnmValue(f, at + offsetof(L, cutoff), flags & L::FLAG_CUTOFF_CONST);
    AnmColor(f, at + offsetof(L, specColor), flags & L::FLAG_SPEC_COLOR_CONST, numFrame);
    AnmValue(f, at + offsetof(L, shininess), flags & L::FLAG_SHININESS_CONST);
    s32 user = static_cast<s32>(f.N32(at + offsetof(L, toResUserData)));
    if (user != 0) {
        UserData(f, at + user);
    }
}

void ScnFog(Fixer& f, u32 at, u32 numFrame) {
    typedef ResAnmFogData L;
    f.S32(at, 7); /* size .. flags, type */
    Name(f, at + static_cast<s32>(f.N32(at + offsetof(L, name))));
    u32 flags = f.N32(at + offsetof(L, flags));
    AnmValue(f, at + offsetof(L, startz), flags & L::FLAG_START_CONST);
    AnmValue(f, at + offsetof(L, endz), flags & L::FLAG_END_CONST);
    AnmColor(f, at + offsetof(L, color), flags & L::FLAG_COLOR_CONST, numFrame);
}

void ScnCamera(Fixer& f, u32 at) {
    typedef ResAnmCameraData C;
    f.S32(at, 8); /* size .. toResUserData */
    Name(f, at + static_cast<s32>(f.N32(at + offsetof(C, name))));
    u32 flags = f.N32(at + offsetof(C, flags));
    AnmValue(f, at + offsetof(C, posX), flags & C::FLAG_POS_X_CONST);
    AnmValue(f, at + offsetof(C, posY), flags & C::FLAG_POS_Y_CONST);
    AnmValue(f, at + offsetof(C, posZ), flags & C::FLAG_POS_Z_CONST);
    AnmValue(f, at + offsetof(C, aspect), flags & C::FLAG_ASPECT_CONST);
    AnmValue(f, at + offsetof(C, near), flags & C::FLAG_NEAR_CONST);
    AnmValue(f, at + offsetof(C, far), flags & C::FLAG_FAR_CONST);
    AnmValue(f, at + offsetof(C, rotX), flags & C::FLAG_ROT_X_CONST);
    AnmValue(f, at + offsetof(C, rotY), flags & C::FLAG_ROT_Y_CONST);
    AnmValue(f, at + offsetof(C, rotZ), flags & C::FLAG_ROT_Z_CONST);
    AnmValue(f, at + offsetof(C, aimX), flags & C::FLAG_AIM_X_CONST);
    AnmValue(f, at + offsetof(C, aimY), flags & C::FLAG_AIM_Y_CONST);
    AnmValue(f, at + offsetof(C, aimZ), flags & C::FLAG_AIM_Z_CONST);
    AnmValue(f, at + offsetof(C, twist), flags & C::FLAG_TWIST_CONST);
    AnmValue(f, at + offsetof(C, perspFovy), flags & C::FLAG_PERSP_FOVY_CONST);
    AnmValue(f, at + offsetof(C, orthoHeight), flags & C::FLAG_ORTHO_HEIGHT_CONST);
    s32 user = static_cast<s32>(f.N32(at + offsetof(C, toResUserData)));
    if (user != 0) {
        UserData(f, at + user);
    }
}

void Scn(Fixer& f, u32 at) {
    typedef ResAnmScnData A;
    u32 dic = AnmHeader(f, at, (offsetof(A, info) - offsetof(A, revision)) / 4, offsetof(A, name),
                        offsetof(A, original_path), offsetof(A, toScnTopLevelDic), offsetof(A, toResUserData));
    u32 info = at + offsetof(A, info);
    f.S16(info, 2);
    f.S32(info + offsetof(ResAnmScnInfoData, policy));
    f.S16(info + offsetof(ResAnmScnInfoData, numResLightSetData), 5);
    u32 numFrame = f.N16(info);
    /* The top-level dictionary names the kinds ("LightSet(NW4R)", ...); each entry is a
     * dictionary of that kind. */
    if (dic == 0) {
        return;
    }
    Dict(f, dic, [](u32) {});
    u32 count = f.N32(dic + 4);
    for (u32 i = 1; i <= count && f.ok; i++) {
        u32 node = dic + 8 + i * sizeof(ResDicNodeData);
        u32 nameAt = dic + static_cast<s32>(f.N32(node + offsetof(ResDicNodeData, ofsString)));
        u32 folder = dic + static_cast<s32>(f.N32(node + offsetof(ResDicNodeData, ofsData)));
        const char* name = reinterpret_cast<const char*>(f.base + nameAt);
        if (!f.In(nameAt, 8)) {
            continue;
        }
        if (strncmp(name, "LightSet", 8) == 0) {
            Dict(f, folder, [&](u32 e) { ScnLightSet(f, e); });
        } else if (strncmp(name, "AmbLights", 9) == 0) {
            Dict(f, folder, [&](u32 e) { ScnAmbLight(f, e, numFrame); });
        } else if (strncmp(name, "Lights", 6) == 0) {
            Dict(f, folder, [&](u32 e) { ScnLight(f, e, numFrame); });
        } else if (strncmp(name, "Fogs", 4) == 0) {
            Dict(f, folder, [&](u32 e) { ScnFog(f, e, numFrame); });
        } else if (strncmp(name, "Cameras", 7) == 0) {
            Dict(f, folder, [&](u32 e) { ScnCamera(f, e); });
        }
    }
}

/* ---------------------------------------------------------------------------------- */

struct Kind {
    const char* tag;
    u32 version;
    void (*fix)(Fixer& f, u32 at);
};

const Kind kKinds[] = {
    {"MDL0", 11, Mdl}, {"TEX0", 3, Tex}, {"PLT0", 3, Pltt}, {"CHR0", 5, Chr},
    {"CLR0", 4, Clr},  {"SRT0", 5, Srt}, {"VIS0", 4, Vis},  {"SCN0", 5, Scn},
};

void SubFile(Fixer& f, u32 at) {
    static int reported;
    if (!f.In(at, 12)) {
        return;
    }
    u32 version = PortBE32(f.base + at + 8);
    for (size_t i = 0; i < sizeof(kKinds) / sizeof(kKinds[0]); i++) {
        if (memcmp(f.base + at, kKinds[i].tag, 4) == 0) {
            if (version != kKinds[i].version) {
                break;
            }
            kKinds[i].fix(f, at);
            return;
        }
    }
    PortRedOnce(&reported, "brres fixup at %p: %.4s version %u at 0x%x is not handled", f.base,
                reinterpret_cast<const char*>(f.base + at), version, at);
    f.ok = false;
}

}

extern "C" int PortFixupBRRES(void* file) {
    u8* base = static_cast<u8*>(file);
    u32 native;
    if (base == NULL) {
        return 0;
    }
    memcpy(&native, base, 4);
    if (native == kBres) {
        return 1;
    }
    if (PortBE32(base) != kBres) {
        return 0;
    }
    u32 size = PortBE32(base + 8);
    Fixer f = {base, size, static_cast<u8*>(calloc(size / 16 + 1, 1)), true, "brres"};
    f.S32(0);
    f.S16(6);
    f.S32(8);
    f.S16(12, 2);
    u32 root = f.N16(12); /* header size: the root block follows */
    f.S32(root, 2);       /* 'root', size */
    u32 top = root + offsetof(ResTopLevelDictData, topLevel);
    Dict(f, top, [&](u32 folder) { Dict(f, folder, [&](u32 sub) { SubFile(f, sub); }); });
    free(f.done);
    return f.ok;
}
