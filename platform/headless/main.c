/* The headless runner (macOS): runs the game with no window, no sound and no input.
 *
 *   rumble [--frames N] [--rom DIR] [--nand DIR] [--realtime]
 *   rumble --check-assets [--rom DIR]
 *
 * --frames N   stop after N video retraces (60 per second of game time)
 * --rom DIR    the imported content files (default: rom/content in the repository)
 * --nand DIR   the folder standing in for the console's NAND, where saves go
 *              (default: build/nand in the repository)
 * --realtime   run at the console's speed instead of skipping idle time
 * --check-assets  run every content file through its byte-order fixup and exit
 *
 * The exit status is 0 when the run reached its end without a fatal error. The summary
 * on stderr counts [RED] lines, frames and GX commands. */
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
    fprintf(stderr, "usage: rumble [--frames N] [--rom DIR] [--nand DIR] [--realtime]\n"
                    "       rumble --check-assets [--rom DIR]\n");
    exit(2);
}

int main(int argc, char** argv) {
    int i;
    struct stat st;
    int checkAssets = 0;

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

    PortOSStart(GameMain);
    return PortOSWaitForExit();
}
