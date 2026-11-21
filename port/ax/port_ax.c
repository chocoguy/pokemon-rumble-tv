/* AX and AI: the mixer's frame and the AX API NW4R calls.
 *
 * The console's DSP mixed a 3 ms audio frame (96 samples at 32 kHz) and raised the AI
 * DMA interrupt for the next one. Here a 3 ms timer plays that role. Each frame it mixes
 * every running voice (port_ax_mix.c) into the main and aux buses, hands each aux bus to
 * its callback (NW4R's effects, the SDK's axfx compiled from the fork), adds the
 * processed aux buses back at their return volumes, applies the master volume, and sends
 * the 16-bit stereo result to the sink (port_audio.h: the AVAudioEngine output, a WAV
 * file). Then it calls the frame callback NW4R registered, which drives its sound thread.
 *
 * Voice addresses are DSP addresses: ADPCM in nibbles, PCM16 in 16-bit words, PCM8 in
 * bytes, all physical addresses in the game's window (port_addr.h). */
#include <port_audio.h>
#include <port_os.h>
#include <revolution/ai.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "port_ax_internal.h"

static AXVPB sVoices[AX_VOICE_MAX];
static AXOutCallback sFrameCallback;
static AIDCallback sDmaCallback;
static AXAuxCallback sAuxCallback[3];
static void* sAuxContext[3];
static u16 sAuxReturnVolume[3] = {AX_MAX_VOLUME, AX_MAX_VOLUME, AX_MAX_VOLUME};
static u16 sMasterVolume = AX_MAX_VOLUME;
static u32 sMode;
static u32 sMaxDspCycles;
static int sInitialized;
static unsigned long long sFrames;
static unsigned long long sClippedSamples;
static int sMostVoices;

static PortAXBuses sBuses;
static s16 sOut[AX_SAMPLES_PER_FRAME * 2];

static PortAudioSink sSink;
static void* sSinkContext;
static FILE* sWav;
static unsigned long long sWavFrames;

void PortAudioSetSink(PortAudioSink sink, void* context) {
    sSink = sink;
    sSinkContext = context;
}

static void PutLE32(u8* p, u32 v) {
    p[0] = (u8)v;
    p[1] = (u8)(v >> 8);
    p[2] = (u8)(v >> 16);
    p[3] = (u8)(v >> 24);
}

/* The 44-byte header of a 16-bit stereo PCM WAV holding `frames` frames. */
static void WavHeader(u8* h, unsigned long long frames) {
    u32 bytes = (u32)(frames * 4 > 0xFFFFFFD3ull ? 0xFFFFFFD3ull : frames * 4);
    memcpy(h, "RIFF", 4);
    PutLE32(h + 4, 36 + bytes);
    memcpy(h + 8, "WAVEfmt ", 8);
    PutLE32(h + 16, 16);
    PutLE32(h + 20, 1 | (2 << 16)); /* PCM, two channels */
    PutLE32(h + 24, PORT_AUDIO_RATE);
    PutLE32(h + 28, PORT_AUDIO_RATE * 4);
    PutLE32(h + 32, 4 | (16 << 16)); /* block align, bits per sample */
    memcpy(h + 36, "data", 4);
    PutLE32(h + 40, bytes);
}

static void FinishWav(void) {
    u8 header[44];
    if (sWav == NULL) {
        return;
    }
    WavHeader(header, sWavFrames);
    fseek(sWav, 0, SEEK_SET);
    fwrite(header, 1, sizeof(header), sWav);
    fclose(sWav);
    sWav = NULL;
    PortLog("AX: wrote %.1f s of sound", (double)sWavFrames / PORT_AUDIO_RATE);
}

int PortAudioWriteWav(const char* path) {
    u8 header[44];
    sWav = fopen(path, "wb");
    if (sWav == NULL) {
        return 0;
    }
    WavHeader(header, 0);
    fwrite(header, 1, sizeof(header), sWav);
    return 1;
}

static void WriteWav(const s16* frames, int count) {
    u8 bytes[AX_SAMPLES_PER_FRAME * 4];
    int i;
    for (i = 0; i < count * 2; i++) {
        bytes[i * 2] = (u8)frames[i];
        bytes[i * 2 + 1] = (u8)((u16)frames[i] >> 8);
    }
    fwrite(bytes, 4, (size_t)count, sWav);
    sWavFrames += (unsigned long long)count;
}

/* PORT_AX_TRACE=1: once a second, what the mixer is doing. */
static int sTrace;

static void TraceVoices(void) {
    int running = 0, panned = 0, aux[3] = {0, 0, 0}, formats[3] = {0, 0, 0}, looping = 0;
    int i;
    for (i = 0; i < AX_VOICE_MAX; i++) {
        AXPB* pb = &sVoices[i].pb;
        if (sVoices[i].priority == AX_PRIORITY_FREE || pb->state != AX_VOICE_RUN) {
            continue;
        }
        running++;
        panned += pb->mix.vL != pb->mix.vR;
        aux[0] += (pb->mixerCtrl & (AX_MIXER_CTRL_AL | AX_MIXER_CTRL_AR)) != 0;
        aux[1] += (pb->mixerCtrl & (AX_MIXER_CTRL_BL | AX_MIXER_CTRL_BR)) != 0;
        aux[2] += (pb->mixerCtrl & (AX_MIXER_CTRL_CL | AX_MIXER_CTRL_CR)) != 0;
        formats[pb->addr.format == AX_FORMAT_ADPCM ? 0 : pb->addr.format == AX_FORMAT_PCM16 ? 1 : 2]++;
        looping += pb->addr.loopFlag != 0;
    }
    if (getenv("PORT_AX_TRACE")[0] == '2') {
        for (i = 0; i < AX_VOICE_MAX; i++) {
            AXPB* pb = &sVoices[i].pb;
            if (sVoices[i].priority == AX_PRIORITY_FREE || pb->state != AX_VOICE_RUN) {
                continue;
            }
            PortLog("  voice %2d prio %2u ve %04x%+d L %04x R %04x AL %04x BL %04x ratio %u.%04x lpf %d bq %d cur %08x "
                    "loop %08x end %08x",
                    i, sVoices[i].priority, pb->ve.currentVolume, (s16)pb->ve.currentDelta, pb->mix.vL, pb->mix.vR,
                    pb->mix.vAuxAL, pb->mix.vAuxBL, pb->src.ratioHi, pb->src.ratioLo, pb->lpf.on, pb->biquad.on,
                    ((u32)pb->addr.currentAddressHi << 16) | pb->addr.currentAddressLo,
                    ((u32)pb->addr.loopAddressHi << 16) | pb->addr.loopAddressLo,
                    ((u32)pb->addr.endAddressHi << 16) | pb->addr.endAddressLo);
        }
    }
    PortLog("AX trace %.1f s: %d running (%d ADPCM, %d PCM16, %d PCM8; %d looping), %d panned, aux A/B/C %d/%d/%d "
            "(callbacks %d%d%d), master 0x%x",
            sFrames * 0.003, running, formats[0], formats[1], formats[2], looping, panned, aux[0], aux[1], aux[2],
            sAuxCallback[0] != NULL, sAuxCallback[1] != NULL, sAuxCallback[2] != NULL, sMasterVolume);
}

static s16 Output(s32 v) {
    v = (s32)(((s64)v * sMasterVolume) >> 15);
    if (v > 32767 || v < -32768) {
        sClippedSamples++;
        return v > 0 ? 32767 : -32768;
    }
    return (s16)v;
}

static void AudioFrame(void) {
    int voices = 0;
    int i, bus, ch;
    sFrames++;
    memset(&sBuses, 0, sizeof(sBuses));
    for (i = 0; i < AX_VOICE_MAX; i++) {
        AXVPB* vpb = &sVoices[i];
        if (vpb->priority != AX_PRIORITY_FREE && vpb->pb.state == AX_VOICE_RUN) {
            PortAXMixVoice(vpb, &sBuses);
            voices++;
        }
        vpb->sync = 0;
    }
    if (voices > sMostVoices) {
        sMostVoices = voices;
    }
    if (sTrace && sFrames % 333 == 0) {
        TraceVoices();
    }

    /* Aux buses through their effects and back into the main mix. Without a callback a
     * bus is dropped. Stereo output: the surround channels are not heard. */
    for (bus = 0; bus < 3; bus++) {
        if (sAuxCallback[bus] != NULL) {
            void* chans[3];
            s32 ret = sAuxReturnVolume[bus];
            chans[0] = sBuses.aux[bus][0];
            chans[1] = sBuses.aux[bus][1];
            chans[2] = sBuses.aux[bus][2];
            sAuxCallback[bus](chans, sAuxContext[bus]);
            for (ch = 0; ch < 2; ch++) {
                for (i = 0; i < AX_SAMPLES_PER_FRAME; i++) {
                    sBuses.main[ch][i] += (s32)(((s64)sBuses.aux[bus][ch][i] * ret) >> 15);
                }
            }
        }
    }

    for (i = 0; i < AX_SAMPLES_PER_FRAME; i++) {
        sOut[i * 2] = Output(sBuses.main[0][i]);
        sOut[i * 2 + 1] = Output(sBuses.main[1][i]);
    }
    if (sWav != NULL) {
        WriteWav(sOut, AX_SAMPLES_PER_FRAME);
    }
    if (sSink != NULL) {
        sSink(sOut, AX_SAMPLES_PER_FRAME, sSinkContext);
    }

    if (sFrameCallback != NULL) {
        sFrameCallback();
    }
    if (sDmaCallback != NULL) {
        sDmaCallback();
    }
}

static void PrintSummary(void) {
    int running = 0;
    int acquired = 0;
    int i;
    for (i = 0; i < AX_VOICE_MAX; i++) {
        if (sVoices[i].priority != AX_PRIORITY_FREE) {
            acquired++;
            if (sVoices[i].pb.state == AX_VOICE_RUN) {
                running++;
            }
        }
    }
    PortLog("AX: %llu frames, at most %d voices mixed at once, %llu samples clipped; %d voices acquired, %d running at exit",
            sFrames, sMostVoices, sClippedSamples, acquired, running);
    FinishWav();
}

/* ------------------------------------------------------------------------------------ */
/* AI                                                                                    */

void AIInit(u8* stack) {
    (void)stack;
}

AIDCallback AIRegisterDMACallback(AIDCallback callback) {
    AIDCallback old = sDmaCallback;
    sDmaCallback = callback;
    return old;
}

/* ------------------------------------------------------------------------------------ */
/* AX                                                                                    */

static void ResetVoice(AXVPB* vpb, u32 index) {
    memset(vpb, 0, sizeof(*vpb));
    vpb->index = index;
    vpb->priority = AX_PRIORITY_FREE;
}

void AXInit(void) {
    u32 i;
    if (sInitialized) {
        return;
    }
    sInitialized = 1;
    for (i = 0; i < AX_VOICE_MAX; i++) {
        ResetVoice(&sVoices[i], i);
    }
    PortAXMixInit();
    sTrace = getenv("PORT_AX_TRACE") != NULL;
    /* The first frame follows the next; NW4R expects the callback from interrupt context. */
    PortSetTimerCatchUp(PortAddTimer(AudioFrame, PortNow() + OSMillisecondsToTicks(3), OSMillisecondsToTicks(3)),
                        OSMillisecondsToTicks(150));
    PortAddSummary(PrintSummary);
}

void AXInitEx(u32 mode) {
    (void)mode;
    AXInit();
}

void AXQuit(void) {}

AXOutCallback AXRegisterCallback(AXOutCallback callback) {
    BOOL enabled = OSDisableInterrupts();
    AXOutCallback old = sFrameCallback;
    sFrameCallback = callback;
    OSRestoreInterrupts(enabled);
    return old;
}

AXVPB* AXAcquireVoice(u32 prio, AXVoiceCallback callback, uintptr_t userContext) {
    BOOL enabled = OSDisableInterrupts();
    AXVPB* chosen = NULL;
    int i;

    for (i = 0; i < AX_VOICE_MAX; i++) {
        if (sVoices[i].priority == AX_PRIORITY_FREE) {
            chosen = &sVoices[i];
            break;
        }
    }
    if (chosen == NULL) {
        /* Steal the lowest-priority voice below `prio`, telling its owner. */
        for (i = 0; i < AX_VOICE_MAX; i++) {
            if (sVoices[i].priority < prio && (chosen == NULL || sVoices[i].priority < chosen->priority)) {
                chosen = &sVoices[i];
            }
        }
        if (chosen == NULL) {
            OSRestoreInterrupts(enabled);
            return NULL;
        }
        if (chosen->callback != NULL) {
            chosen->callback(chosen);
        }
    }
    ResetVoice(chosen, chosen->index);
    chosen->priority = prio;
    chosen->callback = callback;
    chosen->userContext = userContext;
    OSRestoreInterrupts(enabled);
    return chosen;
}

void AXFreeVoice(AXVPB* vpb) {
    BOOL enabled = OSDisableInterrupts();
    ResetVoice(vpb, vpb->index);
    OSRestoreInterrupts(enabled);
}

void AXSetVoicePriority(AXVPB* vpb, u32 prio) {
    vpb->priority = prio;
}

void AXSetVoiceState(AXVPB* voice, u16 state) {
    voice->pb.state = state;
    voice->sync |= AX_PBSYNC_STATE;
}

void AXSetVoiceAddr(AXVPB* voice, AXPBADDR* addr) {
    voice->pb.addr = *addr;
    voice->sync |= AX_PBSYNC_ADDR;
}

/* One-pole low-pass: b = e^(-2 pi f / 32000), a = 1 - b, both as 1.15 fixed point. */
void AXGetLpfCoefs(u16 frequency, u16* a, u16* b) {
    double coef = exp(-2.0 * 3.14159265358979323846 * frequency / AX_SAMPLE_RATE);
    *b = (u16)(coef * 32768.0);
    *a = (u16)(32768 - *b);
}

s32 AXGetMaxVoices(void) {
    return AX_VOICE_MAX;
}

void AXSetMode(u32 mode) {
    sMode = mode;
}

u32 AXGetMode(void) {
    return sMode;
}

void AXSetMasterVolume(u16 volume) {
    sMasterVolume = volume;
}

u16 AXGetMasterVolume(void) {
    return sMasterVolume;
}

void AXSetMaxDspCycles(u32 cycles) {
    sMaxDspCycles = cycles;
}

static void RegisterAux(int bus, AXAuxCallback callback, void* context) {
    BOOL enabled = OSDisableInterrupts();
    sAuxCallback[bus] = callback;
    sAuxContext[bus] = context;
    OSRestoreInterrupts(enabled);
}

void AXRegisterAuxACallback(AXAuxCallback callback, void* context) {
    RegisterAux(0, callback, context);
}
void AXRegisterAuxBCallback(AXAuxCallback callback, void* context) {
    RegisterAux(1, callback, context);
}
void AXRegisterAuxCCallback(AXAuxCallback callback, void* context) {
    RegisterAux(2, callback, context);
}
void AXGetAuxACallback(AXAuxCallback* callback, void** context) {
    *callback = sAuxCallback[0];
    *context = sAuxContext[0];
}
void AXGetAuxBCallback(AXAuxCallback* callback, void** context) {
    *callback = sAuxCallback[1];
    *context = sAuxContext[1];
}
void AXGetAuxCCallback(AXAuxCallback* callback, void** context) {
    *callback = sAuxCallback[2];
    *context = sAuxContext[2];
}
void AXSetAuxAReturnVolume(u16 volume) {
    sAuxReturnVolume[0] = volume;
}
void AXSetAuxBReturnVolume(u16 volume) {
    sAuxReturnVolume[1] = volume;
}
void AXSetAuxCReturnVolume(u16 volume) {
    sAuxReturnVolume[2] = volume;
}
u16 AXGetAuxAReturnVolume(void) {
    return sAuxReturnVolume[0];
}
u16 AXGetAuxBReturnVolume(void) {
    return sAuxReturnVolume[1];
}
u16 AXGetAuxCReturnVolume(void) {
    return sAuxReturnVolume[2];
}

/* Wii Remote speaker output: nothing to send. */
s32 AXRmtGetSamplesLeft(void) {
    return 0;
}
s32 AXRmtGetSamples(s32 chan, s16* out, s32 num) {
    (void)chan;
    (void)out;
    (void)num;
    return 0;
}
s32 AXRmtAdvancePtr(s32 num) {
    (void)num;
    return 0;
}
