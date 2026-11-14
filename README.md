# Pokémon Rumble for tvOS

A native port of the Pokémon Rumble (WiiWare, USA) decompilation in `pokemon-rumble-decomp-tv/` to arm64 macOS and Apple TV. It contains no game data: you supply your own copy of the game.

Status: phase 2 of the plan. The game boots headless on the Mac: it loads its archives, layouts, fonts, messages, effects and sound archive, shows the strap-reminder screen and reaches the title session. Nothing is drawn or played yet (the renderer is phase 3, audio phase 5), and the title screen stops at its first music sequence, a sound format phase 5 converts.

## Game files

Install your WAD in Dolphin (Tools > Install WAD), then run the import with Python 3.10 or newer (macOS's own `/usr/bin/python3` is 3.9, so use Homebrew's):

```bash
/opt/homebrew/bin/python3 tools/import_wad.py
```

It copies the decrypted contents to `rom/content/`, plus the Home Button Menu's shared contents (2-4), which Dolphin keeps in `Wii/shared1`, and checks the executable against the decomp's manifest.

## Building

Needs Xcode, CMake and Ninja.

```bash
cmake -S . -B build/mac -G Ninja && cmake --build build/mac
```

```bash
cmake -S . -B build/tvos -G Ninja -DCMAKE_SYSTEM_NAME=tvOS && cmake --build build/tvos
```

`build/*/link_stubs.txt` lists the SDK functions the port does not implement yet.

## Running

```bash
build/mac/rumble --frames 600
```

This runs the game with no window, sound or input, skipping idle time, and stops after 600 video frames (10 seconds of game time). `--nand DIR` sets the folder that stands in for the console's flash memory (default `build/nand`), `--realtime` runs at the console's speed, `--check-assets` runs every content file through its byte-order fixup and exits. The summary on stderr counts frames, `[RED]` lines (anything the port does not handle yet) and GX commands.

## Checks

```bash
python3 tools/compile_check.py
```

```bash
python3 tools/narrow_casts.py
```

```bash
tests/native/run.sh
```

```bash
cmake -S . -B build/mac-asan -G Ninja -DPORT_SANITIZE=ON && cmake --build build/mac-asan && (cd build/mac-asan && ctest --output-on-failure)
```

`compile_check.py` compiles every unit for arm64 (add `--sdk appletvos` for tvOS). `narrow_casts.py` finds pointers squeezed into 32 bits, which clang does not report for `reinterpret_cast`. The native tests cover the C versions of the Wii's paired-single math, MSL's 16-bit wide-character functions and the layout of structs read from game files. `ctest` (in any build folder) runs the asset check and the boot check: 600 frames from an empty NAND with no `[RED]` line, no sanitizer report and a well-formed GX command stream; the `PORT_SANITIZE` build runs it under ASan and UBSan.
