/* SPDX-License-Identifier: Apache-2.0 */
/*
 * vec-ramend.S - vector accesses at the end of RAM, against Spike: the
 * boundary of rv_vec.c's direct path (vmem_direct), which copies an
 * access whose bytes all lie in RAM and must leave one that runs past
 * it to the per-element path -- which faults at the first element
 * outside, with the elements before it done.
 *
 * Both run 8 MiB of RAM at 0x80000000 (run-probe.py's --ram and -m), so
 * 0x80800000 is the first byte past it in each. No PMP: the direct path
 * is open, which is the point.
 *
 * probe-march: rv32imac_zicsr_zve32x
 * probe-models: spike
 * probe-spike-isa: rv32imac_zicsr_zve32x_zvl128b
 * probe-regs: s1 s2 s3 s4 s5 s6 a1 a2
 *
 *   s1  vle32.v, 4 elements from 8 bytes before the end: element 2 faults
 *   s2  vse32.v the same: elements 0 and 1 stored, read back
 *   s3  vle32ff.v the same: vl 2, no trap
 *   s4  vle8.v, 16 bytes ending exactly at the end: no fault
 *   s5  vlse32.v from the last word down, stride -4: no fault
 *   s6  vlseg2e32.v, 2 segments from 12 bytes before the end: segment 1's
 *       second field faults
 *   a2  every trap: case, cause, mtval - 0x80800000, vstart
 *   a1  set once every case has run
 */
    .option norvc
    .section .text.init
    .globl rvtest_entry_point
rvtest_entry_point:
    la   t0, mtrap
    csrw mtvec, t0
    li   t0, (1 << 9)           /* mstatus.VS Initial: Spike resets it Off */
    csrs mstatus, t0
    li   s1, 0
    li   s2, 0
    li   s3, 0
    li   s4, 0
    li   s5, 0
    li   s6, 0
    li   a1, 0
    li   a2, 0
    li   s7, 0x80800000         /* the first byte past RAM */

/* acc = rotl(acc, 5) ^ x */
.macro MIX acc, x
    slli t3, \acc, 5
    srli \acc, \acc, 27
    or   \acc, \acc, t3
    xor  \acc, \acc, \x
.endm

/* Fold register v (16 bytes) into acc, through vbuf. */
.macro FOLDV acc, v
    la   t5, vbuf
    vs1r.v \v, (t5)
    li   t6, 4
1:  lw   t4, 0(t5)
    MIX  \acc, t4
    addi t5, t5, 4
    addi t6, t6, -1
    bnez t6, 1b
.endm

.macro FOLDVL acc
    csrr t1, vl
    MIX  \acc, t1
    csrr t1, vstart
    MIX  \acc, t1
.endm

/* v1 and v2 to a known pattern. */
.macro CLEAR
    vsetivli zero, 4, e32, m1, ta, ma
    li   t0, 0x0BADF00D
    vmv.v.x v1, t0
    vmv.v.x v2, t0
.endm

    /* Something to load: the last 16 bytes of RAM. */
    li   t0, 0x11112222
    sw   t0, -16(s7)
    li   t0, 0x33334444
    sw   t0, -12(s7)
    li   t0, 0x55556666
    sw   t0, -8(s7)
    li   t0, 0x77778888
    sw   t0, -4(s7)

/* --- s1 ------------------------------------------------------------- */
    CLEAR
    addi a0, s7, -8
    li   s0, 1
    vle32.v v1, (a0)
    FOLDV s1, v1
    FOLDVL s1

/* --- s2 ------------------------------------------------------------- */
    CLEAR
    li   t0, 0x0C0FFEE0
    vadd.vx v1, v1, t0
    addi a0, s7, -8
    li   s0, 2
    vse32.v v1, (a0)
    lw   t1, -8(s7)
    MIX  s2, t1
    lw   t1, -4(s7)
    MIX  s2, t1
    FOLDVL s2

/* --- s3 ------------------------------------------------------------- */
    CLEAR
    addi a0, s7, -8
    li   s0, 3
    vle32ff.v v1, (a0)
    FOLDV s3, v1
    FOLDVL s3

/* --- s4 ------------------------------------------------------------- */
    vsetivli zero, 16, e8, m1, ta, ma
    addi a0, s7, -16
    li   s0, 4
    vle8.v v1, (a0)
    FOLDV s4, v1
    FOLDVL s4

/* --- s5 ------------------------------------------------------------- */
    CLEAR
    addi a0, s7, -4
    li   t0, -4
    li   s0, 5
    vlse32.v v1, (a0), t0
    FOLDV s5, v1
    FOLDVL s5

/* --- s6 ------------------------------------------------------------- */
    CLEAR
    vsetivli zero, 2, e32, m1, ta, ma
    addi a0, s7, -12
    li   s0, 6
    vlseg2e32.v v1, (a0)
    FOLDV s6, v1
    FOLDV s6, v2
    FOLDVL s6

    li   a1, 6
    la   t0, tohost
    li   t1, 1
    sw   t1, 0(t0)
2:  j    2b

/* Any trap: case, mcause, mtval less the end of RAM, vstart into a2;
 * then past the instruction, vstart cleared. */
    .align 2
mtrap:
    MIX  a2, s0
    csrr t1, mcause
    MIX  a2, t1
    csrr t1, mtval
    sub  t1, t1, s7
    MIX  a2, t1
    csrr t1, vstart
    MIX  a2, t1
    csrw vstart, zero
    csrr t1, mepc
    addi t1, t1, 4
    csrw mepc, t1
    mret

    .data
    .align 6
    .globl tohost
tohost: .quad 0
    .globl fromhost
fromhost: .quad 0
    .align 4
vbuf:
    .space 64
