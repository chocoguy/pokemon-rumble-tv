/* Controllers (phase 2): one Wii Remote, held sideways with no extension, connected on
 * channel 0 and pressing nothing; no GameCube controllers. Phase 4 feeds real input
 * (GameController, keyboard, scripted files) through the same calls.
 *
 * Wii Remote memory (the game's Pokémon transfer) and the speaker are stubbed: every
 * request is refused, which the game treats as a remote it cannot use for that. */
#include <port_os.h>
#include <revolution/pad.h>
#include <revolution/wpad.h>
#include <revolution/kpad.h>
#include <string.h>

#define PORT_REMOTES 1

static WPADCallback sConnectCallback[4];
static u32 sSamples;

/* ------------------------------------------------------------------------------------ */
/* GameCube controllers: none                                                            */

BOOL PADInit(void) {
    return TRUE;
}

u32 PADRead(PADStatus* status) {
    int i;
    for (i = 0; i < 4; i++) {
        memset(&status[i], 0, sizeof(status[i]));
        status[i].err = PAD_ERR_NO_CONTROLLER;
    }
    return 0;
}

BOOL PADReset(u32 mask) {
    (void)mask;
    return TRUE;
}

BOOL PADRecalibrate(u32 mask) {
    (void)mask;
    return TRUE;
}

void PADControlMotor(s32 chan, u32 command) {
    (void)chan;
    (void)command;
}

void PADClamp(PADStatus* status) {
    (void)status;
}
void PADClamp2(PADStatus* status, u32 type) {
    (void)status;
    (void)type;
}
void PADClampCircle(PADStatus* status) {
    (void)status;
}
void PADClampCircle2(PADStatus* status, u32 type) {
    (void)status;
    (void)type;
}
void PADClampTrigger(PADStatus* status, u32 type) {
    (void)status;
    (void)type;
}

/* ------------------------------------------------------------------------------------ */
/* WPAD                                                                                  */

static WPADAlloc sAlloc;
static WPADFree sFree;

void WPADRegisterAllocator(WPADAlloc alloc, WPADFree free_) {
    sAlloc = alloc;
    sFree = free_;
}

s32 WPADProbe(s32 chan, u32* type) {
    if (chan < PORT_REMOTES) {
        if (type != NULL) {
            *type = WPAD_DEV_CORE;
        }
        return WPAD_ERR_NONE;
    }
    if (type != NULL) {
        *type = WPAD_DEV_NOT_FOUND;
    }
    return WPAD_ERR_NO_CONTROLLER;
}

s32 WPADGetInfo(s32 chan, WPADInfo* info) {
    if (chan >= PORT_REMOTES) {
        return WPAD_ERR_NO_CONTROLLER;
    }
    memset(info, 0, sizeof(*info));
    info->battery = 4;
    info->led = (u8)(1 << chan);
    return WPAD_ERR_NONE;
}

void WPADDisconnect(s32 chan) {
    (void)chan;
}

void WPADControlMotor(s32 chan, u32 command) {
    (void)chan;
    (void)command;
}

BOOL WPADIsMotorEnabled(void) {
    return TRUE;
}

s32 WPADControlSpeaker(s32 chan, u32 command, WPADCallback callback) {
    (void)chan;
    (void)command;
    (void)callback;
    return WPAD_ERR_NO_CONTROLLER;
}

BOOL WPADCanSendStreamData(s32 chan) {
    (void)chan;
    return FALSE;
}

s32 WPADSendStreamData(s32 chan, void* data, u16 length) {
    (void)chan;
    (void)data;
    (void)length;
    return WPAD_ERR_NO_CONTROLLER;
}

void WPADSetGameTitleUtf16(const u16* title) {
    (void)title;
}

s32 WPADGetGameTitleUtf16(s32 chan, u16** title) {
    (void)chan;
    *title = NULL;
    return WPAD_ERR_INVALID;
}

s32 WPADReadGameData(s32 chan, void* buf, u16 len, u16 addr, WPADCallback callback) {
    (void)chan;
    (void)buf;
    (void)len;
    (void)addr;
    (void)callback;
    return WPAD_ERR_INVALID;
}

s32 WPADWriteGameData(s32 chan, void* buf, u16 len, u16 addr, WPADCallback callback) {
    (void)chan;
    (void)buf;
    (void)len;
    (void)addr;
    (void)callback;
    return WPAD_ERR_INVALID;
}

/* ------------------------------------------------------------------------------------ */
/* KPAD                                                                                  */

static void NotifyConnected(void* param) {
    s32 chan = (s32)(intptr_t)param;
    if (sConnectCallback[chan] != NULL) {
        sConnectCallback[chan](chan, WPAD_ERR_NONE);
    }
}

void KPADInit(void) {}

WPADCallback KPADSetConnectCallback(s32 chan, WPADCallback callback) {
    WPADCallback old = sConnectCallback[chan];
    sConnectCallback[chan] = callback;
    if (callback != NULL && chan < PORT_REMOTES) {
        /* The remote is already connected: report it as the console does after start-up. */
        PortPostCallback(NotifyConnected, (void*)(intptr_t)chan);
    }
    return old;
}

s32 KPADRead(s32 chan, KPADStatus samples[], u32 length) {
    KPADStatus* s;
    if (length == 0) {
        return 0;
    }
    s = &samples[0];
    memset(s, 0, sizeof(*s));
    if (chan >= PORT_REMOTES) {
        s->wpad_err = WPAD_ERR_NO_CONTROLLER;
        s->dev_type = WPAD_DEV_NOT_FOUND;
        return 0;
    }
    sSamples++;
    s->dev_type = WPAD_DEV_CORE;
    s->wpad_err = WPAD_ERR_NONE;
    s->data_format = 0;
    /* At rest, held sideways: gravity along the remote's x axis. */
    s->acc.x = 0.0f;
    s->acc.y = -1.0f;
    s->acc.z = 0.0f;
    s->acc_value = 1.0f;
    s->horizon.x = 1.0f;
    s->horizon.y = 0.0f;
    s->dist = 1.0f;
    s->acc_vertical.x = 1.0f;
    return 1;
}

void KPADEnableDPD(s32 chan) {
    (void)chan;
}
void KPADDisableDPD(s32 chan) {
    (void)chan;
}
void KPADEnableAimingMode(s32 chan) {
    (void)chan;
}
void KPADDisableAimingMode(s32 chan) {
    (void)chan;
}
void KPADSetPosParam(s32 chan, f32 playRadius, f32 sensitivity) {
    (void)chan;
    (void)playRadius;
    (void)sensitivity;
}
void KPADSetHoriParam(s32 chan, f32 playRadius, f32 sensitivity) {
    (void)chan;
    (void)playRadius;
    (void)sensitivity;
}
void KPADSetDistParam(s32 chan, f32 playRadius, f32 sensitivity) {
    (void)chan;
    (void)playRadius;
    (void)sensitivity;
}
void KPADSetAccParam(s32 chan, f32 playRadius, f32 sensitivity) {
    (void)chan;
    (void)playRadius;
    (void)sensitivity;
}
