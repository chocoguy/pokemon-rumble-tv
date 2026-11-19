# Pokémon Rumble for tvOS

A native port of the Pokémon Rumble (WiiWare, USA) decompilation in `pokemon-rumble-decomp-tv/` to arm64 macOS and Apple TV. It contains no game data: you supply your own copy of the game.

Status: phase 3 of the plan (the renderer). The game runs on the Mac and in the Apple TV simulator with its own Metal renderer under the Wii SDK's GX library: the strap-reminder and title screens, and, driven by a scripted input file, a new game through the intro story into the hub, with 3D models, particles and EFB copies drawn. Not yet compared with Dolphin; no controllers (phase 4) or sound mixing (phase 5).

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
build/mac/rumble --frames 1900 --dump-frame 1800 build/title.png
```

```bash
build/mac/rumble --frames 7300 --input tests/input/new_game.txt --dump-frame 7201 build/hub.png
```

The first runs the game with no window or sound, on a virtual clock that only moves when the game is idle (so a run repeats exactly, however fast the Mac is), and stops after 1900 video frames; the second presses the Wii Remote's buttons as `tests/input/new_game.txt` says (format in `port/pad/port_pad.c`) to start a new game and reach the hub. `--dump-frame K PATH` (repeatable) writes what the display shows at frame K as a PNG, stretched to 16:9 like the TV (the game draws anamorphic widescreen). Other options: `--nand DIR` sets the folder that stands in for the console's flash memory (default `build/nand`), `--realtime` runs at the console's speed, `--scale N` renders at N times the console's resolution, `--no-gpu` draws nothing, `--dump-every N DIR` writes every Nth frame to DIR (for writing an input script), `--check-assets` runs every content file through its byte-order fixup and exits (`PORT_EXTRACT=DIR PORT_EXTRACT_EXT=.brres` also saves the files of those types). The summary on stderr counts frames, `[RED]` lines (anything the port does not handle yet), GX commands and draws. `PORT_GX_TRACE=K` in the environment logs every draw batch of frame K (`PORT_GX_TRACE_VERTS=1` adds vertex data), and `PORT_GX_DRAW=A-B` draws only batches A to B, as numbered in the trace (a frame's batches show at the next retrace's dump). The trace also lists EFB copies, lighting, texgen matrices and matrix indices. `PORT_GX_SHOW=ras0|tx<stage>|nrm|mat|amb` draws that input instead of the TEV's result on lit draws, and `PORT_GX_SHADER_DUMP=FILE` appends every generated Metal shader to FILE. `tests/input/first_battle.txt` carries on from the new game through the first Battle Royale (which the story makes RATTATA lose) into free play in the Terminal (~14300 retraces).

## Apple TV

```bash
tvos/tools/build.sh
```

This builds the game's libraries for the tvOS simulator with CMake, generates the Xcode project with xcodegen (`brew install xcodegen`) and builds `tvos/build/Build/Products/Debug-appletvsimulator/Rumble.app`, with the imported game files inside. `tvos/tools/build.sh device` builds for a real Apple TV once `DEVELOPMENT_TEAM` is set in `tvos/Local.xcconfig`. The app only shows the game so far: no controller, sound or saves beyond the simulator's container.

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

`compile_check.py` compiles every unit for arm64 (add `--sdk appletvos` for tvOS). `narrow_casts.py` finds pointers squeezed into 32 bits, which clang does not report for `reinterpret_cast`. The native tests cover the C versions of the Wii's paired-single math, MSL's 16-bit wide-character functions and the layout of structs read from game files. `ctest` (in any build folder) runs the asset check and two boot checks from an empty NAND, 600 frames and 1900 frames (to the drawn title screen, dumped as `title-test-title.png` in the build folder), each with no `[RED]` line, no sanitizer report and a well-formed GX command stream; the `PORT_SANITIZE` build runs them under ASan and UBSan. The native tests include the GX texture decoder, one block per format.
