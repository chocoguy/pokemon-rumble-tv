# Pokémon Rumble for tvOS

A native port of the Pokémon Rumble (WiiWare, USA) decompilation in `pokemon-rumble-decomp-tv/` to arm64 macOS and Apple TV. It contains no game data: you supply your own copy of the game.

Status: phase 7 of the plan (the Apple TV app). The game is playable on the Mac and the Apple TV (simulator) with its own Metal renderer under the Wii SDK's GX library, controllers presented as Wii Remotes with a Classic Controller, and sound: the DSP's voice mixer is rebuilt in `port/ax` (DSP-ADPCM/PCM decoding, resampling, filters, volume ramps, the aux buses through the SDK's reverb, delay and chorus) and played through AVAudioEngine. Not yet compared with Dolphin, by eye or by ear.

One deliberate change from the original: analog movement. The game walks at a fixed speed whenever the stick is pushed at all; the port patches the player's script as it loads (`port/script/port_script_patch.c`) so walking speed and the walk animation follow how far the stick is pushed (30% just past the dead zone up to the original speed at a full push), and the Pokémon turns toward the stick twice as fast. `PORT_NO_SCRIPT_PATCH=1` runs the original script.

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

The first runs the game with no window and no live sound, on a virtual clock that only moves when the game is idle (so a run repeats exactly, however fast the Mac is), and stops after 1900 video frames; the second presses the Wii Remote's buttons as `tests/input/new_game.txt` says (format in `port/pad/port_pad.c`) to start a new game and reach the hub. `--dump-frame K PATH` (repeatable) writes what the display shows at frame K as a PNG, stretched to 16:9 like the TV (the game draws anamorphic widescreen). Other options: `--nand DIR` sets the folder that stands in for the console's flash memory (default `build/nand`), `--realtime` runs at the console's speed, `--scale N` renders at N times the console's resolution, `--no-gpu` draws nothing, `--dump-every N DIR` writes every Nth frame to DIR (for writing an input script), `--wav PATH` writes everything the sound mixer outputs to a 32 kHz stereo WAV (in game time, so it works on the virtual clock), `--audio` plays the sound on the Mac's output (and runs in real time), `--check-assets` runs every content file through its byte-order fixup and exits (`PORT_EXTRACT=DIR PORT_EXTRACT_EXT=.brres` also saves the files of those types). The summary on stderr counts frames, `[RED]` lines (anything the port does not handle yet), GX commands and draws. `PORT_GX_TRACE=K` in the environment logs every draw batch of frame K (`PORT_GX_TRACE_VERTS=1` adds vertex data), and `PORT_GX_DRAW=A-B` draws only batches A to B, as numbered in the trace (a frame's batches show at the next retrace's dump). The trace also lists EFB copies, lighting, texgen matrices and matrix indices. `PORT_GX_SHOW=ras0|tx<stage>|nrm|mat|amb` draws that input instead of the TEV's result on lit draws, and `PORT_GX_SHADER_DUMP=FILE` appends every generated Metal shader to FILE. `PORT_WALK_TRACE=1` logs each step the player's Pokémon takes and its walk-animation rate. In an input script, `LS_UP@0.5` (any `LS_` name) pushes the stick part way. `PORT_AX_TRACE=1` logs once a second how many voices the mixer runs and where they send (`=2` lists each voice's parameter block). `tests/input/first_battle.txt` carries on from the new game through the first Battle Royale (which the story makes RATTATA lose) into free play in the Terminal (~14300 retraces).

## Apple TV

```bash
tvos/tools/build.sh
```

This builds the game's libraries for the tvOS simulator with CMake, generates the Xcode project with xcodegen (`brew install xcodegen`) and builds `tvos/build/Build/Products/Debug-appletvsimulator/Rumble.app`, with the imported game files inside. `tvos/tools/build.sh device` builds for a real Apple TV once `DEVELOPMENT_TEAM` is set in `tvos/Local.xcconfig`. The app plays the game with controllers and sound and carries its own icon, Top Shelf images and launch screen (`tvos/Resources/Images.xcassets`).

- **Saves** live in the app's `Library/Caches/save`. tvOS may empty Caches while the app is closed, so a second after each save the game makes, its files are also copied (LZFSE, about 30 KB) into the app's preferences, and written back at the next launch if the folder is empty. Deleting the app deletes both. Keep `RUMBLE_BUNDLE_ID` the same between builds, or the copy is out of reach.
- **Log:** `Library/Caches/log.txt` (the previous run's is `log.prev.txt`), with a status line every minute (thermal state, memory, frames shown). With a debugger attached, or `RUMBLE_LOG_FILE=0`, it goes to Xcode's console instead.
- **env.txt:** `KEY=VALUE` lines in `Library/Caches/env.txt` (or `env.txt` in the app bundle) set environment variables at launch, such as `PORT_NAND_TRACE=1`.
- **Leaving the app** (the TV button, Control Center, the screen saver) freezes the game where it is, with sound and drawing stopped; it carries on when you come back. The remote's Back button is the game's pause button (+).

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

`compile_check.py` compiles every unit for arm64 (add `--sdk appletvos` for tvOS). `narrow_casts.py` finds pointers squeezed into 32 bits, which clang does not report for `reinterpret_cast`. The native tests cover the AX voice mixer on known samples (PCM, DSP-ADPCM through a loop, resampling, volume ramps), the C versions of the Wii's paired-single math, MSL's 16-bit wide-character functions and the layout of structs read from game files. `ctest` (in any build folder) runs the asset check and two boot checks from an empty NAND, 600 frames and 1900 frames (to the drawn title screen, dumped as `title-test-title.png` in the build folder), each with no `[RED]` line, no sanitizer report and a well-formed GX command stream; `analog_walk_8000_frames` walks the player at part pushes of the stick and checks the speeds; `title_sound_1900_frames` writes the title music to `sound-test.wav` and checks it is audible, unclipped and stereo. The `PORT_SANITIZE` build runs them under ASan and UBSan. The native tests include the GX texture decoder, one block per format.
