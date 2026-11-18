#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# ir-board-mutant.sh - break the Thumb-2 lowering on purpose and ask the
# F746's corpus replay to notice.
#
#   scripts/ir-board-mutant.sh FILE OLD NEW [CORPUS]
#
# Replaces the one occurrence of the string OLD in FILE with NEW, runs
# scripts/ir-board.sh -- fuzz-ir's corpus through the Thumb-2 lowering,
# on the board (CORPUS, by default this tree's build/fuzz/corpus/ir) --
# and puts FILE back. The lowering runs nowhere else, so this is the
# only way to ask whether the corpus reaches a line of it.
#
# Prints one verdict and the capture:
#
#   found: D of N inputs differ       the replay compared, and disagreed
#   found: a fault                    the board took a fault (IR-BOARD-FAULT)
#   not found: N inputs, none differ
#   the build failed / not rebuilt    nothing was measured
#
# A find is believed only against an unmutated replay that is clean:
# run scripts/ir-board.sh once first. The two mutants of the lazy pc it
# missed (docs/fuzzing.md) needed inputs written for them -- a block
# shape the corpus, grown on x86-64, never had -- and a "not found" asks
# for the same.
#
# The mutation is in this tree's sources for the length of the run, ten
# minutes or so: run it from a git worktree, and build nothing else from
# that tree meanwhile. As fuzz-mutant.sh does, it takes the backup in the
# run, requires one match, gives the restored file a new mtime, compares
# the restore with the backup, and believes a run only if the firmware
# was relinked after the edit.
#
# **It restores before it prints.** Its first version printed the verdict
# and left the restore to an EXIT trap; called as `... | head -1`, it was
# killed by SIGPIPE at its second line, an EXIT trap does not run on a
# death by signal, and nine mutants stacked in one file -- each verdict
# the sum of all before it, two pairs identical.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
[ $# -ge 3 ] || {
    echo "usage: ir-board-mutant.sh FILE OLD NEW [CORPUS]" >&2
    exit 2
}
FILE=$1 OLD=$2 NEW=$3
CORPUS=${4:-$ROOT/build/fuzz/corpus/ir}
OUT=$ROOT/build/ir-board
LOG=$ROOT/build/ir-board-mutant.log
ELF=$OUT/ir-board.elf

backup=$(mktemp)
cp "$FILE" "$backup"
restored=0
restore() {
    [ "$restored" -eq 0 ] || return 0
    restored=1
    cp "$backup" "$FILE"
    touch "$FILE"
    if ! cmp -s "$backup" "$FILE"; then
        echo "ir-board-mutant: RESTORING $FILE FAILED; the original is $backup" >&2
        exit 1
    fi
    rm -f "$backup"
}
trap restore EXIT INT TERM HUP PIPE

python3 - "$FILE" "$OLD" "$NEW" <<'PY'
import sys

path, old, new = sys.argv[1:4]
text = open(path).read()
n = text.count(old)
if n != 1:
    sys.exit(f"ir-board-mutant: {n} matches for the mutation in {path}, want 1")
open(path, "w").write(text.replace(old, new))
PY

mkdir -p "$ROOT/build"
start=$(date +%s)
status=0
"$ROOT/scripts/ir-board.sh" "$CORPUS" >"$LOG" 2>&1 || status=$?

if grep -q "the build failed" "$LOG"; then
    restore
    echo "the build failed; nothing was measured; $LOG"
    exit 1
fi
if [ ! -f "$ELF" ] || [ "$(stat -c %Y "$ELF")" -lt "$start" ]; then
    echo "ir-board-mutant: $ELF was not rebuilt; see $LOG" >&2
    exit 1
fi
restore

python3 - "$LOG" "$status" <<'PY'
import re, sys

text = open(sys.argv[1], errors="replace").read()
rows = [(int(n), int(d)) for n, d in re.findall(r"IR-BOARD inputs (\d+) differ (\d+)", text)]
n = sum(r[0] for r in rows)
d = sum(r[1] for r in rows)
if "IR-BOARD-FAULT" in text:
    print("found: a fault (%d inputs before it, %d differ)" % (n, d))
elif d:
    print("found: %d of %d inputs differ" % (d, n))
elif rows and sys.argv[2] == "0":
    print("not found: %d inputs, none differ" % n)
else:
    print("no verdict: the replay did not finish (exit %s)" % sys.argv[2])
PY
echo "capture: $LOG"
