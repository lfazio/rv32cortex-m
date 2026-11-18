#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""loop-census.py - which guest loops a vectoriser could take, and how
often they run: P0 of docs/jit/autovectorisation.md.

    scripts/loop-census.py RUNNER GUEST.elf [GUEST.elf ...] [--top N]
                           [--runner-args "..."]

RUNNER is an emu-host built with -DEMU_JIT_LOOP_CENSUS=ON, which counts,
for every translated block that branches back to its own start, its
entries, the back edges taken and the loop cap's exits, and prints a
`census` line a record at exit. For each guest this runs it under --jit
and prints:

  - the share of the run's guest instructions retired in such loops,
    and in those running at least --min-trip (8) iterations an entry;
  - the loops by that share, each with its trip count -- iterations an
    entry from outside, the cap's re-entries taken out -- what its
    optimised IR holds (loads, stores, multiplies, calls), and the
    guest's code, from the ELF.

A loop's instructions are estimated as (entries + back edges) times the
instructions an iteration retires: exact for a loop with no side exit,
an overestimate by at most one iteration an entry for one that has.
"""

import argparse
import collections
import re
import subprocess
import sys

OBJDUMP = {
    "RISC-V": "riscv64-unknown-elf-objdump",
    "ARM": "arm-none-eabi-objdump",
    "PowerPC": "powerpc64-linux-gnu-objdump",
}


def machine(elf):
    out = subprocess.run(["readelf", "-h", elf], stdout=subprocess.PIPE,
                         universal_newlines=True).stdout
    m = re.search(r"Machine:\s+(.*)", out)
    text = m.group(1) if m else ""
    for k in OBJDUMP:
        if k in text:
            return k
    return None


def guest_code(elf, pc, n):
    arch = machine(elf)
    if arch is None:
        return []
    out = subprocess.run(
        [OBJDUMP[arch], "-d", "--start-address=0x%x" % pc,
         "--stop-address=0x%x" % (pc + 4 * n + 4), elf],
        stdout=subprocess.PIPE, universal_newlines=True).stdout
    sym = re.findall(r"<([^>+]+)", out)
    rows = []
    for line in out.splitlines():
        m = re.match(r"\s*[0-9a-f]+:\s+(?:[0-9a-f]{2,8} ?)+\s+(.*)$", line)
        if m:
            rows.append(re.sub(r"\s+", " ", m.group(1).split("<")[0]).strip())
    return (sym[0] if sym else "?"), rows[:n]


def run(runner, elf, extra):
    out = subprocess.run([runner, "--jit"] + extra + [elf], stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, universal_newlines=True,
                         errors="replace").stdout
    m = re.search(r"emu-result .*retired=(\d+)", out)
    retired = int(m.group(1)) if m else 0
    loops = collections.defaultdict(lambda: collections.Counter())
    shape = {}
    for line in out.splitlines():
        if not line.startswith("census "):
            continue
        f = dict(kv.split("=") for kv in line.split()[1:])
        pc = int(f["pc"], 16)
        c = loops[pc]
        for k in ("entries", "iters", "caps"):
            c[k] += int(f[k])
        shape[pc] = {k: int(f[k]) for k in ("insns", "loads", "stores", "muls", "calls", "ops")}
        shape[pc]["shape"] = f.get("shape", "-")
    return retired, loops, shape


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("runner")
    ap.add_argument("guests", nargs="+")
    ap.add_argument("--top", type=int, default=8)
    ap.add_argument("--min-trip", type=float, default=8.0)
    ap.add_argument("--runner-args", default="")
    a = ap.parse_args()

    for elf in a.guests:
        retired, loops, shape = run(a.runner, elf, a.runner_args.split())
        if retired == 0:
            print("%s: no emu-result line -- did it run?" % elf)
            continue
        rows = []
        for pc, c in loops.items():
            s = shape[pc]
            outside = max(1, c["entries"] - c["caps"])
            trip = (c["entries"] + c["iters"]) / outside
            insns = (c["entries"] + c["iters"]) * s["insns"]
            rows.append((insns, pc, trip, c, s))
        rows.sort(reverse=True)
        in_loops = sum(r[0] for r in rows)
        long_loops = sum(r[0] for r in rows if r[2] >= a.min_trip)
        print("\n== %s: %d retired; in self-loops %.1f%%, in those of %g+ "
              "iterations an entry %.1f%%" % (elf, retired, 100.0 * in_loops / retired,
                                             a.min_trip, 100.0 * long_loops / retired))
        for insns, pc, trip, c, s in rows[:a.top]:
            sym, code = guest_code(elf, pc, s["insns"])
            print("  %5.1f%%  0x%08x %-26s trip %7.1f  %2d insns  ld %d st %d mul %d call %d  %s" %
                  (100.0 * insns / retired, pc, sym[:26], trip, s["insns"], s["loads"],
                   s["stores"], s["muls"], s["calls"], s["shape"]))
            print("            " + " ; ".join(code)[:150])


if __name__ == "__main__":
    sys.exit(main())
