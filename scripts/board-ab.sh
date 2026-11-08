#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# board-ab.sh - two source trees on the board: every frontend's CoreMark,
# both backends, at two layouts.
#
#   scripts/board-ab.sh OLD NEW [ROW...]
#
# OLD and NEW are source trees -- git worktrees, never two build
# directories of one tree (CLAUDE.md). For each row and each .text shift
# (EMU_LAYOUT_PAD, 0 and 64 by default) it builds, flashes and runs OLD
# and then NEW with board-measure.sh, so the two halves of a pair are
# minutes apart, and prints one line a pair:
#
#   ROW  pad  old  new  change%  same?
#
# `old` and `new` are the firmware's host cycles per guest instruction.
# `same?` compares what must not move: the retired count, the CRC and
# CoreMark's validations. A difference that does not follow the layout
# in both pads is not a difference (CLAUDE.md, "Measuring").
#
# Rows (all by default):
#   rv32-j rv32-i   RV32 CoreMark x120, JIT and interpreter
#   a7-j            ARMv7-M CoreMark x200 under the JIT: past its buffer
#   a7-i            ARMv7-M CoreMark x40 interpreted
#   ppc-j ppc-i     PowerPC CoreMark x40
#   g4-j g4-i       G4MH CoreMark, CC-RH's (scripts/g4mh-build-coremark.sh)
#
# Environment:
#   BOARD_AB_PADS   the .text shifts (default "0 64")
#   BOARD_AB_TAG    build directory prefix (default ab)
#   EMU_PROBE       the probe, required with more than one attached
#
# Every line board-measure.sh printed is in build/board/$TAG.log.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
OLD=$(CDPATH='' cd -- "${1:?usage: board-ab.sh OLD NEW [ROW...]}" && pwd)
NEW=$(CDPATH='' cd -- "${2:?usage: board-ab.sh OLD NEW [ROW...]}" && pwd)
shift 2
ROWS=${*:-rv32-j rv32-i a7-j a7-i ppc-j ppc-i g4-j g4-i}
PADS=${BOARD_AB_PADS:-0 64}
TAG=${BOARD_AB_TAG:-ab}
LOG=$ROOT/build/board/$TAG.log
mkdir -p "$ROOT/build/board"
: >"$LOG"

# The guests the worktrees cannot build themselves come from this tree.
PPC="-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_PPC=ON -DPPC_GUEST=coremark"
PPC="$PPC -DPPC_RUNTIME=$ROOT/build/ppc-runtime"
A7="-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_ARMV7M=ON -DARMV7M_GUEST=coremark"
G4="-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON"
G4="$G4 -DG4MH_GUEST=$ROOT/build/g4mh-coremark.bin"

opts() {
    case $1 in
    rv32-j) echo "-DRV32_GUEST=coremark -DCOREMARK_ITERATIONS=120" ;;
    rv32-i) echo "-DRV32_GUEST=coremark -DCOREMARK_ITERATIONS=120 -DEMU_JIT=OFF" ;;
    a7-j)   echo "$A7 -DARMV7M_COREMARK_ITERATIONS=200" ;;
    a7-i)   echo "$A7 -DARMV7M_COREMARK_ITERATIONS=40 -DEMU_JIT=OFF" ;;
    ppc-j)  echo "$PPC" ;;
    ppc-i)  echo "$PPC -DEMU_JIT=OFF" ;;
    g4-j)   echo "$G4" ;;
    g4-i)   echo "$G4 -DEMU_JIT=OFF" ;;
    *)      echo "board-ab: no row $1" >&2; exit 2 ;;
    esac
}

# The value after a label on a board-measure.sh line.
after() { echo "$2" | awk -v k="$1" '{for (i = 1; i < NF; i++) if ($i == k) { print $(i + 1); exit }}'; }

for row in $ROWS; do
    o=$(opts "$row")
    case $row in g4-*)
        if [ ! -f "$ROOT/build/g4mh-coremark.bin" ]; then
            echo "$row  skipped: no build/g4mh-coremark.bin"
            continue
        fi ;;
    esac
    for pad in $PADS; do
        # shellcheck disable=SC2086 # $o is a list of options
        a=$(BOARD_SRC=$OLD "$ROOT/scripts/board-measure.sh" \
            "$TAG-old-$row-p$pad" 200 $o -DEMU_LAYOUT_PAD="$pad" 2>&1 | tail -n 1)
        # shellcheck disable=SC2086
        b=$(BOARD_SRC=$NEW "$ROOT/scripts/board-measure.sh" \
            "$TAG-new-$row-p$pad" 200 $o -DEMU_LAYOUT_PAD="$pad" 2>&1 | tail -n 1)
        printf '%s\n%s\n' "$a" "$b" >>"$LOG"
        ra=$(after ratio "$a")
        rb=$(after ratio "$b")
        same=yes
        for k in retired crc valid; do
            [ "$(after $k "$a")" = "$(after $k "$b")" ] || same="no ($k)"
        done
        change=$(awk -v a="$ra" -v b="$rb" \
            'BEGIN { if (a + 0 > 0 && b + 0 > 0) printf "%+.2f%%", 100 * (b - a) / a; else print "-" }')
        printf '%-7s pad %-4s old %-8s new %-8s %-8s same %s\n' \
            "$row" "$pad" "${ra:--}" "${rb:--}" "$change" "$same"
    done
done
