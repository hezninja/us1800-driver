#!/usr/bin/env python3
"""Decode a raw bulk-IN 0x86 capture (see PROTOCOL.md) and print per-slot high-passed RMS.
usage: analyze_capture.py file.bin"""
import sys, numpy as np
b = np.fromfile(sys.argv[1], dtype=np.uint8); w = b[0::2].astype(np.uint16)
n = len(w) // 64; f = w[:n * 64].reshape(n, 64)
def plane(blk):
    o = np.zeros((blk.shape[0], 9), dtype=np.int64)
    for k in range(24): o = (o << 1) | ((blk[:, k:k + 1] >> np.arange(9)) & 1)
    return o
ch = np.empty((n, 18), dtype=np.int64); ch[:, 0::2] = plane(f[:, 0:24]); ch[:, 1::2] = plane(f[:, 32:56])
ch = np.where(ch >= 1 << 23, ch - (1 << 24), ch).astype(float)
d = np.diff(ch, axis=0)
print("frames", n)
for i in range(18): print(f"slot {i:2d}  hp-rms {d[:, i].std():8.1f}  peak {np.abs(d[:, i]).max():8.0f}")
