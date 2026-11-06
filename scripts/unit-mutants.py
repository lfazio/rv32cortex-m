#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
"""unit-mutants.py - run scripts/unit-mutant.sh over a list of mutants.

    scripts/unit-mutants.py BUILD LIST.py [LABEL-SUBSTRING]

LIST.py defines M, a list of (label, file, old, new): each a fix
reverted or a rule broken, `old` the smallest string that occurs once in
`file` (relative to the tree BUILD was configured from). Each is applied,
built, tested and put back by unit-mutant.sh, one at a time; the summary
names every mutant the unit tests did *not* notice, and the build
failures, which measured nothing. Exit status 1 if any went unnoticed.

The baseline -- the unmutated build passing -- is the caller's to have
shown first, and nothing else may build from the tree meanwhile.
"""

import os
import runpy
import subprocess
import sys


def main():
    if len(sys.argv) not in (3, 4):
        sys.exit(__doc__)
    build, listfile = sys.argv[1], sys.argv[2]
    only = sys.argv[3] if len(sys.argv) == 4 else None
    mutants = runpy.run_path(listfile)["M"]
    here = os.path.dirname(os.path.abspath(__file__))
    tree = os.path.dirname(here)
    unnoticed, failed = [], []
    for label, path, old, new in mutants:
        if only is not None and only not in label:
            continue
        r = subprocess.run(
            [os.path.join(here, "unit-mutant.sh"), build,
             os.path.join(tree, path), old, new],
            capture_output=True, text=True)
        verdict = (r.stdout.strip().splitlines() or ["(no output)"])[0]
        print("%-44s %s" % (label, verdict), flush=True)
        if "not found" in verdict:
            unnoticed.append(label)
        elif "found" not in verdict:
            failed.append((label, (r.stdout + r.stderr).strip()))
    print()
    print("%d mutants, %d not found, %d not measured" %
          (len(mutants) if only is None else
           sum(1 for m in mutants if only in m[0]),
           len(unnoticed), len(failed)))
    for label in unnoticed:
        print("  NOT FOUND: %s" % label)
    for label, out in failed:
        print("  NOT MEASURED: %s\n    %s" % (label, out.replace("\n", "\n    ")))
    sys.exit(1 if unnoticed or failed else 0)


if __name__ == "__main__":
    main()
