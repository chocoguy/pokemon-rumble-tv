/* Shared by port_ax.c (the AX API and the frame) and port_ax_mix.c (the voice mixer),
 * and by tests/native/test_ax_mix.c. */
#ifndef PORT_AX_INTERNAL_H
#define PORT_AX_INTERNAL_H

#include <revolution/types.h>
#include <revolution/ax.h>

#define AX_FORMAT_ADPCM 0x00
#define AX_FORMAT_PCM16 0x0A
#define AX_FORMAT_PCM8 0x19

#define AX_PB_TYPE_NORMAL 0
#define AX_PB_TYPE_STREAM 1

/* The buses one frame mixes into, each AX_SAMPLES_PER_FRAME samples of s16 range held in
 * 32 bits. Aux buses are laid out as the aux callbacks take them: left, right, surround. */
typedef struct PortAXBuses {
    s32 main[3][AX_SAMPLES_PER_FRAME];
    s32 aux[3][3][AX_SAMPLES_PER_FRAME];
} PortAXBuses;

/* Builds the resampling tables (once). */
void PortAXMixInit(void);

/* Mixes one frame of a running voice into `buses`, advancing it through its samples as
 * the DSP did: decode, resample to 32 kHz, filter, volume envelope, bus volumes. Updates
 * the parameter block (addresses, decoder history, ramped volumes) and sets the state to
 * AX_VOICE_STOP when a voice without a loop passes its end. */
void PortAXMixVoice(AXVPB* vpb, PortAXBuses* buses);

#endif
