/* Byte-order fixup for the stages' collision trees (.bsp, attr_NN / attrk_NN in each
 * stage archive), as CBSPTree reads them (game/world/CBSPTree.h). The file is an array
 * of 24-byte nodes:
 *
 *   node 0   magic 'BSP0', the byte size of the node array, then padding
 *   node 1   bounding box: six floats (minimum x y z, maximum x y z)
 *   node 2+  a plane (normal x y z, distance: four floats), then either four u16 child
 *            indices (0 and 0xFFFF are "none") or, in a leaf, the marker 0xFFFFFFFF and
 *            the u32 offset of the leaf's payload (attribute bytes, left alone)
 *
 * Without it every plane is garbage and Pokémon walk through walls, fall off stages and
 * leave battles they should lose. The magic tells the state: it reads 'BSP0' natively
 * only once the header has been swapped. Hooked in CDividedGround::LoadAttr. */
#include <port_endian.h>
#include <port_os.h>
#include <stdint.h>
#include <string.h>

#define BSP_MAGIC 0x42535030u /* 'BSP0' */
#define BSP_NODE 24u

int PortFixupBSP(void* file, size_t size) {
    uint8_t* b = (uint8_t*)file;
    uint32_t magic, bytes, count, i;
    static int reported;
    if (b == NULL || size < 2 * BSP_NODE) {
        return 0;
    }
    memcpy(&magic, b, 4);
    if (magic == BSP_MAGIC) {
        return 1; /* converted already */
    }
    if (PortBE32(b) != BSP_MAGIC) {
        return 0;
    }
    bytes = PortBE32(b + 4);
    if (bytes < 2 * BSP_NODE || bytes > size || bytes % BSP_NODE != 0) {
        PortRedOnce(&reported, "bsp fixup at %p: node array of 0x%x bytes in a 0x%zx-byte file", file, bytes, size);
        return 0;
    }
    count = bytes / BSP_NODE;
    PortSwap32Array(b, 2);
    PortSwap32Array(b + BSP_NODE, 6);
    for (i = 2; i < count; i++) {
        uint8_t* n = b + i * BSP_NODE;
        PortSwap32Array(n, 4);
        if (n[16] == 0xFF && n[17] == 0xFF && n[18] == 0xFF && n[19] == 0xFF) {
            PortSwap32Array(n + 20, 1);
        } else {
            PortSwap16Array(n + 16, 4);
        }
    }
    return 1;
}
