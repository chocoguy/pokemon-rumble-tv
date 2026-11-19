/* PortStartGame (port_app.h): the app shell's way in. */
#include <port_app.h>
#include <port_os.h>
#include <stdlib.h>
#include <string.h>

void GameMain(void); /* src/game/core/main.cpp */

int PortStartGame(const char* romDir, const char* nandDir, int scale) {
    int gpu;
    gPortConfig.romDir = strdup(romDir);
    gPortConfig.nandDir = strdup(nandDir);
    gPortConfig.fastTime = 0;
    gPortConfig.stopAfterRetraces = 0;
    PortLog("content %s, NAND %s, EFB scale %d", romDir, nandDir, scale);
    gpu = PortGXStart(scale);
    PortOSStart(GameMain);
    return gpu;
}
