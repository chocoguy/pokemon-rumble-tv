/* The AX voice mixer: what the DSP did with each running voice's parameter block every
 * 3 ms frame (AX_SAMPLES_PER_FRAME output samples at 32 kHz).
 *
 *   fetch     the sample memory through the voice's DSP addresses: DSP-ADPCM in nibbles
 *             (a frame of 16 nibbles is a header byte, the predictor and scale, then 14
 *             samples), PCM16 in 16-bit words, PCM8 in bytes; all big-endian as stored.
 *             After the sample at the end address the voice goes to the loop address;
 *             without the loop flag it then stops (NW4R points such a voice's loop at its
 *             zero buffer and treats "playing from the zero buffer" as finished). On a
 *             loop the ADPCM predictor comes from the loop context, and so does the
 *             history unless the voice is a stream (whose data runs on continuously).
 *   resample  by the 16.16 ratio: none (one input per output), linear, or 4 taps with
 *             one of three low-pass strengths (NW4R picks it from the pitch). The four
 *             taps' history lives in the block's src.last_samples.
 *   filter    the one-pole low-pass (coefficients from AXGetLpfCoefs) and the biquad
 *             (1.14 coefficients, feedback terms added, as NW4R's presets are stored).
 *   envelope  the volume envelope (1.15, ramped per sample by its delta).
 *   mix       into main L/R/S and aux A/B/C L/R/S, each with its own 1.15 volume, ramped
 *             per sample when mixerCtrl says so.
 *
 * Only the stereo path the game uses (SCGetSoundMode says stereo): ITD, the depop
 * accumulators and the remote speaker mix are not modelled. The resampling kernels are
 * this port's own (windowed sinc), not the DSP's tables. */
#include "port_ax_internal.h"
#include <port_addr.h>
#include <port_os.h>
#include <math.h>
#include <string.h>

#define SRC_PHASES 128

/* [coefSelect][phase][tap]: taps for h[0..3], the output lying between h[1] and h[2]. */
static s32 sSrcTaps[3][SRC_PHASES][4];
static int sInitialized;

void PortAXMixInit(void) {
    /* Low-pass strength as a fraction of the input's Nyquist: 8k, 12k and 16k. */
    static const double kCutoff[3] = {0.5, 0.75, 1.0};
    int set, phase, tap;
    if (sInitialized) {
        return;
    }
    sInitialized = 1;
    for (set = 0; set < 3; set++) {
        double c = kCutoff[set];
        for (phase = 0; phase < SRC_PHASES; phase++) {
            double f = (double)phase / SRC_PHASES;
            double w[4];
            double sum = 0;
            int total = 0;
            for (tap = 0; tap < 4; tap++) {
                double x = (tap - 1) - f; /* distance from the output position */
                double sinc = x == 0 ? 1.0 : sin(M_PI * c * x) / (M_PI * c * x);
                double window = fabs(x) < 2.0 ? cos(M_PI * x / 4.0) * cos(M_PI * x / 4.0) : 0.0;
                w[tap] = sinc * window;
                sum += w[tap];
            }
            /* 1.15, unity gain at DC; the rounding goes on the nearer middle tap. */
            for (tap = 0; tap < 4; tap++) {
                sSrcTaps[set][phase][tap] = (s32)lround(w[tap] / sum * 32768.0);
                total += sSrcTaps[set][phase][tap];
            }
            sSrcTaps[set][phase][f < 0.5 ? 1 : 2] += 32768 - total;
        }
    }
}

static s16 Clamp16(s32 v) {
    return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v;
}

static u32 Addr(u16 hi, u16 lo) {
    return ((u32)hi << 16) | lo;
}

/* A voice's sample fetch for one frame: the parameter block's state in locals. */
typedef struct Fetch {
    AXPB* pb;
    const u8* base;
    u32 cur;
    u32 end;
    u32 loop;
    u16 format;
    u16 predScale;
    s16 yn1;
    s16 yn2;
    int running;
} Fetch;

static int sBadAddressReported;
static int sBadFormatReported;

/* The byte at a physical address, or NULL (the voice stops) outside the window. */
static const u8* Mem(Fetch* f, u32 byteAddr, u32 size) {
    if (byteAddr + size > PORT_ADDR_WINDOW_SIZE || byteAddr + size < byteAddr) {
        PortRedOnce(&sBadAddressReported, "AX: voice reads outside the address window (0x%08x)", byteAddr);
        f->running = 0;
        return NULL;
    }
    return f->base + byteAddr;
}

static s16 FetchSample(Fetch* f) {
    const u8* p;
    u32 at;
    s32 s;

    if (!f->running) {
        return 0;
    }
    switch (f->format) {
    case AX_FORMAT_ADPCM: {
        s32 nibble;
        int index;
        if ((f->cur & 15) == 0) {
            if ((p = Mem(f, f->cur >> 1, 1)) == NULL) {
                return 0;
            }
            f->predScale = *p;
            f->cur += 2;
        }
        at = f->cur;
        if ((p = Mem(f, at >> 1, 1)) == NULL) {
            return 0;
        }
        nibble = (at & 1) ? (*p & 15) : (*p >> 4);
        if (nibble >= 8) {
            nibble -= 16;
        }
        index = (f->predScale >> 4) & 7;
        s = ((nibble * (1 << (f->predScale & 15))) << 11) + 1024 + (s16)f->pb->adpcm.a[index][0] * f->yn1 +
            (s16)f->pb->adpcm.a[index][1] * f->yn2;
        s = Clamp16(s >> 11);
        f->yn2 = f->yn1;
        f->yn1 = (s16)s;
        break;
    }
    case AX_FORMAT_PCM16:
        at = f->cur;
        if ((p = Mem(f, at * 2, 2)) == NULL) {
            return 0;
        }
        s = (s16)((p[0] << 8) | p[1]);
        break;
    case AX_FORMAT_PCM8:
        at = f->cur;
        if ((p = Mem(f, at, 1)) == NULL) {
            return 0;
        }
        s = (s8)p[0] * 256;
        break;
    default:
        PortRedOnce(&sBadFormatReported, "AX: voice sample format 0x%x", f->format);
        f->running = 0;
        return 0;
    }

    if (at == f->end) {
        f->cur = f->loop;
        if (f->format == AX_FORMAT_ADPCM) {
            f->predScale = f->pb->adpcmLoop.loop_pred_scale;
            if (f->pb->type != AX_PB_TYPE_STREAM) {
                f->yn1 = (s16)f->pb->adpcmLoop.loop_yn1;
                f->yn2 = (s16)f->pb->adpcmLoop.loop_yn2;
            }
        }
        if (!f->pb->addr.loopFlag) {
            f->running = 0;
        }
    } else {
        f->cur = at + 1;
    }
    return (s16)s;
}

/* Adds `in` times a ramped 1.15 volume into `bus`, writing the ramped volume back. */
static void MixInto(s32* bus, const s32* in, u16* volume, const u16* delta, int ramp) {
    s32 v = *volume;
    s32 d = ramp ? (s16)*delta : 0;
    int i;
    if (v == 0 && d == 0) {
        return;
    }
    for (i = 0; i < AX_SAMPLES_PER_FRAME; i++) {
        bus[i] += (in[i] * v) >> 15;
        v += d;
        v = v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : v;
    }
    *volume = (u16)v;
}

void PortAXMixVoice(AXVPB* vpb, PortAXBuses* buses) {
    AXPB* pb = &vpb->pb;
    Fetch f;
    s32 out[AX_SAMPLES_PER_FRAME];
    s16 h[4];
    u32 ratio = Addr(pb->src.ratioHi, pb->src.ratioLo);
    u32 frac = pb->src.currentAddressFrac;
    u32 ctrl = pb->mixerCtrl;
    int i;

    f.pb = pb;
    f.base = (const u8*)gPortAddrBase;
    f.cur = Addr(pb->addr.currentAddressHi, pb->addr.currentAddressLo);
    f.end = Addr(pb->addr.endAddressHi, pb->addr.endAddressLo);
    f.loop = Addr(pb->addr.loopAddressHi, pb->addr.loopAddressLo);
    f.format = pb->addr.format;
    f.predScale = pb->adpcm.pred_scale;
    f.yn1 = (s16)pb->adpcm.yn1;
    f.yn2 = (s16)pb->adpcm.yn2;
    f.running = 1;
    for (i = 0; i < 4; i++) {
        h[i] = (s16)pb->src.last_samples[i];
    }

    /* Fetch and resample. */
    if (pb->srcSelect == 2) {
        for (i = 0; i < AX_SAMPLES_PER_FRAME; i++) {
            h[0] = h[1];
            h[1] = h[2];
            h[2] = h[3];
            h[3] = FetchSample(&f);
            out[i] = h[3];
        }
    } else {
        const s32(*taps)[4] = sSrcTaps[pb->coefSelect < 3 ? pb->coefSelect : 2];
        for (i = 0; i < AX_SAMPLES_PER_FRAME; i++) {
            u32 n;
            frac += ratio;
            for (n = frac >> 16; n > 0; n--) {
                h[0] = h[1];
                h[1] = h[2];
                h[2] = h[3];
                h[3] = FetchSample(&f);
            }
            frac &= 0xFFFF;
            if (pb->srcSelect == 1) {
                out[i] = h[1] + (s32)(((s64)(h[2] - h[1]) * frac) >> 16);
            } else {
                const s32* t = taps[frac >> 9];
                out[i] = Clamp16((s32)(((s64)h[0] * t[0] + (s64)h[1] * t[1] + (s64)h[2] * t[2] + (s64)h[3] * t[3]) >> 15));
            }
        }
    }

    /* Filters, at the output rate. */
    if (pb->lpf.on == AX_PB_LPF_ON) {
        s32 y = (s16)pb->lpf.yn1;
        s32 a = pb->lpf.a0;
        s32 b = pb->lpf.b0;
        for (i = 0; i < AX_SAMPLES_PER_FRAME; i++) {
            y = Clamp16((a * out[i] + b * y) >> 15);
            out[i] = y;
        }
        pb->lpf.yn1 = (u16)y;
    }
    if (pb->biquad.on == AX_PB_BIQUAD_ON) {
        AXPBBIQUAD* q = &pb->biquad;
        s32 x1 = (s16)q->xn1, x2 = (s16)q->xn2, y1 = (s16)q->yn1, y2 = (s16)q->yn2;
        for (i = 0; i < AX_SAMPLES_PER_FRAME; i++) {
            s32 x = out[i];
            s32 y = Clamp16((s32)(((s64)(s16)q->b0 * x + (s64)(s16)q->b1 * x1 + (s64)(s16)q->b2 * x2 +
                                   (s64)(s16)q->a1 * y1 + (s64)(s16)q->a2 * y2) >>
                                  14));
            x2 = x1;
            x1 = x;
            y2 = y1;
            y1 = y;
            out[i] = y;
        }
        q->xn1 = (u16)x1;
        q->xn2 = (u16)x2;
        q->yn1 = (u16)y1;
        q->yn2 = (u16)y2;
    }

    /* Volume envelope. */
    {
        s32 v = pb->ve.currentVolume;
        s32 d = pb->ve.currentDelta;
        for (i = 0; i < AX_SAMPLES_PER_FRAME; i++) {
            out[i] = (out[i] * v) >> 15;
            v += d;
            v = v < 0 ? 0 : v > 0xFFFF ? 0xFFFF : v;
        }
        pb->ve.currentVolume = (u16)v;
    }

    /* Buses. */
    {
        AXPBMIX* m = &pb->mix;
        if (ctrl & AX_MIXER_CTRL_L) {
            MixInto(buses->main[0], out, &m->vL, &m->vDeltaL, ctrl & AX_MIXER_CTRL_DELTA);
        }
        if (ctrl & AX_MIXER_CTRL_R) {
            MixInto(buses->main[1], out, &m->vR, &m->vDeltaR, ctrl & AX_MIXER_CTRL_DELTA);
        }
        if (ctrl & AX_MIXER_CTRL_S) {
            MixInto(buses->main[2], out, &m->vS, &m->vDeltaS, ctrl & AX_MIXER_CTRL_DELTA_S);
        }
        if (ctrl & AX_MIXER_CTRL_AL) {
            MixInto(buses->aux[0][0], out, &m->vAuxAL, &m->vDeltaAuxAL, ctrl & AX_MIXER_CTRL_DELTA_A);
        }
        if (ctrl & AX_MIXER_CTRL_AR) {
            MixInto(buses->aux[0][1], out, &m->vAuxAR, &m->vDeltaAuxAR, ctrl & AX_MIXER_CTRL_DELTA_A);
        }
        if (ctrl & AX_MIXER_CTRL_AS) {
            MixInto(buses->aux[0][2], out, &m->vAuxAS, &m->vDeltaAuxAS, ctrl & AX_MIXER_CTRL_DELTA_AS);
        }
        if (ctrl & AX_MIXER_CTRL_BL) {
            MixInto(buses->aux[1][0], out, &m->vAuxBL, &m->vDeltaAuxBL, ctrl & AX_MIXER_CTRL_DELTA_B);
        }
        if (ctrl & AX_MIXER_CTRL_BR) {
            MixInto(buses->aux[1][1], out, &m->vAuxBR, &m->vDeltaAuxBR, ctrl & AX_MIXER_CTRL_DELTA_B);
        }
        if (ctrl & AX_MIXER_CTRL_BS) {
            MixInto(buses->aux[1][2], out, &m->vAuxBS, &m->vDeltaAuxBS, ctrl & AX_MIXER_CTRL_DELTA_BS);
        }
        if (ctrl & AX_MIXER_CTRL_CL) {
            MixInto(buses->aux[2][0], out, &m->vAuxCL, &m->vDeltaAuxCL, ctrl & AX_MIXER_CTRL_DELTA_C);
        }
        if (ctrl & AX_MIXER_CTRL_CR) {
            MixInto(buses->aux[2][1], out, &m->vAuxCR, &m->vDeltaAuxCR, ctrl & AX_MIXER_CTRL_DELTA_C);
        }
        if (ctrl & AX_MIXER_CTRL_CS) {
            MixInto(buses->aux[2][2], out, &m->vAuxCS, &m->vDeltaAuxCS, ctrl & AX_MIXER_CTRL_DELTA_CS);
        }
    }

    /* Back into the parameter block. */
    pb->addr.currentAddressHi = (u16)(f.cur >> 16);
    pb->addr.currentAddressLo = (u16)f.cur;
    pb->adpcm.pred_scale = f.predScale;
    pb->adpcm.yn1 = (u16)f.yn1;
    pb->adpcm.yn2 = (u16)f.yn2;
    pb->src.currentAddressFrac = (u16)frac;
    for (i = 0; i < 4; i++) {
        pb->src.last_samples[i] = (u16)h[i];
    }
    if (!f.running) {
        pb->state = AX_VOICE_STOP;
    }
}
