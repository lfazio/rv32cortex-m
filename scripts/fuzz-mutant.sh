#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# fuzz-mutant.sh - break the emulator on purpose and ask a fuzzer to notice.
#
#   scripts/fuzz-mutant.sh SECS TARGET FILE OLD NEW [libFuzzer options...]
#
# Replaces the one occurrence of the string OLD in FILE with NEW, builds
# the fuzz target through scripts/fuzz.sh in build/fuzz-mutant
# (FUZZ_BUILD overrides), fuzzes for SECS seconds from a fresh corpus --
# so one mutant's finds cannot help the next -- and puts FILE back.
# TARGET is any of fuzz.sh's: rv32, armv7m, g4mh, g4mh-smp, ppc.
# FUZZ_NO_DEVICE_SEEDS=1 leaves the hand-written device seeds out, to ask
# whether fuzzing reaches the mutant without being shown the way, and
# FUZZ_NO_DICT=1 the dictionary of device instructions (fuzz.sh).
#
# Prints one verdict and the log:
#
#   found, by fuzzing, at input #N    an input libFuzzer made
#   found by an input it started from a seed or a saved regression
#   pre-existing: ...                 the find is not the mutant's: the
#                                     restored build fails on it too
#   not found in SECS s
#   the build failed / not rebuilt    nothing was measured
#
# **Every find is replayed on the unmutated build before it is believed.**
# The first session's "found by fuzzing" on RV32 was a difference the
# tree already had -- mstatus.FS, nothing to do with the mutant -- and
# read exactly like a success.
#
# A target counts only once a mutant of the code it is meant to reach is
# found (docs/fuzzing.md). Run nothing else that builds from this tree
# meanwhile: the mutation is in the sources, and any build of any
# directory in that window compiles it. A git worktree is the way to keep
# working beside it.
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
    echo "usage: fuzz-mutant.sh SECS TARGET FILE OLD NEW [libFuzzer options...]" >&2
    exit 2
}
SECS=$1 T=$2 FILE=$3 OLD=$4 NEW=$5
shift 5
ISA=${T%-smp} # the binary: g4mh-smp is fuzz-g4mh-jit
B=${FUZZ_BUILD:-$ROOT/build/fuzz-mutant}
LOG=$B.$T.log
NAME=fuzz-$ISA-jit
SET=$T-jit
if [ "$T" = ir ]; then # the IR's own target, which has no frontend
    NAME=fuzz-ir
    SET=ir
fi
BIN=$B/tests/fuzz/$NAME
ART=$B/artifacts/$T

backup=$(mktemp)
cp "$FILE" "$backup"
restored=0
restore() {
    [ "$restored" -eq 0 ] || return 0
    restored=1
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

mkdir -p "$B" # and its parent, where the log goes: a fresh checkout has no build/
rm -rf "$B/corpus/$SET" "$ART"
start=$(date +%s)
status=0
FUZZ_BUILD=$B FUZZ_SMP_BUILD=$B "$ROOT/scripts/fuzz.sh" "$SECS" "$T" "$@" \
    >"$LOG" 2>&1 || status=$?
end=$(date +%s)

if grep -q "^fuzz: .*failed" "$LOG"; then
    echo "the build failed; nothing was measured; $LOG"
    exit 1
fi
if [ ! -f "$BIN" ] || [ "$(stat -c %Y "$BIN")" -lt "$start" ]; then
    echo "fuzz-mutant: $BIN was not rebuilt; see $LOG" >&2
    exit 1
fi

found=
if grep -q "disagree\|runtime error\|deadly signal" "$LOG"; then
    found=diverged
elif grep -q "ERROR: libFuzzer: timeout" "$LOG"; then
    found=hung
fi
if [ -z "$found" ]; then
    echo "not found in $SECS s (exit $status); $LOG"
    exit 0
fi

# The input that did it, replayed on the unmutated build.
input=$(ls -t "$ART"/crash-* "$ART"/timeout-* 2>/dev/null | head -n 1 || true)
restore
if [ -z "$input" ]; then
    echo "found ($found), but no input was written to $ART; $LOG"
    exit 1
fi
if ! cmake --build "$B" --target "$NAME" >"$LOG.base" 2>&1; then
    echo "found ($found), but the unmutated build failed; $LOG.base"
    exit 1
fi
if "$BIN" -runs=0 -timeout=10 "$input" >>"$LOG.base" 2>&1; then
    if grep -q "INITED" "$LOG"; then
        n=$(grep -o '^#[0-9]*' "$LOG" | tail -n 1)
        echo "found ($found), by fuzzing, at input $n ($((end - start)) s with the build); $input"
    else
        echo "found ($found) by an input it started from ($((end - start)) s); $input"
    fi
else
    echo "pre-existing: the unmutated build fails on the same input; $input, $LOG.base"
fi
