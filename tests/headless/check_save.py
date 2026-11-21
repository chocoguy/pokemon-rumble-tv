#!/usr/bin/env python3
"""Checks a Pokémon Rumble save file the way the game's Load() does: undoes the
block scramble, then compares the SHA-1 of everything after the digest with the
stored digest, and checks the format version (4). The port writes the version in
host (little-endian) order, so a save copied from a Wii reads as version 0x0400.

    tests/headless/check_save.py <savedata.bin> [...]

Prints one line per file and exits 1 if any is not a valid save."""
import hashlib
import struct
import sys

SAVE_FILE_SIZE = 0x34000
VERSION = 4


def unscramble(data):
    size = len(data)
    block = (size - 1) // 16 * 16
    shift = block // 16
    while block > 0:
        for i in range(16):
            if block + i >= size:
                break
            data[block + i] = (data[block + i] - data[block + (shift + i) % 16 - 16]) & 0xFF
        block -= 16
        shift -= 1


def check(path):
    data = bytearray(open(path, "rb").read())
    if len(data) != SAVE_FILE_SIZE:
        return "size %d, expected %d" % (len(data), SAVE_FILE_SIZE)
    unscramble(data)
    if hashlib.sha1(data[20:]).digest() != bytes(data[:20]):
        return "digest mismatch (corrupt)"
    version, data_size = struct.unpack_from("<HH", data, 20)
    if version != VERSION:
        return "version %#x, expected %d" % (version, VERSION)
    return None


def main():
    bad = 0
    for path in sys.argv[1:]:
        error = check(path)
        print("check_save.py: %s: %s" % (path, error or "valid"))
        bad |= error is not None
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
