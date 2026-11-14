#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# probe-mutant.sh - unit-mutant.sh for the probes: break the emulator on
# purpose and ask a probe whether it still agrees with its reference
# models (scripts/run-probe.py).
#
#   scripts/probe-mutant.sh BUILD FILE OLD NEW PROBE.S...
#
# Replaces the one occurrence of OLD in FILE with NEW, rebuilds emu-host
# in BUILD, runs each PROBE.S interpreted and then under the JIT, prints
# a verdict naming the registers that moved, and puts FILE back.
#
#   found: PROBE [jit] s3 s5     those registers now differ from the model
#   not found                    every probe still agrees
#   the build failed             nothing was measured
#
# The rules are unit-mutant.sh's: a backup taken in the run, exactly one
# match, the binary relinked after the edit, the restored file given a
# new mtime and compared with the backup, and emu-host rebuilt from it.
# Run nothing else that builds from this tree meanwhile, and show the
# unmutated probes passing first.

set -eu

[ $# -ge 5 ] || {
    echo "usage: probe-mutant.sh BUILD FILE OLD NEW PROBE.S..." >&2
    exit 2
}
B=$1 FILE=$2 OLD=$3 NEW=$4
shift 4
ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BIN=$B/emu-host
LOG=$B.probe-mutant.log

backup=$(mktemp)
cp "$FILE" "$backup"
restored=0
restore() {
    [ "$restored" -eq 0 ] || return 0
    restored=1
    cp "$backup" "$FILE"
    touch "$FILE"
    if ! cmp -s "$backup" "$FILE"; then
        echo "probe-mutant: RESTORING $FILE FAILED; the original is $backup" >&2
        exit 1
    fi
    rm -f "$backup"
    if ! cmake --build "$B" --target emu-host >"$LOG.restore" 2>&1; then
        echo "probe-mutant: rebuilding $BIN after the restore failed; $LOG.restore" >&2
    fi
}
trap restore EXIT INT TERM

python3 - "$FILE" "$OLD" "$NEW" <<'PY'
import sys

path, old, new = sys.argv[1:4]
text = open(path).read()
n = text.count(old)
if n != 1:
    sys.exit(f"probe-mutant: {n} matches for the mutation in {path}, want 1")
open(path, "w").write(text.replace(old, new))
PY

start=$(date +%s)
sleep 1 # so a relinked binary's mtime is past `start`
if ! cmake --build "$B" --target emu-host -j"$(nproc 2>/dev/null || echo 4)" \
        >"$LOG" 2>&1; then
    echo "the build failed; nothing was measured; $LOG"
    exit 1
fi
if [ ! -f "$BIN" ] || [ "$(stat -c %Y "$BIN")" -lt "$start" ]; then
    echo "probe-mutant: $BIN was not relinked; see $LOG" >&2
    exit 1
fi

verdict=""
for p in "$@"; do
    for mode in "" jit; do
        arg=""
        [ -z "$mode" ] || arg=--emu-arg=--jit
        # shellcheck disable=SC2086 # $arg is empty or one word
        out=$("$ROOT/scripts/run-probe.py" --emu "$BIN" $arg "$p" 2>&1 || true)
        regs=$(echo "$out" | awk '/DIFFERS/ {printf " %s", $1}')
        if ! echo "$out" | grep -q '^probe: PASS'; then
            [ -n "$regs" ] || regs=" ($(echo "$out" | tail -n 1))"
            verdict="$verdict
    $(basename "$p")${mode:+ $mode}$regs"
        fi
    done
done
if [ -z "$verdict" ]; then
    echo "not found"
else
    echo "found:$verdict"
fi
