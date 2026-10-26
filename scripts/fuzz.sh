#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# fuzz.sh - build the fuzz tree and run an interpreter-against-JIT
# fuzzer.
#
#   scripts/fuzz.sh [SECONDS] [rv32|armv7m|g4mh|g4mh-smp|ppc|all] [libFuzzer options...]
#
# Configures build/fuzz (FUZZ_BUILD overrides) with clang and
# -DEMU_FUZZ=ON, which builds the whole tree under ASan and UBSan with
# libFuzzer's coverage instrumentation, and runs fuzz-<TARGET>-jit for
# SECONDS (default 60; target default rv32; `all` runs the five in
# turn, SECONDS each). g4mh-smp is fuzz-g4mh-jit built with three PEs,
# in build/fuzz-smp (FUZZ_SMP_BUILD overrides): G4MH's translated stores
# are built differently for one PE and for several, so a second PE is
# only meaningful in a build that has one, and the configuration word
# chooses one PE or two per input. PowerPC's default build has two
# cores already. The corpus is seeded with every saved regression
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
SECS=${1:-60}
[ $# -gt 0 ] && shift
T=${1:-rv32}
[ $# -gt 0 ] && shift

case $T in
rv32 | armv7m | g4mh | g4mh-smp | ppc | all) ;;
*)
    echo "fuzz: no target '$T' (rv32, armv7m, g4mh, g4mh-smp, ppc or all)"
    exit 2
    ;;
esac
if [ "$T" = all ]; then
    for t in rv32 armv7m g4mh ppc g4mh-smp; do
        "$0" "$SECS" "$t" "$@" || exit 1
    done
    exit 0
fi

# ISA names the fuzz target, T the run: its corpus and its artifacts.
ISA=$T
EXTRA=
B=${FUZZ_BUILD:-$ROOT/build/fuzz}
if [ "$T" = g4mh-smp ]; then
    ISA=g4mh
    EXTRA=-DG4MH_PE_COUNT=3
    B=${FUZZ_SMP_BUILD:-$ROOT/build/fuzz-smp}
fi

mkdir -p "$B"
# shellcheck disable=SC2086 # EXTRA is empty or one word
if ! CC=${CC:-clang} cmake -S "$ROOT" -B "$B" -DEMU_FUZZ=ON $EXTRA \
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
        --target "fuzz-$ISA-jit" $GUESTS >"$B.build.log" 2>&1; then
    echo "fuzz: build failed, see $B.build.log"
    tail -n 5 "$B.build.log"
    exit 1
fi

C=$B/corpus/$T-jit
mkdir -p "$C" "$B/artifacts/$T"
cp "$ROOT"/tests/fuzz/regressions/"$ISA"-jit/* "$C"/ 2>/dev/null || true

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

# asm SRC OUT: one of tests/fuzz/seeds/<TARGET>-jit/*.S, linked at the
# program's address and cut to its bytes. These are what reach the
# devices: a compiled guest's code addresses a CLINT or an NVIC through
# constants the fuzzer would take a long time to happen on.
asm() {
    case $T in
    rv32)
        CC_SEED=$(grep -m1 '^RISCV_GCC:' "$B/CMakeCache.txt" | cut -d= -f2)
        FLAGS="-march=rv32imac_zicsr -mabi=ilp32"
        ;;
    armv7m)
        CC_SEED=$(command -v arm-none-eabi-gcc || true)
        FLAGS="-mcpu=cortex-m7 -mthumb"
        ;;
    ppc) # Book E: the seeds run behind configuration word 1
        CC_SEED=$(grep -m1 '^PPC_GCC:' "$B/CMakeCache.txt" | cut -d= -f2)
        FLAGS="-m32 -Wa,-a32,-mbig,-mregnames,-me500mc"
        ;;
    *) return 1 ;;
    esac
    [ -n "$CC_SEED" ] || return 1
    # Assembled by the driver, linked by ld itself: the drivers' default
    # link scripts carry a `flash` region that 0x2000_0000 is not in.
    LD_EMU=
    [ "$T" = rv32 ] && LD_EMU="-m elf32lriscv"
    [ "$T" = ppc ] && LD_EMU="-m elf32ppc"
    # shellcheck disable=SC2086 # FLAGS and LD_EMU are word lists
    "$CC_SEED" $FLAGS -c -o "$2.o" "$1" &&
        "${CC_SEED%gcc}ld" $LD_EMU -Ttext=0x20000000 -e 0x20000000 \
            -o "$2.elf" "$2.o" &&
        "${CC_SEED%gcc}objcopy" -O binary -j .text "$2.elf" "$2"
}
# FUZZ_NO_DEVICE_SEEDS=1 leaves them out, to see what fuzzing reaches
# without being shown the way (scripts/fuzz-mutant.sh).
seeds() {
    [ -z "${FUZZ_NO_DEVICE_SEEDS:-}" ] || return 0
    for src in "$ROOT"/tests/fuzz/seeds/"$ISA"-jit/*.S; do
        [ -f "$src" ] || continue
        n=$(basename "$src" .S)
        if asm "$src" "$B/corpus/$n.bin"; then
            seed "$B/corpus/$n.bin" "dev-$n" "$@"
        else
            echo "fuzz: could not assemble $src; the devices start unseeded"
        fi
    done
}

Z='\000\000\000\000'
case $T in
rv32)
    for g in isatest coremark dhrystone; do
        text "$B/guest/$g.elf" "$B/corpus/$g.text" && seed "$B/corpus/$g.text" "$g"
    done
    seeds
    ;;
armv7m)
    for g in coremark dhrystone crypto; do
        text "$B/guest/armv7m-$g.elf" "$B/corpus/a7-$g.text" &&
            seed "$B/corpus/a7-$g.text" "$g" "$Z"
    done
    seeds "$Z"
    ;;
ppc)
    # VLE, Book E, and VLE on two cores (CFG_CORE1, bit 11).
    for g in coremark dhrystone alu; do
        text "$B/guest/ppc-$g.elf" "$B/corpus/ppc-$g.text" &&
            seed "$B/corpus/ppc-$g.text" "$g" "$Z" '\001\000\000\000' \
                '\000\010\000\000'
    done
    seeds '\001\000\000\000' '\001\010\000\000'
    ;;
g4mh | g4mh-smp)
    # CC-RH's images are committed: the toolchain is not free to install.
    # So are the device seeds, from scripts/g4mh-build-seeds.sh. With
    # three PEs, each behind one PE and two (CFG_PE1, bit 11).
    CFGS=$Z
    [ "$T" = g4mh-smp ] && CFGS="$Z \000\010\000\000"
    for f in "$ROOT"/tests/guest/g4mh/*.bin; do
        # shellcheck disable=SC2086 # a word list on purpose
        seed "$f" "$(basename "$f" .bin)" $CFGS
    done
    for f in "$ROOT"/tests/fuzz/seeds/g4mh-jit/*.bin; do
        [ -z "${FUZZ_NO_DEVICE_SEEDS:-}" ] || break
        # shellcheck disable=SC2086
        seed "$f" "dev-$(basename "$f" .bin)" $CFGS
    done
    ;;
esac

# -timeout: an input running ten seconds has hung -- libFuzzer's own
# default is twenty minutes, which is most of a run spent on one input
# while the time limit says it fuzzed. A hang is reported like any other
# finding, as timeout-* beside the crashes; a later -timeout wins.
cd "$B/artifacts/$T"
FUZZ_STATS=1 exec "$B/tests/fuzz/fuzz-$ISA-jit" -max_len=1024 \
    -max_total_time="$SECS" -timeout=10 "$@" "$C"
