/* The rest of the OS library: reports and panics, the run summary, calendar time,
 * reset and shutdown, and the system-menu calls the port answers itself. */
#include <port_os.h>
#include <port_app.h>
#include <revolution/os.h>
#include <revolution/os/OSTime.h>
#include <revolution/os/OSReset.h>
#include <revolution/os/OSResetSW.h>
#include <revolution/os/OSFont.h>
#include <revolution/gx/GXStruct.h>
#include <revolution/os/OSAlloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

PortOSConfig gPortConfig;

/* ------------------------------------------------------------------------------------ */
/* Logging                                                                               */

static unsigned int sRedCount;

void PortLog(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fputs("[port] ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

static void VRed(const char* fmt, va_list args) {
    sRedCount++;
    fputs("[RED] ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
}

void PortRed(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    VRed(fmt, args);
    va_end(args);
}

void PortRedOnce(int* reported, const char* fmt, ...) {
    va_list args;
    if (*reported) {
        return;
    }
    *reported = 1;
    va_start(args, fmt);
    VRed(fmt, args);
    va_end(args);
}

unsigned int PortRedCount(void) {
    return sRedCount;
}

void PortFatal(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    fputs("[RED] fatal: ", stderr);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
    PortDumpThreads();
    fflush(NULL);
    abort();
}

void OSReport(const char* msg, ...) {
    va_list args;
    va_start(args, msg);
    vfprintf(stdout, msg, args);
    va_end(args);
    fflush(stdout);
}

void OSVReport(const char* msg, va_list args) {
    vfprintf(stdout, msg, args);
    fflush(stdout);
}

void OSPanic(const char* file, int line, const char* msg, ...) {
    va_list args;
    va_start(args, msg);
    fprintf(stderr, "[RED] OSPanic %s:%d: ", file, line);
    vfprintf(stderr, msg, args);
    fputc('\n', stderr);
    va_end(args);
    PortDumpThreads();
    fflush(NULL);
    abort();
}

void OSFatal(GXColor fg, GXColor bg, const char* msg) {
    (void)fg;
    (void)bg;
    PortFatal("OSFatal: %s", msg);
}

void OSRegisterVersion(const char* id) {
    (void)id;
}

/* MSL's helper: the length printf would produce. */
int __CountPrintf(const char* format, va_list args) {
    return vsnprintf(NULL, 0, format, args);
}

/* ------------------------------------------------------------------------------------ */
/* Run summary and exit                                                                  */

static PortSummaryFunc sSummaries[16];
static int sSummaryCount;
static unsigned int sRetraces;
static unsigned int sFramesPresented;
static unsigned int sLastPresentRetrace;
static int sExitCode = -1;

void PortAddSummary(PortSummaryFunc func) {
    if (sSummaryCount < (int)(sizeof(sSummaries) / sizeof(sSummaries[0]))) {
        sSummaries[sSummaryCount++] = func;
    }
}

unsigned int PortFramesPresented(void) {
    return sFramesPresented;
}

void PortNoteFramePresented(void) {
    sFramesPresented++;
    sLastPresentRetrace = sRetraces;
}

void PortNoteRetrace(unsigned int retraceCount) {
    sRetraces = retraceCount;
    if (gPortConfig.stopAfterRetraces != 0 && retraceCount >= gPortConfig.stopAfterRetraces) {
        PortExit(0);
    }
    /* A game that stops presenting for a minute is stuck. */
    if (retraceCount - sLastPresentRetrace == 60 * 60) {
        PortRed("watchdog: no frame presented for 60 seconds");
        PortDumpThreads();
        PortExit(4);
    }
}

void PortExit(int code) {
    int i;
    fflush(stdout);
    fprintf(stderr, "[port] exit %d after %u retraces, %u frames presented, %u [RED] lines\n", code, sRetraces,
            sFramesPresented, sRedCount);
    for (i = 0; i < sSummaryCount; i++) {
        sSummaries[i]();
    }
    fflush(NULL);
    _exit(code);
}

int PortOSWaitForExit(void) {
    for (;;) {
        pause();
    }
    return sExitCode;
}

/* ------------------------------------------------------------------------------------ */
/* Calendar (OSTime.c)                                                                   */

static int YearDays[12] = {0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334};
static int LeapYearDays[12] = {0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335};

static BOOL IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

static int GetLeapDays(int year) {
    if (year < 1) {
        return 0;
    }
    return (year + 3) / 4 - (year - 1) / 100 + (year - 1) / 400;
}

static void GetDates(s32 days, OSCalendarTime* pTime) {
    int year;
    int dayCount;
    int month;
    int* monthArr;

    pTime->wday = (days + 6) % 7;
    for (year = days / 365; days < (dayCount = GetLeapDays(year) + 365 * year); --year)
        ;
    days -= dayCount;
    pTime->year = year;
    pTime->yday = days;
    monthArr = IsLeapYear(year) ? LeapYearDays : YearDays;
    for (month = 12; days < monthArr[--month];)
        ;
    pTime->mon = month;
    pTime->mday = days - monthArr[month] + 1;
}

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* pTime) {
    int numDays;
    int numSecs;
    OSTime ticksAfter = ticks % OSSecondsToTicks(1);

    if (ticksAfter < 0) {
        ticksAfter += OSSecondsToTicks(1);
    }
    pTime->usec = (int)(OSTicksToMicroseconds(ticksAfter) % 1000);
    pTime->msec = (int)(OSTicksToMilliseconds(ticksAfter) % 1000);
    ticks -= ticksAfter;
    numDays = (int)(OSTicksToSeconds(ticks) / 86400 + 0xB2575);
    numSecs = (int)(OSTicksToSeconds(ticks) % 86400);
    if (numSecs < 0) {
        numDays -= 1;
        numSecs += 86400;
    }
    GetDates(numDays, pTime);
    pTime->hour = numSecs / 60 / 60;
    pTime->min = (numSecs / 60) % 60;
    pTime->sec = numSecs % 60;
}

static int GetYearDays(int year, int mon) {
    return (IsLeapYear(year) ? LeapYearDays : YearDays)[mon];
}

OSTime OSCalendarTimeToTicks(const OSCalendarTime* cal) {
    s64 seconds;
    int ovMon = cal->mon / 12;
    int month = cal->mon - (ovMon * 12);
    int year;

    if (month < 0) {
        month += 12;
        ovMon--;
    }
    year = cal->year + ovMon;
    seconds = (s64)31536000 * year + (s64)86400 * (cal->mday + GetLeapDays(year) + GetYearDays(year, month) - 1) +
              (s64)3600 * cal->hour + (s64)60 * cal->min + cal->sec - (s64)0xEB1E1BF80ULL;
    return OSSecondsToTicks(seconds) + OSMillisecondsToTicks((s64)cal->msec) + OSMicrosecondsToTicks((s64)cal->usec);
}

/* ------------------------------------------------------------------------------------ */
/* Reset, shutdown and the system menu                                                   */

static OSShutdownFunctionInfo* sShutdownFunctions;
static OSResetCallback sResetCallback;
static OSPowerCallback sPowerCallback;

void OSRegisterShutdownFunction(OSShutdownFunctionInfo* info) {
    OSShutdownFunctionInfo** link = &sShutdownFunctions;
    while (*link != NULL && (*link)->priority <= info->priority) {
        link = &(*link)->next;
    }
    info->next = *link;
    info->prev = NULL;
    *link = info;
}

void OSUnregisterShutdownFunction(OSShutdownFunctionInfo* info) {
    OSShutdownFunctionInfo** link = &sShutdownFunctions;
    while (*link != NULL && *link != info) {
        link = &(*link)->next;
    }
    if (*link == info) {
        *link = info->next;
    }
}

OSResetCallback OSSetResetCallback(OSResetCallback callback) {
    OSResetCallback old = sResetCallback;
    sResetCallback = callback;
    return old;
}

OSPowerCallback OSSetPowerCallback(OSPowerCallback callback) {
    OSPowerCallback old = sPowerCallback;
    sPowerCallback = callback;
    return old;
}

static void Leave(const char* how) {
    PortLog("the game asked to %s", how);
    PortExit(0);
}

void OSRestart(u32 resetCode) {
    (void)resetCode;
    Leave("restart");
}
void OSReturnToMenu(void) {
    Leave("return to the Wii Menu");
}
void OSReturnToDataManager(void) {
    Leave("open the Data Management screen");
}
void OSReturnToSetting(u32 type) {
    (void)type;
    Leave("open Wii Settings");
}
void OSLaunchManualViewer(u32 arg) {
    (void)arg;
    PortLog("OSLaunchManualViewer: there is no manual on the port");
}
void OSShutdownSystem(void) {
    Leave("power off");
}

/* ------------------------------------------------------------------------------------ */
/* Title and system font                                                                 */

const char* OSGetAppGamename(void) {
    return "WPSE";
}

/* OS_FONT_ENCODE_ANSI: the USA console's IPL font. The port has no IPL font, so the
 * font calls report failure and the game uses its own fonts. */
u16 OSGetFontEncode(void) {
    return 0;
}

BOOL OSInitFont(OSFontHeader* fontData) {
    static int reported;
    (void)fontData;
    PortRedOnce(&reported, "OSInitFont: no IPL font on the port");
    return FALSE;
}

char* OSGetFontTexture(const char* string, void** image, s32* x, s32* y, s32* width) {
    *image = NULL;
    *x = *y = *width = 0;
    return (char*)(string + (*string != '\0'));
}

char* OSGetFontWidth(const char* string, s32* width) {
    *width = 0;
    return (char*)(string + (*string != '\0'));
}

/* ------------------------------------------------------------------------------------ */
/* Low-level bits the compiled SDK code calls                                            */

void PPCHalt(void) {
    PortFatal("PPCHalt");
}

s32 NETGetWirelessMacAddress(u8* macAddr) {
    /* A fixed, locally administered address. */
    static const u8 kMac[6] = {0x02, 0x00, 0x57, 0x50, 0x53, 0x45};
    memcpy(macAddr, kMac, sizeof(kMac));
    return 0;
}

/* ------------------------------------------------------------------------------------ */
/* OSAlloc: only AXFX's default hooks reach it, and NW4R replaces those.                 */

volatile OSHeapHandle __OSCurrHeap = -1;

void* OSAllocFromHeap(OSHeapHandle heap, u32 size) {
    PortRed("OSAllocFromHeap(%d, %u): the port has no OS heaps", (int)heap, size);
    return NULL;
}

void OSFreeToHeap(OSHeapHandle heap, void* ptr) {
    (void)heap;
    (void)ptr;
}
