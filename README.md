# Pokémon Rumble TV
## A port of Pokemon Rumble for tvOS

![Logo](https://mangogravy.com/rumble-small.jpg)

A native port of the Pokémon Rumble (WiiWare, USA) decompilation in `pokemon-rumble-decomp-tv/` for tvOS

**no game data or assets included**. You will need your own very legitimate .wad copy of the USA WiiWare release of pokemon rumble

Analog movement has been reworked to better fit modern game controls but everything else is the same as the original

## Requirements

- An Apple Silicon Mac
- macOS 27
- XCode 27
- An Apple TV 4K (3rd gen or newer) with tvOS 27, paired with Xcode (**Xcode → Window → Devices and Simulators**).
- An Apple ID signed in to Xcode (**Xcode → Settings → Accounts**). A free one works, but its installs open for 7 days; then reinstall to renew. A paid Apple Developer account lasts a year.
- Gamepad


## Install

1. **Install the game in Dolphin.** Open [Dolphin](https://dolphin-emu.org), choose **Tools → Install WAD…**, and pick your Pokémon Rumble (USA) WAD. Dolphin keeps the decrypted game in its own folder, where the installer finds it. You can delete the WAD afterwards, but keep Dolphin's copy.
2. **Download Rumble TV Installer** from the [latest release](https://github.com/chocoguy/pokemon-rumble-tv/releases/latest), unzip it and move it to Applications.
3. **Open it and go through Setup.** It checks for Xcode, tvOS support in Xcode, CMake (it downloads it for you) and an Apple ID in Xcode, with a button to fix each one. Turn on Developer Mode on the Apple TV and pair it in Xcode (**Window → Devices and Simulators**).
4. **Game → Install on an Apple TV…**, and pick your TV. The first build takes about 10 minutes. The installer never opens the game: find **Rumble** on the Apple TV's Home screen.

**Updates:** the installer checks GitHub for new releases. **Updates → Update All** rebuilds the game once and reinstalls it on every Apple TV you installed it on. Your saves stay. The installer updates itself, too (**Rumble TV Installer → Check for Updates…**).

**Free Apple ID:** the game opens for 7 days after each install. The Game screen shows the date; reinstall before then. A paid Apple Developer membership lasts a year.

Prefer the Terminal? `tools/tvos/port` does the same steps (see For Developers).

## Controls

Each controller is a Wii Remote with a Classic Controller to the game, so the game's on-screen hints show Classic Controller buttons. Up to four players.

| Gamepad (Xbox, PlayStation, MFi) | Classic Controller | Wii Remote, held sideways |
|---|---|---|
| Left stick | Left stick | Movement: walking speed follows how far you push |
| D-pad | D-pad | D-pad |
| A (Cross) | a | 2 |
| B (Circle) | b | 1 |
| X (Square), Y (Triangle) | x, y | A |
| Menu (Options, Start) | + | + (pause) |
| Options (Share, Select) | − | − |

Buttons are matched by name, not position: the button labelled A acts as a.

**Siri Remote:** swipe on the touch surface to move. Click is a, Play/Pause is b, and Back is + (pause). The TV button leaves the game, which freezes it until you come back.

**Keyboard:** WASD or the arrows move; Z, J or Space is a; X, K or Escape is b; C or L is x; V or I is y; Q and E are L and R; Return is +; Tab or Delete is −.

The Home button and its menu aren't available.

## For Developers

Clone with the decomp submodule:

```bash
git clone --recursive https://github.com/chocoguy/pokemon-rumble-tv.git
```

Install the WAD in Dolphin, then import the game files into `rom/content/` (any Python 3.9 or newer, including Xcode's):

```bash
python3 tools/import_wad.py
```

Build and test on the Mac: a headless runner, `build/mac/rumble` (its options are listed at the top of `platform/headless/main.c`), and the ctest checks. Needs Xcode, CMake 3.31 or newer and Ninja.

```bash
cmake --preset mac && cmake --build --preset mac && ctest --test-dir build/mac
```

```bash
tests/native/run.sh
```

Build the Apple TV app for the simulator, or for a real Apple TV once `DEVELOPMENT_TEAM` is set in `tvos/Local.xcconfig`:

```bash
tvos/tools/build.sh
```

```bash
tvos/tools/build.sh device
```

What the installer runs, from the Terminal (`--json` gives its events, one per line):

```bash
tools/tvos/port check
```

```bash
tools/tvos/port build
```

```bash
tools/tvos/port install --device <ID from xcrun devicectl list devices>
```

- **Layout:** `port/` is the Wii SDK's host side, `platform/` the Metal renderer, the sound output and the Apple TV shell, `apps/installer/` the installer, and `tools/tvos/port` the build engine.
- **Releasing:** `docs/release.md`. Try `tools/release.sh 1.0.0 --dry-run` first.

## Credits


Original game © Ambrella, Nintendo


Shoutouts to the original [decomp](https://github.com/revvmp/pokemon-rumble-decomp) creators and claude 