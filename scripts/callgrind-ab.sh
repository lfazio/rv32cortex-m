#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# callgrind-ab.sh - host instructions of two runners, guest by guest.
#
#   scripts/callgrind-ab.sh OLD_RUNNER NEW_RUNNER [ROW ...]
#
# Runs each ROW under valgrind --tool=callgrind with both runners and
# prints, per row:
#
#   rv-dhry-jit   old 184871585 new 136968104  -25.91%  out 44e4cae9/44e4cae9  retired 9431234/9431256
#
# **Callgrind counts instructions, so a row is exact and layout cannot
# move it** -- which is what makes one run per side enough, and lets it
# run beside a build. It cannot see what a Cortex-M7 charges for a
# loop's shape, so an interpreter change is measured on the board too
# (CLAUDE.md).
#
# `out` hashes what the guest printed, less the lines that report its
# own timing, and `retired` is the instruction count: the two sides
# must agree on both, or the row is measuring two different runs. A
# guest that reads its clock may retire a few instructions more or less.
#
# The runners are binaries, not build directories. Build the old one
# from a worktree at the commit before (git worktree add --detach), not
# from a second build directory of this tree: that one becomes the new
# code the moment it is rebuilt (CLAUDE.md).
#
# Rows are LABEL names from the table below; with none, all of them.
# g4mh-coremark.bin is not built by the tree -- it needs CC-RH: build it
# with scripts/g4mh-build-coremark.sh and copy it into GUESTS, or its
# rows say the image is missing.
# Guest images come from GUESTS (default build/figures/guest, which
# scripts/report-figures.sh builds at the defaults); profiles and logs go
# to OUT (default build/callgrind-ab).

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
OLD=${1:?usage: callgrind-ab.sh OLD_RUNNER NEW_RUNNER [ROW ...]}
NEW=${2:?usage: callgrind-ab.sh OLD_RUNNER NEW_RUNNER [ROW ...]}
shift 2
G=${GUESTS:-$ROOT/build/figures/guest}
OUT=${OUT:-$ROOT/build/callgrind-ab}
mkdir -p "$OUT"
command -v valgrind >/dev/null || { echo "callgrind-ab: needs valgrind" >&2; exit 2; }

# LABEL|runner arguments, relative to $G
ROWS='rv-dhry-interp|dhrystone.bin
rv-dhry-jit|--jit dhrystone.bin
rv-whet-interp|whetstone.bin
rv-whet-jit|--jit whetstone.bin
rv-cm-interp|coremark.bin
rv-cm-jit|--jit coremark.bin
rv-bench-jit|--jit bench.bin
rv-isatest-jit|--jit isatest.bin
a7-dhry-interp|--frontend armv7m armv7m-dhrystone.bin
a7-dhry-jit|--frontend armv7m --jit armv7m-dhrystone.bin
a7-cm-interp|--frontend armv7m armv7m-coremark.bin
a7-cm-jit|--frontend armv7m --jit armv7m-coremark.bin
ppc-dhry-jit|--jit ppc-dhrystone.elf
ppc-cm-jit|--jit ppc-coremark.elf
g4-cm-interp|--frontend g4mh --load 0x80000000 g4mh-coremark.bin
g4-cm-jit|--frontend g4mh --load 0x80000000 --jit g4mh-coremark.bin'

total() { # PREFIX RUNNER ARGS...: host instructions of one run
    p=$1 b=$2
    shift 2
    # stdin from /dev/null: the runner reads it as the guest's console,
    # and here it is the row list -- the first run swallowed the rest.
    valgrind --tool=callgrind --callgrind-out-file="$p.cg" "$b" "$@" \
        </dev/null >"$p.log" 2>&1 || true
    callgrind_annotate "$p.cg" 2>/dev/null | grep 'PROGRAM TOTALS' |
        awk '{gsub(",", "", $1); print $1}'
}

guestout() { # LOG: a hash of the guest's own lines, less its timing
    sed -n '/^backend /,/^-- done --/p' "$1" |
        grep -v -E 'backend|Microseconds|Dhrystones per|MIPS|Whetstones|Iterations/Sec|Total time|Total ticks|Duration' |
        md5sum | cut -c1-8
}

# A row asked for and not in the table is a typo, and silence would read
# as "nothing to report".
for r in "$@"; do
    echo "$ROWS" | grep -q "^$r|" || { echo "callgrind-ab: no row '$r'" >&2; exit 2; }
done

echo "$ROWS" | while IFS='|' read -r label args; do
    if [ $# -gt 0 ]; then
        want=0
        for r in "$@"; do [ "$r" = "$label" ] && want=1; done
        [ $want = 1 ] || continue
    fi
    # The last word of a row is its image; the rest are runner options.
    img=${args##* }
    opts=${args% *}
    [ "$opts" = "$args" ] && opts=""
    if [ ! -f "$G/$img" ]; then
        printf '%-18s (no %s in %s)\n' "$label" "$img" "$G"
        continue
    fi
    # shellcheck disable=SC2086 # opts is a word list on purpose
    o=$(total "$OUT/$label.old" "$OLD" $opts "$G/$img")
    # shellcheck disable=SC2086
    n=$(total "$OUT/$label.new" "$NEW" $opts "$G/$img")
    ro=$(grep -a -m1 '^  retired' "$OUT/$label.old.log" | awk '{print $2}')
    rn=$(grep -a -m1 '^  retired' "$OUT/$label.new.log" | awk '{print $2}')
    awk -v l="$label" -v o="$o" -v n="$n" \
        -v go="$(guestout "$OUT/$label.old.log")" -v gn="$(guestout "$OUT/$label.new.log")" \
        -v ro="$ro" -v rn="$rn" 'BEGIN {
            if (o == "" || n == "" || o == 0) { printf "%-18s (a run failed: see the logs)\n", l; exit }
            printf "%-18s old %13d new %13d  %+7.2f%%  out %s/%s  retired %s/%s\n",
                   l, o, n, 100 * (n - o) / o, go, gn, ro, rn }'
done
