/* The Apple TV's controllers as the game's Wii Remotes, each with a Classic Controller
 * (port_app.h, port/pad/port_pad.c). Polled once per display refresh on the main thread.
 *
 * Players: each gamepad (an extended gamepad: Xbox, PlayStation, MFi) takes the lowest
 * free channel when it connects and keeps it until it disconnects. The Siri Remote plays
 * on channel 0 only while no gamepad is on channel 0. A keyboard (and the arrow keys,
 * Return and Escape of the Simulator) adds to channel 0.
 *
 * Gamepad, by label (the game shows the Classic Controller's letters):
 *   left stick, d-pad     move              A  a (confirm, first move)
 *   right stick           right stick       B  b (back, second move)
 *   L1 / R1  L / R        L2 / R2  ZL / ZR  X  x        Y  y
 *   Menu (≡)  +  (pause)  Options  -
 * Siri Remote: touch surface or clickpad to move, click a, play/pause b, Back +.
 * Keyboard: arrows or WASD move; Z, J or Space a; X, K or Escape b; C or L x; V or I y;
 * Q L; E R; Return +; Tab or Delete -. */
#import <GameController/GameController.h>
#include <math.h>
#include <string.h>
#include <port_app.h>
#include "controllers_tvos.h"

#define CHANNELS 4

/* The Siri Remote's touch surface: a light touch near the middle should not move. */
static const float kTouchDeadZone = 0.25f;

static GCController* sGamepads[CHANNELS];
static int sLogged;

static void Log(NSString* what, GCController* c, int chan) {
    NSLog(@"rumble: controller %@ \"%@\" (%@) on channel %d", what, c.vendorName ?: @"?", c.productCategory, chan);
}

static int IsGamepad(GCController* c) {
    return c.extendedGamepad != nil;
}

static void Attach(GCController* c) {
    int i;
    if (!IsGamepad(c)) {
        return;
    }
    for (i = 0; i < CHANNELS; i++) {
        if (sGamepads[i] == c) {
            return;
        }
    }
    for (i = 0; i < CHANNELS; i++) {
        if (sGamepads[i] == nil) {
            sGamepads[i] = c;
            c.playerIndex = (GCControllerPlayerIndex)i;
            Log(@"connected", c, i);
            return;
        }
    }
    NSLog(@"rumble: controller \"%@\" ignored: four players already", c.vendorName ?: @"?");
}

static void Detach(GCController* c) {
    int i;
    for (i = 0; i < CHANNELS; i++) {
        if (sGamepads[i] == c) {
            sGamepads[i] = nil;
            Log(@"disconnected", c, i);
        }
    }
}

static void Press(PortClassicState* s, BOOL pressed, unsigned int bit) {
    if (pressed) {
        s->buttons |= bit;
    }
}

/* The larger push of two stick readings wins on each axis. */
static void Push(float* axis, float v) {
    if (fabsf(v) > fabsf(*axis)) {
        *axis = v;
    }
}

static void ReadGamepad(GCExtendedGamepad* g, PortClassicState* s) {
    Push(&s->lx, g.leftThumbstick.xAxis.value);
    Push(&s->ly, g.leftThumbstick.yAxis.value);
    Push(&s->rx, g.rightThumbstick.xAxis.value);
    Push(&s->ry, g.rightThumbstick.yAxis.value);
    Press(s, g.dpad.up.pressed, PORT_CL_UP);
    Press(s, g.dpad.down.pressed, PORT_CL_DOWN);
    Press(s, g.dpad.left.pressed, PORT_CL_LEFT);
    Press(s, g.dpad.right.pressed, PORT_CL_RIGHT);
    Press(s, g.buttonA.pressed, PORT_CL_A);
    Press(s, g.buttonB.pressed, PORT_CL_B);
    Press(s, g.buttonX.pressed, PORT_CL_X);
    Press(s, g.buttonY.pressed, PORT_CL_Y);
    Press(s, g.leftShoulder.pressed, PORT_CL_L);
    Press(s, g.rightShoulder.pressed, PORT_CL_R);
    Press(s, g.leftTrigger.pressed, PORT_CL_ZL);
    Press(s, g.rightTrigger.pressed, PORT_CL_ZR);
    Press(s, g.buttonMenu.pressed, PORT_CL_PLUS);
    Press(s, g.buttonOptions.pressed, PORT_CL_MINUS);
    if (g.leftTrigger.value > s->lt) {
        s->lt = g.leftTrigger.value;
    }
    if (g.rightTrigger.value > s->rt) {
        s->rt = g.rightTrigger.value;
    }
}

static float TouchAxis(float v) {
    float a = fabsf(v);
    if (a < kTouchDeadZone) {
        return 0.0f;
    }
    a = (a - kTouchDeadZone) / (1.0f - kTouchDeadZone) * 1.5f; /* reach full speed early */
    a = a > 1.0f ? 1.0f : a;
    return v < 0 ? -a : a;
}

static void ReadRemote(GCMicroGamepad* m, PortClassicState* s) {
    Push(&s->lx, TouchAxis(m.dpad.xAxis.value));
    Push(&s->ly, TouchAxis(m.dpad.yAxis.value));
    Press(s, m.buttonA.pressed, PORT_CL_A);
    Press(s, m.buttonX.pressed, PORT_CL_B);
    Press(s, m.buttonMenu.pressed, PORT_CL_PLUS);
}

static BOOL Key(GCKeyboardInput* k, GCKeyCode code) {
    return [k buttonForKeyCode:code].pressed;
}

static void ReadKeyboard(GCKeyboardInput* k, PortClassicState* s) {
    float x = 0, y = 0;
    if (Key(k, GCKeyCodeLeftArrow) || Key(k, GCKeyCodeKeyA)) x -= 1;
    if (Key(k, GCKeyCodeRightArrow) || Key(k, GCKeyCodeKeyD)) x += 1;
    if (Key(k, GCKeyCodeUpArrow) || Key(k, GCKeyCodeKeyW)) y += 1;
    if (Key(k, GCKeyCodeDownArrow) || Key(k, GCKeyCodeKeyS)) y -= 1;
    Push(&s->lx, x);
    Push(&s->ly, y);
    Press(s, Key(k, GCKeyCodeKeyZ) || Key(k, GCKeyCodeKeyJ) || Key(k, GCKeyCodeSpacebar), PORT_CL_A);
    Press(s, Key(k, GCKeyCodeKeyX) || Key(k, GCKeyCodeKeyK) || Key(k, GCKeyCodeEscape), PORT_CL_B);
    Press(s, Key(k, GCKeyCodeKeyC) || Key(k, GCKeyCodeKeyL), PORT_CL_X);
    Press(s, Key(k, GCKeyCodeKeyV) || Key(k, GCKeyCodeKeyI), PORT_CL_Y);
    Press(s, Key(k, GCKeyCodeKeyQ), PORT_CL_L);
    Press(s, Key(k, GCKeyCodeKeyE), PORT_CL_R);
    Press(s, Key(k, GCKeyCodeReturnOrEnter), PORT_CL_PLUS);
    Press(s, Key(k, GCKeyCodeTab) || Key(k, GCKeyCodeDeleteOrBackspace), PORT_CL_MINUS);
}

void RumbleControllersStart(void) {
    NSNotificationCenter* nc = NSNotificationCenter.defaultCenter;
    [nc addObserverForName:GCControllerDidConnectNotification
                    object:nil
                     queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification* n) {
                  GCController* c = n.object;
                  if (c.microGamepad != nil && !IsGamepad(c)) {
                      c.microGamepad.reportsAbsoluteDpadValues = YES;
                      c.microGamepad.allowsRotation = NO;
                      NSLog(@"rumble: Siri Remote \"%@\" connected", c.vendorName ?: @"?");
                  }
                  Attach(c);
                }];
    [nc addObserverForName:GCControllerDidDisconnectNotification
                    object:nil
                     queue:NSOperationQueue.mainQueue
                usingBlock:^(NSNotification* n) {
                  Detach(n.object);
                }];
    for (GCController* c in GCController.controllers) {
        if (c.microGamepad != nil && !IsGamepad(c)) {
            c.microGamepad.reportsAbsoluteDpadValues = YES;
            c.microGamepad.allowsRotation = NO;
        }
        Attach(c);
    }
}

void RumbleControllersPoll(const PortClassicState* extra) {
    PortClassicState state[CHANNELS];
    int present[CHANNELS] = {0};
    GCKeyboardInput* keyboard = GCKeyboard.coalescedKeyboard.keyboardInput;
    int i;

    memset(state, 0, sizeof(state));
    for (i = 0; i < CHANNELS; i++) {
        if (sGamepads[i] != nil && sGamepads[i].extendedGamepad != nil) {
            ReadGamepad(sGamepads[i].extendedGamepad, &state[i]);
            present[i] = 1;
        }
    }
    if (!present[0]) {
        for (GCController* c in GCController.controllers) {
            if (!IsGamepad(c) && c.microGamepad != nil) {
                ReadRemote(c.microGamepad, &state[0]);
                present[0] = 1;
                break;
            }
        }
    }
    if (keyboard != nil) {
        ReadKeyboard(keyboard, &state[0]);
        present[0] = 1;
    }
    if (extra != NULL) {
        state[0].buttons |= extra->buttons;
        Push(&state[0].lx, extra->lx);
        Push(&state[0].ly, extra->ly);
        present[0] = 1;
    }
    if (!sLogged) {
        sLogged = 1;
        NSLog(@"rumble: %lu controllers, keyboard %s", (unsigned long)GCController.controllers.count,
              keyboard != nil ? "yes" : "no");
    }
    for (i = 0; i < CHANNELS; i++) {
        PortPadSetClassic(i, present[i] ? &state[i] : NULL);
    }
}
