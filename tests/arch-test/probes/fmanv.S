/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fmanv.S - a fused multiply-add of zero times infinity plus a quiet NaN:
 * does it raise NV?
 *
 * probe-march: rv32imafd_zicsr
 * probe-models: sail spike
 * probe-spike-isa: rv32imafd_zicsr
 * probe-regs: s1 s2 s3 s4 s5 s6 s7 s8
 *
 *   scripts/run-probe.py tests/arch-test/probes/fmanv.S
 *
 * IEEE 754 leaves it to the implementation whether 0 * inf + qNaN
 * signals invalid; RISC-V's F chapter says the fused multiply-adds must.
 * Each case clears fflags, runs one instruction and keeps fflags and
 * the result's bits.
 *
 *   s1, s2  fmadd.s 0 * inf + qNaN: fflags, the result
 *   s3, s4  fmadd.s inf * 0 + qNaN, the operands swapped
 *   s5, s6  fnmsub.s 0 * inf + qNaN
 *   s7, s8  fmadd.d 0 * inf + qNaN: fflags, the result's high word
 */
    .option norvc
    .section .text.init
    .globl rvtest_entry_point
rvtest_entry_point:
    li   t0, 0x2000          /* FS = Initial */
    csrs mstatus, t0
    fmv.w.x ft0, zero        /* +0 */
    li   t0, 0x7f800000      /* +inf */
    fmv.w.x ft1, t0
    li   t0, 0x7fc00001      /* a quiet NaN, not the canonical one */
    fmv.w.x ft2, t0

    csrw fflags, zero
    fmadd.s ft3, ft0, ft1, ft2
    csrr s1, fflags
    fmv.x.w s2, ft3

    csrw fflags, zero
    fmadd.s ft3, ft1, ft0, ft2
    csrr s3, fflags
    fmv.x.w s4, ft3

    csrw fflags, zero
    fnmsub.s ft3, ft0, ft1, ft2
    csrr s5, fflags
    fmv.x.w s6, ft3

    la   t3, dwords
    fld  ft4, 0(t3)          /* +0.0 */
    fld  ft5, 8(t3)          /* +inf */
    fld  ft6, 16(t3)         /* a quiet NaN */
    csrw fflags, zero
    fmadd.d ft7, ft4, ft5, ft6
    csrr s7, fflags
    fsd  ft7, 24(t3)
    lw   s8, 28(t3)

    la   t0, tohost
    li   t1, 1
    sw   t1, 0(t0)
4:  j    4b

    .data
    .align 3
dwords:
    .dword 0x0000000000000000
    .dword 0x7ff0000000000000
    .dword 0x7ff8000000000001
    .dword 0
    .align 6
    .globl tohost
tohost: .quad 0
    .globl fromhost
fromhost: .quad 0
