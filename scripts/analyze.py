#!/usr/bin/env python3
"""Summarise a frames.bin produced by ./replay (385 bytes per frame)."""
import struct
import sys
import statistics

FRAME = 385
NSAMP = 192


def load(path):
    raw = open(path, "rb").read()
    n = len(raw) // FRAME
    frames = []
    for i in range(n):
        f = raw[i * FRAME:(i + 1) * FRAME]
        frames.append([struct.unpack_from("<h", f, 1 + 2 * j)[0] for j in range(NSAMP)])
    return frames


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else "frames.bin"
    frames = load(path)
    live = [f for f in frames if any(f)]
    print(f"frames: {len(frames)}  non-zero: {len(live)}")
    if not live:
        print("no valid frames")
        return 1

    energy = [(sum(v * v for v in f) / NSAMP) ** 0.5 for f in live]
    print(f"frame energy RMS: min {min(energy):.0f}  median {statistics.median(energy):.0f}  max {max(energy):.0f}")

    prof = [sum(abs(f[j]) for f in live) / len(live) for j in range(NSAMP)]
    top = sorted(range(NSAMP), key=lambda j: -prof[j])[:6]
    print("strongest sample bins: " + ", ".join(f"{j}={prof[j]:.0f}" for j in top))

    diff = [sum(abs(a - b) for a, b in zip(live[i], live[i + 1])) / NSAMP
            for i in range(len(live) - 1)]
    if diff:
        s = sorted(diff)
        print(f"motion metric: median {statistics.median(diff):.2f}  p95 {s[int(len(s) * 0.95)]:.2f}  max {max(diff):.2f}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
