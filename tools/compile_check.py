#!/usr/bin/env python3
"""Phase 1 metric: how much of the decomp compiles with clang for arm64.

Runs `clang -fsyntax-only` over every unit the port compiles from the fork (the game,
NW4R and the SDK libraries the port keeps) and reports which pass, which fail and the
most common errors. Nothing is linked and no game data is needed.

    tools/compile_check.py                 # macOS arm64, summary
    tools/compile_check.py --sdk appletvos # tvOS arm64
    tools/compile_check.py --show src/game/core/CFiber.cpp   # full errors for one unit
    tools/compile_check.py --only nw4r     # one group
"""
import argparse
import collections
import concurrent.futures
import json
import os
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DECOMP = ROOT / "pokemon-rumble-decomp-tv"

# Units compiled from the fork, by group. Everything else in src/ is replaced by port/.
GROUPS = {
    "game": ["src/game/**/*.cpp"],
    "nw4r": ["src/nw4r/ef/**/*.cpp", "src/nw4r/g3d/**/*.cpp", "src/nw4r/lyt/**/*.cpp", "src/nw4r/math/**/*.cpp",
             "src/nw4r/snd/**/*.cpp", "src/nw4r/ut/**/*.cpp"],
    # MSL's own C library where the host's differs: 16-bit wchar_t text functions, the
    # console's rand(), and the C++ runtime pieces the reconstructed headers call into.
    "msl": ["src/MSL_C/extras.c", "src/MSL_C/locale.c", "src/MSL_C/rand.c", "src/MSL_C/wcstoul.c",
            "src/MSL_C/wide_decimal.c", "src/MSL_C/wmem.c", "src/MSL_C/wprintf.c", "src/MSL_C/wstring.c",
            "src/MSL_C/wstrtold.c", "src/MSL_C/ansi_fp.c", "src/MSL_C/strnicmp.c", "src/MSL_C/mbstring.c",
            "src/MSL_C/wscanf.c", "src/MSL_C/ctype_data.c", "src/MSL_C/wide_ctype_data.c",
            "src/MSL_C/floating_constants.c", "src/MSL_CPP/msl_thread.cpp", "src/Runtime/new_handler.cpp"],
    "sdk": ["src/RVL_SDK/gx/GX[!F]*.c", "src/RVL_SDK/gx/GXFrameBuf.c",  # GXFifo.c: replaced by port/gx
             "src/RVL_SDK/mtx/*.c", "src/RVL_SDK/mem/*.c", "src/RVL_SDK/arc/*.c",
            "src/RVL_SDK/cx/*.c", "src/RVL_SDK/tpl/*.c", "src/RVL_SDK/axfx/*.c", "src/RVL_SDK/enc/*.c",
            "src/RVL_SDK/gd/*.c", "src/TMC_JPEG/*.c", "src/RevoEX/net/sha1.c",
            # OS units with no hardware access; the scheduler they call is port/os.
            "src/RVL_SDK/os/OSMutex.c", "src/RVL_SDK/os/OSMessage.c", "src/RVL_SDK/os/OSSemaphore.c",
            "src/RVL_SDK/os/OSCrc.c"],
}

# Units that include files tools/import_wad.py generates from the user's game files.
NEEDS_GAME_FILES = {
    "src/game/ui/SystemMessageArchive.cpp": ROOT / "build/generated/include/system_message.inc",
}

def read_flags():
    """(common, c, cxx) flags from port/compile_flags.txt, with paths made absolute."""
    common, c, cxx = [], [], []
    for raw in (ROOT / "port/compile_flags.txt").read_text(encoding="utf-8").splitlines():
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        target = common
        if line.startswith("c:"):
            target, line = c, line[2:].strip()
        elif line.startswith("cxx:"):
            target, line = cxx, line[4:].strip()
        if line.startswith("-I"):
            line = "-I" + str(ROOT / line[2:])
        elif line.startswith("port/"):
            line = str(ROOT / line)
        target.append(line)
    return common, c, cxx


COMMON_FLAGS, C_LANG_FLAGS, CXX_LANG_FLAGS = read_flags()
COMMON_FLAGS = ["-fsyntax-only", "-arch", "arm64", "-ferror-limit=0"] + COMMON_FLAGS
CXX_FLAGS = ["-x", "c++"] + CXX_LANG_FLAGS
C_FLAGS = ["-x", "c"] + C_LANG_FLAGS


def units(only):
    for group, patterns in GROUPS.items():
        if only and group != only:
            continue
        for pattern in patterns:
            for path in sorted(DECOMP.glob(pattern)):
                yield group, path


def command(path, sdk):
    flags = ["xcrun", "--sdk", sdk, "clang"] + COMMON_FLAGS
    flags += C_FLAGS if path.suffix == ".c" else CXX_FLAGS
    if sdk == "appletvos":
        flags += ["-mtvos-version-min=26.0"]
    return flags + [str(path)]


def check(item, sdk):
    group, path = item
    result = subprocess.run(command(path, sdk), capture_output=True, text=True, cwd=DECOMP)
    errors = [line for line in result.stderr.splitlines() if re.search(r": (fatal )?error: ", line)]
    return group, path, result.returncode, errors, result.stderr


def normalise(line):
    message = re.split(r": (?:fatal )?error: ", line, maxsplit=1)[1]
    return re.sub(r"'[^']*'", "'…'", message)[:110]


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sdk", default="macosx", choices=["macosx", "appletvos", "appletvsimulator"])
    parser.add_argument("--only", choices=sorted(GROUPS))
    parser.add_argument("--show", help="print the full compiler output for one unit (path relative to the fork)")
    parser.add_argument("--top", type=int, default=25, help="how many error kinds and failing units to list")
    parser.add_argument("--sites", type=int, default=0, help="also list the N most repeated error locations")
    parser.add_argument("--list", action="store_true", help="print the units to compile (absolute paths, ';'-separated) and exit")
    parser.add_argument("--report", type=Path, default=ROOT / "build/compile_check.json")
    args = parser.parse_args()

    if args.list:
        paths = [str(path) for _, path in units(args.only)
                 if not (str(path.relative_to(DECOMP)) in NEEDS_GAME_FILES and not NEEDS_GAME_FILES[str(path.relative_to(DECOMP))].exists())]
        print(";".join(paths), end="")
        return 0

    if args.show:
        path = DECOMP / args.show
        _, _, code, _, stderr = check(("", path), args.sdk)
        sys.stdout.write(stderr)
        return code

    items = list(units(args.only))
    waiting = [str(path.relative_to(DECOMP)) for _, path in items
               if str(path.relative_to(DECOMP)) in NEEDS_GAME_FILES and not NEEDS_GAME_FILES[str(path.relative_to(DECOMP))].exists()]
    items = [item for item in items if str(item[1].relative_to(DECOMP)) not in waiting]
    totals = collections.Counter()
    passed = collections.Counter()
    kinds = collections.Counter()
    sites = collections.Counter()
    failing = []
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        for group, path, code, errors, _ in pool.map(lambda item: check(item, args.sdk), items):
            totals[group] += 1
            if code == 0:
                passed[group] += 1
            else:
                failing.append((len(errors), str(path.relative_to(DECOMP)), group))
                for line in errors:
                    kinds[normalise(line)] += 1
                    sites[line.replace(str(DECOMP) + "/", "").replace(str(ROOT) + "/", "")] += 1

    print(f"clang -fsyntax-only, {args.sdk} arm64")
    for group in GROUPS:
        if totals[group]:
            print(f"  {group:5s} {passed[group]:4d} / {totals[group]:4d} units compile")
    print(f"  total {sum(passed.values()):4d} / {sum(totals.values()):4d}")
    for unit in waiting:
        print(f"  not checked until the game files are imported (tools/import_wad.py): {unit}")
    if kinds:
        print(f"\nMost common errors ({sum(kinds.values())} in all):")
        for message, count in kinds.most_common(args.top):
            print(f"  {count:6d}  {message}")
        if args.sites:
            print("\nMost repeated error sites (an error in a header counts once per unit that includes it):")
            for line, count in sites.most_common(args.sites):
                print(f"  {count:6d}  {line[:230]}")
        print("\nFailing units with the fewest errors:")
        for count, path, _ in sorted(failing)[: args.top]:
            print(f"  {count:6d}  {path}")

    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps({
        "sdk": args.sdk,
        "passed": dict(passed), "totals": dict(totals),
        "failing": [{"unit": path, "errors": count, "group": group} for count, path, group in sorted(failing)],
        "error_kinds": kinds.most_common(),
    }, indent=1), encoding="utf-8")
    return 0 if not failing else 1


if __name__ == "__main__":
    raise SystemExit(main())
