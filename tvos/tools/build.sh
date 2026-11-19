#!/bin/sh
# Builds the Apple TV app. Run from anywhere:
#   tvos/tools/build.sh            for the tvOS simulator
#   tvos/tools/build.sh device     for a real Apple TV (signed: set DEVELOPMENT_TEAM in tvos/Local.xcconfig)
#   tvos/tools/build.sh device ID  the same, for the TV with that identifier (xcrun devicectl list
#                                  devices): Xcode adds it to the provisioning profile
# Needs Xcode, CMake, Ninja and xcodegen (brew install cmake ninja xcodegen), and the
# game files imported with tools/import_wad.py: they are copied into the app.
set -e
cd "$(dirname "$0")/.."
tvos=$(pwd)
root=$(cd .. && pwd)
[ -f Local.xcconfig ] || cp Local.xcconfig.example Local.xcconfig
if [ ! -d "$root/rom/content" ]; then
  echo "error: no game files in rom/content; run tools/import_wad.py first" >&2
  exit 1
fi
if [ "$1" = "device" ]; then
  libs="$root/build/tvos"
  cmake -S "$root" -B "$libs" -G Ninja -DCMAKE_SYSTEM_NAME=tvOS
else
  libs="$root/build/tvos-sim"
  cmake -S "$root" -B "$libs" -G Ninja -DCMAKE_SYSTEM_NAME=tvOS -DCMAKE_OSX_SYSROOT=appletvsimulator
fi
cmake --build "$libs" --target rumble_fork rumble_port rumble_metal rumble_stubs
xcodegen generate --quiet
if [ "$1" = "device" ]; then
  dest="generic/platform=tvOS"
  [ -n "$2" ] && dest="id=$2"
  xcodebuild -project RumbleTV.xcodeproj -scheme Rumble -configuration Release \
    -destination "${dest}" -derivedDataPath build -allowProvisioningUpdates -allowProvisioningDeviceRegistration \
    RUMBLE_LIBS="$libs" build
  echo "built: $tvos/build/Build/Products/Release-appletvos/Rumble.app"
else
  xcodebuild -project RumbleTV.xcodeproj -scheme Rumble -configuration Debug \
    -destination 'generic/platform=tvOS Simulator' -derivedDataPath build \
    RUMBLE_LIBS="$libs" build
  echo "built: $tvos/build/Build/Products/Debug-appletvsimulator/Rumble.app"
fi
