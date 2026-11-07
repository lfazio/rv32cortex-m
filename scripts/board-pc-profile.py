#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""board-pc-profile.py - read a firmware's PC profile and bucket it.

    scripts/board-pc-profile.py ELF [--probe VID:PID:SERIAL] [--lines N]
                                    [--addresses N]

A firmware built with -DEMU_PC_PROFILE=<samples> (F746) keeps the pc the
SysTick interrupted, once a millisecond. After a run, this reads the
samples over the probe -- under reset, before the start-up code can
clear them -- and prints the hottest functions and source lines, by
addr2line on ELF. A cycle count says how long; this says where, which
is what a difference between two firmwares needs.

--addresses N also prints the N hottest pcs themselves: addr2line says
`??` for code no symbol covers -- the JIT's buffer, a veneer, a stub --
and a share of `??` that moves between two firmwares needs its
addresses to be told apart.

--timeline MS prints the samples in order, MS at a time (a sample is a
millisecond): the hottest functions of each window. The buffer fills
from the start of the run, so this says *when*, which a total cannot --
two firmwares a fixed tenth of a second apart whatever the run length
differ in one phase, and the totals of a long run hide it.

--regions N buckets the samples by the F746 memory they ran from: ITCM,
flash, DTCM and SRAM1 -- and the JIT's buffer (`g_static_code`) apart,
split where the DTCM ends. Translated code has no symbol, so this is
what says how much of the time in it was spent behind the I-cache. N
is how many of each region's hottest functions to name.
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


def nm_sym(elf, sym):
    """(address, size) of sym, or None."""
    out = subprocess.run(["arm-none-eabi-nm", "-S", elf],
                         stdout=subprocess.PIPE, universal_newlines=True,
                         check=True).stdout
    for line in out.splitlines():
        f = line.split()
        if len(f) == 4 and f[3] == sym:
            return int(f[0], 16), int(f[1], 16)
    return None


# RM0385, 2.2.2. Flash through the ITCM bus is 0x0020_0000.
F746_REGIONS = [
    ("ITCM", 0x00000000, 0x00004000),
    ("flash (ITCM bus)", 0x00200000, 0x00300000),
    ("flash (AXIM)", 0x08000000, 0x08100000),
    ("DTCM", 0x20000000, 0x20010000),
    ("SRAM1", 0x20010000, 0x2004C000),
    ("SRAM2", 0x2004C000, 0x20050000),
]
DTCM_END = 0x20010000


def regions(elf, pcs, where, nfn):
    total = len(pcs)
    code = nm_sym(elf, "g_static_code")
    by = collections.Counter()
    fns = collections.defaultdict(collections.Counter)
    jit = collections.Counter()
    for pc in pcs:
        if code and code[0] <= pc < code[0] + code[1]:
            jit["DTCM" if pc < DTCM_END else "SRAM1"] += 1
            continue
        name = "elsewhere"
        for r, lo, hi in F746_REGIONS:
            if lo <= pc < hi:
                name = r
                break
        by[name] += 1
        fns[name][where[pc][0]] += 1
    print("by region:")
    for name, k in by.most_common():
        print("  %6.2f%%  %s" % (100.0 * k / total, name))
        for fn, j in fns[name].most_common(nfn):
            print("  %6.2f%%    %s" % (100.0 * j / total, fn))
    if code:
        lo, size = code
        split = min(max(DTCM_END - lo, 0), size)
        n = sum(jit.values())
        print("  %6.2f%%  the JIT's buffer, 0x%08x + %d (%d in DTCM)" %
              (100.0 * n / total, lo, size, split))
        for part in ("DTCM", "SRAM1"):
            k = jit[part]
            print("  %6.2f%%    in %s (%.1f%% of the buffer's samples)" %
                  (100.0 * k / total, part, 100.0 * k / n if n else 0.0))


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
    ap.add_argument("--addresses", type=int, default=0)
    ap.add_argument("--timeline", type=int, default=0,
                    help="samples a window, printed in order")
    ap.add_argument("--regions", type=int, default=0, metavar="N",
                    help="samples by memory, the JIT's buffer split, "
                         "with each region's N hottest functions")
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
    if a.regions:
        regions(a.elf, pcs, where, a.regions)
    if a.timeline:
        print("timeline, %d samples a window:" % a.timeline)
        for at in range(0, total, a.timeline):
            win = collections.Counter(where[pc][0]
                                      for pc in pcs[at:at + a.timeline])
            n = sum(win.values())
            print("  %6d  " % at + "  ".join(
                "%s %d%%" % (name[:22], 100 * k // n)
                for name, k in win.most_common(4)))
    if a.addresses:
        print("hottest pcs:")
        for pc, k in collections.Counter(pcs).most_common(a.addresses):
            print("  %6.2f%%  0x%08x  %s" % (100.0 * k / total, pc, where[pc][0]))


if __name__ == "__main__":
    main()
