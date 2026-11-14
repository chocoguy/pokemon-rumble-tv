#!/usr/bin/env python3
"""Give pointer members of file-overlaid structs a 4-byte Ptr32 type on the port.

    tools/ptr32_fields.py <header in the fork> <Type:member[:nth]> ...

Each `Type* member;` (or `Type* member[N];`) becomes

    #if defined(PORT)
    Ptr32<Type> member; /* 4-byte slot in the file */
    #else
    Type* member;
    #endif

`nth` picks the n-th matching declaration in the header (default 1). A member that
already has a port branch is left alone.
"""
import re
import sys
from pathlib import Path

DECOMP = Path(__file__).resolve().parents[1] / "pokemon-rumble-decomp-tv"


def convert(path, typ, name, nth):
    s = path.read_text(encoding="utf-8")
    pat = re.compile(r"^([ \t]*)" + re.escape(typ) + r"\s*\*\s*" + re.escape(name) + r"(\[[^\]]*\])?;[^\n]*$", re.M)
    matches = list(pat.finditer(s))
    if len(matches) < nth:
        raise SystemExit(f"{path}: no declaration #{nth} of {typ}* {name}")
    m = matches[nth - 1]
    before = s[:m.start()].rstrip("\n").rsplit("\n", 1)[-1].strip()
    if before == "#else":
        return False
    indent, array = m.group(1), m.group(2) or ""
    block = (f"#if defined(PORT)\n{indent}Ptr32<{typ}> {name}{array}; /* 4-byte slot in the file */\n#else\n"
             f"{m.group(0)}\n#endif")
    path.write_text(s[:m.start()] + block + s[m.end():], encoding="utf-8")
    return True


def main():
    path = DECOMP / sys.argv[1]
    changed = 0
    for spec in sys.argv[2:]:
        parts = spec.split(":")
        changed += convert(path, parts[0], parts[1], int(parts[2]) if len(parts) > 2 else 1)
    print(f"{sys.argv[1]}: {changed} members converted")


if __name__ == "__main__":
    main()
