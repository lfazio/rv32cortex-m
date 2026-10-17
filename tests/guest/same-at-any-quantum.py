#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""same-at-any-quantum.py - a guest timed by the processor clock must not
care how the run is sliced.

    same-at-any-quantum.py EMU GUEST [EMU-ARGS...]

Runs GUEST at three quanta -- 509 (prime, so its boundaries fall
everywhere), the default 4096 and 65536 -- and requires the guest's
output and the retired count to be identical. Prints PASS and the count,
or what differed.

Why a property and not a window. A device write that moved a timer's
deadline *earlier* without ending the interpreter's stretch was noticed
when the stretch ran out by itself: a quantum or a decrementer period
late. A test of "the interrupt came within N ticks" caught that only
when a slice boundary happened to fall outside the window -- ppc-intc
did at 4096 and not at 512 -- and ppc-swt's reset, the case that showed
it, cannot be seen by the guest at all. Comparing whole runs at quanta
that place their boundaries differently catches it wherever it lands.
"""
import re
import subprocess
import sys

QUANTA = ("509", "4096", "65536")


def run(emu, guest, args, q):
    out = subprocess.run([emu, "--quantum", q] + args + [guest],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         universal_newlines=True, timeout=300).stdout
    m = re.search(r"emu-result .*retired=(\d+)", out)
    # The guest's own lines, and the stop reason; not the host's timing.
    keep = [l for l in out.splitlines()
            if not re.match(r"\s*(host|ratio|speed|guest  |ram  |backend|"
                            r"core \d|-- |emu: |  )", l)
            or "system reset" in l]
    return (m.group(1) if m else None), keep


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    emu, guest, args = sys.argv[1], sys.argv[2], sys.argv[3:]
    runs = [(q,) + run(emu, guest, args, q) for q in QUANTA]
    q0, n0, l0 = runs[0]
    ok = n0 is not None
    for q, n, lines in runs[1:]:
        if n != n0:
            print("FAIL retired %s at --quantum %s, %s at %s" % (n0, q0, n, q))
            ok = False
        if lines != l0:
            diff = [(a, b) for a, b in zip(l0, lines) if a != b][:3]
            print("FAIL output differs at --quantum %s: %s" % (q, diff))
            ok = False
    if ok:
        print("PASS retired %s at quanta %s" % (n0, ", ".join(QUANTA)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
