/* In-place byte-order fixups for the game's big-endian file formats. Each runs once per
 * loaded buffer and is idempotent: it recognises data it has already converted. */
#ifndef PORT_ENDIAN_H
#define PORT_ENDIAN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

static inline uint16_t PortBE16(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return (uint16_t)((b[0] << 8) | b[1]);
}
static inline uint32_t PortBE32(const void* p) {
    const uint8_t* b = (const uint8_t*)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}
static inline void PortSwap16(void* p) {
    uint16_t v;
    __builtin_memcpy(&v, p, 2);
    v = __builtin_bswap16(v);
    __builtin_memcpy(p, &v, 2);
}
static inline void PortSwap32(void* p) {
    uint32_t v;
    __builtin_memcpy(&v, p, 4);
    v = __builtin_bswap32(v);
    __builtin_memcpy(p, &v, 4);
}
static inline void PortSwap32Array(void* p, size_t count) {
    uint8_t* b = (uint8_t*)p;
    size_t i;
    for (i = 0; i < count; i++) {
        PortSwap32(b + i * 4);
    }
}
static inline void PortSwap16Array(void* p, size_t count) {
    uint8_t* b = (uint8_t*)p;
    size_t i;
    for (i = 0; i < count; i++) {
        PortSwap16(b + i * 2);
    }
}

/* U8 archive (.arc, content files): header and file system table. `size` is how many
 * bytes of the archive are in memory (at least through the end of the table). Returns 0
 * if the buffer is not a U8 archive. */
int PortFixupU8(void* archive, size_t size);

/* NW4R layout family (port/endian/port_nw4r.c). Return 1 when the file is (now) native,
 * 0 when it is not that format or could not be converted. */
int PortFixupBRLYT(void* file);
int PortFixupBRLAN(void* file);
int PortFixupTPL(void* file);
int PortFixupBRFNT(void* file);

/* BMG messages (port/endian/port_bmg.c). */
int PortFixupBMG(void* file);

/* NW4R effects (port/endian/port_ef.cpp). */
int PortFixupBREFF(void* file);
int PortFixupBREFT(void* file);

/* NW4R sound archive: header, SYMB and INFO (port/endian/port_brsar.cpp). */
int PortFixupBRSAR(void* file);
/* .brseq (port_brseq.c): sequence header, DATA and LABL blocks. */
int PortFixupBRSEQ(void* file);
/* The sound archive's wave sounds, banks, wave archives and waves (port_brsnd.cpp). */
int PortFixupRWSD(void* file);
int PortFixupRBNK(void* file);
int PortFixupRWAR(void* file);
int PortFixupRWAV(void* file);

/* NW4R g3d resources: models, textures, animations (.brres, port_brres.cpp). */
int PortFixupBRRES(void* file);

/* The game's compiled scripts (.pkc, port_pkc.c). */
int PortFixupPKC(void* file, size_t size);

/* The stages' collision trees (.bsp, port_bsp.c). */
int PortFixupBSP(void* file, size_t size);

/* rumble --check-assets (port/endian/port_asset_check.c): every content file through its
 * fixup. Returns 0 when all converted formats pass. */
int PortCheckAssets(const char* romDir);

#ifdef __cplusplus
}
#endif

#endif
