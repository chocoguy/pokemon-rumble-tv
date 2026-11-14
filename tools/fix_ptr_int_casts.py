#!/usr/bin/env python3
"""Widen C-style pointer-to-u32 casts that only do pointer arithmetic.

Old SDK code often writes `(T*)((u32)p + offset)` or tests `(u32)p & 3`. clang reports
each `(u32)` applied to a pointer with its exact column; this tool rewrites that cast to
`(uintptr_t)` (or `(intptr_t)` for s32) and repeats until no such error is left. Use it
only on files where every such cast has been read and is arithmetic or an alignment test,
not a pointer stored into a 32-bit slot.

    tools/fix_ptr_int_casts.py src/RVL_SDK/cx/CXUncompression.c ...
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_check as cc

ERR = re.compile(r"^(.*?):(\d+):(\d+): error: cast to smaller integer type '(u32|s32|unsigned int|int)'")
WIDE = {"u32": "uintptr_t", "unsigned int": "uintptr_t", "s32": "intptr_t", "int": "intptr_t"}


def main():
    files = [cc.DECOMP / f for f in sys.argv[1:]]
    total = 0
    for _ in range(10):
        changed = 0
        for path in files:
            _, _, _, errors, _ = cc.check(("", path), "macosx")
            sites = sorted({(int(m.group(2)), int(m.group(3)), m.group(4)) for m in map(ERR.match, errors)
                            if m and (cc.DECOMP / m.group(1)).resolve() == path.resolve()}, reverse=True)
            lines = path.read_text(encoding="utf-8").split("\n")
            for line, col, ty in sites:
                text = lines[line - 1]
                m = re.match(r"\(\s*" + re.escape(ty).replace(r"\ ", r"\s+") + r"\s*\)", text[col - 1:])
                if not m:
                    print(f"{path.relative_to(cc.DECOMP)}:{line}:{col}: not a plain cast, left alone")
                    continue
                lines[line - 1] = text[:col - 1] + f"({WIDE[ty]})" + text[col - 1 + m.end():]
                changed += 1
            path.write_text("\n".join(lines), encoding="utf-8")
        total += changed
        if not changed:
            break
    print(f"{total} casts widened")


if __name__ == "__main__":
    main()
