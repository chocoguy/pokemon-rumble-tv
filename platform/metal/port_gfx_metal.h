/* What the Metal backend offers an app shell (Objective-C). */
#ifndef PORT_GFX_METAL_H
#define PORT_GFX_METAL_H

#import <QuartzCore/CAMetalLayer.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The device the backend renders with (nil before PortStartGame). */
id<MTLDevice> PortGfxDevice(void);

/* Draws the frame the display currently shows into the layer's next drawable, scaled to
 * fit at the display aspect (port_gfx.h) with black bars, and presents it. Call once per
 * screen refresh from the main thread. */
void PortGfxPresentToLayer(CAMetalLayer* layer);

#ifdef __cplusplus
}
#endif

#endif
