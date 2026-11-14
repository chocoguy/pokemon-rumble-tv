/* The GX command pipe on the port.
 *
 * On the Wii, GX commands were stored to the write-gather pipe at 0xCC008000, which
 * packed them into the GPU's FIFO as big-endian bytes. Here every such store appends the
 * same bytes to the current command buffer: the frame's FIFO, or a display list being
 * recorded. port/gx owns the buffer and decodes it, so the byte stream is exactly what
 * the console's GPU received, and display lists from model files (also big-endian) go
 * through the same decoder.
 */
#ifndef PORT_GX_PIPE_H
#define PORT_GX_PIPE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PORTGXPipe {
    uint8_t* cur;
    uint8_t* end;
} PORTGXPipe;

extern PORTGXPipe gPORTGXPipe;

/* Makes room for at least `bytes` more (grows the frame buffer, or reports a display
 * list that overflowed its buffer, as the console's GXEndDisplayList would). */
void PORTGXPipeReserve(uint32_t bytes);

static inline uint8_t* PORTGXPipeTake(uint32_t bytes) {
    uint8_t* p;
    if ((uint32_t)(gPORTGXPipe.end - gPORTGXPipe.cur) < bytes) {
        PORTGXPipeReserve(bytes);
    }
    p = gPORTGXPipe.cur;
    gPORTGXPipe.cur += bytes;
    return p;
}

static inline void PORTGXWrite_u8(uint8_t v) {
    *PORTGXPipeTake(1) = v;
}
static inline void PORTGXWrite_u16(uint16_t v) {
    uint8_t* p = PORTGXPipeTake(2);
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}
static inline void PORTGXWrite_u32(uint32_t v) {
    uint8_t* p = PORTGXPipeTake(4);
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}
static inline void PORTGXWrite_f32(float v) {
    union {
        float f;
        uint32_t u;
    } bits;
    bits.f = v;
    PORTGXWrite_u32(bits.u);
}
static inline void PORTGXWrite_s8(int8_t v) {
    PORTGXWrite_u8((uint8_t)v);
}
static inline void PORTGXWrite_s16(int16_t v) {
    PORTGXWrite_u16((uint16_t)v);
}
static inline void PORTGXWrite_s32(int32_t v) {
    PORTGXWrite_u32((uint32_t)v);
}

#ifdef __cplusplus
}
#endif

#endif
