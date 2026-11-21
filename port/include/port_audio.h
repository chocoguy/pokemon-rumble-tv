/* Sound out of the AX mixer (port/ax), in plain C so the shells need not include the
 * fork's headers.
 *
 * The mixer makes one 3 ms frame at a time: 96 stereo frames at 32 kHz, signed 16-bit,
 * interleaved left/right. It runs on a game thread in interrupt context (the AX frame
 * timer), in real time on the app and in game time in fast-time runs. */
#ifndef PORT_AUDIO_H
#define PORT_AUDIO_H

#ifdef __cplusplus
extern "C" {
#endif

#define PORT_AUDIO_RATE 32000

/* Receives every mixed frame. Must not block: copy and return. */
typedef void (*PortAudioSink)(const short* frames, int count, void* context);

/* Sets where mixed frames go (one sink; NULL for none). Call before the game starts. */
void PortAudioSetSink(PortAudioSink sink, void* context);

/* Also writes everything mixed to a 16-bit stereo WAV file, finished at exit (headless
 * runs). Returns 0 if the file cannot be created. */
int PortAudioWriteWav(const char* path);

/* The output device (platform/apple/port_audio_apple.m, AVAudioEngine): installs itself
 * as the sink. Returns 0 if no output could be started (the game runs silent). */
int PortAudioDeviceStart(void);
/* Stops and restarts the output (the app going to the background and back). */
void PortAudioDeviceSetPaused(int paused);

#ifdef __cplusplus
}
#endif

#endif
