/* The sound output on Apple platforms (tvOS app, macOS runner with --audio): an
 * AVAudioEngine source node at the mixer's 32 kHz, fed from a ring buffer. The engine's
 * mixer converts to the device's rate.
 *
 * One writer (the AX frame on a game thread, 96 frames every 3 ms of real time) and one
 * reader (the audio render thread). The reader starts, and restarts after running dry,
 * only once kStartFrames are queued, so the game's scheduling jitter does not turn into
 * crackle. The game's frame timer drops frames it falls behind on, so the queue drains
 * on a stall rather than growing; a queue past kMaxFrames (the device running slow)
 * drops the newest frames. */
#import <AVFoundation/AVFoundation.h>
#include <TargetConditionals.h>
#include <port_audio.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

enum {
    kRingFrames = 1 << 14, /* 512 ms; a power of two */
    kStartFrames = 1536,   /* 48 ms */
    kMaxFrames = 6400,     /* 200 ms */
};

typedef struct {
    int16_t data[kRingFrames * 2];
    _Atomic uint32_t head; /* frames written, ever */
    _Atomic uint32_t tail; /* frames read, ever */
    _Atomic uint32_t underruns;
    _Atomic uint32_t dropped;
    int playing;           /* reader only */
    uint32_t written;      /* writer only: frames since the last report */
    uint32_t reportedUnderruns, reportedDropped;
} Ring;

static Ring sRing;
static AVAudioEngine* sEngine;
static AVAudioSourceNode* sSource;
static id sConfigObserver;
static int sPaused;

/* Every 10 s of sound, a line if the output ran dry or dropped frames since the last. */
static void Report(Ring* r, int count) {
    uint32_t underruns, dropped;
    r->written += (uint32_t)count;
    if (r->written < PORT_AUDIO_RATE * 10) {
        return;
    }
    r->written = 0;
    underruns = atomic_load_explicit(&r->underruns, memory_order_relaxed);
    dropped = atomic_load_explicit(&r->dropped, memory_order_relaxed);
    if (underruns != r->reportedUnderruns || dropped != r->reportedDropped) {
        NSLog(@"rumble: audio ran dry %u times, dropped %u frames (in all)", underruns, dropped);
        r->reportedUnderruns = underruns;
        r->reportedDropped = dropped;
    }
}

static void Write(const short* frames, int count, void* context) {
    Ring* r = (Ring*)context;
    uint32_t head = atomic_load_explicit(&r->head, memory_order_relaxed);
    uint32_t queued = head - atomic_load_explicit(&r->tail, memory_order_acquire);
    int i;
    if (queued + (uint32_t)count > kMaxFrames) {
        atomic_fetch_add_explicit(&r->dropped, (uint32_t)count, memory_order_relaxed);
        Report(r, count);
        return;
    }
    for (i = 0; i < count; i++) {
        uint32_t at = (head + (uint32_t)i) & (kRingFrames - 1);
        r->data[at * 2] = frames[i * 2];
        r->data[at * 2 + 1] = frames[i * 2 + 1];
    }
    atomic_store_explicit(&r->head, head + (uint32_t)count, memory_order_release);
    Report(r, count);
}

static OSStatus Render(Ring* r, BOOL* silence, AVAudioFrameCount frames, AudioBufferList* abl) {
    float* left = (float*)abl->mBuffers[0].mData;
    float* right = abl->mNumberBuffers > 1 ? (float*)abl->mBuffers[1].mData : NULL;
    uint32_t tail = atomic_load_explicit(&r->tail, memory_order_relaxed);
    uint32_t have = atomic_load_explicit(&r->head, memory_order_acquire) - tail;
    uint32_t n = 0;
    uint32_t i;

    if (!r->playing && have >= kStartFrames) {
        r->playing = 1;
    }
    if (r->playing) {
        n = have < frames ? have : frames;
        if (n < frames) {
            r->playing = 0; /* ran dry: wait for the queue to fill again */
            atomic_fetch_add_explicit(&r->underruns, 1, memory_order_relaxed);
        }
    }
    for (i = 0; i < frames; i++) {
        float a = 0, b = 0;
        if (i < n) {
            uint32_t at = (tail + i) & (kRingFrames - 1);
            a = r->data[at * 2] * (1.0f / 32768.0f);
            b = r->data[at * 2 + 1] * (1.0f / 32768.0f);
        }
        left[i] = right != NULL ? a : (a + b) * 0.5f;
        if (right != NULL) {
            right[i] = b;
        }
    }
    atomic_store_explicit(&r->tail, tail + n, memory_order_release);
    *silence = n == 0;
    return noErr;
}

static int StartEngine(void) {
    NSError* error = nil;
    if (![sEngine startAndReturnError:&error]) {
        NSLog(@"rumble: [RED] audio: the engine did not start: %@", error.localizedDescription);
        return 0;
    }
    return 1;
}

int PortAudioDeviceStart(void) {
    AVAudioFormat* format;
    AVAudioFormat* device;
    Ring* ring = &sRing;
#if TARGET_OS_TV || TARGET_OS_IOS
    {
        NSError* error = nil;
        AVAudioSession* session = [AVAudioSession sharedInstance];
        [session setCategory:AVAudioSessionCategoryPlayback error:&error];
        [session setPreferredIOBufferDuration:0.010 error:nil];
        if (![session setActive:YES error:&error]) {
            NSLog(@"rumble: audio session: %@", error.localizedDescription);
        }
    }
#endif
    sEngine = [AVAudioEngine new];
    format = [[AVAudioFormat alloc] initStandardFormatWithSampleRate:PORT_AUDIO_RATE channels:2];
    sSource = [[AVAudioSourceNode alloc]
        initWithFormat:format
           renderBlock:^OSStatus(BOOL* silence, const AudioTimeStamp* time, AVAudioFrameCount frames,
                                 AudioBufferList* abl) {
             (void)time;
             return Render(ring, silence, frames, abl);
           }];
    [sEngine attachNode:sSource];
    /* (connect:to:format:error: is new in tvOS 27; the app still runs on 26) */
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
    [sEngine connect:sSource to:sEngine.mainMixerNode format:format];
#pragma clang diagnostic pop
    if (!StartEngine()) {
        sEngine = nil;
        sSource = nil;
        return 0;
    }
    /* Another output (TV input, headphones) stops the engine; start it again. */
    sConfigObserver = [[NSNotificationCenter defaultCenter]
        addObserverForName:AVAudioEngineConfigurationChangeNotification
                    object:sEngine
                     queue:[NSOperationQueue mainQueue]
                usingBlock:^(NSNotification* note) {
                  (void)note;
                  if (!sPaused && !sEngine.running && StartEngine()) {
                      NSLog(@"rumble: audio restarted after the output changed");
                  }
                }];
    device = [sEngine.outputNode outputFormatForBus:0];
    NSLog(@"rumble: audio out, %d Hz mixed, device %.0f Hz %u channels", PORT_AUDIO_RATE, device.sampleRate,
          (unsigned)device.channelCount);
    PortAudioSetSink(Write, ring);
    return 1;
}

void PortAudioDeviceSetPaused(int paused) {
    if (sEngine == nil || sPaused == paused) {
        return;
    }
    sPaused = paused;
    if (paused) {
        [sEngine pause];
    } else {
#if TARGET_OS_TV || TARGET_OS_IOS
        [[AVAudioSession sharedInstance] setActive:YES error:nil];
#endif
        StartEngine();
    }
}
