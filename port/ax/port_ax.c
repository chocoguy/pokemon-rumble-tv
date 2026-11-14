/* AX and AI, silent (phase 2).
 *
 * The console's DSP mixed a 3 ms audio frame (96 samples at 32 kHz) and raised the AI
 * DMA interrupt for the next one. Here a 3 ms timer plays that role: each frame it
 * advances every running voice through its sample data, as the DSP would, so one-shot
 * sounds end and streams ask for more data; it runs the aux (effect) callbacks on silent
 * buffers; then it calls the frame callback NW4R registered, which drives its sound
 * thread. Nothing is mixed or played: phase 5 replaces the advance with a real mixer.
 *
 * Voice addresses are DSP addresses: ADPCM in nibbles, PCM16 in 16-bit words, PCM8 in
 * bytes. */
#include <port_os.h>
#include <revolution/ax.h>
#include <revolution/ai.h>
#include <math.h>
#include <string.h>

#define AX_FORMAT_ADPCM 0x00
#define AX_FORMAT_PCM16 0x0A
#define AX_FORMAT_PCM8 0x19

static AXVPB sVoices[AX_VOICE_MAX];
static AXOutCallback sFrameCallback;
static AIDCallback sDmaCallback;
static AXAuxCallback sAuxCallback[3];
static void* sAuxContext[3];
static u16 sAuxReturnVolume[3];
static u16 sMasterVolume = AX_MAX_VOLUME;
static u32 sMode;
static u32 sMaxDspCycles;
static int sInitialized;
static unsigned long long sFrames;

/* Silent buffers for the aux callbacks: left, right, surround. */
static s32 sAuxBuffers[3][3][AX_SAMPLES_PER_FRAME];

static u32 Addr(u16 hi, u16 lo) {
    return ((u32)hi << 16) | lo;
}

static void AdvanceVoice(AXVPB* vpb) {
    AXPB* pb = &vpb->pb;
    u32 ratio = ((u32)pb->src.ratioHi << 16) | pb->src.ratioLo;
    u32 frac = pb->src.currentAddressFrac;
    u64 samples = (u64)ratio * AX_SAMPLES_PER_FRAME + frac;
    u32 step = (u32)(samples >> 16);
    u32 current = Addr(pb->addr.currentAddressHi, pb->addr.currentAddressLo);
    u32 end = Addr(pb->addr.endAddressHi, pb->addr.endAddressLo);
    u32 loop = Addr(pb->addr.loopAddressHi, pb->addr.loopAddressLo);
    u32 next;

    if (pb->src.ratioHi == 0 && pb->src.ratioLo == 0 && pb->srcSelect != 2) {
        step = AX_SAMPLES_PER_FRAME; /* no rate set yet: treat as 1:1 */
    }
    pb->src.currentAddressFrac = (u16)samples;
    if (pb->addr.format == AX_FORMAT_ADPCM) {
        /* 14 samples per 8-byte frame, whose first byte is a header (2 nibbles). */
        u32 position = current & 15;
        u32 nibbles = 0;
        while (step > 0) {
            if (position < 2) {
                nibbles += 2 - position;
                position = 2;
            }
            {
                u32 room = 16 - position;
                u32 take = step < room ? step : room;
                nibbles += take;
                position += take;
                step -= take;
            }
            if (position == 16) {
                position = 0;
            }
        }
        next = current + nibbles;
    } else {
        next = current + step;
    }
    if (next > end) {
        if (pb->addr.loopFlag) {
            next = loop + (next - end - 1);
        } else {
            next = end;
            pb->state = AX_VOICE_STOP;
        }
    }
    pb->addr.currentAddressHi = (u16)(next >> 16);
    pb->addr.currentAddressLo = (u16)next;
}

static void AudioFrame(void) {
    int i;
    sFrames++;
    for (i = 0; i < AX_VOICE_MAX; i++) {
        AXVPB* vpb = &sVoices[i];
        if (vpb->priority != AX_PRIORITY_FREE && vpb->pb.state == AX_VOICE_RUN) {
            AdvanceVoice(vpb);
        }
        vpb->sync = 0;
    }
    for (i = 0; i < 3; i++) {
        if (sAuxCallback[i] != NULL) {
            void* chans[3];
            memset(sAuxBuffers[i], 0, sizeof(sAuxBuffers[i]));
            chans[0] = sAuxBuffers[i][0];
            chans[1] = sAuxBuffers[i][1];
            chans[2] = sAuxBuffers[i][2];
            sAuxCallback[i](chans, sAuxContext[i]);
        }
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
    PortLog("AX: %llu frames, %d voices acquired, %d running at exit", sFrames, acquired, running);
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
    /* The first frame follows the next; NW4R expects the callback from interrupt context. */
    PortAddTimer(AudioFrame, PortNow() + OSMillisecondsToTicks(3), OSMillisecondsToTicks(3));
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
