/* SC (system configuration), DVD and FA (SD card) as the port's fixed console:
 * 16:9, English, stereo; no disc drive (a WiiWare title never reads one); no SD card. */
#include <port_os.h>
#include <revolution/sc.h>
#include <revolution/dvd.h>
#include <revolution/fa.h>

void SCInit(void) {}

u32 SCCheckStatus(void) {
    return SC_STATUS_OK;
}

u8 SCGetAspectRatio(void) {
    return 1; /* 16:9 */
}

u8 SCGetLanguage(void) {
    return SC_LANG_ENGLISH;
}

u8 SCGetSoundMode(void) {
    return SC_SOUND_MODE_STEREO;
}

u8 SCGetEuRgb60Mode(void) {
    return 0;
}

u8 SCGetProgressiveMode(void) {
    return 1;
}

u8 SCGetScreenSaverMode(void) {
    return 0;
}

void DVDInit(void) {}

s32 DVDGetDriveStatus(void) {
    return DVD_STATE_END;
}

/* FA: the SD card library. No card is ever present: registration and initialisation
 * succeed, attaching a drive fails, so photo saving reports that there is no SD card. */
#define FA_NO_CARD (-1)

s32 FAInit(long config) {
    (void)config;
    return 0;
}

s32 FARegistCB(u8 device, FAMediaCallback inserted, FAMediaCallback ejected) {
    (void)device;
    (void)inserted;
    (void)ejected;
    return 0;
}

s32 FAAttachAsync(u8 device, s8* nandPath, u32 nandSize, FADrvTbl* drivers, FACallback callback, FACommandBlock* block) {
    (void)device;
    (void)nandPath;
    (void)nandSize;
    (void)drivers;
    (void)callback;
    (void)block;
    return FA_NO_CARD;
}

s32 FADetach(s8 drive) {
    (void)drive;
    return FA_NO_CARD;
}
s32 FAErrnum(void) {
    return FA_NO_CARD;
}
s32 FAMountAsync(s8 drive, FACallback callback, FACommandBlock* block) {
    (void)drive;
    (void)callback;
    (void)block;
    return FA_NO_CARD;
}
s32 FAUnmount(s8 drive, u32 mode) {
    (void)drive;
    (void)mode;
    return FA_NO_CARD;
}
s32 FAUnmountAsync(s8 drive, u32 mode, FACallback callback, FACommandBlock* block) {
    (void)drive;
    (void)mode;
    (void)callback;
    (void)block;
    return FA_NO_CARD;
}
s32 FAGetdev(s8 drive, struct PF_DEV_INF* info) {
    (void)drive;
    (void)info;
    return FA_NO_CARD;
}
s32 FAIsWriteProtected(s8 drive) {
    (void)drive;
    return 1;
}
s32 FASetupFSI(s8 drive, s16 flags) {
    (void)drive;
    (void)flags;
    return FA_NO_CARD;
}
s32 FABuffering(s8 drive, s32 mode) {
    (void)drive;
    (void)mode;
    return FA_NO_CARD;
}
s32 FAChdir(const char* path) {
    (void)path;
    return FA_NO_CARD;
}
s32 FARemove(const char* path) {
    (void)path;
    return FA_NO_CARD;
}
s32 FAFclose(FAFile* file) {
    (void)file;
    return FA_NO_CARD;
}
s32 FACreateAsync(const char* path, s32 mode, FAFile** file, FACallback callback, FACommandBlock* block) {
    (void)path;
    (void)mode;
    (void)file;
    (void)callback;
    (void)block;
    return FA_NO_CARD;
}
s32 FAFwriteAsync(void* buffer, u32 size, u32 count, FAFile* file, u32* written, FACallback callback,
                  FACommandBlock* block) {
    (void)buffer;
    (void)size;
    (void)count;
    (void)file;
    (void)written;
    (void)callback;
    (void)block;
    return FA_NO_CARD;
}
s32 FAFsfirstAsync(const char* path, u8 attributes, FADta* dta, FACallback callback, FACommandBlock* block) {
    (void)path;
    (void)attributes;
    (void)dta;
    (void)callback;
    (void)block;
    return FA_NO_CARD;
}
s32 FAFsnextAsync(FADta* dta, FACallback callback, FACommandBlock* block) {
    (void)dta;
    (void)callback;
    (void)block;
    return FA_NO_CARD;
}
s32 FAMkdirAsync(const char* path, FACallback callback, FACommandBlock* block) {
    (void)path;
    (void)callback;
    (void)block;
    return FA_NO_CARD;
}
s32 FARemoveAsync(const char* path, FACallback callback, FACommandBlock* block) {
    (void)path;
    (void)callback;
    (void)block;
    return FA_NO_CARD;
}
s32 FARmdirAsync(const char* path, FACallback callback, FACommandBlock* block) {
    (void)path;
    (void)callback;
    (void)block;
    return FA_NO_CARD;
}
