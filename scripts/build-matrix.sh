#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# build-matrix.sh - configure and build every configuration that is
# supposed to work, because the ones nobody builds do not.
#
#   scripts/build-matrix.sh              # every configuration
#   scripts/build-matrix.sh host         # only the host ones
#   scripts/build-matrix.sh f746         # only the firmware ones
#   scripts/build-matrix.sh --list       # print the table and stop
#   scripts/build-matrix.sh --test       # also run ctest where there is one
#
# Why this exists
# ---------------
# `-DEMU_JIT=OFF` on the F746 had not compiled since the IR backend
# landed: `EMU_IR_JIT_ON_THUMB2` was defined unconditionally while
# `RV_ENABLE_JIT` followed `EMU_JIT`, so the JIT source was compiled into
# a build that had asked for no JIT and referred to a type that build
# removes. Nothing noticed, because nothing built it.
#
# That is the fourth capability in this project found broken by being
# behind a flag or a count nobody set -- VLE mode, the IMR register, the
# RISC-V JIT off ARM, `PE_COUNT > 1`. CLAUDE.md's rule is to build the
# other value or accept that it does not work; this is the "build the
# other value" half, and it is cheap: the whole matrix is a few minutes.
#
# **The 3-PE firmware row carries explicit RAM sizes and must.** The
# defaults are 128 KiB of cluster RAM plus 64 KiB of local RAM *per PE*,
# which at three PEs is 320 KiB -- the F746's entire memory, with nothing
# left for the emulator itself. The link fails with "cannot move location
# counter backwards", which names the symptom and not the arithmetic. The
# guest memory map is a build setting for exactly this reason; see
# docs/memory.md.
#
# **A row that repeats a default tests nothing.** `f746-net` passed
# `-DEMU_NET=ON`, which is already the F746's default -- so it built the
# same configuration as `f746-rv32` while `EMU_NET=OFF` went untested,
# which is the exact failure this script exists to catch, in this script.
# Where an option has a default, the row states the *other* value.
#
# Each row is a *reason*, not a permutation. The axes multiply out to
# far more than this, and building all of them would take long enough
# that it would stop being run -- which is the failure mode this is
# trying to fix. Add a row when a configuration has something only it
# can break.

set -eu

cd "$(dirname "$0")/.."

filter=${1:-all}
run_tests=0
[ "$filter" = "--test" ] && { run_tests=1; filter=all; }

#
# name | platform | cmake options | what only this one covers
#
# Keep the RV32 and G4MH single-frontend rows: the contract check in
# CLAUDE.md is that a G4MH-only firmware links, and that has been broken
# twice by RV32-only assumptions.
#
matrix='
host-both|host|-DEMU_GUEST_ARCH_RV32=ON -DEMU_GUEST_ARCH_G4MH=ON|the default: both frontends, JIT, ctest
host-nojit|host|-DEMU_GUEST_ARCH_RV32=ON -DEMU_GUEST_ARCH_G4MH=ON -DEMU_JIT=OFF|interpreter only; where a fetch-path cost lands
host-g4-x3|host|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON -DG4MH_PE_COUNT=3|multicore; PE_COUNT>1 was untested for years
host-g4-mpu8|host|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON -DG4MH_MPU_ENTRIES=8|the MPU out-of-range guard is unreachable at 32
host-ppc|host|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_PPC=ON|big-endian frontend, alone
host-trace|host|-DEMU_GUEST_ARCH_G4MH=ON -DEMU_ENABLE_TRACE=ON -DEMU_JIT=OFF|the trace build, which is how pc deltas get read
f746-rv32|stm32f746|-DEMU_GUEST_ARCH_RV32=ON -DEMU_GUEST_ARCH_G4MH=OFF|the shipping firmware: Thumb-2 emitter, lwIP/SLIP/TFTP (EMU_NET defaults ON)
f746-rv32-nojit|stm32f746|-DEMU_GUEST_ARCH_RV32=ON -DEMU_GUEST_ARCH_G4MH=OFF -DEMU_JIT=OFF|**the one that was broken**
f746-g4|stm32f746|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON|the contract check: G4MH-only must link
f746-g4-x3|stm32f746|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON -DG4MH_PE_COUNT=3 -DG4MH_CRAM_KIB=64 -DG4MH_LRAM_KIB=16|3 PEs of .bss on a 320 KB part -- see the sizing note
f746-nonet|stm32f746|-DEMU_GUEST_ARCH_RV32=ON -DEMU_NET=OFF|**the other value**: serial console, no lwIP
f446-rv32|stm32f446|-DEMU_GUEST_ARCH_RV32=ON -DEMU_GUEST_ARCH_G4MH=OFF|the M4: no caches, no DWT lock
'

if [ "${1:-}" = "--list" ]; then
    printf '%-18s %-12s %s\n' NAME PLATFORM COVERS
    echo "$matrix" | while IFS='|' read -r name plat opts why; do
        [ -n "$name" ] || continue
        printf '%-18s %-12s %s\n' "$name" "$plat" "$why"
    done
    exit 0
fi

#
# The loop below runs in a subshell -- `echo ... | while` always does --
# so a counter incremented inside it does not survive. The first version
# of this script tried anyway and then re-derived the verdict by grepping
# the logs for "FAILED", which is a word it prints to *stdout* and never
# writes to a log: it announced "all configurations built" directly under
# two failures it had just printed. A status line that cannot say no is
# worse than none, which this script's own header says about builds.
#
# A file is the simplest thing that crosses the subshell boundary.
#
failures=$(mktemp)
trap 'rm -f "$failures"' EXIT

echo "$matrix" | while IFS='|' read -r name plat opts why; do
    [ -n "$name" ] || continue
    case "$filter" in
        all) ;;
        host) [ "$plat" = host ] || continue ;;
        f746) case "$plat" in stm32f7*) ;; *) continue ;; esac ;;
        f446) case "$plat" in stm32f4*) ;; *) continue ;; esac ;;
        *)    [ "$name" = "$filter" ] || continue ;;
    esac

    dir=build/mx-$name
    printf '%-18s ' "$name"

    # The firmware platforms cross-compile; the toolchain file is not
    # optional and CMake's error for its absence names the option rather
    # than the cause.
    tc=
    case "$plat" in
        stm32*) tc=-DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake ;;
    esac

    # shellcheck disable=SC2086
    if ! cmake -S . -B "$dir" -DEMU_PLATFORM="$plat" $tc $opts >"$dir.log" 2>&1; then
        echo "CONFIGURE FAILED  (see $dir.log)"
        echo "$name configure" >>"$failures"
        continue
    fi
    if ! cmake --build "$dir" -j"$(nproc)" >>"$dir.log" 2>&1; then
        echo "BUILD FAILED      (see $dir.log)"
        grep -m3 -E ' error|undefined reference|overflowed' "$dir.log" | sed 's/^/    /'
        echo "$name build" >>"$failures"
        continue
    fi

    if [ "$run_tests" -eq 1 ] && [ "$plat" = host ]; then
        if ctest --test-dir "$dir" >>"$dir.log" 2>&1; then
            echo "ok (tests pass)"
        else
            echo "TESTS FAILED      (see $dir.log)"
            echo "$name tests" >>"$failures"
            continue
        fi
    else
        echo "ok"
    fi
done

echo
if [ -s "$failures" ]; then
    echo "build-matrix: $(wc -l <"$failures") configuration(s) failed:" >&2
    sed 's/^/  /' "$failures" >&2
    exit 1
fi
echo "build-matrix: all configurations built"
