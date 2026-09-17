#!/usr/bin/env bash
# SPDX-License-Identifier: Apache-2.0
#
# t2-check-encodings.sh - check this tree's Thumb-2 emitters against the
# assembler that already knows.
#
# **An encoder whose wrong answers are other valid instructions cannot be
# checked by reading it.** This backend has been caught twice that way: a
# 16-bit CMP that assembled as a different instruction because a register
# did not fit the field, and an imm5 shift where `LSR #0` means `LSR #32`.
# Neither computed a wrong answer at the point of the mistake; both
# produced a working program that did something else.
#
# So the constants are verified against `arm-none-eabi-as`, exactly as
# scripts/g4mh-check-encodings.sh verifies the RH850 ones against CC-RH.
# The emitters are compiled for the *host* here -- they are plain C that
# writes bytes, and nothing about them needs an ARM to run on.
#
#   scripts/t2-check-encodings.sh

set -uo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="${TMPDIR:-/tmp}/t2enc.$$"
mkdir -p "$work"
trap 'rm -rf "$work"' EXIT

command -v arm-none-eabi-as >/dev/null || {
    echo "error: arm-none-eabi-as not found" >&2
    exit 1
}

# The instructions the inlined guest-RAM path emits, and the loop cap's
# compare. Kept in step with emit_fast_guard and loop_back by hand --
# which is the point: if one changes and this does not, the diff shows it.
cat >"$work/ref.s" <<'EOF'
.syntax unified
.thumb
ldr.w   r5, [r12, r2]
ldrb.w  r5, [r12, r2]
ldrh.w  r5, [r12, r2]
ldrsb.w r5, [r12, r2]
ldrsh.w r5, [r12, r2]
str.w   r3, [r12, r2]
strb.w  r3, [r12, r2]
strh.w  r3, [r12, r2]
tst.w   r1, #3
cmp.w   r6, #128
EOF

cat >"$work/emit.c" <<'EOF'
#include "emu/emu_thumb2.h"

#include <stdint.h>
#include <stdio.h>

/* What the inline emitters in emu_jit.h write through. */
uint8_t *emu_jit_cursor;
uint8_t *emu_jit_limit;
bool emu_jit_overflow;

static uint8_t buf[256];

/*
 * t2_sync_code reaches the platform for cache maintenance. Nothing here
 * calls it; this is only so the object links.
 */
void board_sync_icache(const void *addr, uint32_t len);
void board_sync_icache(const void *addr, uint32_t len)
{
    (void)addr;
    (void)len;
}

int main(void)
{
    emu_jit_cursor = buf;
    emu_jit_limit = buf + sizeof(buf);
    emu_jit_overflow = false;

    (void)t2_ld_reg(5u, 12u, 2u, 4u, false);
    (void)t2_ld_reg(5u, 12u, 2u, 1u, false);
    (void)t2_ld_reg(5u, 12u, 2u, 2u, false);
    (void)t2_ld_reg(5u, 12u, 2u, 1u, true);
    (void)t2_ld_reg(5u, 12u, 2u, 2u, true);
    (void)t2_st_reg(3u, 12u, 2u, 4u);
    (void)t2_st_reg(3u, 12u, 2u, 1u);
    (void)t2_st_reg(3u, 12u, 2u, 2u);
    (void)t2_tst_imm8(1u, 3u);
    (void)t2_cmp_imm8(6u, 128u);

    for (uint8_t *p = buf; p < emu_jit_cursor; p++) {
        printf("%02x", *p);
    }
    printf("\n");
    return emu_jit_overflow ? 1 : 0;
}
EOF

cc -o "$work/emit" "$work/emit.c" "$here/src/backend/thumb2/encode.c" \
   -DEMU_HOST_JIT_THUMB2=1 -I"$here/include" -I"$here/src/backend/thumb2" \
   -O1 -w 2>"$work/cc.log" || {
    echo "error: could not build the emitter; see $work/cc.log" >&2
    sed -n '1,12p' "$work/cc.log" >&2
    exit 1
}

ours=$("$work/emit") || { echo "error: the emitter overflowed" >&2; exit 1; }

arm-none-eabi-as -mthumb -mcpu=cortex-m7 "$work/ref.s" -o "$work/ref.o" \
    2>"$work/as.log" || { cat "$work/as.log" >&2; exit 1; }
arm-none-eabi-objcopy -O binary "$work/ref.o" "$work/ref.bin"
theirs=$(od -An -tx1 -v "$work/ref.bin" | tr -d ' \n')

if [[ "$ours" == "$theirs" ]]; then
    echo "t2-check-encodings: $(( ${#ours} / 8 )) instructions match the assembler"
    exit 0
fi

echo "t2-check-encodings: MISMATCH" >&2
echo "  emitted:   $ours" >&2
echo "  assembler: $theirs" >&2
exit 1
