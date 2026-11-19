/* Texture decode (port/gx/port_gx_texture.c): one hand-built block per GX format, checked
 * texel by texel against the format's documented layout. The fork's
 * tools/assets/gx_texture.py only encodes I4; the I4 case uses its packing (left texel in
 * the high nibble, 8x8 tiles, rows of 4 bytes). */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

uint32_t PortGXTextureSize(int format, int width, int height);
int PortGXDecodeTexture(uint8_t* out, const uint8_t* src, int format, int width, int height, const uint8_t* tlut,
                        int tlutFormat);

static int sFailures;

static void Expect(const char* what, const uint8_t* out, int width, int x, int y, int r, int g, int b, int a) {
    const uint8_t* p = out + 4 * (y * width + x);
    if (p[0] != r || p[1] != g || p[2] != b || p[3] != a) {
        printf("FAIL %s (%d,%d): got %d %d %d %d, want %d %d %d %d\n", what, x, y, p[0], p[1], p[2], p[3], r, g, b, a);
        sFailures++;
    }
}

int main(void) {
    uint8_t src[256];
    uint8_t out[16 * 16 * 4];
    uint8_t tlut[512];
    int i;

    /* Sizes: block-rounded. */
    if (PortGXTextureSize(0, 10, 10) != 4 * 32 || PortGXTextureSize(6, 4, 4) != 64 || PortGXTextureSize(14, 8, 8) != 32 ||
        PortGXTextureSize(1, 8, 4) != 32) {
        printf("FAIL sizes\n");
        sFailures++;
    }

    /* I4, 16x8: two tiles side by side; texel (x, y) of tile t = (x + y + 4t) & 15. */
    memset(src, 0, sizeof(src));
    for (i = 0; i < 64; i++) {
        int t = i / 32, y = (i % 32) / 4, x = (i % 4) * 2;
        src[i] = (uint8_t)((((x + y + 4 * t) & 15) << 4) | ((x + 1 + y + 4 * t) & 15));
    }
    PortGXDecodeTexture(out, src, 0, 16, 8, NULL, 0);
    Expect("I4", out, 16, 0, 0, 0, 0, 0, 0);
    Expect("I4", out, 16, 1, 0, 17, 17, 17, 17);
    Expect("I4", out, 16, 3, 2, 85, 85, 85, 85);
    Expect("I4", out, 16, 8, 0, 68, 68, 68, 68);
    Expect("I4", out, 16, 15, 7, 34, 34, 34, 34); /* tile 1, (7 + 7 + 4) & 15 = 2 */

    /* I8, 8x4: one tile, row-major. */
    for (i = 0; i < 32; i++) {
        src[i] = (uint8_t)(i * 8);
    }
    PortGXDecodeTexture(out, src, 1, 8, 4, NULL, 0);
    Expect("I8", out, 8, 0, 0, 0, 0, 0, 0);
    Expect("I8", out, 8, 7, 3, 248, 248, 248, 248);

    /* IA4: alpha in the high nibble. */
    src[0] = 0xA5;
    PortGXDecodeTexture(out, src, 2, 8, 4, NULL, 0);
    Expect("IA4", out, 8, 0, 0, 85, 85, 85, 170);

    /* IA8: alpha byte, then intensity. */
    src[0] = 0x80;
    src[1] = 0x40;
    PortGXDecodeTexture(out, src, 3, 4, 4, NULL, 0);
    Expect("IA8", out, 4, 0, 0, 0x40, 0x40, 0x40, 0x80);

    /* RGB565: pure red, then pure blue at (1,0). */
    src[0] = 0xF8, src[1] = 0x00, src[2] = 0x00, src[3] = 0x1F;
    PortGXDecodeTexture(out, src, 4, 4, 4, NULL, 0);
    Expect("RGB565", out, 4, 0, 0, 255, 0, 0, 255);
    Expect("RGB565", out, 4, 1, 0, 0, 0, 255, 255);

    /* RGB5A3: opaque 555 green; then 3-bit alpha 4 with RGB444 (15, 0, 0). */
    src[0] = 0x83, src[1] = 0xE0;
    src[2] = 0x4F, src[3] = 0x00;
    PortGXDecodeTexture(out, src, 5, 4, 4, NULL, 0);
    Expect("RGB5A3", out, 4, 0, 0, 0, 255, 0, 255);
    Expect("RGB5A3", out, 4, 1, 0, 255, 0, 0, 146);

    /* RGBA8: AR pairs in the first 32 bytes, GB pairs in the next 32. */
    memset(src, 0, 64);
    src[2 * 5] = 0x11, src[2 * 5 + 1] = 0x22, src[32 + 2 * 5] = 0x33, src[32 + 2 * 5 + 1] = 0x44;
    PortGXDecodeTexture(out, src, 6, 4, 4, NULL, 0);
    Expect("RGBA8", out, 4, 1, 1, 0x22, 0x33, 0x44, 0x11);

    /* C8 with an RGB5A3 palette: index 3 is opaque white. */
    memset(tlut, 0, sizeof(tlut));
    tlut[6] = 0xFF, tlut[7] = 0xFF;
    memset(src, 0, 32);
    src[9] = 3;
    PortGXDecodeTexture(out, src, 9, 8, 4, tlut, 2);
    Expect("C8", out, 8, 1, 1, 255, 255, 255, 255);
    Expect("C8", out, 8, 0, 0, 0, 0, 0, 0);

    /* C4 with an IA8 palette: index 2 = alpha 0x80, intensity 0x10. */
    tlut[4] = 0x80, tlut[5] = 0x10;
    memset(src, 0, 32);
    src[0] = 0x02;
    PortGXDecodeTexture(out, src, 8, 8, 8, tlut, 0);
    Expect("C4", out, 8, 1, 0, 0x10, 0x10, 0x10, 0x80);

    /* CMPR: sub-blocks in Z order; colour 0 red, colour 1 blue, c0 > c1: index 2 is
     * 5/8 red + 3/8 blue. Sub-block 1 (top right) row 0 = indices 0 1 2 3. */
    memset(src, 0, 32);
    src[8] = 0xF8, src[9] = 0x00, src[10] = 0x00, src[11] = 0x1F;
    src[12] = 0x1B; /* 00 01 10 11 */
    PortGXDecodeTexture(out, src, 14, 8, 8, NULL, 0);
    Expect("CMPR", out, 8, 4, 0, 255, 0, 0, 255);
    Expect("CMPR", out, 8, 5, 0, 0, 0, 255, 255);
    Expect("CMPR", out, 8, 6, 0, 159, 0, 95, 255);
    /* c0 <= c1: index 3 is transparent black. */
    src[0] = 0x00, src[1] = 0x1F, src[2] = 0xF8, src[3] = 0x00;
    src[4] = 0xC0;
    PortGXDecodeTexture(out, src, 14, 8, 8, NULL, 0);
    Expect("CMPR", out, 8, 0, 0, 0, 0, 0, 0);

    if (sFailures == 0) {
        printf("test_gx_texture: ok\n");
    }
    return sFailures != 0;
}
