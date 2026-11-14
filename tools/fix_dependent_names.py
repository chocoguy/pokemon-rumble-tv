#!/usr/bin/env python3
"""Insert `this->` where CodeWarrior accepted an unqualified member of a dependent base.

Standard C++ does not look in a base class that depends on a template parameter unless
the name is qualified. clang reports each such use with its exact position:

    explicit qualification required to use member 'X' from dependent base class

This tool compiles the units (same flags as compile_check.py), inserts `this->` at each
reported position and repeats until none are left. With --undeclared-in it also fixes
plain "use of undeclared identifier" errors, but only inside the listed files, for
headers where every such error has been checked by hand to be this same problem.
"""
import argparse
import concurrent.futures
import os
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_check as cc

QUALIFY = re.compile(r"^(.*?):(\d+):(\d+): error: explicit qualification required to use member '(\w+)' from dependent base class")
UNDECLARED = re.compile(r"^(.*?):(\d+):(\d+): error: use of undeclared identifier '(\w+)'$")


def collect(units, sdk, undeclared_in):
    sites = set()
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        for _, _, _, errors, _ in pool.map(lambda item: cc.check(item, sdk), units):
            for line in errors:
                match = QUALIFY.match(line)
                if not match:
                    match = UNDECLARED.match(line)
                    if not match:
                        continue
                    path = (cc.DECOMP / match.group(1)).resolve()
                    if not any(path == allowed for allowed in undeclared_in):
                        continue
                path = (cc.DECOMP / match.group(1)).resolve()
                sites.add((path, int(match.group(2)), int(match.group(3)), match.group(4)))
    return sites


def apply(sites):
    changed = 0
    by_file = {}
    for path, line, col, name in sites:
        by_file.setdefault(path, []).append((line, col, name))
    for path, entries in by_file.items():
        lines = path.read_text(encoding="utf-8").split("\n")
        # Right to left within a line, so earlier columns stay valid.
        for line, col, name in sorted(entries, reverse=True):
            text = lines[line - 1]
            start = col - 1
            if text[start:start + len(name)] != name or text[max(0, start - 6):start] == "this->":
                continue
            lines[line - 1] = text[:start] + "this->" + text[start:]
            changed += 1
        path.write_text("\n".join(lines), encoding="utf-8")
    return changed


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sdk", default="macosx")
    parser.add_argument("--only", choices=sorted(cc.GROUPS))
    parser.add_argument("--unit", action="append", default=[], help="extra unit to compile (absolute or relative to the repo root)")
    parser.add_argument("--undeclared-in", action="append", default=[], help="file (relative to the fork) where undeclared identifiers are also qualified")
    parser.add_argument("--max-rounds", type=int, default=12)
    args = parser.parse_args()

    units = list(cc.units(args.only)) + [("extra", (cc.ROOT / unit).resolve()) for unit in args.unit]
    undeclared_in = [(cc.DECOMP / name).resolve() for name in args.undeclared_in]
    total = 0
    for round_number in range(1, args.max_rounds + 1):
        changed = apply(collect(units, args.sdk, undeclared_in))
        print(f"round {round_number}: {changed} insertions")
        total += changed
        if not changed:
            break
    print(f"{total} insertions in all")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
