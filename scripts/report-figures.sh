#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# report-figures.sh - regenerate the numbers quoted in docs/.
#
# Why this exists: every figure in the docs used to be hand-copied from a
# run, and this project has already been bitten twice by that. A whole
# generation of performance numbers was measured with a 48 KB code cache
# inherited from a stale build directory while the declared default was
# 12 KB -- `rm -rf build/` moved them by 68% with no code change. And a
# comment asserting an invariant ("the two halves concatenate to the
# unsplit binary, which the build checks") was load-bearing, false, and
# checked by nothing.
#
# So this script does two things, and the second matters as much as the
# first: it runs the measurements, and it prints **the CMake cache
# variables that produced them** beside each one. A figure without its
# EMU_JIT_CODE_BYTES is not a figure.
#
# It deliberately does not touch hardware. Board numbers need a flash
# cycle and a UART; they carry the commit they were measured at instead,
# and scripts/report-board.sh is the thing that would produce them.
#
# Usage:
#   scripts/report-figures.sh [--quick] [--build DIR]
#
#   --quick   skip CoreMark, which dominates the run time
#   --build   use an existing build directory instead of configuring one

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
BUILD="$ROOT/build/figures"
QUICK=0
CONFIGURE=1

while [ $# -gt 0 ]; do
    case "$1" in
    --quick) QUICK=1; shift ;;
    --build) BUILD=$2; CONFIGURE=0; shift 2 ;;
    -h|--help) sed -n '3,30p' "$0"; exit 0 ;;
    *) echo "report-figures: unknown option: $1" >&2; exit 2 ;;
    esac
done

say() { printf '%s\n' "$*"; }
rule() { say '------------------------------------------------------------'; }

# ------------------------------------------------------------------
# Provenance. Print this first: it is what makes the rest quotable.
# ------------------------------------------------------------------
say "# Figures for rv32cortex-m"
say ""
say "generated   $(date -u '+%Y-%m-%d %H:%M UTC')"
say "commit      $(git -C "$ROOT" rev-parse --short HEAD 2>/dev/null || echo '(not a git tree)')"
if ! git -C "$ROOT" diff --quiet 2>/dev/null; then
    say "tree        DIRTY -- these figures do not correspond to a commit"
fi
say "host        $(uname -sm)"
say ""

if [ "$CONFIGURE" = 1 ]; then
    say "configuring $BUILD ..."
    cmake -B "$BUILD" -S "$ROOT" -DEMU_PLATFORM=host \
          -DEMU_FRONTEND_G4MH=ON -DCMAKE_BUILD_TYPE=Release >/dev/null
    cmake --build "$BUILD" -j"$(nproc 2>/dev/null || echo 4)" >/dev/null
fi

HOST="$BUILD/emu-host"
UNIT="$BUILD/tests/unit/emu-unit"
[ -x "$HOST" ] || { echo "report-figures: no $HOST" >&2; exit 1; }

# ------------------------------------------------------------------
# The cache variables that set every performance number below. All
# three silently outlive the tree they were set in, which is exactly
# how the 48 KB/12 KB confusion happened.
# ------------------------------------------------------------------
rule
say "## Build settings"
say ""
for v in EMU_JIT EMU_JIT_CODE_BYTES EMU_JIT_MAX_BLOCKS EMU_JIT_LOOP_CAP \
         EMU_JIT_LOOP_CHAIN EMU_JIT_INLINE_PERIPH RV_GUEST_MARCH \
         COREMARK_ITERATIONS CMAKE_BUILD_TYPE RV32_EXT_F RV32_EXT_ZCB; do
    line=$(grep -E "^$v:" "$BUILD/CMakeCache.txt" 2>/dev/null || true)
    if [ -n "$line" ]; then
        say "  ${line%%:*} = ${line#*=}"
    fi
done
say ""

# ------------------------------------------------------------------
# Correctness. These are the claims the README makes about passing.
# ------------------------------------------------------------------
rule
say "## Suites"
say ""

if [ -x "$UNIT" ]; then
    say "  unit          $("$UNIT" 2>&1 | tail -1)"
fi

if ctest --test-dir "$BUILD" -L fast >/dev/null 2>&1; then
    n=$(ctest --test-dir "$BUILD" -L fast -N 2>/dev/null |
        sed -n 's/^Total Tests: //p')
    say "  ctest -L fast pass (${n:-?} tests)"
else
    say "  ctest -L fast FAIL"
fi

say ""
say "  riscv-tests and riscv-arch-test are separate scripts, because they"
say "  build a toolchain and a golden model:"
say "      ./scripts/run-riscv-tests.sh     expect 77/77"
say "      ./scripts/run-arch-test.sh       expect 274/274"
say ""

# ------------------------------------------------------------------
# JIT coverage. The ratio, not the pass, is what says a run proved
# anything -- a backend that declines everything passes every suite.
# ------------------------------------------------------------------
rule
say "## JIT coverage (xlat / entries / interp)"
say ""
say "  Read the interpreted share, not the pass. isatest arms PMP early,"
say "  so it interprets most of its run whatever the backend can lower;"
say "  use bench or coremark to measure translation."
say ""

jit_line() {
    _name=$1; _img=$2; shift 2
    [ -f "$_img" ] || { say "  $(printf '%-12s' "$_name") (not built)"; return; }
    # Not --quiet: the stats line *is* the exit summary. Guest output
    # goes to stdout and the summary to stderr, so keep only stderr.
    _out=$("$HOST" --jit "$@" "$_img" 2>&1 >/dev/null |
           sed -n 's/^emu: jit //p' || true)
    say "  $(printf '%-12s' "$_name") ${_out:-<no stats line>}"
}

for g in isatest bench mmiobench; do
    jit_line "$g" "$BUILD/guest/$g.bin" --load 0x80000000
done

# ------------------------------------------------------------------
# Throughput. CoreMark last: it dominates the run time.
# ------------------------------------------------------------------
rule
say "## Throughput"
say ""

time_guest() {
    _name=$1; _img=$2; shift 2
    [ -f "$_img" ] || { say "  $(printf '%-22s' "$_name") (not built)"; return; }
    _t0=$(date +%s%N)
    _r=$("$HOST" "$@" "$_img" 2>&1 >/dev/null |
         sed -n 's/^emu: \([0-9]*\) instructions retired/\1/p' || true)
    _t1=$(date +%s%N)
    say "  $(printf '%-22s' "$_name") $(( (_t1 - _t0) / 1000000 )) ms  ${_r:-?} insns"
}

for mode in "" "--jit"; do
    label=${mode:-interp}
    time_guest "bench $label" "$BUILD/guest/bench.bin" $mode --load 0x80000000
done

if [ "$QUICK" = 0 ] && [ -f "$BUILD/guest/coremark.bin" ]; then
    for mode in "" "--jit"; do
        label=${mode:-interp}
        time_guest "coremark $label" "$BUILD/guest/coremark.bin" $mode \
                   --load 0x80000000
    done
else
    say "  coremark               skipped (--quick)"
fi

# ------------------------------------------------------------------
# G4MH. No architecture suite exists, so the compiled guest is the
# evidence -- and it must agree on both backends.
# ------------------------------------------------------------------
rule
say "## G4MH"
say ""
G4="$ROOT/tests/guest/g4mh/guest.bin"
if [ -f "$G4" ]; then
    for mode in "" "--jit"; do
        label=${mode:-interp}
        out=$("$HOST" --frontend g4mh $mode --load 0x80000000 "$G4" \
              2>/dev/null | tr -cd '[:print:]\n' | tail -3 | tr '\n' ' ')
        say "  $(printf '%-8s' "$label") $out"
    done
    say ""
    say "  The two must agree. They are the only cross-check this frontend"
    say "  has: there is no reference model for G4MH."
else
    say "  no CC-RH guest in the tree ($G4)"
    say "  build one with docs/renesas/Dockerfile; see docs/frontend/g4mh.md"
fi

rule
say ""
say "Board figures are not regenerated here. They need a flash cycle, and"
say "the board's tick counter is exactly deterministic -- so a difference"
say "between two runs of the *same* binary is real, and a difference"
say "between two binaries may be code layout, which is worth up to 10% on"
say "that part. Quote them with the commit they were measured at."
