#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""board-pc-profile.py - read a firmware's PC profile and bucket it.

    scripts/board-pc-profile.py ELF [--probe VID:PID:SERIAL] [--lines N]

A firmware built with -DEMU_PC_PROFILE=<samples> (F746) keeps the pc the
SysTick interrupted, once a millisecond. After a run, this reads the
samples over the probe -- under reset, before the start-up code can
clear them -- and prints the hottest functions and source lines, by
addr2line on ELF. A cycle count says how long; this says where, which
is what a difference between two firmwares needs.
"""
import argparse
import collections
import os
import re
import subprocess
import sys


def nm_addr(elf, sym):
    out = subprocess.run(["arm-none-eabi-nm", elf], stdout=subprocess.PIPE,
                         universal_newlines=True, check=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) == 3 and f[2] == sym:
            return int(f[0], 16)
    sys.exit("no %s in %s" % (sym, elf))


def read_words(chip, probe, addr, n):
    cmd = ["probe-rs", "read", "--chip", chip, "--connect-under-reset"]
    if probe:
        cmd += ["--probe", probe]
    cmd += ["b32", "0x%x" % addr, str(n)]
    out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         universal_newlines=True).stdout
    words = []
    for line in out.splitlines():
        m = re.match(r"^\s*([0-9a-fA-F]+):\s*(.*)$", line)
        if m:
            words += [int(w, 16) for w in m.group(2).split()]
    return words[:n]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("elf")
    ap.add_argument("--probe", default=os.environ.get("EMU_PROBE", ""))
    ap.add_argument("--chip", default="STM32F746ZGTx")
    ap.add_argument("--lines", type=int, default=30)
    ap.add_argument("--functions", type=int, default=15)
    a = ap.parse_args()

    # **One read.** Each attach is under reset, and the firmware's start-up
    # zeroes .bss as soon as the probe lets go -- so a second read finds the
    # samples cleared, and the first version of this script reported 96% of
    # them as address 0. The count follows the array, so one read takes
    # both.
    base = nm_addr(a.elf, "g_pc_profile")
    count = nm_addr(a.elf, "g_pc_profile_n")
    if count <= base or (count - base) % 4 != 0:
        sys.exit("g_pc_profile_n does not follow g_pc_profile")
    words = read_words(a.chip, a.probe, base, (count - base) // 4 + 1)
    if not words:
        sys.exit("could not read the profile")
    pcs = words[:min(words[-1], len(words) - 1)]
    pcs = [pc & ~1 for pc in pcs]
    uniq = sorted(set(pcs))
    out = subprocess.run(["arm-none-eabi-addr2line", "-f", "-C", "-e", a.elf] +
                         ["0x%x" % pc for pc in uniq], stdout=subprocess.PIPE,
                         universal_newlines=True, check=True).stdout.splitlines()
    where = {pc: (out[2 * i], out[2 * i + 1].split(" ")[0].split("/")[-1])
             for i, pc in enumerate(uniq)}
    by_fn = collections.Counter(where[pc][0] for pc in pcs)
    by_line = collections.Counter("%s  %s" % where[pc] for pc in pcs)
    total = len(pcs)
    print("%d samples" % total)
    for name, k in by_fn.most_common(a.functions):
        print("  %6.2f%%  %s" % (100.0 * k / total, name))
    print("hottest lines:")
    for name, k in by_line.most_common(a.lines):
        print("  %6.2f%%  %s" % (100.0 * k / total, name))


if __name__ == "__main__":
    main()
