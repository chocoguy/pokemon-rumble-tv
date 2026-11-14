#!/usr/bin/env python3
"""Leave out the CodeWarrior-only explicit specializations clang rejects.

To reproduce the original executable, the decomp declares explicit specializations of
library templates after they were implicitly instantiated (to choose which unit emits a
function and into which section), and defines some through friend functions of
`...Generation` structs. Standard C++ rejects both; the generic template produces the
same function, so on the port these declarations are wrapped in `#if !defined(PORT)`.

Only what clang reports is touched:
  - "explicit specialization of ... after instantiation"
  - "specialization of member ... does not specialize an instantiated member"
      -> the one declaration (or definition) at that line
  - "friend function specialization cannot be defined"
  - "friend function definition cannot be qualified"
      -> the enclosing Generation struct, plus the explicit specialization
         declarations directly above it that the friend was defining

    tools/drop_late_specializations.py src/game/graphics/CModel.cpp ...
"""
import re
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import compile_check as cc
from port_replace import strip_code

LATE = re.compile(r"^(.*?):(\d+):\d+: error: (explicit specialization of .* after instantiation|specialization of member .* does not specialize an instantiated member)")
FRIEND = re.compile(r"^(.*?):(\d+):\d+: error: (friend function specialization cannot be defined|friend function definition cannot be qualified)")
BEGIN = "#if !defined(PORT) /* CodeWarrior-only specialization */"


def statement_end(lines, start):
    """Last line of the declaration or definition that starts at `start`."""
    depth = 0
    seen_brace = False
    for k in range(start, len(lines)):
        code = strip_code(lines[k])
        for ch in code:
            if ch == "{":
                depth += 1
                seen_brace = True
            elif ch == "}":
                depth -= 1
                if seen_brace and depth == 0:
                    rest = code[code.rindex("}") + 1:].strip()
                    if not rest or rest.startswith((";", "}")) or not seen_brace:
                        return k
            elif ch == ";" and depth == 0:
                return k
        if seen_brace and depth <= 0:
            return k
    raise ValueError(f"no end for statement at line {start + 1}")


def statement_start(lines, line):
    """First line of the declaration containing 0-based `line` (back to `template <>`)."""
    k = line
    while k > 0 and not re.match(r"\s*(namespace\s+\w+\s*\{\s*)*template\s*<\s*>", lines[k]):
        prev = lines[k - 1].strip()
        if not prev or prev.endswith((";", "}", "{")) or prev.startswith("#"):
            break
        k -= 1
    return k


def generation_span(lines, line):
    """The Generation struct around `line`, and the specialization declarations above it."""
    k = line
    while k >= 0 and not re.match(r"\s*template\s*<\s*typename\s+\w+\s*>\s*struct\s+\w+Generation", lines[k]):
        k -= 1
    if k < 0:
        raise ValueError(f"no Generation struct above line {line + 1}")
    end = statement_end(lines, k)
    first = k
    while first > 0 and re.match(r"\s*template\s*<\s*>", lines[first - 1]):
        first -= 1
    return first, end


def wrap(path, spans):
    lines = path.read_text(encoding="utf-8").split("\n")
    merged = []
    for first, last in sorted(set(spans)):
        if merged and first <= merged[-1][1] + 1:
            merged[-1] = (merged[-1][0], max(merged[-1][1], last))
        else:
            merged.append((first, last))
    changed = 0
    for first, last in reversed(merged):
        if first > 0 and lines[first - 1].strip() == BEGIN:
            continue
        lines[last + 1:last + 1] = ["#endif"]
        lines[first:first] = [BEGIN]
        changed += 1
    path.write_text("\n".join(lines), encoding="utf-8")
    return changed


def main():
    units = [cc.DECOMP / u for u in sys.argv[1:]]
    total = 0
    for _ in range(6):
        spans = {}
        for unit in units:
            _, _, _, errors, _ = cc.check(("", unit), "macosx")
            for line in errors:
                for pattern, kind in ((LATE, "late"), (FRIEND, "friend")):
                    m = pattern.match(line)
                    if not m:
                        continue
                    path = (cc.DECOMP / m.group(1)).resolve()
                    lines = path.read_text(encoding="utf-8").split("\n")
                    n = int(m.group(2)) - 1
                    if kind == "late":
                        start = statement_start(lines, n)
                        span = (start, statement_end(lines, start))
                    else:
                        span = generation_span(lines, n)
                    spans.setdefault(path, []).append(span)
        changed = sum(wrap(path, s) for path, s in spans.items())
        total += changed
        print(f"pass: {changed} blocks wrapped")
        if not changed:
            break
    print(f"{total} blocks left out on the port")


if __name__ == "__main__":
    main()
