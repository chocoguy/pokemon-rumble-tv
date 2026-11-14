// Compile-time checks that structs laid over file data keep the Wii's layout on the port.
// Sizes and offsets are the console's (4-byte pointers). Compiled by tests/native/run.sh.
#include <stddef.h>
#include <nw4r/ut/ut_ResFontBase.h>
#include <nw4r/g3d/res/g3d_resmat.h>
#include <nw4r/ef/ef_res_texture.h>
#include <nw4r/ef/ef_res_animcurve.h>
#include <nw4r/ef/ef_particle.h>
#include <revolution/tpl.h>
#include <revolution/gx/GXTypes.h>

#define CHECK_SIZE(T, n) typedef char size_of_##T[sizeof(T) == (n) ? 1 : -1]
#define CHECK_OFFSET(T, m, n) typedef char offset_of_##T##_##m[offsetof(T, m) == (n) ? 1 : -1]

using namespace nw4r;

// ut: font blocks (brfnt)
typedef ut::FontTextureGlyph FontTextureGlyph;
typedef ut::FontWidth FontWidth;
typedef ut::FontCodeMap FontCodeMap;
typedef ut::FontInformation FontInformation;
CHECK_SIZE(FontTextureGlyph, 24);
CHECK_OFFSET(FontTextureGlyph, sheetImage, 20);
CHECK_OFFSET(FontWidth, pNext, 4);
CHECK_OFFSET(FontWidth, widthTable, 8);
CHECK_OFFSET(FontCodeMap, pNext, 8);
CHECK_OFFSET(FontCodeMap, mapInfo, 12);
CHECK_SIZE(FontInformation, 24);
CHECK_OFFSET(FontInformation, pGlyph, 8);
CHECK_OFFSET(FontInformation, pWidth, 12);
CHECK_OFFSET(FontInformation, pMap, 16);
CHECK_OFFSET(FontInformation, height, 20);

// g3d: material texture info (brres)
typedef g3d::ResTexPlttInfoData ResTexPlttInfoData;
CHECK_OFFSET(ResTexPlttInfoData, pTexData, 8);
CHECK_OFFSET(ResTexPlttInfoData, pPlttData, 12);
CHECK_OFFSET(ResTexPlttInfoData, mapID, 16);

// ef: textures (breft), emitters and particle descriptors (breff)
typedef ef::TextureData TextureData;
CHECK_SIZE(TextureData, 64);
CHECK_OFFSET(TextureData, width, 4);
CHECK_OFFSET(TextureData, lod_bias, 24);
CHECK_OFFSET(TextureData, texture, 28);
CHECK_OFFSET(TextureData, tlut, 32);
typedef ef::AnimCurveNameTable::Name AnimCurveName;
CHECK_SIZE(AnimCurveName, 4);
typedef ef::ParticleParameterDesc ParticleParameterDesc;
typedef char particle_textures[sizeof(((ParticleParameterDesc*)0)->mTexture) == 3 * 4 ? 1 : -1];
typedef char particle_texture_wrap[offsetof(ParticleParameterDesc, textureWrap) == offsetof(ParticleParameterDesc, mTexture) + 12 ? 1 : -1];

// SDK: TPL texture palettes
CHECK_SIZE(TPLPalette, 12);
CHECK_SIZE(TPLDescriptor, 8);
CHECK_SIZE(TPLClutHeader, 12);
CHECK_OFFSET(TPLClutHeader, data, 8);
CHECK_SIZE(TPLHeader, 36);
CHECK_OFFSET(TPLHeader, data, 8);
CHECK_OFFSET(TPLHeader, wrapS, 12);

// gx: the SDK's internal object layouts must fit the public opaque structs (GXTexObj is
// also embedded in .brres materials).
CHECK_SIZE(GXTexObj, 32);
typedef char gx_texobj_fits[sizeof(GXTexObjInt) <= sizeof(GXTexObj) ? 1 : -1];
typedef char gx_tlutobj_fits[sizeof(GXTlutObjInt) <= sizeof(GXTlutObj) ? 1 : -1];
typedef char gx_lightobj_fits[sizeof(GXLightObjInt) <= sizeof(GXLightObj) ? 1 : -1];
typedef char gx_texregion_fits[sizeof(GXTexRegionInt) <= sizeof(GXTexRegion) ? 1 : -1];
typedef char gx_tlutregion_fits[sizeof(GXTlutRegionInt) <= sizeof(GXTlutRegion) ? 1 : -1];
typedef char gx_fifo_fits[sizeof(__GXFifoObj) <= sizeof(GXFifoObj) ? 1 : -1];
