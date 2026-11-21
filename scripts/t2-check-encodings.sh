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
.fpu fpv5-d16
ldr.w   r5, [r12, r2]
ldrb.w  r5, [r12, r2]
ldrh.w  r5, [r12, r2]
ldrsb.w r5, [r12, r2]
ldrsh.w r5, [r12, r2]
str.w   r3, [r12, r2]
strb.w  r3, [r12, r2]
strh.w  r3, [r12, r2]
tst.w   r1, #3
cmp     r6, #128
cmp.w   r12, #128
cmp     r0, r1
cmp     r12, r0
cmp     r0, r12
cmp     r12, lr
mov     r12, r1
mov     r0, r12
mov.w   r0, sp
ldr     r0, [r1, #8]
ldr     r0, [r1, #124]
ldr.w   r0, [r1, #128]
ldr.w   r12, [r1, #8]
ldr     r0, [sp, #4]
str     r0, [r1, #12]
str     r0, [sp, #1020]
addw    r1, r6, #8
subw    r1, r6, #8
ldr.w   r5, [r12, r2, lsl #2]
ldrh.w  r5, [r12, r2, lsl #1]
str.w   r3, [r12, r2, lsl #2]
cmp.w   r2, #41984
cmn.w   r9, #1
cmn.w   r12, #0x00FF00FF
bic.w   r0, r9, #15
orn     r12, r3, #0xFF000000
ubfx    r9, r12, #30, #2
ubfx    r0, r12, #0, #1
bfi     r3, r0, #2, #2
bfi     r10, r1, #31, #1
sub     sp, #40
add     sp, #40
add     r3, sp, #1020
movs    r2, #66
ror.w   r2, r2, #2
sub.w   r2, r1, #0x80000000
mov.w   r12, #0x40000000
ldrb    r0, [r4, #31]
ldrb.w  r0, [r4, #2100]
add.w   sp, sp, ip
sub.w   sp, sp, ip
sdiv    r0, r2, r3
udiv    r0, r2, r3
sdiv    r1, r2, r3
mls     r0, r1, r3, r2
uadd8   r0, r0, r1
sadd16  r0, r0, r12
usub8   r2, r0, r1
ssub16  r2, r0, lr
uhadd8  r0, r0, r1
shadd16 r12, r0, r1
sel     r0, r0, r1
sel     r0, r12, r0
smulbb  r2, r0, r1
smultt  r0, r0, r12
smlabb  r2, r0, r1, r2
smlatt  lr, r0, r12, lr
pkhbt   r0, r2, r0, lsl #16
bic.w   r1, r1, r2
bic.w   r2, r12, r2
ssat    r0, #16, r1
msr     APSR_nzcvq, r0
mvn.w   r2, r2
sxtah   r1, r1, r0
sxtah   r1, r1, r0, ror #16
uxtah   r1, r1, r0
uxtah   r1, r1, r0, ror #16
sxtab16 r1, r1, r0
sxtab16 r1, r1, r0, ror #8
uxtab16 r1, r1, r0
uxtab16 r12, lr, r12, ror #24
ror.w   r0, r1, #16
ror.w   r0, r1, #8
mul     r0, r0, r12
add.w   r0, r1, #0x1000000
cmp.w   r0, #0x2000000
ldr.w   r0, [r4, r2]
ldrh.w  r0, [r4, r2]
strh.w  r0, [r12, r3]
strb.w  r0, [r4, r1]
add.w   r0, r0, r2, lsr #1
add.w   r0, r0, r2, asr #1
add.w   r12, r12, r12, lsl #16
clz     lr, lr
clz     r2, r2
lsl.w   lr, lr, r12
lsl.w   r2, r2, r12
asr.w   r1, r1, r12
eor     r2, r2, #1
eor     lr, lr, #1
sub.w   lr, lr, #1
tst.w   r1, #128
it      eq
moveq   r0, #0
smultt  lr, r2, r12
smulbb  r2, r2, r1
pkhbt   r2, r2, lr, lsl #16
uadd16  r0, r0, r2
usub16  r0, r2, r0
sxth    lr, r0
uxth    r12, r1
mla     r2, lr, r12, r2
mul     lr, lr, r1
asr.w   lr, r0, #16
lsr.w   r12, r1, #16
revsh.w r3, r12
revsh.w lr, r0
vfma.f32  s0, s1, s2
vadd.f32  s1, s3, s5
vsub.f32  s3, s0, s1
vmul.f32  s0, s3, s1
vdiv.f32  s1, s1, s3
vfma.f32  s1, s3, s2
vfms.f32  s0, s3, s1
vfnma.f32 s1, s2, s3
vfnms.f32 s3, s3, s1
vsqrt.f32 s1, s3
vsqrt.f32 s0, s1
vmov      s3, r0
vmov      r1, s3
vmov      s0, r12
vldr      s1, [r0, #496]
vldr      s0, [r0]
vstr      s3, [r0, #4]
vstr      s0, [r12, #1020]
vmrs      r0, fpscr
vmsr      fpscr, r1
lsrs      r1, r1, #1
EOF

# MVE (Helium), the N6's Cortex-M55 only: assembled for that core, Q7
# and negative offsets among them, since a Q register's three bits sit in
# a different place for each operand.
cat >"$work/ref-mve.s" <<'EOF'
.syntax unified
.thumb
vldrw.u32 q0, [r12, #496]
vldrw.u32 q7, [r0, #16]
vstrw.32  q3, [r12, #-16]
vldrw.u32 q2, [r2, #-508]
vldrb.u8  q1, [r12, #1]
vldrb.u8  q3, [r3, #112]
vstrb.8   q2, [r3, #16]
vldrb.u8  q1, [r3, #-16]
vadd.i8   q0, q0, q1
vadd.i16  q0, q0, q1
vadd.i32  q2, q3, q7
vsub.i8   q0, q0, q1
vsub.i32  q0, q1, q0
vand      q0, q0, q1
vorr      q0, q7, q1
veor      q5, q0, q1
vmin.s8   q0, q0, q1
vmin.u16  q0, q0, q1
vmax.s32  q0, q0, q1
vmax.u8   q0, q0, q1
vmul.i8   q0, q0, q1
vmul.i32  q3, q2, q1
vshl.u8   q0, q0, q1
vshl.s32  q0, q0, q3
vneg.s8   q1, q1
vneg.s32  q3, q0
vrhadd.u8 q0, q0, q1
vrhadd.s16 q0, q0, q1
vhadd.u32 q0, q0, q1
vhadd.s8  q0, q0, q1
vdup.8    q1, r2
vdup.16   q1, r2
vdup.32   q3, r12
usat      r2, #8, r2
usat      r0, #8, lr
vpsel     q1, q1, q3
vpsel     q2, q2, q0
vctp.32   r2
vctp.8    lr
vmsr      p0, r2
vmrs      r1, p0
vpst
vaddvat.u8 r0, q0
vpst
vaddvat.s16 r2, q0
vaddva.u32 r0, q3
vminv.s8  r0, q0
vminv.u32 r2, q0
vmaxv.s32 r0, q3
vmaxv.u16 r0, q0
vldrb.s16 q0, [r1, #8]
vldrb.u16 q1, [r2, #0]
vldrb.s16 q1, [r7, #120]
vldrh.s32 q0, [r1, #8]
vldrh.u32 q3, [r2, #120]
vstrh.32  q2, [r1, #120]
vstrb.16  q1, [r7, #120]
vrshl.s32 q0, q0, q1
vrshl.u16 q2, q3, q1
vcmp.i32  ne, q0, q1
vcmp.i16  ne, q2, q3
EOF

cat >"$work/emit.c" <<'EOF'
#include "emu/emu_thumb2.h"

#include <stdint.h>
#include <stdio.h>

/* What the inline emitters in emu_jit.h write through. */
uint8_t *emu_jit_cursor;
uint8_t *emu_jit_limit;
bool emu_jit_overflow;

static uint8_t buf[1024];

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
    (void)t2_cmp_imm8(12u, 128u);

    /*
     * The 16-bit forms MOV, CMP, LDR and STR now choose when the
     * operands fit -- and the 32-bit ones they fall back to where they
     * do not. CMP has two 16-bit encodings and which one is right
     * depends on whether both registers are low.
     */
    t2_cmp(0u, 1u);
    t2_cmp(12u, 0u);
    t2_cmp(0u, 12u);
    t2_cmp(12u, 14u);
    t2_mov(12u, 1u);
    t2_mov(0u, 12u);
    t2_mov(0u, 13u);
    t2_ldr_imm(0u, 1u, 8u);
    t2_ldr_imm(0u, 1u, 124u);
    t2_ldr_imm(0u, 1u, 128u);
    t2_ldr_imm(12u, 1u, 8u);
    t2_ldr_imm(0u, 13u, 4u);
    t2_str_imm(0u, 1u, 12u);
    t2_str_imm(0u, 13u, 1020u);
    t2_addw(1u, 6u, 8u);
    t2_subw(1u, 6u, 8u);

    /* The window guard and the access that undoes its rotation. */
    (void)t2_ld_reg_lsl(5u, 12u, 2u, 2u, 4u, false);
    (void)t2_ld_reg_lsl(5u, 12u, 2u, 1u, 2u, false);
    (void)t2_st_reg_lsl(3u, 12u, 2u, 2u, 4u);
    {
        uint16_t enc;

        (void)t2_expand_imm(0xA400u, &enc);
        t2_cmp_imm(2u, enc);
        (void)t2_expand_imm(1u, &enc);
        t2_cmn_imm(9u, enc); /* a compare with -1 */
        (void)t2_expand_imm(0x00FF00FFu, &enc);
        t2_cmn_imm(12u, enc);
        (void)t2_expand_imm(15u, &enc);
        t2_bic_imm(0u, 9u, enc);
        (void)t2_expand_imm(0xFF000000u, &enc);
        t2_orn_imm(12u, 3u, enc);
    }
    t2_ubfx(9u, 12u, 30u, 2u);
    t2_ubfx(0u, 12u, 0u, 1u);
    t2_bfi(3u, 0u, 2u, 2u);
    t2_bfi(10u, 1u, 31u, 1u);
    (void)t2_sp_adjust(true, 40u);
    (void)t2_sp_adjust(false, 40u);
    (void)t2_add_sp_imm(3u, 1020u);
    (void)t2_movs_imm8(2u, 66u);
    t2_shift_imm(T2_ROR, 2u, 2u, 2u);
    {
        uint16_t enc;

        (void)t2_expand_imm(0x80000000u, &enc);
        t2_sub_imm(2u, 1u, enc);
        (void)t2_expand_imm(0x40000000u, &enc);
        t2_mov_imm(12u, enc);
    }
    t2_ldrb_imm(0u, 4u, 31u);
    t2_ldrb_imm(0u, 4u, 2100u);

    /*
     * The frame adjust a chained exit and the prologue emit by hand.
     * ADD.W/SUB.W sp, sp, ip -- written as raw halfwords in ir_lower.c,
     * which is exactly the kind of constant this script exists for.
     */
    t2_emit32(0xEB0Du, (uint16_t)((13u << 8) | 12u));
    t2_emit32(0xEBADu, (uint16_t)((13u << 8) | 12u));

    /*
     * The divide family. `mls` is the remainder's second half and its
     * operand order is the easiest thing here to get backwards -- the
     * encoding is rd = ra - rn * rm, and the assembler writes it
     * `mls rd, rn, rm, ra`.
     */
    t2_sdiv(0u, 2u, 3u);
    t2_udiv(0u, 2u, 3u);
    t2_sdiv(1u, 2u, 3u);
    t2_mls(0u, 1u, 3u, 2u);

    /*
     * The DSP extension's SIMD within a register, for the guest's vector
     * registers (EMU_IR_VOP, EMU_IR_VRED), and the strided loop's moves.
     */
    t2_par(T2_PAR_ADD8, true, 0u, 0u, 1u);
    t2_par(T2_PAR_ADD16, false, 0u, 0u, 12u);
    t2_par(T2_PAR_SUB8, true, 2u, 0u, 1u);
    t2_par(T2_PAR_SUB16, false, 2u, 0u, 14u);
    t2_par_h(T2_PAR_ADD8, true, 0u, 0u, 1u);
    t2_par_h(T2_PAR_ADD16, false, 12u, 0u, 1u);
    t2_sel(0u, 0u, 1u);
    t2_sel(0u, 12u, 0u);
    t2_smulxy(2u, 0u, 1u, false, false);
    t2_smulxy(0u, 0u, 12u, true, true);
    t2_smlaxy(2u, 0u, 1u, 2u, false, false);
    t2_smlaxy(14u, 0u, 12u, 14u, true, true);
    t2_pkhbt16(0u, 2u, 0u);
    t2_bic(1u, 1u, 2u);
    t2_bic(2u, 12u, 2u);
    t2_ssat16(0u, 1u);
    t2_msr_apsr_nzcvq(0u);
    t2_mvn(2u, 2u);
    t2_xta(T2_XTA_SXTAH, 1u, 1u, 0u, 0u);
    t2_xta(T2_XTA_SXTAH, 1u, 1u, 0u, 2u);
    t2_xta(T2_XTA_UXTAH, 1u, 1u, 0u, 0u);
    t2_xta(T2_XTA_UXTAH, 1u, 1u, 0u, 2u);
    t2_xta(T2_XTA_SXTAB16, 1u, 1u, 0u, 0u);
    t2_xta(T2_XTA_SXTAB16, 1u, 1u, 0u, 1u);
    t2_xta(T2_XTA_UXTAB16, 1u, 1u, 0u, 0u);
    t2_xta(T2_XTA_UXTAB16, 12u, 14u, 12u, 3u);
    t2_shift_imm(T2_ROR, 0u, 1u, 16u);
    t2_shift_imm(T2_ROR, 0u, 1u, 8u);
    t2_mul(0u, 0u, 12u);
    {
        uint16_t enc;

        (void)t2_expand_imm(0x01000000u, &enc);
        t2_add_imm(0u, 1u, enc);
        (void)t2_expand_imm(0x02000000u, &enc);
        t2_cmp_imm(0u, enc);
    }
    (void)t2_ld_reg(0u, 4u, 2u, 4u, false);
    (void)t2_ld_reg(0u, 4u, 2u, 2u, false);
    (void)t2_st_reg(0u, 12u, 3u, 2u);
    (void)t2_st_reg(0u, 4u, 1u, 1u);

    /* The averages' and clips' other rounding modes. */
    t2_add_shifted(0u, 0u, 2u, T2_LSR, 1u);
    t2_add_shifted(0u, 0u, 2u, T2_ASR, 1u);
    t2_add_lsl(12u, 12u, 12u, 16u);
    t2_clz(14u, 14u);
    t2_clz(2u, 2u);
    t2_shift_reg(T2_LSL, 14u, 14u, 12u);
    t2_shift_reg(T2_LSL, 2u, 2u, 12u);
    t2_shift_reg(T2_ASR, 1u, 1u, 12u);
    t2_eor_imm(2u, 2u, 1u);
    t2_eor_imm(14u, 14u, 1u);
    t2_sub_imm(14u, 14u, 1u);

    /* A widening operation's element bit under a mask. */
    (void)t2_tst_imm8(1u, 128u);
    t2_emit16(0xBF08u); /* IT EQ */
    t2_emit16(0x2000u); /* MOV r0, #0, in the IT */

    /* The 16-bit and the unsigned widening multiply-adds. */
    t2_smulxy(14u, 2u, 12u, true, true);
    t2_smulxy(2u, 2u, 1u, false, false);
    t2_pkhbt16(2u, 2u, 14u);
    t2_par(T2_PAR_ADD16, true, 0u, 0u, 2u);
    t2_par(T2_PAR_SUB16, true, 0u, 2u, 0u);
    t2_sxth(14u, 0u);
    t2_uxth(12u, 1u);
    t2_mla(2u, 14u, 12u, 2u, false);
    t2_mul(14u, 14u, 1u);
    t2_shift_imm(T2_ASR, 14u, 0u, 16u);
    t2_shift_imm(T2_LSR, 12u, 1u, 16u);
    /* A big-endian element of the emitted dot product: high registers both ways. */
    t2_revsh(3u, 12u);
    t2_revsh(14u, 0u);

    /*
     * VFP, with odd registers in every field: the D, N and M bits are
     * each a register's low bit, two of them in the other halfword, and
     * an even register cannot tell where they went (t2_vfp3 put D in
     * `sub` until this asked). Then the vector unit's element loop.
     */
    t2_vfp3(T2_VFMA_VFMS, false, 0u, 1u, 2u);
    t2_vfp3(T2_VADD, false, 1u, 3u, 5u);
    t2_vfp3(T2_VSUB, true, 3u, 0u, 1u);
    t2_vfp3(T2_VMUL, false, 0u, 3u, 1u);
    t2_vfp3(T2_VDIV, false, 1u, 1u, 3u);
    t2_vfp3(T2_VFMA_VFMS, false, 1u, 3u, 2u);
    t2_vfp3(T2_VFMA_VFMS, true, 0u, 3u, 1u);
    t2_vfp3(T2_VFNMS_VFNMA, true, 1u, 2u, 3u);
    t2_vfp3(T2_VFNMS_VFNMA, false, 3u, 3u, 1u);
    t2_vsqrt(1u, 3u);
    t2_vsqrt(0u, 1u);
    t2_vmov_core(3u, 0u, false);
    t2_vmov_core(3u, 1u, true);
    t2_vmov_core(0u, 12u, false);
    t2_vldr(1u, 0u, 496u);
    t2_vldr(0u, 0u, 0u);
    t2_vstr(3u, 0u, 4u);
    t2_vstr(0u, 12u, 1020u);
    t2_vmrs(0u);
    t2_vmsr(1u);
    t2_emit16(0x0849u); /* LSRS r1, r1, #1: the mask's next bit to C */

    for (uint8_t *p = buf; p < emu_jit_cursor; p++) {
        printf("%02x", *p);
    }
    printf("\n");

    /* the second line: MVE, against ref-mve.s */
    emu_jit_cursor = buf;
    t2_vldrw(0u, 12u, 496);
    t2_vldrw(7u, 0u, 16);
    t2_vstrw(3u, 12u, -16);
    t2_vldrw(2u, 2u, -508);
    t2_vldrb(1u, 12u, 1);
    t2_vldrb(3u, 3u, 112);
    t2_vstrb(2u, 3u, 16);
    t2_vldrb(1u, 3u, -16);
    t2_mve3(T2_MVE_VADD_HI | T2_MVE_SZ(0), T2_MVE_VADD_LO, 0u, 0u, 1u);
    t2_mve3(T2_MVE_VADD_HI | T2_MVE_SZ(1), T2_MVE_VADD_LO, 0u, 0u, 1u);
    t2_mve3(T2_MVE_VADD_HI | T2_MVE_SZ(2), T2_MVE_VADD_LO, 2u, 3u, 7u);
    t2_mve3(T2_MVE_VSUB_HI | T2_MVE_SZ(0), T2_MVE_VSUB_LO, 0u, 0u, 1u);
    t2_mve3(T2_MVE_VSUB_HI | T2_MVE_SZ(2), T2_MVE_VSUB_LO, 0u, 1u, 0u);
    t2_mve3(T2_MVE_VAND_HI, T2_MVE_VLOGIC_LO, 0u, 0u, 1u);
    t2_mve3(T2_MVE_VORR_HI, T2_MVE_VLOGIC_LO, 0u, 7u, 1u);
    t2_mve3(T2_MVE_VEOR_HI, T2_MVE_VLOGIC_LO, 5u, 0u, 1u);
    t2_mve3(T2_MVE_VMIN_HI | T2_MVE_SZ(0), T2_MVE_VMIN_LO, 0u, 0u, 1u);
    t2_mve3(T2_MVE_VMIN_HI | T2_MVE_U | T2_MVE_SZ(1), T2_MVE_VMIN_LO, 0u, 0u, 1u);
    t2_mve3(T2_MVE_VMAX_HI | T2_MVE_SZ(2), T2_MVE_VMAX_LO, 0u, 0u, 1u);
    t2_mve3(T2_MVE_VMAX_HI | T2_MVE_U | T2_MVE_SZ(0), T2_MVE_VMAX_LO, 0u, 0u, 1u);
    t2_mve3(T2_MVE_VMUL_HI | T2_MVE_SZ(0), T2_MVE_VMUL_LO, 0u, 0u, 1u);
    t2_mve3(T2_MVE_VMUL_HI | T2_MVE_SZ(2), T2_MVE_VMUL_LO, 3u, 2u, 1u);
    t2_vshl(0u, true, 0u, 0u, 1u);
    t2_vshl(2u, false, 0u, 0u, 3u);
    t2_vneg(0u, 1u, 1u);
    t2_vneg(2u, 3u, 0u);
    t2_mve3(T2_MVE_VRHADD_HI | T2_MVE_U | T2_MVE_SZ(0), T2_MVE_VRHADD_LO, 0u, 0u, 1u);
    t2_mve3(T2_MVE_VRHADD_HI | T2_MVE_SZ(1), T2_MVE_VRHADD_LO, 0u, 0u, 1u);
    t2_mve3(T2_MVE_VHADD_HI | T2_MVE_U | T2_MVE_SZ(2), T2_MVE_VHADD_LO, 0u, 0u, 1u);
    t2_mve3(T2_MVE_VHADD_HI | T2_MVE_SZ(0), T2_MVE_VHADD_LO, 0u, 0u, 1u);
    t2_vdup(0u, 1u, 2u);
    t2_vdup(1u, 1u, 2u);
    t2_vdup(2u, 3u, 12u);
    t2_usat(2u, 8u, 2u);
    t2_usat(0u, 8u, 14u);
    t2_mve3(T2_MVE_VPSEL_HI, T2_MVE_VPSEL_LO, 1u, 1u, 3u);
    t2_mve3(T2_MVE_VPSEL_HI, T2_MVE_VPSEL_LO, 2u, 2u, 0u);
    t2_vctp(2u, 2u);
    t2_vctp(0u, 14u);
    t2_vmsr_p0(2u);
    t2_vmrs_p0(1u);
    t2_vpst();
    t2_vredv(T2_MVE_VADDVA_HI, T2_MVE_VADDVA_LO, 0u, true, 0u, 0u);
    t2_vpst();
    t2_vredv(T2_MVE_VADDVA_HI, T2_MVE_VADDVA_LO, 1u, false, 2u, 0u);
    t2_vredv(T2_MVE_VADDVA_HI, T2_MVE_VADDVA_LO, 2u, true, 0u, 3u);
    t2_vredv(T2_MVE_VMINV_HI, T2_MVE_VMINV_LO, 0u, false, 0u, 0u);
    t2_vredv(T2_MVE_VMINV_HI, T2_MVE_VMINV_LO, 2u, true, 2u, 0u);
    t2_vredv(T2_MVE_VMAXV_HI, T2_MVE_VMAXV_LO, 2u, false, 0u, 3u);
    t2_vredv(T2_MVE_VMAXV_HI, T2_MVE_VMAXV_LO, 1u, true, 0u, 0u);
    t2_vldr_widen(0u, true, 0u, 1u, 8u);
    t2_vldr_widen(0u, false, 1u, 2u, 0u);
    t2_vldr_widen(0u, true, 1u, 7u, 120u);
    t2_vldr_widen(1u, true, 0u, 1u, 8u);
    t2_vldr_widen(1u, false, 3u, 2u, 120u);
    t2_vstr_narrow(1u, 2u, 1u, 120u);
    t2_vstr_narrow(0u, 1u, 7u, 120u);
    t2_mve3(T2_MVE_VRSHL_HI | T2_MVE_SZ(2), T2_MVE_VRSHL_LO, 0u, 1u, 0u);
    t2_mve3(T2_MVE_VRSHL_HI | T2_MVE_U | T2_MVE_SZ(1), T2_MVE_VRSHL_LO, 2u, 1u, 3u);
    t2_vcmp_ne(2u, 0u, 1u);
    t2_vcmp_ne(1u, 2u, 3u);
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

both=$("$work/emit") || { echo "error: the emitter overflowed" >&2; exit 1; }
ours=$(echo "$both" | sed -n 1p)
ours_mve=$(echo "$both" | sed -n 2p)

arm-none-eabi-as -mthumb -mcpu=cortex-m55 "$work/ref-mve.s" -o "$work/ref-mve.o" \
    2>"$work/as-mve.log" || { cat "$work/as-mve.log" >&2; exit 1; }
arm-none-eabi-objcopy -O binary "$work/ref-mve.o" "$work/ref-mve.bin"
theirs_mve=$(od -An -tx1 -v "$work/ref-mve.bin" | tr -d ' \n')
if [[ "$ours_mve" != "$theirs_mve" ]]; then
    echo "t2-check-encodings: MVE MISMATCH" >&2
    echo "  emitted:   $ours_mve" >&2
    echo "  assembler: $theirs_mve" >&2
    exit 1
fi

arm-none-eabi-as -mthumb -mcpu=cortex-m7 "$work/ref.s" -o "$work/ref.o" \
    2>"$work/as.log" || { cat "$work/as.log" >&2; exit 1; }
arm-none-eabi-objcopy -O binary "$work/ref.o" "$work/ref.bin"
theirs=$(od -An -tx1 -v "$work/ref.bin" | tr -d ' \n')

if [[ "$ours" == "$theirs" ]]; then
    echo "t2-check-encodings: $(( ${#ours} / 8 )) instructions match the assembler, and $(( ${#ours_mve} / 8 )) of MVE"
    exit 0
fi

echo "t2-check-encodings: MISMATCH" >&2
echo "  emitted:   $ours" >&2
echo "  assembler: $theirs" >&2
exit 1
