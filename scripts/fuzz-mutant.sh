#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# fuzz-mutant.sh - break the emulator on purpose and ask a fuzzer to notice.
#
#   scripts/fuzz-mutant.sh SECS ISA FILE OLD NEW [libFuzzer options...]
#
# Replaces the one occurrence of the string OLD in FILE with NEW, builds
# fuzz-ISA-jit in build/fuzz-mutant (FUZZ_BUILD overrides) through
# scripts/fuzz.sh, fuzzes for SECS seconds from the seeds alone -- a
# fresh corpus each time, so one mutant's finds cannot help the next --
# and puts FILE back. Prints `found` or `not found`, and the log.
#
# A target counts only once a mutant of the code it is meant to reach is
# found (docs/fuzzing.md). Run nothing else that builds from this tree
# meanwhile: the mutation is in the sources, and any build of any
# directory in that window compiles it.
#
# What CLAUDE.md asks of a mutation driver, which is why this is a script
# and not a loop typed into a shell: the backup is taken in the run, the
# edit must match exactly once, the restored file gets a new mtime so
# that the next build sees it, the restore is compared with the backup,
# and the binary is shown to have been relinked before its run is
# believed.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
[ $# -ge 5 ] || {
    echo "usage: fuzz-mutant.sh SECS ISA FILE OLD NEW [libFuzzer options...]" >&2
    exit 2
}
SECS=$1 ISA=$2 FILE=$3 OLD=$4 NEW=$5
shift 5
B=${FUZZ_BUILD:-$ROOT/build/fuzz-mutant}
LOG=$B.$ISA.log

backup=$(mktemp)
cp "$FILE" "$backup"
restore() {
    cp "$backup" "$FILE"
    touch "$FILE"
    if ! cmp -s "$backup" "$FILE"; then
        echo "fuzz-mutant: RESTORING $FILE FAILED; the original is $backup" >&2
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
    sys.exit(f"fuzz-mutant: {n} matches for the mutation in {path}, want 1")
open(path, "w").write(text.replace(old, new))
PY

start=$(date +%s)
rm -rf "$B/corpus/$ISA-jit"
FUZZ_BUILD=$B "$ROOT/scripts/fuzz.sh" "$SECS" "$ISA" "$@" >"$LOG" 2>&1 || true
end=$(date +%s)

bin=$B/tests/fuzz/fuzz-$ISA-jit
if [ ! -f "$bin" ] || [ "$(stat -c %Y "$bin")" -lt "$start" ]; then
    echo "fuzz-mutant: $bin was not rebuilt; see $LOG" >&2
    exit 1
fi
if grep -q "disagree\|runtime error\|deadly signal" "$LOG"; then
    # Before libFuzzer's INITED line, the find was an input it started
    # with -- a seed or a saved regression -- not one it made.
    if grep -q "INITED" "$LOG"; then
        n=$(grep -o '^#[0-9]*' "$LOG" | tail -n 1)
        echo "found, by fuzzing, at input $n ($((end - start)) s with the build); $LOG"
    else
        echo "found by an input it started from ($((end - start)) s); $LOG"
    fi
elif grep -q "ERROR: libFuzzer: timeout" "$LOG"; then
    # A hang is a finding, but not necessarily the mutant's: replay the
    # timeout-* input on the unmutated build before believing either.
    echo "an input hung; check it against the unmutated build; $LOG"
else
    echo "not found in $SECS s; $LOG"
fi
