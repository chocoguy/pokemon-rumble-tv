/* The headless runner (macOS): runs the game with no window and no live input.
 *
 *   rumble [--frames N] [--rom DIR] [--nand DIR] [--realtime] [--scale N] [--no-gpu]
 *          [--dump-frame K PATH]... [--dump-every N DIR] [--input FILE]
 *          [--wav PATH] [--audio]
 *   rumble --check-assets [--rom DIR]
 *
 * --frames N   stop after N video retraces (60 per second of game time)
 * --rom DIR    the imported content files (default: rom/content in the repository)
 * --nand DIR   the folder standing in for the console's NAND, where saves go
 *              (default: build/nand in the repository)
 * --realtime   run at the console's speed instead of skipping idle time
 * --scale N    render the EFB at N times the console's resolution (default 1)
 * --no-gpu     draw nothing; the command stream is only checked
 * --dump-frame K PATH  write what the display shows at retrace K as a PNG (repeatable)
 * --dump-every N DIR  also write every Nth retrace as DIR/frame-K.png
 * --input FILE  press the Wii Remote's buttons as the script says (port/pad/port_pad.c)
 * --wav PATH   write everything the sound mixer outputs to a WAV file (32 kHz stereo)
 * --audio      play the sound on this Mac's output (implies --realtime)
 * --check-assets  run every content file through its byte-order fixup and exit
 *
 * The exit status is 0 when the run reached its end without a fatal error. The summary
 * on stderr counts [RED] lines, frames and GX commands. */
#include <port_audio.h>
#include <port_os.h>
#include <port_endian.h>
#include <limits.h>
#include <mach-o/dyld.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

void GameMain(void); /* src/game/core/main.cpp */

static char sRom[PATH_MAX];
static char sNand[PATH_MAX];

/* The repository root: two levels above the executable (build/<config>/rumble). */
static void RepoPath(char* out, size_t size, const char* relative) {
    char exe[PATH_MAX];
    char real[PATH_MAX];
    uint32_t length = sizeof(exe);
    char* slash;
    int i;
    if (_NSGetExecutablePath(exe, &length) != 0 || realpath(exe, real) == NULL) {
        snprintf(out, size, "%s", relative);
        return;
    }
    for (i = 0; i < 3; i++) {
        slash = strrchr(real, '/');
        if (slash != NULL) {
            *slash = '\0';
        }
    }
    snprintf(out, size, "%s/%s", real, relative);
}

static void Usage(void) {
    fprintf(stderr, "usage: rumble [--frames N] [--rom DIR] [--nand DIR] [--realtime] [--scale N] [--no-gpu]\n"
                    "              [--dump-frame K PATH]... [--dump-every N DIR] [--input FILE]\n"
                    "              [--wav PATH] [--audio]\n"
                    "       rumble --check-assets [--rom DIR]\n");
    exit(2);
}

int main(int argc, char** argv) {
    int i;
    struct stat st;
    int checkAssets = 0;
    int gpu = 1;
    int scale = 1;
    int audio = 0;
    const char* wav = NULL;

    RepoPath(sRom, sizeof(sRom), "rom/content");
    RepoPath(sNand, sizeof(sNand), "build/nand");
    gPortConfig.fastTime = 1;

    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--frames") == 0 && i + 1 < argc) {
            gPortConfig.stopAfterRetraces = (unsigned int)strtoul(argv[++i], NULL, 10);
        } else if (strcmp(argv[i], "--rom") == 0 && i + 1 < argc) {
            snprintf(sRom, sizeof(sRom), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--nand") == 0 && i + 1 < argc) {
            snprintf(sNand, sizeof(sNand), "%s", argv[++i]);
        } else if (strcmp(argv[i], "--realtime") == 0) {
            gPortConfig.fastTime = 0;
        } else if (strcmp(argv[i], "--scale") == 0 && i + 1 < argc) {
            scale = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--no-gpu") == 0) {
            gpu = 0;
        } else if (strcmp(argv[i], "--dump-frame") == 0 && i + 2 < argc) {
            PortGXRequestDump((unsigned int)strtoul(argv[i + 1], NULL, 10), argv[i + 2]);
            i += 2;
        } else if (strcmp(argv[i], "--dump-every") == 0 && i + 2 < argc) {
            PortGXRequestDumpEvery((unsigned int)strtoul(argv[i + 1], NULL, 10), argv[i + 2]);
            i += 2;
        } else if (strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            if (!PortPadLoadScript(argv[++i])) {
                return 2;
            }
        } else if (strcmp(argv[i], "--wav") == 0 && i + 1 < argc) {
            wav = argv[++i];
        } else if (strcmp(argv[i], "--audio") == 0) {
            audio = 1;
            gPortConfig.fastTime = 0;
        } else if (strcmp(argv[i], "--check-assets") == 0) {
            checkAssets = 1;
        } else if (strcmp(argv[i], "--headless") == 0) {
            /* the only mode this runner has */
        } else {
            Usage();
        }
    }
    gPortConfig.romDir = sRom;
    gPortConfig.nandDir = sNand;

    if (stat(sRom, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fprintf(stderr, "rumble: no game files at %s; run tools/import_wad.py first\n", sRom);
        return 1;
    }
    if (checkAssets) {
        return PortCheckAssets(sRom);
    }
    PortLog("content %s, NAND %s, %s time%s", sRom, sNand, gPortConfig.fastTime ? "fast" : "real",
            gPortConfig.stopAfterRetraces ? "" : ", no frame limit");

    if (gpu) {
        PortGXStart(scale);
    }
    if (wav != NULL && !PortAudioWriteWav(wav)) {
        fprintf(stderr, "rumble: cannot write %s\n", wav);
        return 1;
    }
    if (audio && !PortAudioDeviceStart()) {
        PortLog("no sound output; running silent");
    }
    PortOSStart(GameMain);
    return PortOSWaitForExit();
}
