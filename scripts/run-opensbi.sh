#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# Build OpenSBI for RV32 and run it on the emulator.
#
# What this proves
# ----------------
# Every other guest in this tree was *compiled for* these addresses.
# OpenSBI is not: it is built once for the `generic` platform and told
# the shape of the machine at run time, through the flattened device tree
# whose address arrives in a1. So a successful run is evidence about the
# emulator's privileged implementation rather than about one binary --
# M-mode, the CLINT at the legacy SiFive layout, PMP, medeleg/mideleg, and
# an NS16550 a driver written against the real thing can talk to.
#
# It stops where a payload would start. fw_jump hands control to S-mode at
# 0x80400000, and with nothing loaded there the run spins to the
# instruction cap -- so "capped" is the expected outcome here, not a
# failure. Use fw_payload with a kernel to go further.
#
#   scripts/run-opensbi.sh [emu-binary]
#
# Needs riscv64-linux-gnu-gcc (the bare-metal riscv64-unknown-elf- one
# will not do: its linker cannot make PIEs and OpenSBI requires that) and
# dtc.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
EMU=${1:-$ROOT/build/host/emu-host}
WORK=${OPENSBI_WORK:-$ROOT/build/opensbi}

if [ ! -x "$EMU" ]; then
    echo "error: no emulator at $EMU" >&2
    echo "       cmake -B build/host && cmake --build build/host" >&2
    exit 1
fi

#
# ls -la the runner before believing a suite result: this script defaults
# to a path no other workflow rebuilds, which is how a whole session's
# figures once came from a three-day-old binary.
#
echo "runner: $(ls -la "$EMU" | tr -s ' ' | cut -d' ' -f6-)"

for t in dtc riscv64-linux-gnu-gcc; do
    command -v "$t" >/dev/null 2>&1 || { echo "error: $t not found" >&2; exit 1; }
done

mkdir -p "$WORK"

DTB=$WORK/rv32-emu.dtb
dtc -I dts -O dtb -o "$DTB" "$ROOT/boot/rv32-emu.dts" 2>/dev/null

SRC=$WORK/opensbi
if [ ! -d "$SRC/.git" ]; then
    echo "fetching OpenSBI into $SRC"
    git clone --depth 1 -q \
        https://github.com/riscv-software-src/opensbi.git "$SRC"
fi

#
# ilp32d and rv32imafdc, which is more than this core implements -- it has
# no D. That is deliberate and safe: OpenSBI itself executes no
# floating point, the ABI only decides how *it* would pass arguments, and
# building it for a narrower ISA is a separate axis from what the guest
# beneath it may use. If that ever stops being true the symptom is an
# illegal instruction inside the firmware, which the emulator reports.
#
make -C "$SRC" -j"$(nproc)" PLATFORM=generic \
    CROSS_COMPILE=riscv64-linux-gnu- \
    PLATFORM_RISCV_XLEN=32 \
    PLATFORM_RISCV_ISA=rv32imafdc_zicsr_zifencei \
    PLATFORM_RISCV_ABI=ilp32d \
    FW_TEXT_START=0x80000000 >"$WORK/build.log" 2>&1 ||
    { echo "error: OpenSBI build failed; see $WORK/build.log" >&2; exit 1; }

FW=$SRC/build/platform/generic/firmware/fw_jump.bin

echo "firmware: $FW ($(wc -c <"$FW") bytes)"
echo

#
# --load 0x80000000 because this is a flat binary linked to run from RAM,
# and --ram must be at least what the device tree's memory node claims or
# OpenSBI will hand its successor a region that is not there.
#
"$EMU" --ram 0x8000000 --load 0x80000000 --dtb "$DTB" \
       --max-insn 20000000 "$FW"
