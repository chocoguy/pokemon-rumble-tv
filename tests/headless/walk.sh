#!/bin/sh
# Analog walking: runs tests/input/analog_walk.txt with PORT_WALK_TRACE=1 and checks the
# player's average step at a half, quarter and three-quarter stick push against the full
# push (the patched script scales speed by 0.3 + 0.7 x push), and that the d-pad still walks
# at full speed. Also the boot.sh checks.
#
#   tests/headless/walk.sh <rumble binary> <scratch dir>
set -u
here=$(cd "$(dirname "$0")" && pwd)
PORT_WALK_TRACE=1 "$here/boot.sh" "$1" 8000 "$2" --no-gpu --input "$here/../input/analog_walk.txt" || exit 1
grep -q "script patch: CPlayerPii.pkc walks by stick distance" "$2/stderr.txt" || { echo "walk.sh: the player script was not patched"; exit 1; }
sed -n 's/.*walk \([0-9]*\): step \([0-9.]*\) dir.*/\1 \2/p' "$2/stderr.txt" | awk '
function avg(lo, hi,   i, s, n) { s = 0; n = 0; for (i = lo; i < hi; i++) if (i in step) { s += step[i]; n++ } return n ? s / n : 0 }
{ step[$1] = $2 }
END {
    full = avg(7510, 7560)
    if (full <= 0) { print "walk.sh: no full-push steps"; exit 1 }
    split("7610 7710 7810 7910", at); split("0.65 0.475 0.825 1.0", want); split("half quarter three-quarter d-pad", name)
    bad = 0
    for (k = 1; k <= 4; k++) {
        r = avg(at[k], at[k] + 30) / full
        printf "  %s push: %.3f of full (expected %s)\n", name[k], r, want[k]
        if (r < want[k] - 0.03 || r > want[k] + 0.03) bad = 1
    }
    exit bad
}'
