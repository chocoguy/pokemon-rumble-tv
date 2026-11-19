/* Starting the game from an app shell (tvOS), in plain C so the shell need not include
 * the fork's headers. The headless runner (platform/headless/main.c) does the same
 * through port_os.h. */
#ifndef PORT_APP_H
#define PORT_APP_H

#ifdef __cplusplus
extern "C" {
#endif

/* Runs the game on its own threads and returns at once. `romDir` holds the imported
 * content files, `nandDir` stands in for the console's flash memory (saves), `scale` is
 * the EFB's multiple of 640x528. Real time: the game waits for each 60 Hz retrace.
 * Returns 0 if the GPU backend could not start (the game runs, nothing is drawn). */
int PortStartGame(const char* romDir, const char* nandDir, int scale);

/* Controllers (port/pad/port_pad.c). Each of the shell's controllers is a Wii Remote on
 * channel 0-3 with a Classic Controller attached, which the game supports as is. The
 * shell sets a channel's state once per frame from any thread; NULL means no controller
 * (channel 0 then stays connected as a bare remote, so the game never stops for it).
 * Buttons are the Classic Controller's bits (WPAD_CL_BUTTON_* in the SDK's wpad.h);
 * sticks run -1..1 with up positive, triggers 0..1. */
enum {
    PORT_CL_UP = 0x0001,
    PORT_CL_LEFT = 0x0002,
    PORT_CL_ZR = 0x0004,
    PORT_CL_X = 0x0008,
    PORT_CL_A = 0x0010,
    PORT_CL_Y = 0x0020,
    PORT_CL_B = 0x0040,
    PORT_CL_ZL = 0x0080,
    PORT_CL_R = 0x0200,
    PORT_CL_PLUS = 0x0400,
    PORT_CL_HOME = 0x0800,
    PORT_CL_MINUS = 0x1000,
    PORT_CL_L = 0x2000,
    PORT_CL_DOWN = 0x4000,
    PORT_CL_RIGHT = 0x8000
};

typedef struct {
    unsigned int buttons;
    float lx, ly, rx, ry;
    float lt, rt;
} PortClassicState;

void PortPadSetClassic(int chan, const PortClassicState* state);

#ifdef __cplusplus
}
#endif

#endif
