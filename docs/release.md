# Releasing

A release is a git tag (`v1.0.0`) and a GitHub release with two files:
- **`RumbleTVInstaller-<version>.zip`:** the installer, signed with a Developer ID and notarized.
- **`appcast.xml`:** Sparkle's feed. Installers read it from `releases/latest/download/appcast.xml` to update themselves.

Players' installers see the new tag on GitHub, build the game from it on their Mac, and reinstall it on every Apple TV it's installed on.

`tools/release.sh` does all of it on your Mac. Nothing signs or publishes in CI.

## One-time setup (shared with Frontline Rewritten)

This port signs, notarizes and updates its installer with the same credentials as Frontline Rewritten, so if Frontline releases work on this Mac, so do these:

1. **Developer ID certificate** for team `USLHQ524FW`:
   - `security find-identity -v -p codesigning` lists `Developer ID Application: … (USLHQ524FW)`.
2. **Notarization credentials**, in the keychain under the profile `frontline-notary`:
   - `xcrun notarytool history --keychain-profile frontline-notary` works.
   - To make it on a new Mac:
     ```sh
     xcrun notarytool store-credentials frontline-notary --apple-id <your Apple ID> --team-id USLHQ524FW
     ```
     It asks for an app-specific password: account.apple.com → Sign-In and Security → App-Specific Passwords.
3. **Sparkle's signing key:**
   - The private key is in your login keychain ("Private key for signing Sparkle updates"). It's the same one Frontline Rewritten uses.
   - The public key, `zXdaNL5/ThT4IrxiYLM28IufQy6TRClWWwbO3oQaZh4=`, is in `apps/installer/sparkle-public-key.txt`.
   - On a new Mac, restore the private key from your backup with `generate_keys -f <file>`. Sparkle's tools are in `build/xcode/_deps/sparkle-src/bin` after one installer build: `cmake --preset xcode && cmake --build build/xcode --target RumbleTVInstaller`.
   - **Without the private key, installers already out there can't take updates**, for this port and Frontline both.
4. **GitHub CLI:** `gh auth status` says you're signed in (`brew install gh`, then `gh auth login`).

## Each release

From a clean `main` that's pushed, with `pokemon-rumble-decomp-tv`'s commit pushed too:

```sh
tools/release.sh 1.0.0
```

It does this, in order:
1. Checks the above.
2. Opens your editor for the release notes (Markdown), or takes `--notes <file>`.
3. Sets `port.json`'s `version` (and commits "Release 1.0.0" if it changed), then tags `v1.0.0`.
4. Builds Rumble TV Installer Release, stamped 1.0.0 (build number 10000), with Sparkle's public key.
5. Signs Sparkle's parts inside-out, then the app (hardened runtime, timestamp).
6. Notarizes the zip, staples the app, checks it with `spctl`, and zips it again into `build/dist/1.0.0/`.
7. Makes `appcast.xml` with Sparkle's `generate_appcast` (signed with your key, release notes embedded).
8. Asks before pushing, then pushes `main` and the tag, and creates the GitHub release with both files.

If you answer no at the push, the commit and tag stay local. The script prints how to undo them.

**Options:**
- **Release candidates:** `tools/release.sh 1.1.0-rc1 --prerelease`. GitHub marks it a pre-release, so players don't see it (their installers skip pre-releases, and `latest/download` skips them for Sparkle). To test one, turn on pre-releases in your installer:
  ```sh
  defaults write com.vanillacoffeesoft.RumbleTVInstaller includePrereleases -bool YES
  ```
- **A release that needs a newer installer:** `--min-app 1.1.0` sets `port.json`'s `minAppVersion`. Older installers then stop before building it and ask to update themselves first.
- **Dry run:** `--dry-run` runs the checks (reporting, not stopping) and builds an ad hoc installer into `build/dist`. It signs, notarizes, commits, tags and pushes nothing.

## Versions
- Tags are `v` plus a semantic version.
- The apps' `CFBundleShortVersionString` is its numeric part, and `CFBundleVersion` is `major×10000 + minor×100 + patch`.
- `RumbleVersion` (the full version) and `RumbleCommit` are in the Apple TV app's and the installer's Info.plist. The installer compares its own `RumbleVersion` with a tag's `minAppVersion`.
