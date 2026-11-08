#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""same-on-both-backends.py - a guest must give the same run interpreted
and translated.

    same-on-both-backends.py EMU GUEST WANT [EMU-ARGS...]

Runs GUEST with EMU-ARGS, then again with --jit, and requires the
guest's output to be identical, WANT to appear in it, and the run's
total -- `emu-result retired=`, the budget's units -- to be the same.
Prints PASS and the total, or what differed.

Why the total and not only the output. Every core's own retired count
agreed between the backends while what `run` reported did not: the
interpreters of RV32, PowerPC and G4MH charge an exception one unit, and
a block that trapped partway was charged only what it had retired --
one short for each exception a handler resumed from, on all four
frontends (emu_jit_hot_t::charged). The total is what the instruction
cap and a guest's instruction clock read.
"""
import re
import subprocess
import sys

# The runner's own lines: the banner, and the statistics after the run.
OWN = re.compile(r"^(emu: |guest |ram |backend |-- |  )")


def run(emu, guest, args):
    out = subprocess.run([emu] + args + [guest], capture_output=True,
                         text=True, errors="replace", timeout=120)
    text = out.stdout + out.stderr
    m = re.search(r"^emu-result .*retired=(\d+)", text, re.M)
    # The guest's output is what comes between the runner's banner and its
    # "-- done --": after it are statistics, and what they count differs
    # by backend by design (the JIT's, a pair-statistics build's tables).
    guest_out = []
    started = False
    for l in text.splitlines():
        if l.startswith("backend "):
            started = True
            continue
        if l.startswith("-- done --"):
            break
        if started and l.strip() and not OWN.match(l):
            guest_out.append(l)
    return text, guest_out, (m.group(1) if m else None)


def main():
    if len(sys.argv) < 4:
        print(__doc__.strip().splitlines()[2].strip())
        return 2
    emu, guest, want, args = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
    _, out_i, ret_i = run(emu, guest, args)
    text_j, out_j, ret_j = run(emu, guest, args + ["--jit"])
    if "backend jit" not in text_j:
        print("FAIL the --jit run did not use the JIT")
        return 1
    if ret_i is None or ret_j is None:
        print("FAIL no emu-result line (interpreted %s, translated %s)"
              % (ret_i, ret_j))
        return 1
    if not any(want in l for l in out_i):
        print("FAIL %r not in the guest's output" % want)
        return 1
    if out_i != out_j:
        print("FAIL the output differs")
        for a, b in zip(out_i, out_j):
            if a != b:
                print("  interpreted: %s\n  translated:  %s" % (a, b))
                break
        return 1
    if ret_i != ret_j:
        print("FAIL retired %s interpreted, %s translated" % (ret_i, ret_j))
        return 1
    print("PASS retired %s" % ret_i)
    return 0


if __name__ == "__main__":
    sys.exit(main())
