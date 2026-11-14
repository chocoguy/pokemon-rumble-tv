#!/bin/sh
# Compile one unit with the flags in port/compile_flags.txt and print its errors.
# usage: tools/cc1.sh <file> [max lines]
root=$(cd "$(dirname "$0")/.." && pwd)
python3 - "$root" "$1" "${2:-40}" <<'PY'
import subprocess, sys
from pathlib import Path
root, path, limit = Path(sys.argv[1]), Path(sys.argv[2]).resolve(), int(sys.argv[3])
sys.path.insert(0, str(root / "tools"))
import compile_check as cc
r = subprocess.run(cc.command(path, "macosx"), capture_output=True, text=True, cwd=cc.DECOMP)
lines = [l.replace(str(cc.DECOMP) + "/", "").replace(str(root) + "/", "") for l in r.stderr.splitlines() if "error" in l]
print("\n".join(lines[:limit]))
PY
