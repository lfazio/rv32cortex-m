#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# check-nmi-button.sh - the F746's user button as the guest's NMI pin,
# pressed without a finger.
#
#   scripts/check-nmi-button.sh [SECONDS]     # the press, SECONDS in (3)
#
# Two firmwares with -DEMU_NMI_BUTTON=ON, each flashed and captured by
# board-run.py; SECONDS after the banner, it writes EXTI_SWIER bit 13
# through the probe, without a reset -- the request a press makes, in
# software. Neither guest has an NMI handler, and each reports the one it
# took:
#
#   nmi-button       ARMv7-M CoreMark at 400 iterations, about 15 s
#                    under the JIT: the press arrives while it runs, and
#                    its result must not be printed
#   nmi-button-halt  G4MH's test guest, which ends in HALT with nothing
#                    to wake it -- alone, the end of the run: the run
#                    must wait for the press, a FENMI (cause E0H)
#
# Not an ARMv7-M guest for the second: the frontend runs WFI as a NOP,
# which the architecture permits, so such a guest never parks.
#
# The firmware keeps HCLK on in Sleep (DBG_SLEEP, every F746 build), or
# the probe could not reach a core waiting in WFI.
#
# Exit 0 if both did, 1 if not, 2 if a build or the probe failed.
# EMU_PROBE names the probe when more than one is attached.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
AT=${1:-3}
EXTI_SWIER=0x40013C10

probe=
[ -n "${EMU_PROBE:-}" ] && probe="--probe $EMU_PROBE"

# one NAME WANT NOT -D...: build with the options, run, press SECONDS
# after the banner, and want WANT in the capture and NOT nowhere. The
# press goes through board-run.py's BOARD_AT -- its capture is printed
# only once the run is over, so nothing watching that output could time
# it -- and probe-rs attaches without a reset, which would start the run
# again.
one() {
    name=$1
    B=$ROOT/build/board/$name
    want=$2
    not=$3
    shift 3
    rm -f "$B/CMakeCache.txt"
    cmake -S "$ROOT" -B "$B" -DEMU_PLATFORM=stm32f746 \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT/cmake/arm-none-eabi.cmake" \
        -DEMU_NET=OFF -DEMU_MAX_INSN=0 -DEMU_NMI_BUTTON=ON \
        -DEMU_GUEST_ARCH_RV32=OFF \
        ${EMU_PROBE:+-DEMU_PROBE="$EMU_PROBE"} "$@" >"$B.cfg.log" 2>&1 ||
        { echo "check-nmi-button: configure failed ($B.cfg.log)"; exit 2; }
    cmake --build "$B" -j"$(nproc)" >"$B.build.log" 2>&1 ||
        { echo "check-nmi-button: build failed ($B.build.log)"; exit 2; }

    BOARD_AT="$AT:probe-rs write --chip STM32F746ZGTx $probe b32 $EXTI_SWIER 0x2000" \
        python3 "$ROOT/scripts/board-run.py" "$B" 60 >"$B.txt" \
        2>"$B.probe.log" || true
    # Over before the press: what a parked guest does when nothing waits
    # for the button.
    if grep -q "^AT $AT never ran" "$B.txt" && grep -q 'emu-result' "$B.txt"
    then
        echo "check-nmi-button: $name: the run ended before the press"
        grep -E 'PASS|emu-result' "$B.txt" | sed 's/^/    /'
        fails=$((fails + 1))
        return 0
    fi
    if ! grep -q "^AT $AT rc=0" "$B.txt"; then
        echo "check-nmi-button: $name: the probe could not write SWIER" \
             "($B.probe.log)"
        grep '^AT ' "$B.txt" | sed 's/^/    /'
        exit 2
    fi
    if grep -q "$want" "$B.txt" && ! grep -q "$not" "$B.txt"; then
        echo "check-nmi-button: $name: the press took the NMI"
        return 0
    fi
    echo "check-nmi-button: $name: no NMI taken; the capture is $B.txt"
    grep -E 'ipsr|CoreMark Size|TRAP|PASS|emu-result' "$B.txt" |
        sed 's/^/    /'
    fails=$((fails + 1))
}

fails=0
# A guest running: the NMI arrives between slices, ipsr 2 in start.S's
# halt, and CoreMark's result not printed.
one nmi-button 'ipsr 00000002' 'CoreMark Size' \
    -DEMU_GUEST_ARCH_ARMV7M=ON -DARMV7M_GUEST=coremark \
    -DARMV7M_COREMARK_ITERATIONS=400
# A guest parked for good: G4MH's test guest ends in HALT with nothing
# unmasked, which alone ends the run. Armed, the run waits, and the press
# is FENMI, cause E0H, in the crt0's trap report.
one nmi-button-halt '!TRAP 000000E0' 'FAIL' -DEMU_GUEST_ARCH_G4MH=ON
[ "$fails" -eq 0 ] || exit 1
echo "check-nmi-button: both presses took the NMI"
