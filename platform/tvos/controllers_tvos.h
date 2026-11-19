/* The Apple TV's controllers as the game's Wii Remotes (controllers_tvos.m). */
#ifndef CONTROLLERS_TVOS_H
#define CONTROLLERS_TVOS_H

#include <port_app.h>

/* Watches controllers connect and disconnect; call once on the main thread. */
void RumbleControllersStart(void);
/* Reads every controller and hands the game their state; once per display refresh, on
 * the main thread. `extra` (may be NULL) adds to channel 0: the remote's and keyboard's
 * presses that arrive as UIKit events instead. */
void RumbleControllersPoll(const PortClassicState* extra);

#endif
