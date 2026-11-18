#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""board-jit-dump.py - what the hot translated code is, on a board.

    scripts/board-jit-dump.py CAPTURE [--guest ELF] [--top N]
                              [--host-objdump CMD] [--guest-objdump CMD]

A firmware built with -DEMU_JIT_DUMP=ON prints every resident block
after the run (`jitdump`, then `block GUESTPC HOSTADDR LEN INSNS` and the
block's bytes in hex, then `jitdump-end`); one built with
-DEMU_PC_PROFILE=<samples> also prints the pcs the tick interrupted
(`pcprof N` ... `pcprof-end`). Given the console capture of such a run
-- board-measure.sh's build/board/NAME.txt -- this puts each sample in
the block whose code holds it, ranks the blocks, and prints the hottest:
the guest's instructions (from --guest, the guest's ELF) beside the
host's, each host instruction with the samples taken at it. A sample is
the pc the tick interrupted, so it marks the instruction about to run.

A PC profile alone says that 88% of a run is in translated code, which
has no symbols; this says which block, which guest loop it is, and
where in its Thumb-2 the time goes. Blocks are where they were at the
end of the run: a compaction moves them, so a run that compacted
(`compact` in the stats) attributes its earlier samples to whatever is
there now -- the script says so.
"""

import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile


def parse(capture):
    pcs, blocks = [], []
    mode = None
    cur = None
    compact = None
    for raw in open(capture, errors="replace"):
        line = raw.strip()
        m = re.match(r"compact\s+(\d+)", line)
        if m:
            compact = int(m.group(1))
        if line.startswith("pcprof-end") or line.startswith("jitdump-end"):
            mode = None
            cur = None
            continue
        if line.startswith("pcprof "):
            mode = "prof"
            continue
        if line == "jitdump":
            mode = "jit"
            continue
        if mode == "prof" and re.fullmatch(r"[0-9a-fA-F]{8}", line):
            pcs.append(int(line, 16) & ~1)
        elif mode == "jit":
            m = re.match(r"block ([0-9a-f]{8}) ([0-9a-f]{8}) (\d+) (\d+)$", line)
            if m:
                cur = {"gpc": int(m.group(1), 16), "addr": int(m.group(2), 16),
                       "len": int(m.group(3)), "insns": int(m.group(4)),
                       "bytes": bytearray()}
                blocks.append(cur)
            elif cur is not None and re.fullmatch(r"[0-9a-f]+", line):
                cur["bytes"] += bytes.fromhex(line)
    return pcs, blocks, compact


def host_disasm(objdump, blk):
    with tempfile.NamedTemporaryFile(suffix=".bin", delete=False) as f:
        f.write(blk["bytes"])
        path = f.name
    try:
        out = subprocess.run(
            [objdump, "-D", "-b", "binary", "-marm", "-Mforce-thumb",
             "--adjust-vma=0x%x" % blk["addr"], path],
            stdout=subprocess.PIPE, universal_newlines=True, check=True).stdout
    finally:
        os.unlink(path)
    rows = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+((?:[0-9a-f]{4}\s?)+)\s+(.*)$", line)
        if m:
            rows.append((int(m.group(1), 16), m.group(3).strip()))
    return rows


def guest_disasm(objdump, elf, pc, insns):
    out = subprocess.run(
        [objdump, "-d", "--start-address=0x%x" % pc,
         "--stop-address=0x%x" % (pc + 4 * insns + 4), elf],
        stdout=subprocess.PIPE, universal_newlines=True).stdout
    rows = []
    for line in out.splitlines():
        m = re.match(r"\s*([0-9a-f]+):\s+[0-9a-f]+\s+(.*)$", line)
        if m:
            rows.append("%s  %s" % (m.group(1), m.group(2).strip()))
    return rows[:insns]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--guest", default="", help="the guest's ELF")
    ap.add_argument("--top", type=int, default=4)
    ap.add_argument("--host-objdump", default="arm-none-eabi-objdump")
    ap.add_argument("--guest-objdump", default="riscv64-unknown-elf-objdump")
    a = ap.parse_args()

    pcs, blocks, compact = parse(a.capture)
    if not blocks:
        sys.exit("no jitdump in %s (-DEMU_JIT_DUMP=ON)" % a.capture)
    if compact:
        print("note: the run compacted %d times; samples taken before the "
              "last are put in whatever is at their address now" % compact)
    hits = collections.Counter()
    at = collections.Counter(pcs)
    inside = 0
    for b in blocks:
        n = sum(k for pc, k in at.items() if b["addr"] <= pc < b["addr"] + b["len"])
        hits[id(b)] = n
        inside += n
    total = len(pcs)
    code = sum(b["len"] for b in blocks)
    guest = sum(b["insns"] for b in blocks)
    print("%d blocks, %d bytes for %d guest instructions (%.1f each)" %
          (len(blocks), code, guest, code / guest if guest else 0.0))
    if total:
        print("%d samples, %.1f%% in translated code" % (total, 100.0 * inside / total))
    ranked = sorted(blocks, key=lambda b: -hits[id(b)])
    for b in ranked[:a.top]:
        n = hits[id(b)]
        print("\n== block at guest 0x%08x, %d instructions, %d bytes at 0x%08x: "
              "%d samples (%.1f%%)" % (b["gpc"], b["insns"], b["len"], b["addr"],
                                       n, 100.0 * n / total if total else 0.0))
        if a.guest:
            for row in guest_disasm(a.guest_objdump, a.guest, b["gpc"], b["insns"]):
                print("   guest  " + row)
        for addr, text in host_disasm(a.host_objdump, b):
            k = at.get(addr, 0)
            print("  %5s  %08x  %s" % (str(k) if k else "", addr, text))


if __name__ == "__main__":
    main()
