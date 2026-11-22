#!/bin/sh
# Builds the Apple TV app with CMake's Xcode generator (CMakePresets.json). Run from anywhere:
#   tvos/tools/build.sh            for the tvOS simulator
#   tvos/tools/build.sh device     for a real Apple TV (signed: set DEVELOPMENT_TEAM in tvos/Local.xcconfig)
#   tvos/tools/build.sh device ID  the same, for the TV with that identifier (xcrun devicectl list
#                                  devices): Xcode adds it to the provisioning profile
# Needs Xcode and CMake 3.31 or newer, and the game files imported with tools/import_wad.py:
# they are copied into the app. tools/tvos/port does the same for the installer, and installs.
set -e
cd "$(dirname "$0")/../.."
root=$(pwd)
[ -f tvos/Local.xcconfig ] || cp tvos/Local.xcconfig.example tvos/Local.xcconfig
setting() { sed -n "s/^$1 *= *\([^ ]*\).*/\1/p" tvos/Local.xcconfig | tail -1; }
bundle_id=$(setting RUMBLE_BUNDLE_ID)
team=$(setting DEVELOPMENT_TEAM)
if [ ! -d rom/content ]; then
  echo "error: no game files in rom/content; run tools/import_wad.py first" >&2
  exit 1
fi
if [ "$1" = "device" ]; then
  cmake --preset tvos -DRUMBLE_BUNDLE_ID="${bundle_id:-com.example.rumble-tv}" -DRUMBLE_DEVELOPMENT_TEAM="$team"
  set -- -sdk appletvos -allowProvisioningUpdates -allowProvisioningDeviceRegistration ${2:+-destination "id=$2"}
  cmake --build --preset tvos -- "$@"
  echo "built: $root/build/tvos/tvos/Release-appletvos/Rumble.app"
else
  cmake --preset tvos-sim -DRUMBLE_BUNDLE_ID="${bundle_id:-com.example.rumble-tv}"
  cmake --build --preset tvos-sim
  echo "built: $root/build/tvos-sim/tvos/Debug-appletvsimulator/Rumble.app"
fi
