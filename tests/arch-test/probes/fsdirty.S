/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fsdirty.S - which FP instructions make mstatus.FS Dirty?
 *
 * probe-march: rv32imaf_zicsr
 * probe-models: sail spike
 * probe-spike-isa: rv32imaf_zicsr
 * probe-regs: s1 s2 s3 s4 s5 s6 s7
 * probe-permitted: spike s3
 *
 *   scripts/run-probe.py tests/arch-test/probes/fsdirty.S
 *
 * Each case sets FS to Clean, runs one FP instruction and keeps FS
 * (0 Off, 1 Initial, 2 Clean, 3 Dirty). The privileged spec has FS
 * Dirty once the FP state "has been modified", and fcsr is FP state, so
 * the question is the instructions that write no f register but may
 * write fflags.
 *
 *   reg  case
 *   s1   feq.s, two ordinary numbers: no flag
 *   s2   feq.s with a signalling NaN: NV, fflags 0 -> 0x10
 *   s3   the same with NV already set: fflags does not change
 *   s4   fclass.s: never a flag
 *   s5   fmv.x.w: never a flag
 *   s6   fcvt.w.s of a NaN: NV
 *   s7   flw: writes an f register
 *
 * Asked because the fuzzer that runs the interpreter against the JIT
 * found the JIT marking FS Dirty for a block that never reached its FP
 * instruction, and the interpreter not marking it for a compare that
 * raised a flag -- imprecise in opposite directions.
 *
 * The answers: Dirty for s2, s6 and s7, Clean for the rest -- Sail and
 * Spike alike, except s3, where Spike writes fflags whether or not it
 * changes and so goes Dirty, and Sail keeps Clean. "Potentially
 * modified" admits both; this core is Sail's, on both backends. Before,
 * the interpreter said Clean for s2 and s6.
 */
    .option norvc
    .section .text.init
    .globl rvtest_entry_point
rvtest_entry_point:
    li   t0, 0x2000          /* FS = Initial, so FP instructions run */
    csrs mstatus, t0
    li   t0, 0x7f800001      /* a signalling NaN */
    fmv.w.x ft0, t0
    li   t0, 0x3f800000      /* 1.0 */
    fmv.w.x ft1, t0
    la   t3, word
    csrw fflags, zero

    call clean
    feq.s t1, ft1, ft1
    call fs
    mv   s1, t0

    csrw fflags, zero
    call clean
    feq.s t1, ft0, ft1
    call fs
    mv   s2, t0

    li   t0, 0x10            /* NV already set */
    csrw fflags, t0
    call clean
    feq.s t1, ft0, ft1
    call fs
    mv   s3, t0

    call clean
    fclass.s t1, ft0
    call fs
    mv   s4, t0

    call clean
    fmv.x.w t1, ft1
    call fs
    mv   s5, t0

    csrw fflags, zero
    call clean
    fcvt.w.s t1, ft0
    call fs
    mv   s6, t0

    call clean
    flw  ft2, 0(t3)
    call fs
    mv   s7, t0

    la   t0, tohost
    li   t1, 1
    sw   t1, 0(t0)
4:  j    4b

/* FS = Clean: bit 14 set, bit 13 clear. */
clean:
    li   t0, 0x4000
    csrs mstatus, t0
    li   t0, 0x2000
    csrc mstatus, t0
    ret

/* t0 = FS. */
fs:
    csrr t0, mstatus
    srli t0, t0, 13
    andi t0, t0, 3
    ret

    .data
    .align 2
word: .word 0x40490fdb
    .align 6
    .globl tohost
tohost: .quad 0
    .globl fromhost
fromhost: .quad 0
