#!/bin/sh
# Phase 6: save, quit, relaunch, continue; a corrupt save falls back to the backup; the
# stubbed Wii features. Four runs share one NAND folder, each checked as boot.sh does
# (exit status, [RED] lines, sanitizer reports, GX stream); runs 1-3 with PORT_NAND_TRACE=1:
#   1. new game through the first battle until the game first saves (retrace ~10650):
#      savedata.bin, backup/savedata.bin and banner.bin exist and are valid, /tmp is empty.
#   2. relaunch and Continue (tests/input/continue.txt): the game loads savedata.bin
#      without touching the backup, reaches the Terminal and saves again; that save moves
#      the previous savedata.bin over the existing backup (the console replaces it).
#   3. corrupt savedata.bin and relaunch: the game reads the backup, rewrites savedata.bin
#      from it ("The previous data was retrieved"), continues and saves again.
#   4. continue again and use the stubs (tests/input/pause_stubs.txt): take a picture
#      (JPEG encode, no SD card), SD Card > Delete Pictures, then HOME.
#
#   tests/headless/save.sh <rumble binary> <scratch dir>
set -u
here=$(cd "$(dirname "$0")" && pwd)
rumble=$1
scratch=$2
nand=$scratch.nand
data=$nand/title/00010001/57505345/data
rm -rf "$nand"
fail() { echo "save.sh: $*"; exit 1; }
nandlog() { grep '\[nand\]' "$1/stderr.txt" | sed 's/^\[port\] \[nand\] @[0-9]* //'; }

PORT_NAND_TRACE=1 "$here/boot.sh" "$rumble" 10800 "$scratch-1" --no-gpu --nand "$nand" \
    --input "$here/../input/first_battle.txt" || fail "run 1 failed"
for f in savedata.bin backup/savedata.bin banner.bin; do
    [ -f "$data/$f" ] || fail "run 1 did not write $f"
done
[ -z "$(ls -A "$nand/tmp")" ] || fail "run 1 left files in /tmp: $(ls "$nand/tmp")"
python3 "$here/check_save.py" "$data/savedata.bin" "$data/backup/savedata.bin" || fail "run 1 wrote an invalid save"
cp "$data/savedata.bin" "$scratch-1.savedata.bin"

PORT_NAND_TRACE=1 "$here/boot.sh" "$rumble" 2800 "$scratch-2" --no-gpu --nand "$nand" \
    --input "$here/../input/continue.txt" || fail "run 2 failed"
nandlog "$scratch-2" | grep -q '^open r /title/00010001/57505345/data/savedata.bin -> 0$' || fail "run 2 did not read the save"
nandlog "$scratch-2" | grep -q 'backup/savedata.bin' && fail "run 2 read the backup of a valid save"
nandlog "$scratch-2" | grep -q '^move savedata.bin -> backup/ -> 0$' || fail "run 2 did not replace the backup"
nandlog "$scratch-2" | grep -q '^move /tmp/savedata.bin -> ./ -> 0$' || fail "run 2 did not commit a save"
cmp -s "$scratch-1.savedata.bin" "$data/backup/savedata.bin" || fail "run 2: the backup is not run 1's save"
python3 "$here/check_save.py" "$data/savedata.bin" || fail "run 2 wrote an invalid save"

python3 -c "
import sys
p = sys.argv[1]
d = bytearray(open(p, 'rb').read())
d[0x1000] ^= 0xFF
open(p, 'wb').write(d)" "$data/savedata.bin"
python3 "$here/check_save.py" "$data/savedata.bin" >/dev/null && fail "the corrupted save still checks as valid"
PORT_NAND_TRACE=1 "$here/boot.sh" "$rumble" 2800 "$scratch-3" --no-gpu --nand "$nand" \
    --input "$here/../input/continue.txt" || fail "run 3 failed"
nandlog "$scratch-3" | grep -q '^open r /title/00010001/57505345/data/backup/savedata.bin -> 0$' || fail "run 3 did not read the backup"
# The rollback save (no backup step) comes first: its commit precedes any move to backup/.
first_moves=$(nandlog "$scratch-3" | grep '^move' | head -2 | tr '\n' '|')
[ "$first_moves" = "move /tmp/savedata.bin -> ./ -> 0|move /tmp/banner.bin -> ./ -> 0|" ] ||
    fail "run 3 did not restore savedata.bin from the backup first: $first_moves"
nandlog "$scratch-3" | grep -q '^move savedata.bin -> backup/ -> 0$' || fail "run 3 did not save again after continuing"
python3 "$here/check_save.py" "$data/savedata.bin" "$data/backup/savedata.bin" || fail "run 3 left an invalid save"

"$here/boot.sh" "$rumble" 6300 "$scratch-4" --no-gpu --nand "$nand" \
    --input "$here/../input/pause_stubs.txt" || fail "run 4 (pause menu stubs) failed"
grep -q '^Select Btn:0$' "$scratch-4/stdout.txt" || fail "run 4: the Home Button Menu did not open and close"
echo "save.sh: save, continue, backup fallback and the stubs passed"
