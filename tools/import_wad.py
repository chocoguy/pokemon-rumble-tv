#!/usr/bin/env python3
"""Import Pokemon Rumble's decrypted contents for the port.

The WAD is encrypted, so this script does not read it. Install the WAD in Dolphin
(Tools > Install WAD) and Dolphin writes the decrypted contents into its Wii folder;
this script copies them from there, or from any folder given with --from.

It then:
  1. copies every content file to rom/content/, plus the shared contents the title's TMD
     lists (the Home Button Menu's data), which Dolphin keeps in Wii/shared1,
  2. finds the LZ-compressed main executable, decompresses it and checks it against the
     decomp's frozen manifest (size and SHA-256),
  3. writes it to pokemon-rumble-decomp-tv/orig/wpse-v1/main.dol,
  4. runs the decomp's own extractor for the message archive embedded in the executable,
  5. lists what each content file is.

Nothing it writes is tracked by git.
"""
import argparse
import hashlib
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
DECOMP = ROOT / "pokemon-rumble-decomp-tv"
TITLE_HI = "00010001"
TITLE_LO = "57505345"  # "WPSE"
DOLPHIN_DIRS = [
    Path.home() / "Library/Application Support/Dolphin",
    Path.home() / ".local/share/dolphin-emu",
    Path.home() / "Documents/Dolphin Emulator",
]
U8_MAGIC = b"\x55\xAA\x38\x2D"


class ImportError_(Exception):
    pass


def lz_decompress(data: bytes) -> bytes:
    """Nintendo LZ10 / LZ11 (the formats the SDK's CX library reads)."""
    if len(data) < 4 or data[0] not in (0x10, 0x11):
        raise ImportError_("not LZ10/LZ11 data")
    kind = data[0]
    size = int.from_bytes(data[1:4], "little")
    pos = 4
    if size == 0:
        size = int.from_bytes(data[4:8], "little")
        pos = 8
    out = bytearray()
    while len(out) < size:
        flags = data[pos]
        pos += 1
        for bit in range(8):
            if len(out) >= size:
                break
            if not flags & (0x80 >> bit):
                out.append(data[pos])
                pos += 1
                continue
            if kind == 0x10:
                pair = (data[pos] << 8) | data[pos + 1]
                pos += 2
                length = (pair >> 12) + 3
                disp = (pair & 0xFFF) + 1
            else:
                indicator = data[pos] >> 4
                if indicator == 0:
                    length = (((data[pos] & 0xF) << 4) | (data[pos + 1] >> 4)) + 0x11
                    disp = (((data[pos + 1] & 0xF) << 8) | data[pos + 2]) + 1
                    pos += 3
                elif indicator == 1:
                    length = (((data[pos] & 0xF) << 12) | (data[pos + 1] << 4) | (data[pos + 2] >> 4)) + 0x111
                    disp = (((data[pos + 2] & 0xF) << 8) | data[pos + 3]) + 1
                    pos += 4
                else:
                    length = indicator + 1
                    disp = (((data[pos] & 0xF) << 8) | data[pos + 1]) + 1
                    pos += 2
            if disp > len(out):
                raise ImportError_("corrupt LZ stream (back-reference before start)")
            for _ in range(length):
                if len(out) >= size:
                    break
                out.append(out[-disp])
    return bytes(out)


def find_dolphin_content(dolphin_dir):
    candidates = [dolphin_dir] if dolphin_dir else DOLPHIN_DIRS
    for base in candidates:
        content = Path(base) / "Wii/title" / TITLE_HI / TITLE_LO / "content"
        if content.is_dir():
            return content
    return None


def describe(data: bytes) -> str:
    if data[:4] == U8_MAGIC:
        node_offset, = struct.unpack_from(">I", data, 4)
        count, = struct.unpack_from(">I", data, node_offset + 8)
        return f"U8 archive, {count - 1} entries"
    if data[:1] in (b"\x10", b"\x11") and len(data) > 8:
        return f"LZ{data[0]:02X}-compressed, {int.from_bytes(data[1:4], 'little'):,} bytes unpacked"
    if data[4:8] == b"IMD5" or data[0x40:0x44] == b"IMET" or data[0x80:0x84] == b"IMET":
        return "channel banner"
    if len(data) >= 0x100 and struct.unpack_from(">I", data, 0)[0] == 0x100:
        return "DOL executable"
    return "unknown"


def tmd_contents(tmd: bytes):
    """(content id, index, type, size, sha1) for each content a title.tmd lists."""
    count, = struct.unpack_from(">H", tmd, 0x1DE)
    contents = []
    for i in range(count):
        cid, index, ctype, size = struct.unpack_from(">IHHQ", tmd, 0x1E4 + 36 * i)
        contents.append((cid, index, ctype, size, tmd[0x1E4 + 36 * i + 16:0x1E4 + 36 * i + 36]))
    return contents


def find_shared_contents(source: Path, dolphin_dir):
    """Dolphin's Wii/shared1 folder: shared contents stored by SHA-1 (content.map)."""
    candidates = [source.parents[3] / "shared1"] if len(source.parents) > 3 else []
    candidates += [Path(base) / "Wii/shared1" for base in ([dolphin_dir] if dolphin_dir else DOLPHIN_DIRS)]
    for folder in candidates:
        if (folder / "content.map").is_file():
            return folder
    return None


def copy_shared_contents(source: Path, out_dir: Path, dolphin_dir) -> list:
    """Copies the shared contents the TMD lists (the Home Button Menu's data, contents
    2-4) from Dolphin's shared1 folder, where installing the WAD put them. Returns the
    ones it could not find."""
    tmd_path = source / "title.tmd"
    if not tmd_path.is_file():
        return []
    shared = [c for c in tmd_contents(tmd_path.read_bytes()) if c[2] & 0x8000]
    folder = find_shared_contents(source, dolphin_dir)
    by_hash = {}
    if folder is not None:
        table = (folder / "content.map").read_bytes()
        for offset in range(0, len(table) - 27, 28):
            by_hash[table[offset + 8:offset + 28]] = table[offset:offset + 8].decode("ascii").lower() + ".app"
    missing = []
    for cid, index, _, size, sha1 in shared:
        name = by_hash.get(sha1)
        target = out_dir / f"{cid:08x}.app"
        if name is None or not (folder / name).is_file():
            missing.append(index)
            continue
        data = (folder / name).read_bytes()[:size]
        if hashlib.sha1(data).digest() != sha1:
            missing.append(index)
            continue
        target.write_bytes(data)
        print(f"  {target.name:14s} {len(data):>11,} bytes  shared content {index}, from {folder}")
    return missing


def expected_executable():
    manifest = json.loads((DECOMP / "config/targets/wpse-v1.json").read_text(encoding="utf-8"))
    artifact = manifest["artifacts"][0]
    return artifact["size"], artifact["sha256"]


def run(args) -> int:
    source = Path(args.source) if args.source else find_dolphin_content(args.dolphin_dir)
    if source is None or not source.is_dir():
        print(
            "No decrypted contents found.\n"
            "  Open Dolphin, choose Tools > Install WAD..., pick the WAD in rom/, then run this again.\n"
            f"  Looked for: Wii/title/{TITLE_HI}/{TITLE_LO}/content under\n    "
            + "\n    ".join(str(p) for p in ([args.dolphin_dir] if args.dolphin_dir else DOLPHIN_DIRS))
            + "\n  Or pass --from <folder of decrypted .app files>.",
            file=sys.stderr,
        )
        return 2

    apps = sorted(p for p in source.iterdir() if p.suffix.lower() == ".app")
    if not apps:
        print(f"{source} holds no .app content files.", file=sys.stderr)
        return 2

    out_dir = ROOT / "rom/content"
    out_dir.mkdir(parents=True, exist_ok=True)
    for extra in ("title.tmd",):
        if (source / extra).is_file():
            shutil.copyfile(source / extra, out_dir / extra)

    size, sha256 = expected_executable()
    executable = None
    print(f"Contents from {source}:")
    for app in apps:
        data = app.read_bytes()
        shutil.copyfile(app, out_dir / app.name.lower())
        note = describe(data)
        if data[:1] in (b"\x10", b"\x11") and int.from_bytes(data[1:4], "little") == size:
            try:
                unpacked = lz_decompress(data)
            except (ImportError_, IndexError) as error:
                note += f" (failed to unpack: {error})"
            else:
                if hashlib.sha256(unpacked).hexdigest() == sha256:
                    executable = unpacked
                    note += " -> main.dol, matches the decomp's manifest"
                else:
                    note += " -> unpacked, but NOT the WPSE USA v1 executable"
        print(f"  {app.name.lower():14s} {len(data):>11,} bytes  {note}")

    missing = copy_shared_contents(source, out_dir, args.dolphin_dir)
    if missing:
        print(
            f"  shared contents {', '.join(map(str, missing))} not found: Dolphin keeps them in Wii/shared1 after\n"
            "  Tools > Install WAD. Without them the game stops when it loads the Home Button Menu's files.",
            file=sys.stderr,
        )

    if executable is None:
        print(
            f"\nNo content unpacked to the expected main.dol ({size:,} bytes, SHA-256 {sha256}).\n"
            "This port needs the USA WiiWare release (WPSE), title version 1.",
            file=sys.stderr,
        )
        return 1

    dol = DECOMP / "orig/wpse-v1/main.dol"
    dol.parent.mkdir(parents=True, exist_ok=True)
    dol.write_bytes(executable)
    print(f"\nWrote {dol.relative_to(ROOT)}")

    archive = ROOT / "build/generated/assets/system_message.arc"
    include = ROOT / "build/generated/include/system_message.inc"
    archive.parent.mkdir(parents=True, exist_ok=True)
    include.parent.mkdir(parents=True, exist_ok=True)
    result = subprocess.run(
        [sys.executable, str(DECOMP / "tools/assets/extract_system_message.py"), str(dol), str(archive), "--c-include", str(include)],
        cwd=DECOMP,
    )
    if result.returncode != 0:
        print("The decomp's message-archive extractor failed.", file=sys.stderr)
        return 1
    print(f"Wrote {include.relative_to(ROOT)}")
    print("Import complete.")
    return 0


def main() -> int:
    if sys.version_info < (3, 10):
        # The decomp's own extractor (step 4) needs 3.10. macOS's /usr/bin/python3 is 3.9.
        print(
            f"This needs Python 3.10 or newer; {sys.executable} is {sys.version.split()[0]}.\n"
            "Run it with a newer Python, for example: /opt/homebrew/bin/python3 tools/import_wad.py",
            file=sys.stderr,
        )
        return 2
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--from", dest="source", help="folder of decrypted .app content files (default: Dolphin's Wii folder)")
    parser.add_argument("--dolphin-dir", help="Dolphin's user folder, if it is not in the usual place")
    return run(parser.parse_args())


if __name__ == "__main__":
    raise SystemExit(main())
