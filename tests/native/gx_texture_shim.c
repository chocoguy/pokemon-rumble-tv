/* What port_gx_texture.c needs besides the decoder, for test_gx_texture: no GPU, no
 * registers, no game. */
#include <port_gfx.h>
#include <stdarg.h>
#include <stdio.h>
#include "../../port/gx/port_gx_internal.h"

PortGXRegs gPortGX;

PortGfxTexture* PortGfxTextureCreate(int width, int height, int levels, const uint8_t* const* rgba) {
    (void)width, (void)height, (void)levels, (void)rgba;
    return NULL;
}
void PortGfxTextureRelease(PortGfxTexture* texture) {
    (void)texture;
}
void PortRedOnce(int* reported, const char* fmt, ...) {
    (void)reported, (void)fmt;
}
void PortAddSummary(void (*func)(void)) {
    (void)func;
}
