# Releasing

A release is a git tag (`v2.0.0`) and a GitHub release with one file, **`RumbleTVInstaller.zip`**: the installer, signed with a Developer ID and notarized.

Players' installers see the new tag on GitHub, build the game from it on their Mac, and reinstall it on every Apple TV it's installed on.

`tools/release.sh` does all of it on your Mac. Nothing signs or publishes in CI.

## The major version is the installer

- **`N.0.0` is a new installer.** `release.sh` builds, signs, notarizes and staples it, and attaches it.
- **`N.x.y` is a game update only.** `release.sh` doesn't build the installer: it downloads `vN.0.0`'s `RumbleTVInstaller.zip` and attaches it again, unchanged, so the latest release always has the installer (and `releases/latest` links keep working). It stops if `vN.0.0` has no installer zip.
- An installer builds only the releases of its own major version. When a newer major is on GitHub, it says a new installer is available (an alert once per launch, and a banner on the Game and Updates screens) and links to that release. Game updates wait until the player uses the new installer; **Reinstall** still works, so free-account installs can be renewed.
- The installer doesn't update itself (no Sparkle). Players download a new one by hand and replace the old one; everything it made is in `~/Library/Application Support/Rumble TV Installer`, so nothing is lost.
- `port.json`'s `minAppVersion` is set to `N.0.0` only for 1.x installers, which still read it: they stop before building a newer major.
- **Transition:** v1.0.0 shipped a Sparkle installer. The first release with the new installer must be `2.0.0`. 1.x installers then refuse to build it, but their Check for Updates… finds nothing: players download the 2.0.0 installer by hand.

## One-time setup (shared with Frontline Rewritten)

This port signs and notarizes its installer with the same credentials as Frontline Rewritten, so if Frontline releases work on this Mac, so do these:

1. **Developer ID certificate** for team `USLHQ524FW`:
   - `security find-identity -v -p codesigning` lists `Developer ID Application: … (USLHQ524FW)`.
2. **Notarization credentials**, in the keychain under the profile `frontline-notary`:
   - `xcrun notarytool history --keychain-profile frontline-notary` works.
   - To make it on a new Mac:
     ```sh
     xcrun notarytool store-credentials frontline-notary --apple-id <your Apple ID> --team-id USLHQ524FW
     ```
     It asks for an app-specific password: account.apple.com → Sign-In and Security → App-Specific Passwords.
3. **GitHub CLI:** `gh auth status` says you're signed in (`brew install gh`, then `gh auth login`).

## Each release

From a clean `main` that's pushed, with `pokemon-rumble-decomp-tv`'s commit pushed too:

```sh
tools/release.sh 2.0.0
```

It does this, in order:
1. Checks the above. The Developer ID and notarytool profile are needed only for `N.0.0`. For `N.x.y`, it checks that `vN.0.0` has the installer zip on GitHub.
2. Opens your editor for the release notes (Markdown), or takes `--notes <file>`.
3. Sets `port.json`'s `version` (and `minAppVersion` to `N.0.0`), commits "Release 2.0.0" if it changed, then tags `v2.0.0`.
4. For `N.0.0`:
   - builds Rumble TV Installer Release, stamped 2.0.0 (build number 20000)
   - signs the app (hardened runtime, timestamp)
   - notarizes the zip, staples the app, checks it with `spctl`, and zips it again into `build/dist/2.0.0/`
5. For `N.x.y`: downloads `vN.0.0`'s `RumbleTVInstaller.zip` into `build/dist/<version>/`.
6. Asks before pushing, then pushes `main` and the tag, and creates the GitHub release with the zip.

If you answer no at the push, the commit and tag stay local. The script prints how to undo them.

**Options:**
- **Release candidates:** `tools/release.sh 2.1.0-rc1 --prerelease`. GitHub marks it a pre-release, so players don't see it (their installers skip pre-releases). `3.0.0-rc1` builds a new installer, like `3.0.0`. To test one, turn on pre-releases in your installer:
  ```sh
  defaults write com.vanillacoffeesoft.RumbleTVInstaller includePrereleases -bool YES
  ```
- **Dry run:** `--dry-run` runs the checks (reporting, not stopping) and, for `N.0.0`, builds an ad hoc installer into `build/dist`. It signs, notarizes, commits, tags and pushes nothing.

## Versions
- Tags are `v` plus a semantic version.
- The apps' `CFBundleShortVersionString` is its numeric part, and `CFBundleVersion` is `major×10000 + minor×100 + patch`.
- `RumbleVersion` (the full version) and `RumbleCommit` are in the Apple TV app's and the installer's Info.plist. The installer builds the tags whose major version matches its own `RumbleVersion`.
