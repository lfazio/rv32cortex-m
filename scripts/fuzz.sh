#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# fuzz.sh - build the fuzz tree and run an interpreter-against-JIT
# fuzzer.
#
#   scripts/fuzz.sh [SECONDS] [rv32|armv7m|g4mh|ppc|all] [libFuzzer options...]
#
# Configures build/fuzz (FUZZ_BUILD overrides) with clang and
# -DEMU_FUZZ=ON, which builds the whole tree under ASan and UBSan with
# libFuzzer's coverage instrumentation, and runs fuzz-<TARGET>-jit for
# SECONDS (default 60; target default rv32; `all` runs the four in
# turn, SECONDS each). The corpus is seeded with every saved regression
# and with 256-byte slices of real guest code, so the fuzzer starts from
# instruction streams a compiler wrote rather than from nothing -- for
# the three targets whose input begins with a configuration word, behind
# four zero bytes, and PowerPC's both as VLE and as Book E. A divergence
# prints both sides and the program, and libFuzzer writes the input to
# build/fuzz/artifacts/<TARGET>/crash-*.
#
# A finding goes in tests/fuzz/regressions/<TARGET>-jit/ under a name
# that says what it found, once it is fixed. See docs/fuzzing.md.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
B=${FUZZ_BUILD:-$ROOT/build/fuzz}
SECS=${1:-60}
[ $# -gt 0 ] && shift
T=${1:-rv32}
[ $# -gt 0 ] && shift

case $T in
rv32 | armv7m | g4mh | ppc | all) ;;
*)
    echo "fuzz: no target '$T' (rv32, armv7m, g4mh, ppc or all)"
    exit 2
    ;;
esac
if [ "$T" = all ]; then
    for t in rv32 armv7m g4mh ppc; do
        FUZZ_BUILD=$B "$0" "$SECS" "$t" "$@" || exit 1
    done
    exit 0
fi

mkdir -p "$B"
if ! CC=${CC:-clang} cmake -S "$ROOT" -B "$B" -DEMU_FUZZ=ON \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo >"$B.cfg.log" 2>&1; then
    echo "fuzz: configure failed, see $B.cfg.log"
    exit 1
fi
case $T in
rv32) GUESTS="guest-isatest guest-coremark guest-dhrystone" ;;
armv7m) GUESTS="guest-armv7m-coremark guest-armv7m-dhrystone guest-armv7m-crypto" ;;
ppc) GUESTS="guest-ppc-coremark guest-ppc-dhrystone guest-ppc-alu" ;;
*) GUESTS="" ;;
esac
# shellcheck disable=SC2086 # a word list on purpose
if ! cmake --build "$B" -j"$(nproc 2>/dev/null || echo 4)" \
        --target "fuzz-$T-jit" $GUESTS >"$B.build.log" 2>&1; then
    echo "fuzz: build failed, see $B.build.log"
    tail -n 5 "$B.build.log"
    exit 1
fi

C=$B/corpus/$T-jit
mkdir -p "$C" "$B/artifacts/$T"
cp "$ROOT"/tests/fuzz/regressions/"$T"-jit/* "$C"/ 2>/dev/null || true

# seed IMAGE NAME [CONFIG...]: 256-byte slices of IMAGE, each behind
# each 4-byte configuration word given as octal escapes for printf.
seed() {
    img=$1 name=$2
    shift 2
    [ -s "$img" ] || return 0
    rm -f "$B"/corpus/slice-*
    split -b 256 -d -a 4 "$img" "$B/corpus/slice-"
    for s in "$B"/corpus/slice-*; do
        if [ $# -eq 0 ]; then
            cp "$s" "$C/seed-$name-${s##*-}"
        else
            k=0
            for cfg in "$@"; do
                # shellcheck disable=SC2059 # the configuration is the format
                { printf "$cfg"; cat "$s"; } >"$C/seed-$name-$k-${s##*-}"
                k=$((k + 1))
            done
        fi
        rm -f "$s"
    done
}

# text ELF OUT: an ELF's .text, by any objcopy that reads it.
text() {
    OBJCOPY=$(command -v llvm-objcopy || true)
    [ -n "$OBJCOPY" ] || OBJCOPY=$(grep -m1 '^RISCV_OBJCOPY:' "$B/CMakeCache.txt" | cut -d= -f2)
    [ -f "$1" ] && [ -n "$OBJCOPY" ] && "$OBJCOPY" -O binary -j .text "$1" "$2"
}

Z='\000\000\000\000'
case $T in
rv32)
    for g in isatest coremark dhrystone; do
        text "$B/guest/$g.elf" "$B/corpus/$g.text" && seed "$B/corpus/$g.text" "$g"
    done
    ;;
armv7m)
    for g in coremark dhrystone crypto; do
        text "$B/guest/armv7m-$g.elf" "$B/corpus/a7-$g.text" &&
            seed "$B/corpus/a7-$g.text" "$g" "$Z"
    done
    ;;
ppc)
    for g in coremark dhrystone alu; do
        text "$B/guest/ppc-$g.elf" "$B/corpus/ppc-$g.text" &&
            seed "$B/corpus/ppc-$g.text" "$g" "$Z" '\001\000\000\000'
    done
    ;;
g4mh)
    # CC-RH's images are committed: the toolchain is not free to install.
    for f in "$ROOT"/tests/guest/g4mh/*.bin; do
        seed "$f" "$(basename "$f" .bin)" "$Z"
    done
    ;;
esac

cd "$B/artifacts/$T"
FUZZ_STATS=1 exec "$B/tests/fuzz/fuzz-$T-jit" -max_len=1024 \
    -max_total_time="$SECS" "$@" "$C"
