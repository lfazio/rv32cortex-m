#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""smp_want.py - the line tests/guest/g4mh/smp.c must print, from Python.

The counts are what the source asks for; the sums are the xorshift loop,
recomputed here so the expectation is not the emulator's own output.
"""
N_LOCK, N_LLSC, N_BITS, N_WORK, NPE = 20000, 20000, 200001, 1000000, 3
M = 0xFFFFFFFF


def work(x, n):
    s = 0
    for _ in range(n):
        x ^= (x << 13) & M
        x ^= x >> 17
        x ^= (x << 5) & M
        s = (s + x) & M
    return s


sums = ",".join("%08x" % work((0x9E3779B9 * (pe + 1)) & M, N_WORK)
                for pe in range(NPE))
bits = (1 << NPE) - 1 if N_BITS % 2 else 0
print("g4mh smp: lock=%d llsc=%d bits=%d,%d sum=%s"
      % (NPE * N_LOCK, NPE * N_LLSC, bits, bits, sums))
