#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# board-measure.sh - build one firmware configuration, run it on the
# board, and print one line of figures.
#
#   scripts/board-measure.sh NAME SECONDS [-DOPTION=VALUE ...]
#
# Configures build/board/NAME from BOARD_SRC (default: this tree) for
# BOARD_PLATFORM (default stm32f746), with -DEMU_NET=OFF so the console
# is a console, -DEMU_MAX_INSN=0 so the guest runs to its end, and the
# options given. Then builds it, flashes and runs it with board-run.py,
# keeps the capture in build/board/NAME.txt, and prints:
#
#   NAME  ratio  wall  retired  xlat  code  compact  crc  valid  ok  ram | cache
#
# `ratio` is the firmware's host cycles per guest instruction and `wall`
# the host's clock on the same run; believe the first when the second
# agrees with it. `valid` counts CoreMark's own "Correct operation
# validated", which it withholds from a run under ten seconds, and `ok`
# counts PASS and crypto's ok lines. The cache variables that decide a
# JIT figure are printed last, from the build that made it.
#
# **An A/B is two source trees, not two build directories.** A board
# build rebuilds before it flashes, so a "before" directory configured
# from the working tree silently becomes the "after" the moment the tree
# changes (CLAUDE.md). Give the baseline its own worktree:
#
#   git worktree add --detach ../wt-base <commit>
#   BOARD_SRC=../wt-base scripts/board-measure.sh old-dhry 90 -DRV32_GUEST=dhrystone
#   scripts/board-measure.sh new-dhry 90 -DRV32_GUEST=dhrystone
#
# Environment:
#   BOARD_SRC        source tree to build (default: this tree)
#   BOARD_PLATFORM   EMU_PLATFORM (default stm32f746)
#   BOARD_OUT        where build directories go (default build/board)
#   EMU_PROBE        the probe, required with more than one attached
#
# A configuration is configured afresh each time: a cache kept from an
# earlier run keeps whatever was set in it.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
NAME=${1:?usage: board-measure.sh NAME SECONDS [-D...]}
SECS=${2:?usage: board-measure.sh NAME SECONDS [-D...]}
shift 2
SRC=$(CDPATH='' cd -- "${BOARD_SRC:-$ROOT}" && pwd)
PLATFORM=${BOARD_PLATFORM:-stm32f746}
OUT=${BOARD_OUT:-$ROOT/build/board}
D=$OUT/$NAME

mkdir -p "$OUT"
rm -f "$D/CMakeCache.txt"
if ! cmake -S "$SRC" -B "$D" -DEMU_PLATFORM="$PLATFORM" \
        -DCMAKE_TOOLCHAIN_FILE="$SRC/cmake/arm-none-eabi.cmake" \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo -DEMU_NET=OFF -DEMU_MAX_INSN=0 \
        ${EMU_PROBE:+-DEMU_PROBE="$EMU_PROBE"} "$@" >"$D.cfg.log" 2>&1; then
    echo "$NAME CONFIGURE FAILED (see $D.cfg.log)"
    exit 1
fi
if ! cmake --build "$D" -j"$(nproc 2>/dev/null || echo 4)" >"$D.build.log" 2>&1; then
    echo "$NAME BUILD FAILED (see $D.build.log)"
    tail -n 5 "$D.build.log"
    exit 1
fi

python3 "$ROOT/scripts/board-run.py" "$D" "$SECS" >"$D.txt" 2>&1 || true
T=$D.txt

# The first number after a label, or nothing.
field() { grep -a -m1 "$1" "$T" | awk -v n="$2" '{print $n}'; }

printf '%-20s ratio %-8s wall %-7s retired %-11s xlat %-7s code %-12s compact %-5s crc %-7s valid %s ok %s ram %s | %s\n' \
    "$NAME" "$(field 'ratio ' 2)" "$(field '^WALL' 2)" "$(field 'retired ' 2)" \
    "$(field 'blks/xlat' 2)" "$(field '  code ' 2)" "$(field '  compact ' 2)" \
    "$(grep -a -m1 crcfinal "$T" | awk '{print $NF}')" \
    "$(grep -a -c 'Correct operation validated' "$T" || true)" \
    "$(grep -a -c -E '^PASS|  ok  ' "$T" || true)" \
    "$(field '^ram ' 2)" \
    "$(grep -E '^EMU_JIT_CODE_BYTES|^EMU_JIT:' "$D/CMakeCache.txt" | tr '\n' ' ')"
grep -a -E '^(FLASH|RESET) FAILED|^BANNERS|^WALL \?' "$T" || true
