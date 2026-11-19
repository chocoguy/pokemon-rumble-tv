/* Controllers: up to four Wii Remotes; no GameCube controllers.
 *
 * Channel 0 is always connected, so the game never stops to ask for a remote. Without a
 * controller from the shell it is a bare remote held sideways, whose buttons come from a
 * script file (PortPadLoadScript, the headless runner's --input) or PortPadSetButtons.
 * Each controller the shell reports (PortPadSetClassic, port_app.h) is a remote with a
 * Classic Controller attached on its channel; the game reads the Classic Controller's
 * buttons and left stick itself (CControllerClassic, or CControllerWiiRemote when
 * extension controllers are off). Channels 1-3 connect and disconnect with the shell's
 * controllers; the game hears of it through its connect callback, as on the console.
 *
 * Script: one press per line, `RETRACE BUTTONS [FRAMES]`, where BUTTONS is names joined
 * by `+` (A B 1 2 PLUS MINUS HOME UP DOWN LEFT RIGHT, as the remote is held: sideways, so
 * the game sees its own mapping) and FRAMES is how long they are held (default 6). `#`
 * starts a comment. Presses may overlap; their buttons are combined. Classic Controller
 * names (CL_A CL_B CL_X CL_Y CL_L CL_R CL_ZL CL_ZR CL_PLUS CL_MINUS CL_UP CL_DOWN
 * CL_LEFT CL_RIGHT, and LS_UP LS_DOWN LS_LEFT LS_RIGHT for the left stick pushed fully)
 * attach a Classic Controller to channel 0 for the whole run, as the app shell does.
 *
 * Wii Remote memory (the game's Pokémon transfer) and the speaker are stubbed: every
 * request is refused, which the game treats as a remote it cannot use for that. */
#include <port_app.h>
#include <port_os.h>
#include <os/lock.h>
#include <revolution/pad.h>
#include <revolution/vi.h>
#include <revolution/wpad.h>
#include <revolution/kpad.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PORT_CHANNELS 4
#define PORT_SCRIPT_MAX 4096

/* Script-only bits for the left stick, above the Classic Controller's 16 buttons. */
#define LS_UP 0x10000u
#define LS_DOWN 0x20000u
#define LS_LEFT 0x40000u
#define LS_RIGHT 0x80000u

typedef struct {
    u32 start, frames, buttons, classic;
} PortPress;

static PortPress sScript[PORT_SCRIPT_MAX];
static int sScriptCount;
static int sScriptClassic; /* the script uses the Classic Controller */
static volatile u32 sShellButtons;
static u32 sLastHold;

/* The shell's controllers, written by the shell's thread, read when the game samples. */
static os_unfair_lock sPadLock = OS_UNFAIR_LOCK_INIT;
static int sAttached[PORT_CHANNELS];
static PortClassicState sClassic[PORT_CHANNELS];
/* What the game has been told (connect callbacks), and the last Classic buttons. */
static int sReported[PORT_CHANNELS];
static u32 sLastClassic[PORT_CHANNELS];

static const struct {
    const char* name;
    u32 mask;
} sButtonNames[] = {
    {"A", WPAD_BUTTON_A},       {"B", WPAD_BUTTON_B},       {"1", WPAD_BUTTON_1},
    {"2", WPAD_BUTTON_2},       {"PLUS", WPAD_BUTTON_PLUS}, {"MINUS", WPAD_BUTTON_MINUS},
    {"HOME", WPAD_BUTTON_HOME}, {"UP", WPAD_BUTTON_UP},     {"DOWN", WPAD_BUTTON_DOWN},
    {"LEFT", WPAD_BUTTON_LEFT}, {"RIGHT", WPAD_BUTTON_RIGHT},
};

static const struct {
    const char* name;
    u32 mask;
} sClassicNames[] = {
    {"CL_A", WPAD_CL_BUTTON_A},         {"CL_B", WPAD_CL_BUTTON_B},       {"CL_X", WPAD_CL_BUTTON_X},
    {"CL_Y", WPAD_CL_BUTTON_Y},         {"CL_L", WPAD_CL_TRIGGER_L},      {"CL_R", WPAD_CL_TRIGGER_R},
    {"CL_ZL", WPAD_CL_TRIGGER_ZL},      {"CL_ZR", WPAD_CL_TRIGGER_ZR},    {"CL_PLUS", WPAD_CL_BUTTON_PLUS},
    {"CL_MINUS", WPAD_CL_BUTTON_MINUS}, {"CL_HOME", WPAD_CL_BUTTON_HOME}, {"CL_UP", WPAD_CL_BUTTON_UP},
    {"CL_DOWN", WPAD_CL_BUTTON_DOWN},   {"CL_LEFT", WPAD_CL_BUTTON_LEFT}, {"CL_RIGHT", WPAD_CL_BUTTON_RIGHT},
    {"LS_UP", LS_UP},                   {"LS_DOWN", LS_DOWN},             {"LS_LEFT", LS_LEFT},
    {"LS_RIGHT", LS_RIGHT},
};

#define COUNT(a) (sizeof(a) / sizeof((a)[0]))

static int ParseButtons(char* text, u32* out, u32* classic) {
    char* save = NULL;
    char* tok;
    *out = 0;
    *classic = 0;
    for (tok = strtok_r(text, "+", &save); tok != NULL; tok = strtok_r(NULL, "+", &save)) {
        size_t i;
        int found = 0;
        for (i = 0; i < COUNT(sButtonNames) && !found; i++) {
            if (strcasecmp(tok, sButtonNames[i].name) == 0) {
                *out |= sButtonNames[i].mask;
                found = 1;
            }
        }
        for (i = 0; i < COUNT(sClassicNames) && !found; i++) {
            if (strcasecmp(tok, sClassicNames[i].name) == 0) {
                *classic |= sClassicNames[i].mask;
                found = 1;
            }
        }
        if (!found) {
            return 0;
        }
    }
    return *out != 0 || *classic != 0;
}

int PortPadLoadScript(const char* path) {
    FILE* f = fopen(path, "r");
    char line[256];
    int lineNo = 0;
    if (f == NULL) {
        PortLog("[RED] input script %s: cannot open", path);
        return 0;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        char buttons[128];
        unsigned int start, frames = 6;
        char* hash = strchr(line, '#');
        int n;
        lineNo++;
        if (hash != NULL) {
            *hash = 0;
        }
        n = sscanf(line, "%u %127s %u", &start, buttons, &frames);
        if (n <= 0) {
            continue;
        }
        if (n < 2 || sScriptCount == PORT_SCRIPT_MAX || !ParseButtons(buttons, &sScript[sScriptCount].buttons, &sScript[sScriptCount].classic)) {
            PortLog("[RED] input script %s:%d: cannot read this line", path, lineNo);
            fclose(f);
            return 0;
        }
        sScript[sScriptCount].start = start;
        sScript[sScriptCount].frames = frames;
        if (sScript[sScriptCount].classic != 0) {
            sScriptClassic = 1;
        }
        sScriptCount++;
    }
    fclose(f);
    PortLog("input script %s: %d presses", path, sScriptCount);
    return 1;
}

void PortPadSetButtons(unsigned int buttons) {
    sShellButtons = buttons;
}

void PortPadSetClassic(int chan, const PortClassicState* state) {
    if (chan < 0 || chan >= PORT_CHANNELS) {
        return;
    }
    os_unfair_lock_lock(&sPadLock);
    sAttached[chan] = state != NULL;
    if (state != NULL) {
        sClassic[chan] = *state;
    } else {
        memset(&sClassic[chan], 0, sizeof(sClassic[chan]));
    }
    os_unfair_lock_unlock(&sPadLock);
}

/* A snapshot of a channel: 0 no remote, 1 a bare remote, 2 a remote with a Classic
 * Controller (filling *classic). */
static int ChannelState(s32 chan, PortClassicState* classic) {
    int attached;
    if (chan < 0 || chan >= PORT_CHANNELS) {
        return 0;
    }
    os_unfair_lock_lock(&sPadLock);
    attached = sAttached[chan];
    if (attached && classic != NULL) {
        *classic = sClassic[chan];
    }
    os_unfair_lock_unlock(&sPadLock);
    if (attached) {
        return 2;
    }
    if (chan == 0 && sScriptClassic) {
        if (classic != NULL) {
            memset(classic, 0, sizeof(*classic));
        }
        return 2;
    }
    return chan == 0 ? 1 : 0;
}

static float Clamp(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* The script's remote buttons (the result) and Classic Controller bits (*classic) now. */
static u32 CurrentHold(u32* classic) {
    u32 now = VIGetRetraceCount();
    u32 hold = sShellButtons;
    int i;
    *classic = 0;
    for (i = 0; i < sScriptCount; i++) {
        if (now >= sScript[i].start && now < sScript[i].start + sScript[i].frames) {
            hold |= sScript[i].buttons;
            *classic |= sScript[i].classic;
        }
    }
    return hold;
}

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
    int state = ChannelState(chan, NULL);
    if (state != 0) {
        if (type != NULL) {
            *type = state == 2 ? WPAD_DEV_CLASSIC : WPAD_DEV_CORE;
        }
        return WPAD_ERR_NONE;
    }
    if (type != NULL) {
        *type = WPAD_DEV_NOT_FOUND;
    }
    return WPAD_ERR_NO_CONTROLLER;
}

s32 WPADGetInfo(s32 chan, WPADInfo* info) {
    if (ChannelState(chan, NULL) == 0) {
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

static void NotifyDisconnected(void* param) {
    s32 chan = (s32)(intptr_t)param;
    if (sConnectCallback[chan] != NULL) {
        sConnectCallback[chan](chan, WPAD_ERR_NO_CONTROLLER);
    }
}

/* Tells the game when a channel's remote comes or goes. */
static void ReportConnection(s32 chan, int connected) {
    if (sReported[chan] == connected) {
        return;
    }
    sReported[chan] = connected;
    PortLog("pad: channel %d %s", (int)chan, connected ? "connected" : "disconnected");
    PortPostCallback(connected ? NotifyConnected : NotifyDisconnected, (void*)(intptr_t)chan);
}

void KPADInit(void) {}

WPADCallback KPADSetConnectCallback(s32 chan, WPADCallback callback) {
    WPADCallback old = sConnectCallback[chan];
    sConnectCallback[chan] = callback;
    if (callback != NULL && ChannelState(chan, NULL) != 0) {
        /* The remote is already connected: report it as the console does after start-up. */
        sReported[chan] = 1;
        PortPostCallback(NotifyConnected, (void*)(intptr_t)chan);
    }
    return old;
}

s32 KPADRead(s32 chan, KPADStatus samples[], u32 length) {
    KPADStatus* s;
    PortClassicState classic = {0};
    int state;
    if (length == 0 || chan < 0 || chan >= PORT_CHANNELS) {
        return 0;
    }
    s = &samples[0];
    memset(s, 0, sizeof(*s));
    state = ChannelState(chan, &classic);
    if (sConnectCallback[chan] != NULL) {
        ReportConnection(chan, state != 0);
    }
    if (state == 0) {
        s->wpad_err = WPAD_ERR_NO_CONTROLLER;
        s->dev_type = WPAD_DEV_NOT_FOUND;
        sLastClassic[chan] = 0;
        return 0;
    }
    sSamples++;
    if (chan == 0) {
        u32 scriptClassic;
        u32 hold = CurrentHold(&scriptClassic);
        classic.buttons |= scriptClassic & 0xFFFF;
        if (scriptClassic & (LS_UP | LS_DOWN)) {
            classic.ly = (scriptClassic & LS_UP) ? 1.0f : -1.0f;
        }
        if (scriptClassic & (LS_LEFT | LS_RIGHT)) {
            classic.lx = (scriptClassic & LS_RIGHT) ? 1.0f : -1.0f;
        }
        s->hold = hold;
        s->trig = hold & ~sLastHold;
        s->release = sLastHold & ~hold;
        sLastHold = hold;
    }
    s->wpad_err = WPAD_ERR_NONE;
    if (state == 2) {
        u32 hold = classic.buttons & 0xFFFF;
        s->dev_type = WPAD_DEV_CLASSIC;
        s->data_format = WPAD_FMT_CLASSIC;
        s->ex_status.cl.hold = hold;
        s->ex_status.cl.trig = hold & ~sLastClassic[chan];
        s->ex_status.cl.release = sLastClassic[chan] & ~hold;
        s->ex_status.cl.lstick.x = Clamp(classic.lx, -1.0f, 1.0f);
        s->ex_status.cl.lstick.y = Clamp(classic.ly, -1.0f, 1.0f);
        s->ex_status.cl.rstick.x = Clamp(classic.rx, -1.0f, 1.0f);
        s->ex_status.cl.rstick.y = Clamp(classic.ry, -1.0f, 1.0f);
        s->ex_status.cl.ltrigger = Clamp(classic.lt, 0.0f, 1.0f);
        s->ex_status.cl.rtrigger = Clamp(classic.rt, 0.0f, 1.0f);
        sLastClassic[chan] = hold;
    } else {
        s->dev_type = WPAD_DEV_CORE;
        s->data_format = WPAD_FMT_CORE;
        sLastClassic[chan] = 0;
    }
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
