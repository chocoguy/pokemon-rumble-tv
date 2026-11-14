"""Tests for tools/import_wad.py that need no game data."""
import importlib.util
import os
import random
import struct
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
spec = importlib.util.spec_from_file_location("import_wad", ROOT / "tools/import_wad.py")
import_wad = importlib.util.module_from_spec(spec)
spec.loader.exec_module(import_wad)


def lz_compress(data: bytes, kind: int) -> bytes:
    """A simple greedy compressor, only to make streams for the decompressor to read."""
    out = bytearray([kind]) + len(data).to_bytes(3, "little")
    max_len = 18 if kind == 0x10 else 0x110
    pos = 0
    while pos < len(data):
        flags = 0
        chunk = bytearray()
        for bit in range(8):
            if pos >= len(data):
                break
            best_len, best_disp = 0, 0
            for disp in range(1, min(pos, 0x1000) + 1):
                length = 0
                while length < max_len and pos + length < len(data) and data[pos + length - disp] == data[pos + length]:
                    length += 1
                if length > best_len:
                    best_len, best_disp = length, disp
            if best_len >= 3:
                flags |= 0x80 >> bit
                disp = best_disp - 1
                if kind == 0x10:
                    chunk += bytes([((best_len - 3) << 4) | (disp >> 8), disp & 0xFF])
                elif best_len <= 0x10:
                    chunk += bytes([((best_len - 1) << 4) | (disp >> 8), disp & 0xFF])
                else:
                    n = best_len - 0x11
                    chunk += bytes([n >> 4, ((n & 0xF) << 4) | (disp >> 8), disp & 0xFF])
                pos += best_len
            else:
                chunk.append(data[pos])
                pos += 1
        out.append(flags)
        out += chunk
    return bytes(out)


class LzTest(unittest.TestCase):
    def sample(self):
        rng = random.Random(7)
        words = [bytes(rng.randrange(256) for _ in range(rng.randrange(1, 9))) for _ in range(40)]
        return b"".join(rng.choice(words) for _ in range(600)) + b"\0" * 700

    def test_lz10_round_trip(self):
        data = self.sample()
        self.assertEqual(import_wad.lz_decompress(lz_compress(data, 0x10)), data)

    def test_lz11_round_trip(self):
        data = self.sample()
        packed = lz_compress(data, 0x11)
        self.assertLess(len(packed), len(data))
        self.assertEqual(import_wad.lz_decompress(packed), data)

    def test_rejects_other_data(self):
        with self.assertRaises(import_wad.ImportError_):
            import_wad.lz_decompress(b"\x00\x01\x02\x03\x04")


class DescribeTest(unittest.TestCase):
    def test_u8_archive(self):
        nodes = struct.pack(">III", 0x01000000, 0, 3) + b"\0" * 24
        data = import_wad.U8_MAGIC + struct.pack(">III", 0x20, len(nodes), 0x60) + b"\0" * 16 + nodes
        self.assertEqual(import_wad.describe(data), "U8 archive, 2 entries")

    def test_missing_source_explains_what_to_do(self):
        class Args:
            source = None
            dolphin_dir = os.devnull
        self.assertEqual(import_wad.run(Args), 2)


if __name__ == "__main__":
    unittest.main()
