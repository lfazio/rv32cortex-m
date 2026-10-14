#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# n6-load.sh - load the Nucleo-N657X0-Q's firmware into AXISRAM and start it.
#
#   scripts/n6-load.sh GDBSERVER CUBEPROGRAMMER_DIR GDB ELF [STLINK_SERIAL]
#
# What `cmake --build <n6 build> --target flash` runs. Development boot
# (JP2 at 2-3): the boot ROM releases the core and loads nothing, so the
# image has to be put at its link address and the core pointed at it --
# which is what the IDE's launch does, and what nothing in
# STM32_Programmer_CLI can:
#
#   - its -g starts nothing, because the Cortex-M55 is behind access port
#     1 and the CLI, on AP 0, reaches the bus but not the core: a core
#     register read is DEV_TARGET_CMD_ERR, and with ap=1 it cannot even
#     identify the device;
#   - ST-LINK_gdbserver -m 1 reaches the core, and gdb does the rest.
#
# So: start the server; reset through AIRCR, which returns the core to the
# boot ROM -- `monitor reset` does not, and a core left in a HardFault
# runs the next image in handler mode; then load, take sp and pc from the
# image's own vector table, clear the exception number, and detach, which
# lets it run.
#
# GDB_PORT overrides the port (61234), for when another server holds it.

set -eu

if [ $# -lt 4 ]; then
    echo "usage: n6-load.sh GDBSERVER CUBEPROGRAMMER_DIR GDB ELF [SERIAL]" >&2
    exit 2
fi
server=$1
cp=$2
gdb=$3
elf=$4
serial=${5:-}
port=${GDB_PORT:-61234}

for f in "$server" "$gdb"; do
    if [ ! -x "$f" ]; then
        echo "n6-load: not executable: $f" >&2
        exit 2
    fi
done
if [ ! -f "$elf" ]; then
    echo "n6-load: no such image: $elf" >&2
    exit 2
fi

log=$(mktemp)
# -e: persistent, so the reset session's detach does not end the server
# before the load session connects. -g: attach without halting -- a halt
# on connect is what the boot ROM's state refuses.
if [ -n "$serial" ]; then
    "$server" -p "$port" -l 1 -d -e -g -m 1 -i "$serial" -cp "$cp" \
        >"$log" 2>&1 &
else
    "$server" -p "$port" -l 1 -d -e -g -m 1 -cp "$cp" >"$log" 2>&1 &
fi
pid=$!
trap 'kill "$pid" 2>/dev/null || true; rm -f "$log"' EXIT INT TERM

i=0
until grep -q 'Waiting for debugger connection' "$log"; do
    if ! kill -0 "$pid" 2>/dev/null || [ $i -ge 80 ]; then
        echo "n6-load: the gdbserver did not come up:" >&2
        cat "$log" >&2
        exit 1
    fi
    i=$((i + 1))
    sleep 0.25
done

"$gdb" -q -batch -nx "$elf" \
    -ex "target extended-remote :$port" \
    -ex 'set *(unsigned int *)0xE000ED0C = 0x05FA0004' \
    -ex detach >/dev/null 2>&1

# $sp, $pc and $xpsr are gdb's, so the single quotes are the point.
# shellcheck disable=SC2016
out=$("$gdb" -q -batch -nx "$elf" \
    -ex 'set pagination off' \
    -ex "target extended-remote :$port" \
    -ex load \
    -ex 'set $sp = ((unsigned int *)&g_pfnVectors)[0]' \
    -ex 'set $pc = ((unsigned int *)&g_pfnVectors)[1]' \
    -ex 'set $xpsr = 0x01000000' \
    -ex 'printf "n6-load: start pc=%08x sp=%08x\n", $pc, $sp' \
    -ex detach 2>&1) || true
printf '%s\n' "$out" | grep -E 'Transfer rate|n6-load:|rror' || true

# Success is the load completing and the start being printed; anything
# less is a board still running its previous image, which reads exactly
# like a change that did not work.
if ! printf '%s\n' "$out" | grep -q 'Transfer rate' ||
    ! printf '%s\n' "$out" | grep -q 'n6-load: start'; then
    echo "n6-load: the load did not complete" >&2
    printf '%s\n' "$out" >&2
    exit 1
fi
