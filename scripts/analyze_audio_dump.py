#!/usr/bin/env python3
"""Analyze audio_dump.raw (s16 interleaved stereo) for discontinuities that
would explain periodic crackling: zero-gaps, repeated blocks, and large
sample-to-sample jumps (clicks)."""

import struct
import sys

RATE = 32000

data = open("audio_dump.raw", "rb").read()
n = len(data) // 2
samples = struct.unpack(f"<{n}h", data[:n * 2])
frames = n // 2
print(f"{n} samples, {frames} frames, {frames / RATE:.1f}s")

# 1. Zero-run detection (gaps of silence mid-stream)
zero_runs = []
run_start = None
for i in range(0, n, 2):  # scan left channel
    if samples[i] == 0:
        if run_start is None:
            run_start = i
    else:
        if run_start is not None:
            run_len = (i - run_start) // 2
            if run_len >= 32:  # >= 1ms of silence
                zero_runs.append((run_start // 2, run_len))
            run_start = None
print(f"zero-runs >=1ms: {len(zero_runs)}")
for start, length in zero_runs[:15]:
    print(f"  frame {start} (t={start / RATE:.3f}s): {length} frames ({1000 * length / RATE:.1f}ms)")

# 2. Click detection: large instantaneous jumps in the left channel
clicks = []
for i in range(2, n, 2):
    d = abs(samples[i] - samples[i - 2])
    if d > 12000:
        clicks.append((i // 2, d))
print(f"\nlarge jumps (>12000): {len(clicks)}")
for frame, d in clicks[:20]:
    print(f"  frame {frame} (t={frame / RATE:.3f}s): jump {d}")

# 3. Periodicity of clicks: histogram of gaps between consecutive clicks
if len(clicks) > 2:
    gaps = [(clicks[i + 1][0] - clicks[i][0]) / RATE for i in range(len(clicks) - 1)]
    gaps = [g for g in gaps if g > 0.01]
    if gaps:
        avg = sum(gaps) / len(gaps)
        print(f"\nclick spacing: avg {avg * 1000:.0f}ms, min {min(gaps) * 1000:.0f}ms, max {max(gaps) * 1000:.0f}ms")

# 4. Repeated-block detection (duplicate buffer submissions)
BLOCK = 1056  # samples per submission
dupes = 0
for i in range(0, n - 2 * BLOCK, BLOCK):
    if samples[i:i + BLOCK] == samples[i + BLOCK:i + 2 * BLOCK]:
        if any(s != 0 for s in samples[i:i + BLOCK]):
            dupes += 1
            if dupes <= 5:
                print(f"duplicate non-silent block at frame {i // 2} (t={i / 2 / RATE:.3f}s)")
print(f"\nduplicate blocks: {dupes}")
