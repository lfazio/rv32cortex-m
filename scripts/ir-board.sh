#!/bin/sh
# SPDX-License-Identifier: Apache-2.0
#
# ir-board.sh - fuzz-ir's corpus through the Thumb-2 lowering, on the F746.
#
#   scripts/ir-board.sh [CORPUS] [FIRST [COUNT]]
#
# The x86-64 lowering is fuzzed on the host (scripts/fuzz.sh SECS ir);
# the Thumb-2 one runs only on a board, so this takes the corpus fuzzing
# built there -- build/fuzz/corpus/ir by default -- links it into a
# firmware of its own with tests/fuzz/fuzz_ir.c compiled for Thumb-2
# (EMU_IR_BOARD, tests/ir-board/), flashes it, and reads what it says:
# every input built, interpreted as built and as optimised, lowered,
# called and compared, as on the host. FIRST and COUNT take a slice of
# the corpus in name order -- to resume past a case that faulted, which
# stops the run (IR-BOARD-FAULT names it).
#
# Prints the replay's own lines; exits 0 only if it reached IR-BOARD-END
# with no difference and no fault. The whole capture is in
# build/ir-board/run.txt.
#
# Environment:
#   EMU_PROBE      the probe, required with more than one attached
#   BOARD_CONSOLE  the console, if not the F746's ST-LINK by-id link
#   IR_BOARD_SECS  how long to wait for IR-BOARD-END (default 600)

set -eu

ROOT=$(CDPATH='' cd -- "$(dirname -- "$0")/.." && pwd)
CORPUS=${1:-$ROOT/build/fuzz/corpus/ir}
FIRST=${2:-0}
COUNT=${3:-}
OUT=$ROOT/build/ir-board
SECS=${IR_BOARD_SECS:-600}
mkdir -p "$OUT"

[ -d "$CORPUS" ] || { echo "ir-board: no corpus at $CORPUS (scripts/fuzz.sh SECS ir)" >&2; exit 2; }
n=$(python3 "$ROOT/tests/ir-board/pack.py" "$CORPUS" "$OUT/ir-corpus.bin" "$FIRST" $COUNT)
echo "ir-board: $n inputs from $CORPUS, from #$FIRST"

# -O2 as the firmware's backend is built; the core's own sources, the
# Thumb-2 backend and the IR interpreter, nothing of a platform.
arm-none-eabi-gcc -mcpu=cortex-m7 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard \
    -O2 -g -std=gnu11 -DEMU_IR_BOARD -ffunction-sections -fdata-sections \
    -Wall -Wextra -I"$ROOT/include" -I"$OUT" \
    --specs=picolibc.specs -nostartfiles \
    -T "$ROOT/tests/ir-board/board.ld" -Wl,--gc-sections \
    "$ROOT/tests/ir-board/start.S" "$ROOT/tests/ir-board/corpus.S" \
    "$ROOT/tests/ir-board/main.c" "$ROOT/tests/fuzz/fuzz_ir.c" \
    "$ROOT/src/emu/emu_ir.c" "$ROOT/src/emu/emu_ir_jit.c" \
    "$ROOT/src/emu/emu_jit.c" "$ROOT/src/emu/emu_crypto.c" \
    "$ROOT/src/emu/emu_lock.c" "$ROOT/src/backend/common/interp.c" \
    "$ROOT/src/backend/thumb2/encode.c" "$ROOT/src/backend/thumb2/ir_lower.c" \
    -o "$OUT/ir-board.elf" >"$OUT/build.log" 2>&1 || {
    echo "ir-board: the build failed (see $OUT/build.log)" >&2
    tail -n 5 "$OUT/build.log" >&2
    exit 1
}

PROBE_ARG=
[ -n "${EMU_PROBE:-}" ] && PROBE_ARG="--probe $EMU_PROBE"
# shellcheck disable=SC2086 # PROBE_ARG is empty or two words
probe-rs download --chip STM32F746ZGTx $PROBE_ARG "$OUT/ir-board.elf" \
    >"$OUT/flash.log" 2>&1 || {
    echo "ir-board: flashing failed (see $OUT/flash.log)" >&2
    exit 1
}

CONSOLE=${BOARD_CONSOLE:-}
if [ -z "$CONSOLE" ]; then
    CONSOLE=$(ls /dev/serial/by-id/*STM32_STLink_${EMU_PROBE##*:}*-if02 2>/dev/null | head -1)
    [ -n "$CONSOLE" ] || CONSOLE=$(ls /dev/serial/by-id/*STM32_STLink*-if02 | head -1)
fi

python3 - "$CONSOLE" "$SECS" "$OUT/run.txt" "$PROBE_ARG" <<'EOF'
import subprocess, sys, time
import serial

console, secs, out, probe = sys.argv[1], float(sys.argv[2]), sys.argv[3], sys.argv[4]
ser = serial.Serial(console, 460800, timeout=0.05)
ser.reset_input_buffer()
subprocess.run(["probe-rs", "reset", "--chip", "STM32F746ZGTx"] + probe.split(),
               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
buf = b""
start = time.time()
while time.time() - start < secs:
    buf += ser.read(65536)
    if b"IR-BOARD-END" in buf:
        buf += ser.read(4096)
        break
text = buf.decode("latin1").replace("\r", "")
with open(out, "w") as f:
    f.write(text)
keep = [l for l in text.splitlines()
        if l.startswith(("IR-BOARD", "fuzz-ir"))]
print("\n".join(keep[:60]))
if len(keep) > 60:
    print("... %d more lines in %s" % (len(keep) - 60, out))
ok = ("IR-BOARD-END" in text and "IR-BOARD-FAULT" not in text and
      " differ 0" in text)
print("ir-board: %s, %.0f s" % ("clean" if ok else "NOT CLEAN", time.time() - start))
sys.exit(0 if ok else 1)
EOF
