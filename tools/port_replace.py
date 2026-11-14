#!/usr/bin/env python3
"""Give named functions in a fork source file a port implementation.

Each function's original definition is kept for the Wii build and the new one is used
on the port:

    #if !defined(PORT)
    <original definition>
    #else
    <port definition>
    #endif

The replacements come from a spec file holding one or more blocks:

    @@ path/in/the/fork.c FunctionName [n]
    <port definition, written out in full>

`n` (default 1) picks the n-th definition of that name in the file, for overloads.
A function that already has a port branch is left alone, so the tool can be rerun.

    tools/port_replace.py port/replacements/sdk_mtx.txt
"""
import argparse
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DECOMP = ROOT / "pokemon-rumble-decomp-tv"
MARK = "#if !defined(PORT) /* port version below */"


def strip_code(line):
    """The line with comments and string literals blanked, for brace counting."""
    line = re.sub(r'"(\\.|[^"\\])*"', '""', line)
    line = re.sub(r"'(\\.|[^'\\])*'", "''", line)
    return line.split("//", 1)[0]


def brace_kinds(code, pending):
    """Kinds of the braces opened on this line: 'ns' for namespace / extern "C" bodies,
    'blk' for everything else. `pending` carries a namespace keyword seen on an earlier
    line whose brace has not appeared yet."""
    kinds = []
    for match in re.finditer(r'\bnamespace\b[^{;]*|extern\s*""\s*|[{}]', code):
        token = match.group(0)
        if token == "{":
            kinds.append("ns" if pending else "blk")
            pending = False
        elif token == "}":
            kinds.append("}")
        else:
            pending = True
    return kinds, pending


def find_definition(lines, name, nth):
    """(first, last) 0-based inclusive line range of the n-th definition of name at
    namespace scope (inside any number of namespaces, not inside a class or function)."""
    stack = []
    pending = False
    in_comment = False
    found = 0
    port_branch = 0   # >0 inside the #else branch this tool wrote (its own copies are not counted)
    pp_depth = 0
    i = 0
    while i < len(lines):
        raw = lines[i].strip()
        if raw == MARK:
            pp_depth = 1
        elif pp_depth:
            if raw.startswith("#if"):
                pp_depth += 1
            elif raw.startswith("#endif"):
                pp_depth -= 1
                if pp_depth == 0:
                    port_branch = 0
            elif raw.startswith("#else") and pp_depth == 1:
                port_branch = 1
        if port_branch:
            i += 1
            continue
        code = strip_code(lines[i])
        if in_comment:
            if "*/" in code:
                in_comment = False
                code = code.split("*/", 1)[1]
            else:
                i += 1
                continue
        code = re.sub(r"/\*.*?\*/", "", code)
        if "/*" in code:
            in_comment = True
            code = code.split("/*", 1)[0]
        depth = stack.count("blk")
        if depth == 0 and re.search(r"(^|[\s*&:~])" + re.escape(name) + r"\s*(\(|$)", code) and not code.lstrip().startswith("#"):
            # A definition, not a declaration or call: the next code is a '{' before any ';'.
            j = i
            tail = code[code.index(name):]
            while ";" not in tail.split("{", 1)[0] and "{" not in tail and j + 1 < len(lines):
                j += 1
                tail += strip_code(lines[j])
            head = tail.split("{", 1)[0]
            if "{" in tail and ";" not in head:
                found += 1
                if found == nth:
                    first = i
                    # Return type or qualifiers on earlier lines.
                    while first > 0:
                        prev = lines[first - 1].strip()
                        if not prev or prev.endswith((";", "}", "{", "*/")) or prev.startswith(("#", "//")):
                            break
                        first -= 1
                    k = j
                    body_depth = 0
                    started = False
                    while k < len(lines):
                        c = strip_code(lines[k]) if k > j else tail
                        body_depth += c.count("{") - c.count("}")
                        started = started or "{" in c
                        if started and body_depth <= 0:
                            return first, k
                        k += 1
                    raise ValueError(f"unbalanced braces after {name}")
        kinds, pending = brace_kinds(code, pending)
        for kind in kinds:
            if kind == "}":
                if stack:
                    stack.pop()
            else:
                stack.append(kind)
        i += 1
    return None


def apply(path, name, nth, replacement):
    lines = path.read_text(encoding="utf-8").split("\n")
    span = find_definition(lines, name, nth)
    if span is None:
        raise SystemExit(f"{path.relative_to(ROOT)}: no definition #{nth} of {name}")
    first, last = span
    if first > 0 and lines[first - 1] == MARK:
        return False
    block = [MARK] + lines[first:last + 1] + ["#else"] + replacement.strip("\n").split("\n") + ["#endif"]
    lines[first:last + 1] = block
    path.write_text("\n".join(lines), encoding="utf-8")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("spec", nargs="+", type=Path)
    args = parser.parse_args()
    changed = skipped = 0
    for spec in args.spec:
        text = spec.read_text(encoding="utf-8")
        for block in re.split(r"^@@ ", text, flags=re.M)[1:]:
            header, _, body = block.partition("\n")
            parts = header.split()
            path, name = DECOMP / parts[0], parts[1]
            nth = int(parts[2]) if len(parts) > 2 else 1
            if apply(path, name, nth, body):
                changed += 1
            else:
                skipped += 1
    print(f"{changed} functions given a port version, {skipped} already had one")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
