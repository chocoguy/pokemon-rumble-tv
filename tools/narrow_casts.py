#!/usr/bin/env python3
"""Find addresses squeezed into 32 bits: narrow integer-to-pointer casts, and
pointer-width values (uintptr_t, intptr_t) implicitly truncated to 32 bits.

The first kind:

In C++, clang accepts `reinterpret_cast<T*>(u32_value)` without any diagnostic, so a
32-bit address turned straight into a pointer would only show up as a crash. This tool
dumps each unit's syntax tree, finds every IntegralToPointer cast and reports those whose
operand is not a 64-bit integer type. Each one needs PortPtr (if the value is a game
address) or a wider type (if it is a pointer kept in an integer at run time).

    tools/narrow_casts.py               # all units compile_check.py covers
    tools/narrow_casts.py --only nw4r
"""
import argparse
import collections
import concurrent.futures
import os
import re
import subprocess
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_check as cc

WIDE = re.compile(r"'(?:[^']*:)?'?(?:const )?(?:unsigned long|long|unsigned long long|long long|__int128)'?$")
LOC = re.compile(r"<(?:(?P<a>[^<>]*?), (?P<b>[^<>]*?)|(?P<c>[^<>,]*?))>")
POINT = re.compile(r"^(?:(?P<file>.+?):)?(?:line:)?(?P<line>\d+):(?P<col>\d+)$|^col:(?P<col2>\d+)$")


class Position:
    def __init__(self):
        self.file = None
        self.line = None

    def update(self, text):
        """Update from one location as the AST printer writes it; return (file, line, col)."""
        text = text.strip()
        if text.startswith("col:"):
            return self.file, self.line, int(text[4:])
        if text.startswith("line:"):
            _, line, col = text.split(":")
            self.line = int(line)
            return self.file, self.line, int(col)
        m = re.match(r"^(.+):(\d+):(\d+)$", text)
        if m and not m.group(1).startswith("<"):
            self.file, self.line = m.group(1), int(m.group(2))
            return self.file, self.line, int(m.group(3))
        return None


def scan(item, sdk):
    group, path = item
    cmd = cc.command(path, sdk)
    cmd = [c for c in cmd if c != "-fsyntax-only"] + ["-fsyntax-only", "-Xclang", "-ast-dump"]
    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, cwd=cc.DECOMP, errors="replace")
    pos = Position()
    found = []
    pending = None
    for raw in proc.stdout:
        line = raw.rstrip("\n")
        begin = None
        for m in LOC.finditer(line):
            parts = [m.group("a"), m.group("b")] if m.group("a") is not None else [m.group("c")]
            for k, part in enumerate(parts):
                if part is None or part in ("invalid sloc", "<invalid sloc>"):
                    continue
                if " " in part.strip() and not part.strip().startswith(("line:", "col:", "/")):
                    continue
                r = pos.update(part)
                if r and begin is None and k == 0:
                    begin = r
            break  # only the node's own range is on its line
        if pending is not None:
            # The cast's operand is the next node; its type is the last quoted type on the line.
            types = re.findall(r"'([^']*)'(?::'([^']*)')?", line)
            if types:
                spelled, canonical = types[0]
                ty = canonical or spelled
                if not re.fullmatch(r"(const )?(volatile )?(unsigned )?(long|long long|__int128)( int)?", ty) and "*" not in ty:
                    found.append((pending, ty))
            pending = None
        if "<IntegralToPointer>" in line and begin is not None:
            pending = begin
    proc.wait()
    return group, path, found


TRUNC = re.compile(r"^(.*?):(\d+):(\d+): warning: implicit conversion loses integer precision: '([^']*)'(?: \(aka '[^']*'\))? to '([^']*)'")


def truncations(item, sdk):
    """Implicit 64-to-32-bit conversions of pointer-width values (uintptr_t, intptr_t):
    an address widened for arithmetic and then stored in a 32-bit integer again."""
    _, path = item
    cmd = [c for c in cc.command(path, sdk) if c != "-Wno-everything"] + ["-w", "-Wshorten-64-to-32"]
    cmd = [c for c in cmd if c != "-w"]
    result = subprocess.run(cmd, capture_output=True, text=True, cwd=cc.DECOMP)
    out = []
    for line in result.stderr.splitlines():
        m = TRUNC.match(line)
        if m and re.search(r"\b(u?intptr_t|ptrdiff_t)\b", m.group(4)):
            out.append(((m.group(1), int(m.group(2)), int(m.group(3))), f"{m.group(4)} truncated to {m.group(5)}"))
    return out


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--sdk", default="macosx")
    parser.add_argument("--only", choices=sorted(cc.GROUPS))
    args = parser.parse_args()
    items = list(cc.units(args.only))
    sites = collections.OrderedDict()
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        for found in pool.map(lambda item: truncations(item, args.sdk), items):
            for (file, line, col), what in found:
                rel = os.path.relpath(cc.DECOMP / file) if not file.startswith("/") else os.path.relpath(file, cc.DECOMP)
                if not rel.startswith(".."):
                    sites.setdefault((rel, line, col), what)
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count()) as pool:
        for _, _, found in pool.map(lambda item: scan(item, args.sdk), items):
            for (file, line, col), ty in found:
                if file is None:
                    continue
                rel = os.path.relpath(file, cc.DECOMP) if file.startswith("/") else file
                if rel.startswith(".."):
                    continue  # system headers
                sites.setdefault((rel, line, col), f"cast to a pointer from {ty}")
    for (rel, line, col), what in sorted(sites.items()):
        print(f"{rel}:{line}:{col}: {what}")
    print(f"{len(sites)} narrowed addresses (integer-to-pointer casts and truncated uintptr_t values)", file=sys.stderr)
    return 1 if sites else 0


if __name__ == "__main__":
    raise SystemExit(main())
