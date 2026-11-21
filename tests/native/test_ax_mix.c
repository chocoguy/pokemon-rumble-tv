/* The AX voice mixer (port/ax/port_ax_mix.c) on known samples: a voice is set up the way
 * NW4R's AxVoice sets one up, in a small fake address window, and its mixed output is
 * checked against what the samples should become.
 *
 *   PCM16, no resampling: output equals input; the voice stops after its end address
 *     and sits at its loop address (NW4R's zero buffer), with silence after.
 *   PCM16 loop: the loop repeats seamlessly.
 *   PCM8: scaled to 16 bits.
 *   DSP-ADPCM: a sine encoded by a small encoder below decodes back within 30 dB, across
 *     a loop whose start is mid-frame (predictor and history from the loop context).
 *   Resampling: a 1 kHz sine stored at 16 kHz (ratio 0.5), 4-tap and linear, comes out
 *     as a 1 kHz sine at 32 kHz.
 *   Envelope and bus volumes: ramps reach their targets, the bus gets the scaled signal. */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../port/ax/port_ax_internal.h"

#define N AX_SAMPLES_PER_FRAME
#define WINDOW (1 << 20)
#define WAVE_AT 0x1000 /* byte address of the sample data */
#define ZERO_AT 0x100  /* byte address of a zero buffer */

static u8* sMem;
static int sFailures;

void PortRedOnce(int* reported, const char* fmt, ...) {
    (void)fmt;
    if (!*reported) {
        *reported = 1;
        printf("  [RED] reported\n");
    }
}

#define CHECK(cond, ...)                 \
    do {                                 \
        if (!(cond)) {                   \
            printf("  FAIL: " __VA_ARGS__); \
            printf("\n");                \
            sFailures++;                 \
        }                                \
    } while (0)

static void SetAddr(AXPB* pb, u32 cur, u32 end, u32 loop, int loopFlag, u16 format) {
    pb->addr.currentAddressHi = (u16)(cur >> 16);
    pb->addr.currentAddressLo = (u16)cur;
    pb->addr.endAddressHi = (u16)(end >> 16);
    pb->addr.endAddressLo = (u16)end;
    pb->addr.loopAddressHi = (u16)(loop >> 16);
    pb->addr.loopAddressLo = (u16)loop;
    pb->addr.loopFlag = (u16)loopFlag;
    pb->addr.format = format;
}

static u32 CurAddr(const AXPB* pb) {
    return ((u32)pb->addr.currentAddressHi << 16) | pb->addr.currentAddressLo;
}

/* A running voice at unity: envelope and main left/right at 1.0, no resampling. */
static void NewVoice(AXVPB* v) {
    memset(v, 0, sizeof(*v));
    v->priority = 1;
    v->pb.state = AX_VOICE_RUN;
    v->pb.srcSelect = 2;
    v->pb.src.ratioHi = 1;
    v->pb.ve.currentVolume = 0x8000;
    v->pb.mix.vL = 0x8000;
    v->pb.mix.vR = 0x8000;
    v->pb.mixerCtrl = AX_MIXER_CTRL_L | AX_MIXER_CTRL_R;
}

/* Mixes `frames` frames of the voice, collecting main left. */
static void Run(AXVPB* v, s32* out, int frames) {
    static PortAXBuses buses;
    int f;
    for (f = 0; f < frames; f++) {
        memset(&buses, 0, sizeof(buses));
        if (v->pb.state == AX_VOICE_RUN) {
            PortAXMixVoice(v, &buses);
        }
        memcpy(out + f * N, buses.main[0], sizeof(buses.main[0]));
    }
}

static void PutBE16(u8* p, s16 v) {
    p[0] = (u8)((u16)v >> 8);
    p[1] = (u8)v;
}

static void TestPcm16(void) {
    AXVPB v;
    s32 out[3 * N];
    int i, bad = 0;
    printf("PCM16, no resampling, one-shot\n");
    for (i = 0; i < 150; i++) {
        PutBE16(sMem + WAVE_AT + i * 2, (s16)(i * 200 - 15000));
    }
    NewVoice(&v);
    SetAddr(&v.pb, WAVE_AT / 2, WAVE_AT / 2 + 149, ZERO_AT / 2, 0, AX_FORMAT_PCM16);
    Run(&v, out, 3);
    for (i = 0; i < 150; i++) {
        bad += out[i] != i * 200 - 15000;
    }
    for (i = 150; i < 3 * N; i++) {
        bad += out[i] != 0;
    }
    CHECK(bad == 0, "%d samples differ", bad);
    CHECK(v.pb.state == AX_VOICE_STOP, "voice still running after its end");
    CHECK(CurAddr(&v.pb) == ZERO_AT / 2, "stopped at 0x%x, not the loop address", CurAddr(&v.pb));

    printf("PCM16 loop\n");
    NewVoice(&v);
    SetAddr(&v.pb, WAVE_AT / 2, WAVE_AT / 2 + 49, WAVE_AT / 2 + 10, 1, AX_FORMAT_PCM16);
    Run(&v, out, 3);
    bad = 0;
    for (i = 0; i < 3 * N; i++) {
        int k = i < 50 ? i : 10 + (i - 50) % 40;
        bad += out[i] != k * 200 - 15000;
    }
    CHECK(bad == 0, "%d samples differ", bad);
    CHECK(v.pb.state == AX_VOICE_RUN, "looping voice stopped");

    printf("PCM8\n");
    for (i = 0; i < N; i++) {
        sMem[WAVE_AT + i] = (u8)(s8)(i - 48);
    }
    NewVoice(&v);
    SetAddr(&v.pb, WAVE_AT, WAVE_AT + N - 1, ZERO_AT, 0, AX_FORMAT_PCM8);
    Run(&v, out, 1);
    bad = 0;
    for (i = 0; i < N; i++) {
        bad += out[i] != (i - 48) * 256;
    }
    CHECK(bad == 0, "%d samples differ", bad);
}

/* ---- DSP-ADPCM ---------------------------------------------------------------------- */

static const s16 kCoefs[8][2] = {
    {0, 0}, {2048, 0}, {0, 2048}, {1024, 1024}, {4096, -2048}, {3584, -1536}, {3072, -1024}, {4608, -2560},
};

static s16 Clamp(s32 v) {
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v;
}

static s16 Decode1(int nibble, int ps, s16* yn1, s16* yn2) {
    int idx = (ps >> 4) & 7;
    s32 s = ((nibble * (1 << (ps & 15))) << 11) + 1024 + kCoefs[idx][0] * *yn1 + kCoefs[idx][1] * *yn2;
    s16 y = Clamp(s >> 11);
    *yn2 = *yn1;
    *yn1 = y;
    return y;
}

/* Encodes `count` samples (a multiple of 14), trying every predictor and scale per frame;
 * records the decoder state at sample `mark` (for the loop context). */
static void EncodeAdpcm(const s16* in, int count, u8* out, int mark, int* markPs, s16* markY1, s16* markY2) {
    s16 y1 = 0, y2 = 0;
    int frame;
    for (frame = 0; frame < count / 14; frame++) {
        int bestPs = 0;
        double bestErr = 1e300;
        int ps;
        for (ps = 0; ps < 128; ps++) {
            s16 a = y1, b = y2;
            double err = 0;
            int i;
            if ((ps & 15) > 12) {
                continue;
            }
            for (i = 0; i < 14; i++) {
                int idx = ps >> 4;
                s32 pred = (1024 + kCoefs[idx][0] * a + kCoefs[idx][1] * b);
                s32 want = ((s32)in[frame * 14 + i] << 11) - pred;
                int nib = (int)lround((double)want / (2048.0 * (1 << (ps & 15))));
                s16 y;
                nib = nib > 7 ? 7 : nib < -8 ? -8 : nib;
                y = Decode1(nib, ps, &a, &b);
                err += (double)(y - in[frame * 14 + i]) * (y - in[frame * 14 + i]);
            }
            if (err < bestErr) {
                bestErr = err;
                bestPs = ps;
            }
        }
        out[frame * 8] = (u8)bestPs;
        {
            int i;
            for (i = 0; i < 14; i++) {
                int idx = bestPs >> 4;
                s32 pred = (1024 + kCoefs[idx][0] * y1 + kCoefs[idx][1] * y2);
                s32 want = ((s32)in[frame * 14 + i] << 11) - pred;
                int nib = (int)lround((double)want / (2048.0 * (1 << (bestPs & 15))));
                u8* byte = &out[frame * 8 + 1 + i / 2];
                nib = nib > 7 ? 7 : nib < -8 ? -8 : nib;
                if (frame * 14 + i == mark) {
                    *markPs = bestPs;
                    *markY1 = y1;
                    *markY2 = y2;
                }
                Decode1(nib, bestPs, &y1, &y2);
                if (i % 2 == 0) {
                    *byte = (u8)((nib & 15) << 4);
                } else {
                    *byte |= (u8)(nib & 15);
                }
            }
        }
    }
}

/* DSP address of sample `s` in ADPCM data at byte address `at` (AxVoice's formula). */
static u32 AdpcmAddr(u32 at, u32 s) {
    return s / 14 * 16 + s % 14 + at * 2 + 2;
}

static double SnrDb(const s32* got, const s16* want, int count) {
    double sig = 0, err = 0;
    int i;
    for (i = 0; i < count; i++) {
        sig += (double)want[i] * want[i];
        err += (double)(got[i] - want[i]) * (got[i] - want[i]);
    }
    return 10 * log10(sig / (err > 0 ? err : 1e-9));
}

static void TestAdpcm(void) {
    enum { COUNT = 14 * 40, LOOP = 14 * 10 + 5 }; /* loop start mid-frame */
    static s16 pcm[COUNT];
    static s16 want[4 * N];
    static s32 out[4 * N];
    AXVPB v;
    int i, markPs = 0;
    s16 markY1 = 0, markY2 = 0;
    double snr;
    printf("DSP-ADPCM, a loop starting mid-frame\n");
    for (i = 0; i < COUNT; i++) {
        pcm[i] = (s16)lround(12000 * sin(2 * M_PI * 440.0 * i / 32000.0) + 3000 * sin(2 * M_PI * 1250.0 * i / 32000.0));
    }
    EncodeAdpcm(pcm, COUNT, sMem + WAVE_AT, LOOP, &markPs, &markY1, &markY2);
    NewVoice(&v);
    memcpy(v.pb.adpcm.a, kCoefs, sizeof(kCoefs));
    v.pb.adpcm.pred_scale = sMem[WAVE_AT];
    v.pb.adpcmLoop.loop_pred_scale = (u16)markPs;
    v.pb.adpcmLoop.loop_yn1 = (u16)markY1;
    v.pb.adpcmLoop.loop_yn2 = (u16)markY2;
    SetAddr(&v.pb, AdpcmAddr(WAVE_AT, 0), AdpcmAddr(WAVE_AT, COUNT - 1), AdpcmAddr(WAVE_AT, LOOP), 1, AX_FORMAT_ADPCM);
    Run(&v, out, 4);
    for (i = 0; i < 4 * N; i++) {
        want[i] = pcm[i < COUNT ? i : LOOP + (i - COUNT) % (COUNT - LOOP)];
    }
    snr = SnrDb(out, want, 4 * N);
    printf("  SNR %.1f dB\n", snr);
    CHECK(snr > 30, "ADPCM decode SNR %.1f dB", snr);
}

/* ---- Resampling --------------------------------------------------------------------- */

/* Signal-to-residual of `x` against the best-fitting sine of `freq` (Hz at 32 kHz). */
static double SineSnrDb(const s32* x, int count, double freq) {
    double c = 0, s = 0, sig = 0, err = 0;
    int i;
    for (i = 0; i < count; i++) {
        c += x[i] * cos(2 * M_PI * freq * i / 32000.0);
        s += x[i] * sin(2 * M_PI * freq * i / 32000.0);
    }
    c *= 2.0 / count;
    s *= 2.0 / count;
    for (i = 0; i < count; i++) {
        double fit = c * cos(2 * M_PI * freq * i / 32000.0) + s * sin(2 * M_PI * freq * i / 32000.0);
        sig += fit * fit;
        err += (x[i] - fit) * (x[i] - fit);
    }
    return 10 * log10(sig / (err > 0 ? err : 1e-9));
}

static void TestResample(int srcSelect, int coefSelect, double minSnr) {
    enum { FRAMES = 20, SAMPLES = 16000 / 1000 * 100 }; /* 100 ms at 16 kHz */
    static s32 out[FRAMES * N];
    AXVPB v;
    int i;
    double snr;
    printf("resampling 16 kHz -> 32 kHz, %s\n", srcSelect == 1 ? "linear" : "4 taps");
    for (i = 0; i < SAMPLES; i++) {
        PutBE16(sMem + WAVE_AT + i * 2, (s16)lround(16000 * sin(2 * M_PI * 1000.0 * i / 16000.0)));
    }
    NewVoice(&v);
    v.pb.srcSelect = (u16)srcSelect;
    v.pb.coefSelect = (u16)coefSelect;
    v.pb.src.ratioHi = 0;
    v.pb.src.ratioLo = 0x8000;
    SetAddr(&v.pb, WAVE_AT / 2, WAVE_AT / 2 + SAMPLES - 1, ZERO_AT / 2, 0, AX_FORMAT_PCM16);
    Run(&v, out, FRAMES);
    /* Skip the first 32 outputs (the history filling), measure 1856 (58 ms, 58 periods). */
    snr = SineSnrDb(out + 32, 1856, 1000.0);
    printf("  SNR %.1f dB\n", snr);
    CHECK(snr > minSnr, "resampled sine SNR %.1f dB", snr);
    CHECK(CurAddr(&v.pb) - WAVE_AT / 2 <= SAMPLES, "read %u samples for 1920 outputs", CurAddr(&v.pb) - WAVE_AT / 2);
}

static void TestVolumes(void) {
    static PortAXBuses buses;
    AXVPB v;
    int i, bad = 0;
    printf("envelope and bus volumes\n");
    for (i = 0; i < N; i++) {
        PutBE16(sMem + WAVE_AT + i * 2, 16384);
    }
    NewVoice(&v);
    SetAddr(&v.pb, WAVE_AT / 2, WAVE_AT / 2 + N - 1, ZERO_AT / 2, 0, AX_FORMAT_PCM16);
    v.pb.ve.currentVolume = 0;
    v.pb.ve.currentDelta = 0x8000 / N; /* 0 to ~1.0 over the frame */
    v.pb.mix.vL = 0x4000;
    v.pb.mix.vAuxAL = 0x8000;
    v.pb.mix.vDeltaAuxAL = (u16)(s16)(-0x8000 / N);
    v.pb.mixerCtrl = AX_MIXER_CTRL_L | AX_MIXER_CTRL_AL | AX_MIXER_CTRL_DELTA_A;
    memset(&buses, 0, sizeof(buses));
    PortAXMixVoice(&v, &buses);
    for (i = 0; i < N; i++) {
        s32 env = (16384 * (i * (0x8000 / N))) >> 15;
        s32 left = (env * 0x4000) >> 15;
        s32 aux = (env * (0x8000 - i * (0x8000 / N))) >> 15;
        bad += buses.main[0][i] != left || buses.aux[0][0][i] != aux || buses.main[1][i] != 0;
    }
    CHECK(bad == 0, "%d samples differ", bad);
    CHECK(v.pb.ve.currentVolume == N * (0x8000 / N), "envelope ended at 0x%x", v.pb.ve.currentVolume);
    CHECK(v.pb.mix.vAuxAL == 0x8000 - N * (0x8000 / N), "aux A left ended at 0x%x", v.pb.mix.vAuxAL);
}

int main(void) {
    sMem = calloc(1, WINDOW);
    gPortAddrBase = (uintptr_t)sMem;
    PortAXMixInit();
    TestPcm16();
    TestAdpcm();
    TestResample(0, 2, 40);
    TestResample(1, 0, 25);
    TestVolumes();
    printf(sFailures ? "test_ax_mix: %d failures\n" : "test_ax_mix: ok\n", sFailures);
    return sFailures != 0;
}
