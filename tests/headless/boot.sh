#!/bin/sh
# Phase 2 check: boots the game headless for a number of video frames from an empty NAND
# and fails on a non-zero exit, any [RED] line, a sanitizer report or a GX stream error.
#
#   tests/headless/boot.sh <rumble binary> <frames> <scratch dir>
set -u
rumble=$1
frames=$2
scratch=$3
rm -rf "$scratch"
mkdir -p "$scratch"
"$rumble" --frames "$frames" --nand "$scratch/nand" >"$scratch/stdout.txt" 2>"$scratch/stderr.txt"
status=$?
grep -E '^\[port\]' "$scratch/stderr.txt"
fail=0
if [ $status -ne 0 ]; then
    echo "boot.sh: rumble exited with status $status"
    fail=1
fi
if grep -q '^\[RED\]' "$scratch/stderr.txt"; then
    echo "boot.sh: [RED] lines:"
    grep '^\[RED\]' "$scratch/stderr.txt" | head -20
    fail=1
fi
if grep -qE 'runtime error|ERROR: AddressSanitizer' "$scratch/stderr.txt"; then
    echo "boot.sh: sanitizer reports:"
    grep -E 'runtime error|ERROR: AddressSanitizer' "$scratch/stderr.txt" | head -20
    fail=1
fi
if ! grep -q ' 0 stream errors' "$scratch/stderr.txt"; then
    echo "boot.sh: the GX command stream had errors"
    fail=1
fi
exit $fail
