# Pokémon Rumble for tvOS

A native port of the Pokémon Rumble (WiiWare, USA) decompilation in `pokemon-rumble-decomp-tv/` to arm64 macOS and Apple TV. It contains no game data: you supply your own copy of the game.

Status: phase 1 of the plan. Everything compiles and links for macOS and tvOS; the Wii SDK underneath is still mostly stubbed, so the game does not run yet.

## Game files

Install your WAD in Dolphin (Tools > Install WAD), then:

```bash
python3 tools/import_wad.py
```

It copies the decrypted contents to `rom/content/` and checks the executable against the decomp's manifest.

## Building

Needs Xcode, CMake and Ninja.

```bash
cmake -S . -B build/mac -G Ninja && cmake --build build/mac
```

```bash
cmake -S . -B build/tvos -G Ninja -DCMAKE_SYSTEM_NAME=tvOS && cmake --build build/tvos
```

`build/*/link_stubs.txt` lists the SDK functions the port does not implement yet.

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

`compile_check.py` compiles every unit for arm64 (add `--sdk appletvos` for tvOS). `narrow_casts.py` finds pointers squeezed into 32 bits, which clang does not report for `reinterpret_cast`. The native tests cover the C versions of the Wii's paired-single math, MSL's 16-bit wide-character functions and the layout of structs read from game files.
