#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# gdbtrace-f746.sh - the F746 firmware under gdbtrace, a Trace32-style
# debugger on gdb (github.com/lfazio/gdbtrace, checked out beside this
# tree): run-control, breakpoints, registers, memory, the STM32F746's
# peripherals decoded from ST's SVD, and FreeRTOS's tasks -- read live,
# through OpenOCD's TCL port, while the firmware runs.
#
#   scripts/gdbtrace-f746.sh [BUILD] [gdbtrace options...]
#
# BUILD is a configured F746 build directory (default build/f746, as the
# README's firmware recipe makes it); its ELF is loaded -- flashed -- when
# gdbtrace connects, unless --no-load is given. Anything after BUILD goes
# to gdbtrace: --no-load, --rtos, --script FILE --batch, ...
#
# This debugs the *emulator*. To debug the guest, the firmware's own RSP
# stub is the way (docs/gdb.md).
#
# Environment:
#   GDBTRACE      the gdbtrace checkout (default: ../../gdbtrace from here)
#   EMU_PROBE     VID:PID:serial, as everywhere in this tree: its serial
#                 picks the board's ST-LINK when several are attached
#   GDBTRACE_SVD  the STM32F746 SVD (default: STM32CubeIDE's, if installed)

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
GT=${GDBTRACE:-$ROOT/../../gdbtrace}
GT=$(CDPATH='' cd -- "$GT" 2>/dev/null && pwd) || {
    echo "gdbtrace-f746: no gdbtrace checkout at ${GDBTRACE:-$ROOT/../../gdbtrace}; set GDBTRACE" >&2
    exit 2
}

BUILD=build/f746
case "${1:-}" in
-*|"") ;;
*) BUILD=$1; shift ;;
esac
ELF=$BUILD/src/platform/stm32f746/emu-stm32f746.elf
[ -f "$ELF" ] || {
    echo "gdbtrace-f746: no firmware at $ELF (README.md, Firmware)" >&2
    exit 2
}

# gdbtrace's own virtualenv when it has one (its README), else python3.
PY=python3
[ -x "$GT/.venv/bin/python" ] && PY=$GT/.venv/bin/python

SVD=${GDBTRACE_SVD:-}
if [ -z "$SVD" ]; then
    for f in /opt/st/stm32cubeide*/plugins/*productdb.debug*/resources/cmsis/STMicroelectronics_CMSIS_SVD/STM32F746.svd; do
        [ -f "$f" ] && SVD=$f
    done
fi
SVD_ARG=
if [ -n "$SVD" ]; then
    SVD_ARG="--svd $SVD"
else
    echo "gdbtrace-f746: no STM32F746 SVD (set GDBTRACE_SVD): no peripheral view" >&2
fi

# The ST-LINK's serial, the third field of EMU_PROBE.
STLINK_SERIAL=
if [ -n "${EMU_PROBE:-}" ]; then
    STLINK_SERIAL=${EMU_PROBE##*:}
fi
export STLINK_SERIAL

ELF=$(CDPATH='' cd -- "$(dirname -- "$ELF")" && pwd)/$(basename -- "$ELF")
mkdir -p "$ROOT/build"
export PYTHONPATH="$GT${PYTHONPATH:+:$PYTHONPATH}"
# shellcheck disable=SC2086 # SVD_ARG is empty or two words
exec "$PY" -m gdbtrace "$ELF" \
    --openocd "$GT/configs/nucleo-f746zg.cfg" $SVD_ARG \
    --cpu-hz 216000000 --layout "$ROOT/build/gdbtrace-layout.json" "$@"
