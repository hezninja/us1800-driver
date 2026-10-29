"""Finds dropouts in a daemon wire dump (US1800_DUMP): runs of all-zero frames inside the active audio region.
Usage: python3 -I analyze_wire.py wire.s24 [channels=4]"""
import sys, numpy as np
ch = int(sys.argv[2]) if len(sys.argv) > 2 else 4
raw = np.fromfile(sys.argv[1], dtype=np.uint8)
raw = raw[: len(raw) // (3 * ch) * 3 * ch].reshape(-1, ch, 3).astype(np.int32)
v = raw[:, :, 0] | (raw[:, :, 1] << 8) | (raw[:, :, 2] << 16)
v = np.where(v & 0x800000, v - 0x1000000, v)
act = np.flatnonzero(np.abs(v).max(axis=1) > 64)           # frames with real signal
print("total %.1f s, active region %.1f s" % (len(v) / 48000, (act[-1] - act[0]) / 48000 if len(act) else 0))
if not len(act): sys.exit()
seg = np.abs(v[act[0]:act[-1] + 1]).max(axis=1) <= 2
d = np.diff(np.concatenate(([0], seg.view(np.int8), [0])))
starts, ends = np.flatnonzero(d == 1), np.flatnonzero(d == -1)
runs = [(s / 48000, (e - s) / 48.0) for s, e in zip(starts, ends) if e - s >= 48]   # >= 1 ms of digital silence
print("silent runs >= 1 ms inside the music:", len(runs))
for t, ms in runs[:40]: print("  at %.2f s: %.1f ms" % (t, ms))
