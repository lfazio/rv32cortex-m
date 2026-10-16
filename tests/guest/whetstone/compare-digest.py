#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""Run Whetstone's digest build under each frontend and backend, and
require every run to print the same per-module lines.

    compare-digest.py EMU_HOST IMAGE [IMAGE...]

Each image runs interpreted and with --jit. The digest is upstream's
POUT line per module -- loop counts and four results to four significant
digits -- from a build at LOOP=10 with PRINTOUT. The RV32 image computes
with the F extension and the e200z7 one with EFPU2, through two separate
FP implementations in this emulator and two builds of picolibc's libm;
agreeing is evidence that neither is grossly wrong. It is not evidence
about the last bit, which four digits cannot show: that is what
ppc-efpu-vs-exact and the ppc-efpu2 guest are for.

A run that printed no module lines would agree with another that printed
none, so every run must print exactly MODULES of them.
"""
import re
import subprocess
import sys

MODULES = 10
LINE = re.compile(r"^\d+ \d+ \d+ \S+ \S+ \S+ \S+$")


def digest(emu, image, jit):
    cmd = [emu] + (["--jit"] if jit else []) + ["--max-insn", "400000000", image]
    out = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         universal_newlines=True, errors="replace").stdout
    return [l.strip() for l in out.splitlines() if LINE.match(l.strip())]


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    emu, images = sys.argv[1], sys.argv[2:]
    runs = []
    for image in images:
        for jit in (False, True):
            name = "%s%s" % (image.rsplit("/", 1)[-1], " --jit" if jit else "")
            lines = digest(emu, image, jit)
            if len(lines) != MODULES:
                print("FAIL %s: %d module lines, want %d" % (name, len(lines), MODULES))
                return 1
            runs.append((name, lines))
    ref_name, ref = runs[0]
    bad = 0
    for name, lines in runs[1:]:
        for i, (a, b) in enumerate(zip(ref, lines)):
            if a != b:
                print("FAIL module line %d: %s says\n  %s\nand %s says\n  %s"
                      % (i + 1, ref_name, a, name, b))
                bad += 1
    if bad:
        return 1
    print("\n".join(ref))
    print("PASS %d runs agree on all %d module lines" % (len(runs), MODULES))
    return 0


if __name__ == "__main__":
    sys.exit(main())
