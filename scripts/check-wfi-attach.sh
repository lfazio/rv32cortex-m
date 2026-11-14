#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# check-wfi-attach.sh - an F746 firmware parked in WFI keeps its clocks:
# SysTick wakes it, CYCCNT counts, and the probe reaches it.
#
#   scripts/check-wfi-attach.sh [TREE]        # TREE: this one by default
#
# Builds TREE's F746 firmware with -DEMU_NET=OFF -DEMU_WFI_CHECK=ON,
# flashes it and runs its default guest to the end with board-run.py.
# The firmware then parks for good, board_idle's WFI between SysTicks.
#
# **The firmware answers, not the probe.** DBGMCU_CR.DBG_SLEEP is what
# keeps the clocks on in Sleep, only a power-on reset clears it, and
# probe-rs sets it (0x7) whenever it attaches -- so a probe reading a
# parked core proves nothing about the firmware: the commit before every
# build set the bit passed that. So EMU_WFI_CHECK clears the register
# at boot, as a power-on would, before board_hw_init's own line sets it,
# and on the first park times a quarter of a second of WFI by SysTick's
# millisecond with CYCCNT, printing
#
#   wfi-check cycles N wakes N dbgmcu X cleared X
#
# about 54M cycles and 250 wake-ups at 216 MHz, `cleared` 0 -- the clear
# having taken, or the run asks nothing. Without DBG_SLEEP the wake-ups
# still come, SysTick running, CYCCNT counts only the time awake (45,002
# cycles), and the probe cannot connect: a firmware without its line
# fails here.
#
# Then, with no reset -- which would start the run again -- it reads
# through the probe DHCSR's S_SLEEP (bit 18, the core asleep as it was
# read; twice, since it wakes for each SysTick), CYCCNT twice four
# seconds apart by the host's clock, and DBGMCU_CR as the reads leave it.
#
# On the N6 (a second argument, stm32n6) the firmware is loaded by its
# flash target (n6-load.sh) and only the line is asked for: about 150M
# cycles and 250 wake-ups at 600 MHz. There it asks the part, not a line
# of the firmware's: the N6 sets no DBG_SLEEP, since its counter runs
# through a WFI without it (stm32n6/board.c). BOARD_CONSOLE names its
# console if the by-id path will not do.
#
# Exit 0 if the line came at the clock and the probe read the core
# asleep with CYCCNT running, 1 if not, 2 if a build failed. EMU_PROBE
# names the probe when more than one is attached.

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
TREE=$(CDPATH='' cd -- "${1:-$ROOT}" && pwd)
PLATFORM=${2:-stm32f746}

# wfi_line FILE MHZ: the line in FILE, judged against MHZ for 250 ms.
wfi_line() {
    line=$(grep -a '^wfi-check ' "$1" | tr -d '\r' | head -n 1)
    if [ -z "$line" ]; then
        echo "check-wfi-attach: no wfi-check line after the run -- SysTick" \
             "did not wake the parked core ($1)"
        return 1
    fi
    echo "$line"
    python3 -c '
import sys
f = sys.argv[1].split()
cycles, wakes, cleared = int(f[2]), int(f[4]), int(f[8], 16)
want = float(sys.argv[2]) * 1e6 / 4
sys.exit(0 if 0.99 * want < cycles < 1.01 * want and
         245 <= wakes <= 255 and cleared == 0 else 1)' "$line" "$2"
}

if [ "$PLATFORM" = stm32n6 ]; then
    B=$ROOT/build/board/wfi-attach-n6-$(basename "$TREE")
    rm -f "$B/CMakeCache.txt"
    cmake -S "$TREE" -B "$B" -DEMU_PLATFORM=stm32n6 \
        -DCMAKE_TOOLCHAIN_FILE="$TREE/cmake/arm-none-eabi.cmake" \
        -DEMU_NET=OFF -DEMU_WFI_CHECK=ON \
        ${EMU_PROBE:+-DEMU_PROBE="$EMU_PROBE"} >"$B.cfg.log" 2>&1 ||
        { echo "check-wfi-attach: configure failed ($B.cfg.log)"; exit 2; }
    cmake --build "$B" -j"$(nproc)" >"$B.build.log" 2>&1 ||
        { echo "check-wfi-attach: build failed ($B.build.log)"; exit 2; }
    serial=${EMU_PROBE##*:}
    console=${BOARD_CONSOLE:-/dev/serial/by-id/usb-STMicroelectronics_STLINK-V3_${serial}-if01}
    # The console first: the run starts when the load detaches.
    python3 -c '
import serial, sys, time
s = serial.Serial(sys.argv[1], 921600, timeout=0.2)
s.reset_input_buffer()
buf, t0 = b"", time.time()
while time.time() - t0 < 60 and b"wfi-check" not in buf:
    buf += s.read(65536)
time.sleep(0.3)
buf += s.read(65536)
open(sys.argv[2], "wb").write(buf)' "$console" "$B.txt" &
    reader=$!
    sleep 1
    if ! cmake --build "$B" --target flash >"$B.flash.log" 2>&1; then
        echo "check-wfi-attach: the load failed ($B.flash.log)"
        kill "$reader" 2>/dev/null || true
        exit 2
    fi
    wait "$reader" || true
    wfi_line "$B.txt" 600
    exit $?
fi

B=$ROOT/build/board/wfi-attach-$(basename "$TREE")
CHIP=STM32F746ZGTx
DHCSR=0xE000EDF0
CYCCNT=0xE0001004
DBGMCU_CR=0xE0042004

probe=
[ -n "${EMU_PROBE:-}" ] && probe="--probe $EMU_PROBE"

rm -f "$B/CMakeCache.txt"
cmake -S "$TREE" -B "$B" -DEMU_PLATFORM=stm32f746 \
    -DCMAKE_TOOLCHAIN_FILE="$TREE/cmake/arm-none-eabi.cmake" \
    -DEMU_NET=OFF -DEMU_WFI_CHECK=ON \
    ${EMU_PROBE:+-DEMU_PROBE="$EMU_PROBE"} >"$B.cfg.log" 2>&1 ||
    { echo "check-wfi-attach: configure failed ($B.cfg.log)"; exit 2; }
cmake --build "$B" -j"$(nproc)" >"$B.build.log" 2>&1 ||
    { echo "check-wfi-attach: build failed ($B.build.log)"; exit 2; }

python3 "$ROOT/scripts/board-run.py" "$B" 30 >"$B.txt" 2>&1 || true
if ! grep -q 'emu-result' "$B.txt"; then
    echo "check-wfi-attach: the run did not end; the capture is $B.txt"
    exit 1
fi
inside=0
wfi_line "$B.txt" 216 || inside=1
grep -q -a '^wfi-check ' "$B.txt" || exit 1

# word ADDR: one 32-bit read through the probe, attached without a reset;
# empty if it failed.
word() {
    # shellcheck disable=SC2086
    probe-rs read --chip "$CHIP" $probe b32 "$1" 1 2>>"$B.probe.log" |
        awk 'NF { w = $NF } END { if (w != "") print w }'
}

: >"$B.probe.log"
sleep 1
d0=$(word $DHCSR)
t0=$(date +%s.%N)
c0=$(word $CYCCNT)
sleep 4
t1=$(date +%s.%N)
c1=$(word $CYCCNT)
d1=$(word $DHCSR)
dbg=$(word $DBGMCU_CR)

if [ -z "$d0" ] || [ -z "$c0" ] || [ -z "$c1" ] || [ -z "$d1" ]; then
    echo "check-wfi-attach: $(basename "$TREE"): the probe could not read" \
         "the parked core ($B.probe.log)"
    exit 1
fi

python3 - "$inside" "$d0" "$d1" "$c0" "$c1" "$t0" "$t1" "${dbg:-0}" <<'EOF'
import sys
inside = sys.argv[1] == "0"  # wfi_line's verdict on the line
d0, d1, c0, c1 = (int(x, 16) for x in sys.argv[2:6])
t0, t1 = float(sys.argv[6]), float(sys.argv[7])
dbg = int(sys.argv[8], 16)
mhz = ((c1 - c0) & 0xFFFFFFFF) / (t1 - t0) / 1e6
# It wakes for each SysTick, briefly: asleep at one read of two.
asleep = any(d & (1 << 18) for d in (d0, d1))
print("DHCSR %08x %08x  S_SLEEP %s" % (d0, d1, "seen" if asleep else "clear"))
print("CYCCNT %08x to %08x in %.3f s: %.1f MHz" % (c0, c1, t1 - t0, mhz))
print("DBGMCU_CR %08x after the reads" % dbg)
# From the host, whose interval holds the reads' own start-up: within
# 10% of 216 MHz. (The line from inside is held to 1%: wfi_line.)
sys.exit(0 if inside and asleep and 194.0 < mhz < 238.0 else 1)
EOF
