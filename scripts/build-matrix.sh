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
#   scripts/build-matrix.sh --board      # ...and drive the board's link
#
# `--board` needs an F746 attached with scripts/ppp-up.sh already running,
# because bringing PPP up needs root and a check that asks for a password
# does not get run. Without it the f746-net row builds like any other --
# which is what it did for months while the whole EMU_NET path was
# broken.
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

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
filter=all
run_tests=0
run_board=0

#
# A real loop, and it **rejects what it does not know**.
#
# This was `filter=${1:-all}` plus a test for "--test", so any second
# argument was silently ignored: `--test --board` ran the tests, skipped
# the board entirely, and printed `f746-net  ok` -- a row reporting
# success for work it had not done, which is the exact failure that row
# was added to prevent. An option a script quietly drops is worse than one
# it does not have.
#
while [ $# -gt 0 ]; do
    case "$1" in
    --test)  run_tests=1; shift ;;
    --board) run_board=1; shift ;;
    --list)  filter=--list; shift ;;
    -h|--help) sed -n '3,14p' "$0"; exit 0 ;;
    -*) echo "build-matrix: unknown option: $1" >&2; exit 2 ;;
    *)  filter=$1; shift ;;
    esac
done

#
# name | platform | cmake options | what only this one covers
#
# Keep the RV32 and G4MH single-frontend rows: the contract check in
# CLAUDE.md is that a G4MH-only firmware links, and that has been broken
# twice by RV32-only assumptions.
#
matrix='
host-both|host|-DEMU_GUEST_ARCH_RV32=ON -DEMU_GUEST_ARCH_G4MH=ON -DEMU_GUEST_ARCH_PPC=OFF -DEMU_GUEST_ARCH_ARMV7M=OFF|RV32 and G4MH, JIT, ctest -- the default until every frontend was
host-nojit|host|-DEMU_JIT=OFF|interpreter only; where a fetch-path cost lands
host-g4-x3|host|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON -DEMU_GUEST_ARCH_PPC=OFF -DEMU_GUEST_ARCH_ARMV7M=OFF -DG4MH_PE_COUNT=3 -DEMU_OS_SMP=ON|multicore, in parallel (EMU_OS_SMP); PE_COUNT>1 was untested for years
host-g4-x3-coop|host|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON -DEMU_GUEST_ARCH_PPC=OFF -DEMU_GUEST_ARCH_ARMV7M=OFF -DG4MH_PE_COUNT=3 -DEMU_OS_SMP=OFF|multicore on the single-core FreeRTOS port: the cooperative round and its order assertion
host-g4-mpu8|host|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON -DEMU_GUEST_ARCH_PPC=OFF -DEMU_GUEST_ARCH_ARMV7M=OFF -DG4MH_MPU_ENTRIES=8|the MPU out-of-range guard is unreachable at 32
host-ppc|host|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=OFF -DEMU_GUEST_ARCH_PPC=ON -DEMU_GUEST_ARCH_ARMV7M=OFF|big-endian frontend, alone: its two cores as tasks on the single-core port, one block table for both, the order assertion of each round
host-ppc-smp|host|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=OFF -DEMU_GUEST_ARCH_PPC=ON -DEMU_GUEST_ARCH_ARMV7M=OFF -DEMU_OS_SMP=ON|the two PowerPC cores **in parallel**, which only this row does: the compare-and-swap in stwcx., a block table each
host-ppc-1core|host|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=OFF -DEMU_GUEST_ARCH_PPC=ON -DEMU_GUEST_ARCH_ARMV7M=OFF -DPPC_CORE_COUNT=1|**the other value**: one e200z7, no SIU, the inlined stores unguarded
host-armv7m|host|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=OFF -DEMU_GUEST_ARCH_PPC=OFF -DEMU_GUEST_ARCH_ARMV7M=ON|the Cortex-M guest alone: its SoftFloat build, the SCS, the guests in tests/guest/armv7m
host-all|host||**the default**, every frontend in one runner, with no -D naming any: a flat image runs as the default unless its test names the frontend, which 20 tests did not; and gdb-check reaches all four here
host-trace|host|-DEMU_GUEST_ARCH_RV32=ON -DEMU_GUEST_ARCH_G4MH=ON -DEMU_GUEST_ARCH_PPC=OFF -DEMU_GUEST_ARCH_ARMV7M=OFF -DEMU_ENABLE_TRACE=ON -DEMU_JIT=OFF|the trace build, which is how pc deltas get read. RV32 and G4MH, as it always was: a trace line lands between every two bytes of guest output, which splits the OKAAA armv7m-hello looks for
host-nopmp|host|-DEMU_GUEST_ARCH_RV32=ON -DEMU_GUEST_ARCH_G4MH=OFF -DEMU_GUEST_ARCH_PPC=OFF -DEMU_GUEST_ARCH_ARMV7M=OFF -DRV32_EXT_PMP=OFF|PMP compiled out: an absent CSR once read as success here unseen, and isatest hung -- it now tests that the PMP CSRs trap instead
host-pairs|host|-DEMU_PAIR_STATS=ON|the instruction histograms of all four frontends -- the instrument that names what a JIT still interprets, and a build option no other row turned on
host-jitdiff|host|-DEMU_JIT_DIFF=ON|every fresh block run against the IR interpreter as well -- an option no row built, whose counters went unprinted from 8f55ed0 until fuzz-ir needed them
host-net|host|-DEMU_GUEST_ARCH_RV32=ON -DEMU_NET=ON|the IP stack on the host, over a pty -- this row exists because it did not, and the whole EMU_NET path stopped compiling here while every other row passed
host-slip|host|-DEMU_GUEST_ARCH_RV32=ON -DEMU_NET=ON -DEMU_NET_LINK=slip|**the link, exercised without root**: under --test, scripts/check-host-net.sh -- ping, an upload, the console, gdb -- in a network namespace of its own
f746-rv32|stm32f746|-DEMU_GUEST_ARCH_RV32=ON -DEMU_GUEST_ARCH_G4MH=OFF|the shipping firmware: Thumb-2 emitter, lwIP/SLIP/TFTP (EMU_NET defaults ON)
f746-rv32-nojit|stm32f746|-DEMU_GUEST_ARCH_RV32=ON -DEMU_GUEST_ARCH_G4MH=OFF -DEMU_JIT=OFF|**the one that was broken**
f746-g4|stm32f746|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON|the contract check: G4MH-only must link
f746-ppc|stm32f746|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_PPC=ON -DEMU_NET=OFF|the contract check for the big-endian frontend, and its translator through the Thumb-2 emitter: every other PowerPC row is x86-64
f746-armv7m|stm32f746|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_ARMV7M=ON -DEMU_NET=OFF|the Cortex-M guest on a Cortex-M: its staging, and a frontend with no set_time -- the boards called it unconditionally
f746-g4-x3|stm32f746|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON -DG4MH_PE_COUNT=3 -DG4MH_CRAM_KIB=64 -DG4MH_LRAM_KIB=16|3 PEs of .bss on a 320 KB part -- see the sizing note
f746-nonet|stm32f746|-DEMU_GUEST_ARCH_RV32=ON -DEMU_NET=OFF|**the other value**: serial console, no lwIP
f746-net|stm32f746|-DEMU_GUEST_ARCH_RV32=ON -DEMU_NET=ON|**the link, exercised** with --board: ping, telnet, upload, and the new guest running. Building EMU_NET proves nothing -- see scripts/check-board-net.sh
f746-slip|stm32f746|-DEMU_GUEST_ARCH_RV32=ON -DEMU_NET=ON -DEMU_NET_LINK=slip|**the other link**: SLIP, now that PPP is the default
f446-rv32|stm32f446|-DEMU_GUEST_ARCH_RV32=ON -DEMU_GUEST_ARCH_G4MH=OFF|the M4: no caches, no DWT lock
f446-g4|stm32f446|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON|**the default links**: G4MH on a 128 KiB part, interpreted, at the cluster and local RAM a board backs -- it did not, and a G4MH firmware here ran the RV32 isatest until the shared guest staging
f446-net|stm32f446|-DEMU_GUEST_ARCH_RV32=ON -DEMU_NET=ON|**the other value here**: the M4 with PPP, which it had no option for until the wiring moved to cmake/emu_net.cmake
n6-rv32|stm32n6|-DEMU_GUEST_ARCH_RV32=ON -DEMU_GUEST_ARCH_G4MH=OFF|the Armv8.1-M port: cortex-m55, -mcmse, no internal flash, 1471 KiB of guest RAM
n6-g4|stm32n6|-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON|the contract check on the newest platform: a frontend swap must need nothing from src/platform/
host-coremark|host|-DEMU_NATIVE_COREMARK=ON|**the native baseline**, which called a console symbol renamed away years ago and so had not linked since; nothing built it, so nothing said so
f746-coremark|stm32f746|-DEMU_NATIVE_COREMARK=ON -DEMU_NET=OFF|the same option on the board it was written for
'

if [ "$filter" = "--list" ]; then
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
        n6)   case "$plat" in stm32n6*) ;; *) continue ;; esac ;;
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

    # **A fresh cache for every row, every time.** The directories are
    # reused so that a rerun is incremental, and a reused CMakeCache.txt
    # keeps every *computed default* from the configure that first wrote
    # it. host-both carried EMU_MAX_REGIONS=16 from before the region
    # table was sized per frontend -- 27 is the default now -- so the
    # row named "the default" could not start a G4MH guest ("could not
    # bring the cores up"), and passed, because none of its tests ran
    # one. Removing the cache keeps the objects; a row's options are
    # exactly what this table says and nothing an older tree said.
    rm -f "$dir/CMakeCache.txt"
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

    if [ "$run_board" -eq 1 ] && [ "$name" = f746-net ]; then
        if EMU_BOARD_BUILD="$dir" "$ROOT/scripts/check-board-net.sh" \
                >>"$dir.log" 2>&1; then
            echo "ok (link exercised)"
        else
            echo "LINK FAILED       (see $dir.log)"
            grep -m4 -E 'FAIL --' "$dir.log" | sed 's/^/    /'
            echo "$name link" >>"$failures"
        fi
        continue
    fi

    #
    # host-net builds and is not tested here: its console *is* the IP
    # link, so its guests wait on a pty nothing answers and ctest idles
    # until its own timeout. The link is exercised on the board, by the
    # f746-net row under --board; building the host row is what it is
    # for.
    #
    # And ctest reads stdin, which here is the rest of this table: left
    # attached, the first tested row ate every row after it and the loop
    # ended after one configuration while reporting success.
    #
    # host-coremark is the same case from the other side: with
    # EMU_NATIVE_COREMARK the runner *is* CoreMark, compiled for the host,
    # so every guest test hands its image to a program that ignores it.
    # Both surfaced the day this loop started reaching past its first row.
    #
    if [ "$run_tests" -eq 1 ] && [ "$plat" = host ] && [ "$name" = host-net ]; then
        echo "ok (built; tests skipped -- the console is the link)"
    elif [ "$run_tests" -eq 1 ] && [ "$name" = host-slip ]; then
        rc=0
        "$ROOT/scripts/check-host-net.sh" "$dir" </dev/null >>"$dir.log" 2>&1 || rc=$?
        if [ "$rc" -eq 0 ]; then
            echo "ok (link exercised)"
        elif [ "$rc" -eq 77 ]; then
            echo "ok (built; no network namespace to exercise it in)"
        else
            echo "LINK FAILED       (see $dir.log)"
            grep -m4 -E 'FAIL --' "$dir.log" | sed 's/^/    /'
            echo "$name link" >>"$failures"
            continue
        fi
    elif [ "$run_tests" -eq 1 ] && [ "$plat" = host ] && [ "$name" = host-coremark ]; then
        echo "ok (built; tests skipped -- the runner is native CoreMark)"
    elif [ "$run_tests" -eq 1 ] && [ "$plat" = host ]; then
        if ctest --test-dir "$dir" </dev/null >>"$dir.log" 2>&1; then
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
