# Installer status

Rumble TV Installer, built 2026-11-21 from Frontline Rewritten's (see `~/Documents/installer-playbook.md`).

## Where it stands

**Done, and checked by building:**
- The Apple TV app is a CMake target (`tvos/CMakeLists.txt`, presets `tvos` and `tvos-sim`): no xcodegen or Ninja needed.
  - Every configuration compiles unoptimized, as the old Ninja build did. An `-O3` Release build needs 12 extra link stubs for game functions, so it would be broken at runtime.
  - A Release device build from a copy of the source in a folder with spaces gives the same link stubs as the simulator build.
- `tools/import_wad.py` runs on Xcode's Python 3.9, and its output is byte-identical to before.
- `tools/tvos/port` has `check`, `prepare`, `build`, `install`, `installed` and `clean`.
  - Its game files come from Dolphin's folder (found on its own) or `--game`.
  - It accepts free teams and reports their 7-day expiry.
  - `check` and `prepare` were run with a Finder-like PATH.
- Rumble TV Installer builds with the Xcode generator, and the `rumble_installertest` logic tests pass.
- `tools/release.sh 1.0.0 --dry-run` passed. The installer is stamped 1.0.0 (build 10000) with Frontline's Sparkle public key.

**Not done by the agent (yours):**
- Nothing has been signed with the Developer ID, notarized, pushed, tagged or published.
- `tools/tvos/port build` has not been run signed. That would register the bundle ID `com.pokemonrumbletv.<team>` with Apple.
- Nothing has been installed on, or launched on, an Apple TV.
- The installer's window has not been opened.

## Design notes
- **One platform.** `Platform` has only `tvos`, and the Install menu became "Install on an Apple TV…". The Mac and iOS parts of Frontline's installer (Mac install, Undo Mac Update, osascript quit) are gone, along with the migration from Frontline's old in-repo installer.
- **Game files.** `InstallerState.game` is empty for "Dolphin's own folder". `GameFiles.content(in:)` (Workspace.swift) looks for a folder of `.app` files, then `Wii/title/00010001/57505345/content` under the chosen folder or its parent. The engine does the same in `find_content`.
- **Free teams.** Frontline needs a paid team, but this port has no paid-only capabilities. Doctor and the engine prefer a paid team and accept a free one; the expiry date shows in the Game screen (orange within 3 days).
- **Bundle ID.** Installs use `com.pokemonrumbletv.<team>`. Builds from `tvos/tools/build.sh` keep `RUMBLE_BUNDLE_ID` from `tvos/Local.xcconfig` (`com.example.rumble-tv` by default). Saves are kept per bundle ID, so an install made with the installer doesn't see saves from an app built the old way.
- **Shared credentials.** These are Frontline's: Developer ID team `USLHQ524FW`, notary profile `frontline-notary`, and the Sparkle key pair. See `docs/release.md`.

## Your checklist
1. **Look at the work:** `git log`, and the README's Install, Controls and For Developers sections.
2. **Push `main`:**
   ```bash
   git push origin main
   ```
3. **Release candidate:**
   ```bash
   tools/release.sh 1.0.0-rc1 --prerelease
   ```
4. **On a fresh macOS user account:**
   1. Turn on pre-releases:
      ```bash
      defaults write com.vanillacoffeesoft.RumbleTVInstaller includePrereleases -bool YES
      ```
   2. Install the WAD in Dolphin.
   3. Download the rc zip from GitHub and open the installer. Gatekeeper should not complain.
   4. Fix whatever Setup flags (CMake download, tvOS platform, Apple ID).
5. **Install on an Apple TV**, once with a paid team and once with a free one.
   - The free install should show a date 7 days out.
   - Open Rumble from the Home screen, play to the hub, save, quit, and reopen.
6. **Game update:** make `1.0.0-rc2` with a small change, then run Update All. The TV should get rc2 and keep the save.
7. **Installer update:** release `1.0.1-rc1 --prerelease --min-app 1.0.1`.
   - Check for Updates… offers it (Sparkle reads `latest/download`, which skips pre-releases, so test the final `1.0.1` this way, or point a test build's feed at the rc).
   - An old installer refuses to build the tag until it's updated.
8. **Rollback:** Updates → Install Another Version… → the earlier tag.
9. **The real release:**
   ```bash
   tools/release.sh 1.0.0
   ```
