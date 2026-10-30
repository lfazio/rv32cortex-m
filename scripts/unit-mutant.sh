#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# unit-mutant.sh - break the emulator on purpose and ask the unit tests
# to notice: "confirm a fix by reverting it and seeing the failure name
# the mechanism" (CLAUDE.md), as a script.
#
#   scripts/unit-mutant.sh BUILD FILE OLD NEW
#
# Replaces the one occurrence of the string OLD in FILE with NEW, builds
# emu-unit in BUILD, runs it, prints a verdict with the first failures,
# and puts FILE back. BUILD must be a configured build directory of the
# tree FILE is in.
#
#   found: N failures            the mutant broke N checks; the first
#                                few follow, file:line and expression
#   not found                    every check passed against the mutant
#   the build failed             nothing was measured
#
# Run nothing else that builds from this tree meanwhile: the mutation is
# in the sources. A git worktree is the way to keep working beside it.
#
# What CLAUDE.md asks of a mutation driver: the backup is taken in the
# run, the edit must match exactly once, the binary must have been
# relinked after the edit, the restored file gets a new mtime so the next
# build sees it, and the restore is compared with the backup. The
# baseline -- the unmutated build passing -- is the caller's to have
# shown first.

set -eu

[ $# -eq 4 ] || {
    echo "usage: unit-mutant.sh BUILD FILE OLD NEW" >&2
    exit 2
}
B=$1 FILE=$2 OLD=$3 NEW=$4
BIN=$B/tests/unit/emu-unit
LOG=$B.unit-mutant.log

backup=$(mktemp)
cp "$FILE" "$backup"
restored=0
restore() {
    [ "$restored" -eq 0 ] || return 0
    restored=1
    cp "$backup" "$FILE"
    touch "$FILE"
    if ! cmp -s "$backup" "$FILE"; then
        echo "unit-mutant: RESTORING $FILE FAILED; the original is $backup" >&2
        exit 1
    fi
    rm -f "$backup"
}
trap restore EXIT INT TERM

python3 - "$FILE" "$OLD" "$NEW" <<'PY'
import sys

path, old, new = sys.argv[1:4]
text = open(path).read()
n = text.count(old)
if n != 1:
    sys.exit(f"unit-mutant: {n} matches for the mutation in {path}, want 1")
open(path, "w").write(text.replace(old, new))
PY

start=$(date +%s)
sleep 1 # so a relinked binary's mtime is past `start`
if ! cmake --build "$B" --target emu-unit -j"$(nproc 2>/dev/null || echo 4)" \
        >"$LOG" 2>&1; then
    echo "the build failed; nothing was measured; $LOG"
    exit 1
fi
if [ ! -f "$BIN" ] || [ "$(stat -c %Y "$BIN")" -lt "$start" ]; then
    echo "unit-mutant: $BIN was not relinked; see $LOG" >&2
    exit 1
fi

"$BIN" >"$LOG.run" 2>&1 || true
summary=$(grep -E '^[0-9]+ checks, [0-9]+ failures' "$LOG.run" | tail -n 1 || true)
if [ -z "$summary" ]; then
    echo "found: the run did not finish (crashed?); $LOG.run"
    tail -n 5 "$LOG.run"
    exit 0
fi
n=$(echo "$summary" | sed 's/.* \([0-9]*\) failures/\1/')
if [ "$n" -eq 0 ]; then
    echo "not found ($summary)"
else
    echo "found: $n failures ($summary)"
    grep -E '^ *(/|[a-z]).*:[0-9]+: ' "$LOG.run" | sort | uniq -c | sort -rn |
        head -n 4 | sed 's/^/    /'
    grep -E '^  [a-z]' "$LOG.run" | head -n 3 | sed 's/^/    /' || true
fi
