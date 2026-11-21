#!/bin/sh
# Phase 5: boots to the title screen writing the mixer's output to a WAV, then checks
# the run as boot.sh does and the title music's level (check_wav.py) from 22 s on.
#
#   tests/headless/sound.sh <rumble binary> <scratch dir>
set -u
here=$(dirname "$0")
"$here/boot.sh" "$1" 1900 "$2" --no-gpu --wav "$2.wav" || exit 1
python3 "$here/check_wav.py" "$2.wav" 31 22
