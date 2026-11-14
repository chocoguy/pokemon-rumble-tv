/* VI: the video interface. A 60 Hz retrace interrupt, the frame buffer the game asks to
 * show, and the waits on retrace. The port always reports an NTSC progressive display, so
 * the game picks its 480p render mode (GXNtsc480Prog) with no deflicker filter.
 *
 * Phase 2 shows nothing; the renderer (phase 3) presents the frame buffer named here. */
#include <port_os.h>
#include <revolution/os/OSThread.h>
#include <revolution/vi.h>
#include <revolution/gx/GXStruct.h>

static volatile u32 sRetraceCount;
static OSThreadQueue sRetraceQueue;
static VIRetraceCallback sPreRetrace;
static VIRetraceCallback sPostRetrace;
static void* sCurrentFrameBuffer;
static void* sNextFrameBuffer;
static BOOL sFlushPending;
static BOOL sBlack = TRUE;
static const GXRenderModeObj* sMode;
static int sInitialized;

static void Retrace(void) {
    sRetraceCount++;
    if (sPreRetrace != NULL) {
        sPreRetrace(sRetraceCount);
    }
    if (sFlushPending) {
        sCurrentFrameBuffer = sNextFrameBuffer;
        sFlushPending = FALSE;
    }
    if (sPostRetrace != NULL) {
        sPostRetrace(sRetraceCount);
    }
    OSWakeupThread(&sRetraceQueue);
    PortNoteRetrace(sRetraceCount);
}

void VIInit(void) {
    if (sInitialized) {
        return;
    }
    sInitialized = 1;
    OSInitThreadQueue(&sRetraceQueue);
    PortAddTimer(Retrace, PortNow() + OS_TIMER_CLOCK / 60, OS_TIMER_CLOCK / 60);
}

void VIConfigure(const GXRenderModeObj* mode) {
    sMode = mode;
}

void VIConfigurePan(u16 x, u16 y, u16 w, u16 h) {
    (void)x;
    (void)y;
    (void)w;
    (void)h;
}

void VIFlush(void) {
    BOOL enabled = OSDisableInterrupts();
    sFlushPending = TRUE;
    PortNoteFramePresented();
    OSRestoreInterrupts(enabled);
}

void VISetNextFrameBuffer(void* fb) {
    BOOL enabled = OSDisableInterrupts();
    sNextFrameBuffer = fb;
    OSRestoreInterrupts(enabled);
}

void* VIGetNextFrameBuffer(void) {
    return sNextFrameBuffer;
}

void* VIGetCurrentFrameBuffer(void) {
    return sCurrentFrameBuffer;
}

void VISetBlack(BOOL black) {
    sBlack = black;
}

u32 VIGetRetraceCount(void) {
    PortSafePoint();
    return sRetraceCount;
}

void VIWaitForRetrace(void) {
    BOOL enabled = OSDisableInterrupts();
    u32 count = sRetraceCount;
    do {
        OSSleepThread(&sRetraceQueue);
    } while (count == sRetraceCount);
    OSRestoreInterrupts(enabled);
}

VIRetraceCallback VISetPreRetraceCallback(VIRetraceCallback callback) {
    BOOL enabled = OSDisableInterrupts();
    VIRetraceCallback old = sPreRetrace;
    sPreRetrace = callback;
    OSRestoreInterrupts(enabled);
    return old;
}

VIRetraceCallback VISetPostRetraceCallback(VIRetraceCallback callback) {
    BOOL enabled = OSDisableInterrupts();
    VIRetraceCallback old = sPostRetrace;
    sPostRetrace = callback;
    OSRestoreInterrupts(enabled);
    return old;
}

u32 VIGetTvFormat(void) {
    return VI_NTSC;
}

u32 VIGetScanMode(void) {
    return VI_PROGRESSIVE;
}

u32 VIGetDTVStatus(void) {
    return 1;
}

u32 VIGetNextField(void) {
    return 1;
}

/* For the renderer and the summary. */
const GXRenderModeObj* PortVIGetMode(void) {
    return sMode;
}

BOOL PortVIIsBlack(void) {
    return sBlack;
}
