/* Entry point of the headless build (macOS), used to check the link and, from phase 2,
 * to run the game without a window. */
#include <stdio.h>
#include <string.h>

void GameMain(void); /* src/game/core/main.cpp */

int main(int argc, char** argv) {
    int i;
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--run") == 0) {
            GameMain();
            return 0;
        }
    }
    printf("rumble: linked. Most of the Wii SDK is still stubbed (see build/link_stubs.txt);\n"
           "running the game needs phase 2. Pass --run to try anyway.\n");
    return 0;
}
