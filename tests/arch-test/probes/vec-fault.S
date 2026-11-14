/* SPDX-License-Identifier: Apache-2.0 */
/*
 * vec-fault.S - vector accesses whose elements fault, against Spike
 * (rv_vec.c's vmem): which element stops, the cause and address
 * reported, the vstart left, what was written before it, the restart
 * from vstart, and fault-only-first shortening vl -- for unit-stride,
 * strided, indexed, masked and segment accesses, loads and stores.
 *
 * probe-march: rv32imac_zicsr_zve32x
 * probe-models: spike
 * probe-spike-isa: rv32imac_zicsr_zve32x_zvl128b
 * probe-regs: s1 s2 s3 s4 s5 s6 s7 s8 s9 s10 a1 a2
 *
 *   build/vec-host built -DRV32_EXT_ZVE32X=ON:
 *   scripts/run-probe.py --emu build/vec-host/emu-host \
 *       tests/arch-test/probes/vec-fault.S
 *
 * A locked PMP entry with no permissions makes fbuf + 16 to + 31 a hole
 * even to M-mode. The handler mixes the case, mcause, mtval less fbuf
 * and vstart into a2; then, when s0's bit 8 is set, it points a0 at alt
 * and returns to the faulting instruction, which restarts from vstart;
 * otherwise it steps past it and clears vstart, which the next access
 * would otherwise start from.
 *
 *   s1  vle32.v, element 4 in the hole: restarted from alt
 *   s2  vle32ff.v, element 4 in the hole: vl 4, no trap
 *   s3  vle32ff.v, element 0 in the hole: a trap, restarted
 *   s4  vlse32.v, element 2 in the hole: restarted
 *   s5  vluxei32.v, element 1 in the hole: restarted
 *   s6  masked vle32.v, element 4 off, element 5 in the hole
 *   s7  masked vle32ff.v, element 0 off, element 1 in the hole: vl 1
 *   s8  vlseg2e32.v, segment 2's first field in the hole: restarted;
 *       then segment 1's second field, not restarted
 *   s9  vse32.v, element 4 in the hole: restarted into alt
 *   s10 vsseg2e32.v and vsuxei32.v into the hole, not restarted
 *   a2  every trap: case, cause, mtval - fbuf, vstart
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
    li   s7, 0
    li   s8, 0
    li   s9, 0
    li   s10, 0
    li   a1, 0
    li   a2, 0

    /* The hole: 16 bytes at fbuf + 16, NAPOT, locked, no permissions. */
    la   t0, fbuf + 16
    srli t0, t0, 2
    ori  t0, t0, 1
    csrw pmpaddr0, t0
    li   t0, 0x98
    csrw pmpcfg0, t0

/* acc = rotl(acc, 5) ^ x */
.macro MIX acc, x
    slli t3, \acc, 5
    srli \acc, \acc, 27
    or   \acc, \acc, t3
    xor  \acc, \acc, \x
.endm

/* Fold the n words at label buf into acc. */
.macro FOLDMEM acc, buf, n
    la   t5, \buf
    li   t6, \n
1:  lw   t4, 0(t5)
    MIX  \acc, t4
    addi t5, t5, 4
    addi t6, t6, -1
    bnez t6, 1b
.endm

/* Fold the pair of registers at v into acc. */
.macro FOLDV2 acc, v
    la   t5, vbuf
    vs2r.v \v, (t5)
    FOLDMEM \acc, vbuf, 8
.endm

/* Fold vl and vstart into acc. */
.macro FOLDVL acc
    csrr t1, vl
    MIX  \acc, t1
    csrr t1, vstart
    MIX  \acc, t1
.endm

/* --- s1: unit-stride, restarted ------------------------------------- */
    vsetivli zero, 8, e32, m2, ta, ma
    li   t0, 0x0BADF00D
    vmv.v.x v2, t0
    la   a0, fbuf
    li   s0, 0x101
    vle32.v v2, (a0)
    FOLDV2 s1, v2
    FOLDVL s1

/* --- s2: fault-only-first past element 0 ---------------------------- */
    vsetivli zero, 8, e32, m2, ta, ma
    li   t0, 0x0BADF00D
    vmv.v.x v2, t0
    la   a0, fbuf
    li   s0, 0x002
    vle32ff.v v2, (a0)
    FOLDV2 s2, v2
    FOLDVL s2

/* --- s3: fault-only-first at element 0 ------------------------------ */
    vsetivli zero, 8, e32, m2, ta, ma
    la   a0, fbuf + 16
    li   s0, 0x103
    vle32ff.v v2, (a0)
    FOLDV2 s3, v2
    FOLDVL s3

/* --- s4: strided ---------------------------------------------------- */
    vsetivli zero, 4, e32, m1, ta, ma
    li   t0, 0x0BADF00D
    vmv.v.x v4, t0
    la   a0, fbuf
    li   t0, 8
    li   s0, 0x104
    vlse32.v v4, (a0), t0
    FOLDV2 s4, v4
    FOLDVL s4

/* --- s5: indexed ---------------------------------------------------- */
    vsetivli zero, 4, e32, m1, ta, ma
    la   t0, idx
    vle32.v v6, (t0)              /* 0, 20, 4, 8 */
    li   t0, 0x0BADF00D
    vmv.v.x v4, t0
    la   a0, fbuf
    li   s0, 0x105
    vluxei32.v v4, (a0), v6
    FOLDV2 s5, v4
    FOLDVL s5

/* --- s6: masked, the first faulting element off --------------------- */
    vsetivli zero, 8, e32, m2, ta, mu
    li   t0, 0x0BADF00D
    vmv.v.x v2, t0
    li   t0, 0xEF                 /* element 4 off */
    vsetivli zero, 1, e8, m1, ta, ma
    vmv.s.x v0, t0
    vsetivli zero, 8, e32, m2, ta, mu
    la   a0, fbuf
    li   s0, 0x106
    vle32.v v2, (a0), v0.t
    FOLDV2 s6, v2
    FOLDVL s6

/* --- s7: masked fault-only-first, element 0 off --------------------- */
    vsetivli zero, 1, e8, m1, ta, ma
    li   t0, 0xFE
    vmv.s.x v0, t0
    vsetivli zero, 8, e32, m2, ta, mu
    li   t0, 0x0BADF00D
    vmv.v.x v2, t0
    la   a0, fbuf + 12
    li   s0, 0x007
    vle32ff.v v2, (a0), v0.t
    FOLDV2 s7, v2
    FOLDVL s7

/* --- s8: segments --------------------------------------------------- */
    vsetivli zero, 4, e32, m1, ta, ma
    li   t0, 0x0BADF00D
    vmv.v.x v8, t0
    vmv.v.x v9, t0
    la   a0, fbuf
    li   s0, 0x108
    vlseg2e32.v v8, (a0)          /* segment 2 at +16 */
    FOLDV2 s8, v8
    FOLDVL s8
    li   t0, 0x0BADF00D
    vmv.v.x v8, t0
    vmv.v.x v9, t0
    la   a0, fbuf + 4
    li   s0, 0x009
    vlseg2e32.v v8, (a0)          /* segment 1's second field at +16 */
    FOLDV2 s8, v8
    FOLDVL s8

/* --- s9: a store, restarted ----------------------------------------- */
    vsetivli zero, 8, e32, m2, ta, ma
    la   t0, alt
    vle32.v v2, (t0)
    vadd.vi v2, v2, 3
    la   a0, sbuf                 /* sbuf is fbuf: the hole at +16 */
    li   s0, 0x10A
    vse32.v v2, (a0)
    FOLDMEM s9, fbuf, 4
    FOLDMEM s9, alt, 8
    FOLDVL s9

/* --- s10: segment and indexed stores, not restarted ----------------- */
    vsetivli zero, 4, e32, m1, ta, ma
    vid.v v8
    vadd.vi v9, v8, 7
    la   a0, fbuf
    li   s0, 0x00B
    vsseg2e32.v v8, (a0)          /* segment 2 at +16 */
    FOLDMEM s10, fbuf, 4
    FOLDVL s10
    la   t0, idx
    vle32.v v6, (t0)
    vadd.vi v8, v8, 9
    li   s0, 0x00C
    vsuxei32.v v8, (a0), v6       /* element 1 at +20 */
    FOLDMEM s10, fbuf, 4
    FOLDVL s10

    li   a1, 12
    la   t0, tohost
    li   t1, 1
    sw   t1, 0(t0)
2:  j    2b

/* Any trap: case, mcause, mtval - fbuf, vstart into a2. Restart with a0
 * at alt if s0's bit 8 is set; else step past the instruction, vstart
 * cleared. */
    .align 2
mtrap:
    andi t1, s0, 0xFF
    MIX  a2, t1
    csrr t1, mcause
    MIX  a2, t1
    csrr t1, mtval
    la   t2, fbuf
    sub  t1, t1, t2
    MIX  a2, t1
    csrr t1, vstart
    MIX  a2, t1
    andi t1, s0, 0x100
    beqz t1, 3f
    la   a0, alt
    mret
3:  csrw vstart, zero
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
    .align 6
fbuf:
sbuf:
    .word 0x10000001, 0x10000002, 0x10000003, 0x10000004
    .word 0xDEAD0005, 0xDEAD0006, 0xDEAD0007, 0xDEAD0008  /* the hole */
    .word 0x10000009, 0x1000000A, 0x1000000B, 0x1000000C
    .word 0x1000000D, 0x1000000E, 0x1000000F, 0x10000010
    .align 6
alt:
    .word 0x20000001, 0x20000002, 0x20000003, 0x20000004
    .word 0x20000005, 0x20000006, 0x20000007, 0x20000008
    .word 0x20000009, 0x2000000A, 0x2000000B, 0x2000000C
    .word 0x2000000D, 0x2000000E, 0x2000000F, 0x20000010
idx:
    .word 0, 20, 4, 8
vbuf:
    .space 512
