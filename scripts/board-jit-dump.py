#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""board-jit-dump.py - what the hot translated code is, on a board.

    scripts/board-jit-dump.py CAPTURE [--guest ELF] [--top N] [--classes]
                              [--host-objdump CMD] [--guest-objdump CMD]

A firmware built with -DEMU_JIT_DUMP=ON prints every resident block
(`jitdump SAMPLES`, then `block GUESTPC HOSTADDR LEN INSNS` and the
block's bytes in hex, then `jitdump-end`) just before every compaction
and flush and after the run, SAMPLES the profile's count then; one built with
-DEMU_PC_PROFILE=<samples> also prints the pcs the tick interrupted
(`pcprof N` ... `pcprof-end`). Given the console capture of such a run
-- board-measure.sh's build/board/NAME.txt -- this puts each sample in
the block whose code holds it, ranks the blocks, and prints the hottest:
the guest's instructions (from --guest, the guest's ELF) beside the
host's, each host instruction with the samples taken at it. A sample is
the pc the tick interrupted, so it marks the instruction about to run.

A PC profile alone says that 88% of a run is in translated code, which
has no symbols; this says which block, which guest loop it is, and
where in its Thumb-2 the time goes. A sample goes to the first snapshot
taken after it, which still held the code it was taken in; a block is
counted by its guest pc across snapshots, and shown as the snapshot
that took most of its samples has it.

--classes sorts every sample in translated code by what the host
instruction it marks is doing, by the Thumb-2 backend's conventions (r4
the cpu, r5 the retired count, r11 the RAM window's address, sp the frame):
guest register loads and stores, frame traffic, the RAM window's guard,
the access itself, the retired count, constants, compares and branches,
calls, and the rest. A sample marks the instruction about to run, so a
stall is charged to the one after it; the split is a guide to where to
look, not a measurement of any one instruction.
"""

import argparse
import collections
import os
import re
import subprocess
import sys
import tempfile


def parse(capture):
    pcs, snaps = [], []
    mode = None
    cur = None
    for raw in open(capture, errors="replace"):
        line = raw.strip()
        if line.startswith("pcprof-end") or line.startswith("jitdump-end"):
            mode = None
            cur = None
            continue
        if line.startswith("pcprof "):
            mode = "prof"
            continue
        m = re.fullmatch(r"jitdump(?: (\d+))?", line)
        if m:
            mode = "jit"
            snaps.append({"at": int(m.group(1)) if m.group(1) else 1 << 62,
                          "blocks": []})
            continue
        if mode == "prof" and re.fullmatch(r"[0-9a-fA-F]{8}", line):
            pcs.append(int(line, 16) & ~1)
        elif mode == "jit":
            m = re.match(r"block ([0-9a-f]{8}) ([0-9a-f]{8}) (\d+) (\d+)$", line)
            if m:
                cur = {"gpc": int(m.group(1), 16), "addr": int(m.group(2), 16),
                       "len": int(m.group(3)), "insns": int(m.group(4)),
                       "bytes": bytearray()}
                snaps[-1]["blocks"].append(cur)
            elif cur is not None and re.fullmatch(r"[0-9a-f]+", line):
                cur["bytes"] += bytes.fromhex(line)
    return pcs, snaps


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


CLASSES = [
    # (name, regex on objdump's text), first match wins
    ("cpu state load", r"^ldr(?:\.w)?\s+\w+, \[r4, #\d+\]"),
    ("cpu state load (byte/half)", r"^ldr[bh](?:\.w)?\s+\w+, \[r4"),
    ("cpu state store", r"^str(?:[bh])?(?:\.w)?\s+\w+, \[r4"),
    ("frame load/store", r"^(?:ldr|str)[bhd]?(?:\.w)?\s+.*\[sp"),
    ("frame push/pop/adjust", r"^(?:push|pop|stmdb|ldmia)(?:\.w)?\s+.*sp|^(?:add|sub)(?:\.w)?\s+sp, #"),
    ("ram guard", r"ror #2$|^movw\s+ip, #|^cmp(?:\.w)?\s+\w+, ip$|^ldr(?:\.w)?\s+ip, \[pc"),
    # through r11 (objdump's fp) since the window's address lives there,
    # through ip before
    ("guest access", r"^(?:ldr|str)[bhs]*(?:\.w)?\s+\w+, \[(?:ip|fp), \w+"),
    ("other memory", r"^(?:ldr|str|ldm|stm|vldr|vstr)"),
    ("retired count", r"^adds?(?:\.w)?\s+r5, r5"),
    ("call", r"^blx?\b"),
    ("compare", r"^(?:cmp|cmn|tst|teq)(?:\.w)?\s"),
    ("branch", r"^(?:b|b[a-z]{2}|cbn?z)(?:\.[nw])?\s"),
    ("constant", r"^(?:movw|movt|mov\.w|movs|mov)\s+\w+, #"),
    ("register move", r"^movs?(?:\.w)?\s+\w+, \w+$"),
    ("vfp/mve", r"^v"),
    ("alu", r"."),
]


def classes(snaps, at, objdump):
    """Samples per kind of host instruction, over every snapshot."""
    pats = [(n, re.compile(rx)) for n, rx in CLASSES]
    count = collections.Counter()
    loops = collections.Counter()  # the same, in blocks that branch back into themselves
    seen = {}
    back = re.compile(r"^b(?:[a-z]{2})?(?:\.[nw])?\s+0x([0-9a-f]+)$")
    for si, sn in enumerate(snaps):
        for b in sn["blocks"]:
            if not any(at.get((si, a), 0) for a in range(b["addr"], b["addr"] + b["len"], 2)):
                continue
            key = (si, b["addr"])
            if key not in seen:
                seen[key] = host_disasm(objdump, b)
            # a back edge is the loop cap's test and a branch back into
            # the block (a slow path's return jumps back too, unguarded)
            looping = False
            prev = ""
            for addr, text in seen[key]:
                m = back.match(text)
                if (m and prev.startswith("cmp") and "r5, #" in prev and
                        b["addr"] <= int(m.group(1), 16) < addr):
                    looping = True
                    break
                prev = text
            for addr, text in seen[key]:
                k = at.get((si, addr), 0)
                if not k:
                    continue
                for name, rx in pats:
                    if rx.search(text):
                        count[name] += k
                        if looping:
                            loops[name] += k
                        break
    total = sum(count.values())
    ltotal = sum(loops.values())
    print("\nby kind of host instruction, %d samples in translated code, %d (%.1f%%) in "
          "blocks that loop on themselves:" % (total, ltotal,
                                               100.0 * ltotal / total if total else 0.0))
    print("     all   samples   looping")
    for name, _ in CLASSES:
        if count[name]:
            print("  %6.1f%%  %5d  %8d  %s" % (100.0 * count[name] / total, count[name],
                                               loops[name], name))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("capture")
    ap.add_argument("--guest", default="", help="the guest's ELF")
    ap.add_argument("--top", type=int, default=4)
    ap.add_argument("--classes", action="store_true",
                    help="samples by kind of host instruction, over all blocks")
    ap.add_argument("--host-objdump", default="arm-none-eabi-objdump")
    ap.add_argument("--guest-objdump", default="riscv64-unknown-elf-objdump")
    a = ap.parse_args()

    pcs, snaps = parse(a.capture)
    if not snaps:
        sys.exit("no jitdump in %s (-DEMU_JIT_DUMP=ON)" % a.capture)
    snaps.sort(key=lambda sn: sn["at"])
    # each sample to the first snapshot taken after it
    hits = collections.Counter()      # (snapshot, block index) -> samples
    at = collections.Counter()        # (snapshot, host pc) -> samples
    inside = 0
    for i, pc in enumerate(pcs):
        for si, sn in enumerate(snaps):
            if sn["at"] > i:
                break
        else:
            continue
        for bi, b in enumerate(snaps[si]["blocks"]):
            if b["addr"] <= pc < b["addr"] + b["len"]:
                hits[(si, bi)] += 1
                at[(si, pc)] += 1
                inside += 1
                break
    total = len(pcs)
    last = snaps[-1]["blocks"]
    code = sum(b["len"] for b in last)
    guest = sum(b["insns"] for b in last)
    print("%d snapshots; at the end %d blocks, %d bytes for %d guest instructions "
          "(%.1f each)" % (len(snaps), len(last), code, guest,
                           code / guest if guest else 0.0))
    if total:
        print("%d samples, %.1f%% in translated code" % (total, 100.0 * inside / total))
    # by guest pc across snapshots, shown as the snapshot with most of them
    by_gpc = collections.defaultdict(lambda: [0, None, -1])
    for (si, bi), n in hits.items():
        b = snaps[si]["blocks"][bi]
        e = by_gpc[b["gpc"]]
        e[0] += n
        if n > e[2]:
            e[1], e[2] = (si, b), n
    ranked = sorted(by_gpc.items(), key=lambda kv: -kv[1][0])
    if a.classes:
        classes(snaps, at, a.host_objdump)
    for gpc, (n, (si, b), _) in ranked[:a.top]:
        print("\n== block at guest 0x%08x, %d instructions, %d bytes at 0x%08x: "
              "%d samples (%.1f%%)" % (gpc, b["insns"], b["len"], b["addr"], n,
                                       100.0 * n / total if total else 0.0))
        if a.guest:
            for row in guest_disasm(a.guest_objdump, a.guest, gpc, b["insns"]):
                print("   guest  " + row)
        for addr, text in host_disasm(a.host_objdump, b):
            k = at.get((si, addr), 0)
            print("  %5s  %08x  %s" % (str(k) if k else "", addr, text))


if __name__ == "__main__":
    main()
