#!/usr/bin/env python3
"""Checks a WAV the mixer wrote (rumble --wav): 32 kHz 16-bit stereo, at least SECONDS
long, and from FROM seconds on neither silent nor clipping, with both channels in use.

    tests/headless/check_wav.py FILE SECONDS FROM

Prints the level of each second from FROM on; exits 1 if a check fails."""
import math
import struct
import sys
import wave


def main():
    path, seconds, start = sys.argv[1], float(sys.argv[2]), float(sys.argv[3])
    with wave.open(path) as w:
        if (w.getframerate(), w.getsampwidth(), w.getnchannels()) != (32000, 2, 2):
            print(f"check_wav: {path}: not 32 kHz 16-bit stereo")
            return 1
        frames = w.getnframes()
        data = w.readframes(frames)
    rate = 32000
    length = frames / rate
    if length < seconds:
        print(f"check_wav: {length:.1f} s of sound, expected {seconds:.1f}")
        return 1
    samples = struct.unpack(f"<{frames * 2}h", data)
    fail = 0
    for second in range(int(start), int(length)):
        chunk = samples[second * rate * 2:(second + 1) * rate * 2]
        rms = math.sqrt(sum(x * x for x in chunk) / len(chunk))
        peak = max(abs(x) for x in chunk)
        side = sum(abs(a - b) for a, b in zip(chunk[0::2], chunk[1::2])) / (len(chunk) / 2)
        print(f"  {second:4d} s: rms {rms:6.0f}  peak {peak:5d}  |L-R| {side:5.0f}")
        if rms < 100:
            print("check_wav: silent")
            fail = 1
        if peak >= 32767:
            print("check_wav: clipped")
            fail = 1
        if side < 1:
            print("check_wav: both channels the same")
            fail = 1
    return fail


if __name__ == "__main__":
    sys.exit(main())
