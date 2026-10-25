#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# fuzz.sh - build the fuzz tree and run the RV32 interpreter-against-JIT
# fuzzer.
#
#   scripts/fuzz.sh [SECONDS] [libFuzzer options...]
#
# Configures build/fuzz (FUZZ_BUILD overrides) with clang and
# -DEMU_FUZZ=ON, which builds the whole tree under ASan and UBSan with
# libFuzzer's coverage instrumentation. The corpus is seeded with every
# saved regression and with 256-byte slices of real guest code, so the
# fuzzer starts from instruction streams a compiler wrote rather than
# from nothing. It runs for SECONDS (default 60); a divergence prints
# both sides and the program, and libFuzzer writes the input to
# build/fuzz/artifacts/crash-*.
#
# A finding goes in tests/fuzz/regressions/rv32-jit/ under a name that
# says what it found, once it is fixed. See docs/fuzzing.md.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
B=${FUZZ_BUILD:-$ROOT/build/fuzz}
SECS=${1:-60}
[ $# -gt 0 ] && shift

mkdir -p "$B"
if ! CC=${CC:-clang} cmake -S "$ROOT" -B "$B" -DEMU_FUZZ=ON \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo >"$B.cfg.log" 2>&1; then
    echo "fuzz: configure failed, see $B.cfg.log"
    exit 1
fi
if ! cmake --build "$B" -j"$(nproc 2>/dev/null || echo 4)" \
        --target fuzz-rv32-jit guest-isatest guest-coremark guest-dhrystone \
        >"$B.build.log" 2>&1; then
    echo "fuzz: build failed, see $B.build.log"
    tail -n 5 "$B.build.log"
    exit 1
fi

C=$B/corpus/rv32-jit
mkdir -p "$C" "$B/artifacts"
cp "$ROOT"/tests/fuzz/regressions/rv32-jit/* "$C"/
OBJCOPY=$(grep -m1 '^RISCV_OBJCOPY:' "$B/CMakeCache.txt" | cut -d= -f2)
for g in isatest coremark dhrystone; do
    elf=$B/tests/guest/$g.elf
    [ -f "$elf" ] || elf=$B/guest/$g.elf
    if [ -n "$OBJCOPY" ] && [ -f "$elf" ]; then
        "$OBJCOPY" -O binary -j .text "$elf" "$B/corpus/$g.text"
        split -b 256 -d -a 4 "$B/corpus/$g.text" "$C/seed-$g-"
    fi
done

cd "$B/artifacts"
FUZZ_STATS=1 exec "$B/tests/fuzz/fuzz-rv32-jit" -max_len=1024 \
    -max_total_time="$SECS" "$@" "$C"
