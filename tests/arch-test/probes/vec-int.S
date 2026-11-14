/* SPDX-License-Identifier: Apache-2.0 */
/*
 * vec-int.S - the vector unit's integer instructions so far (rv_vec.c,
 * Zve32x in progress) against Spike: vset, unit-stride, strided, indexed,
 * mask and whole-register accesses, the single-width ALU, compares and
 * merge, multiply, divide and multiply-add, mask logic, the moves, the
 * counts and the reductions -- each group folded into one register, by
 * scalar code from a whole-register store, so the check does not lean on
 * what it checks.
 *
 * probe-march: rv32imac_zicsr_zve32x
 * probe-models: spike
 * probe-spike-isa: rv32imac_zicsr_zve32x_zvl128b
 * probe-regs: s1 s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 a1
 *
 *   build/vec-host built -DRV32_EXT_ZVE32X=ON:
 *   scripts/run-probe.py --emu build/vec-host/emu-host \
 *       tests/arch-test/probes/vec-int.S
 *
 *   s1  vsetvl: vl and vtype for SEW 8-32, LMUL 1/4-8, AVL past VLMAX,
 *       the three AVL rules, and vill for SEW 64, mf8 and reserved bits
 *   s2  unit-stride loads and stores at EEW 8, 16 and 32, LMUL 1 and 2
 *   s3  vadd, vsub, vrsub, vand, vor, vxor: .vv, .vx, .vi
 *   s4  vsll, vsrl, vsra, vminu, vmin, vmaxu, vmax
 *   s5  compares into masks, vmerge, vmv.v.v/x/i
 *   s6  vmul, vmulh, vmulhu, vmulhsu, vdiv, vdivu, vrem, vremu, edges
 *   s7  vmacc, vnmsac, vmadd, vnmsub
 *   s8  masked (v0.t) arithmetic, loads and stores
 *   s9  reductions, vmv.x.s, vmv.s.x, vcpop, vfirst, vid, viota,
 *       vmsbf, vmsif, vmsof, mask logic
 *   s10 strided and indexed loads and stores, vlm/vsm, whole registers
 *   s11 illegal instruction: a bit per case that raised it
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
    li   s11, 0
    li   a1, 0

/* acc = rotl(acc, 5) ^ x */
.macro MIX acc, x
    slli t3, \acc, 5
    srli \acc, \acc, 27
    or   \acc, \acc, t3
    xor  \acc, \acc, \x
.endm

/* Fold the n words at vbuf into acc. */
.macro FOLDBUF acc, n
    la   t5, vbuf
    li   t6, \n
1:  lw   t4, 0(t5)
    MIX  \acc, t4
    addi t5, t5, 4
    addi t6, t6, -1
    bnez t6, 1b
.endm

/* Fold register v (one register, 16 bytes) into acc. */
.macro FOLDV acc, v
    la   t5, vbuf
    vs1r.v \v, (t5)
    FOLDBUF \acc, 4
.endm

/* Fold the group at v (8 registers) into acc. */
.macro FOLDV8 acc, v
    la   t5, vbuf
    vs8r.v \v, (t5)
    FOLDBUF \acc, 32
.endm

/* A case that may trap: s0 its number, s11's bit set if it was illegal. */
.macro CASE n, insn:vararg
    li   s0, \n
    \insn
.endm

    /* Every vector register known: v0-v31 from a pattern. */
    la   a0, data_a
    vl8re32.v v0, (a0)
    vl8re32.v v8, (a0)
    la   a0, data_b
    vl8re32.v v16, (a0)
    vl8re32.v v24, (a0)

/* --- s1: vsetvl ---------------------------------------------------- */
    li   a0, 1000
    vsetvli t0, a0, e8, m1, ta, ma
    MIX  s1, t0
    csrr t1, vtype
    MIX  s1, t1
    vsetvli t0, a0, e16, m2, tu, mu
    MIX  s1, t0
    vsetvli t0, a0, e32, m8, ta, mu
    MIX  s1, t0
    vsetvli t0, a0, e8, mf4, ta, ma
    MIX  s1, t0
    vsetvli t0, a0, e16, mf2, ta, ma
    MIX  s1, t0
    li   a0, 5
    vsetvli t0, a0, e32, m4, ta, ma
    MIX  s1, t0
    vsetvli t0, zero, e8, m2, ta, ma    /* rd != 0, rs1 == 0: VLMAX */
    MIX  s1, t0
    vsetvli zero, zero, e16, m4, ta, ma /* keep vl: 32, VLMAX 32 */
    csrr t0, vl
    MIX  s1, t0
    vsetvli zero, zero, e32, m1, ta, ma /* keep vl, clamped: 4 */
    csrr t0, vl
    MIX  s1, t0
    vsetivli t0, 3, e16, m1, ta, ma
    MIX  s1, t0
    vsetivli t0, 31, e8, mf2, ta, ma
    MIX  s1, t0
    li   a2, 0x0D                       /* e16, m2... as a register */
    li   a0, 7
    vsetvl t0, a0, a2
    MIX  s1, t0
    csrr t1, vtype
    MIX  s1, t1
    vsetvli t0, a0, e64, m1, ta, ma     /* SEW past ELEN: vill */
    MIX  s1, t0
    csrr t1, vtype
    MIX  s1, t1
    vsetvli t0, a0, e8, mf8, ta, ma     /* mf8 holds nothing at ELEN 32 */
    MIX  s1, t0
    csrr t1, vtype
    MIX  s1, t1
    li   a2, 0x100                      /* a reserved bit */
    vsetvl t0, a0, a2
    MIX  s1, t0
    csrr t1, vtype
    MIX  s1, t1
    csrr t1, vlenb
    MIX  s1, t1

/* --- s2: unit-stride ----------------------------------------------- */
    li   a0, 1000
    la   a2, data_a
    la   a3, outbuf
    vsetvli t0, a0, e8, m1, ta, ma
    vle8.v v1, (a2)
    FOLDV s2, v1
    vsetivli t0, 5, e16, m1, tu, mu
    vle16.v v1, (a2)
    FOLDV s2, v1
    vsetvli t0, a0, e32, m2, ta, ma
    vle32.v v2, (a2)
    vse32.v v2, (a3)
    vle8.v v4, (a3)              /* EEW 8 at SEW 32, m2: EMUL 1/2 */
    FOLDV s2, v4
    vsetivli t0, 9, e8, m1, tu, mu
    addi a4, a2, 3               /* misaligned base, byte elements */
    vle8.v v5, (a4)
    vse8.v v5, (a3)
    lw   t1, 0(a3)
    MIX  s2, t1
    lw   t1, 4(a3)
    MIX  s2, t1
    lw   t1, 8(a3)
    MIX  s2, t1

/* --- s3: add, sub, rsub, logic ------------------------------------- */
    li   a0, 1000
    li   a4, -3
    vsetvli t0, a0, e8, m1, ta, ma
    vadd.vv v1, v16, v17
    FOLDV s3, v1
    vsub.vx v1, v16, a4
    FOLDV s3, v1
    vrsub.vi v1, v17, 5
    FOLDV s3, v1
    vand.vi v1, v18, -6
    FOLDV s3, v1
    vsetvli t0, a0, e16, m2, ta, ma
    vor.vv v2, v16, v18
    FOLDV s3, v2
    FOLDV s3, v3
    vxor.vx v2, v20, a4
    FOLDV s3, v2
    FOLDV s3, v3
    vsetvli t0, a0, e32, m1, ta, ma
    vadd.vi v1, v19, -16
    FOLDV s3, v1
    vrsub.vx v1, v19, a4
    FOLDV s3, v1

/* --- s4: shifts, min, max ------------------------------------------ */
    li   a4, 37
    vsetvli t0, a0, e8, m1, ta, ma
    vsll.vv v1, v16, v17
    FOLDV s4, v1
    vsra.vx v1, v16, a4
    FOLDV s4, v1
    vsetvli t0, a0, e16, m1, ta, ma
    vsrl.vi v1, v16, 13
    FOLDV s4, v1
    vsra.vi v1, v17, 31
    FOLDV s4, v1
    vmin.vv v1, v16, v18
    FOLDV s4, v1
    vminu.vv v1, v16, v18
    FOLDV s4, v1
    vsetvli t0, a0, e32, m1, ta, ma
    li   a4, -100
    vmax.vx v1, v16, a4
    FOLDV s4, v1
    vmaxu.vx v1, v16, a4
    FOLDV s4, v1

/* --- s5: compares, merge, moves ------------------------------------ */
    vsetvli t0, a0, e8, m1, ta, ma
    vmseq.vv v1, v16, v24
    FOLDV s5, v1
    vmsne.vi v1, v16, -1
    FOLDV s5, v1
    vmsltu.vv v1, v16, v17
    FOLDV s5, v1
    vmslt.vx v1, v16, zero
    FOLDV s5, v1
    vsetvli t0, a0, e16, m2, ta, ma
    vmsleu.vi v1, v18, 4
    FOLDV s5, v1
    vmsle.vv v1, v18, v20
    FOLDV s5, v1
    li   a4, 0x7F
    vmsgtu.vx v1, v18, a4
    FOLDV s5, v1
    vmsgt.vi v1, v20, -2
    FOLDV s5, v1
    vsetvli t0, a0, e32, m1, ta, ma
    vmslt.vv v0, v16, v17       /* the mask for vmerge */
    vmerge.vvm v1, v18, v19, v0
    FOLDV s5, v1
    li   a4, 0x12345678
    vmerge.vxm v1, v18, a4, v0
    FOLDV s5, v1
    vmerge.vim v1, v18, -9, v0
    FOLDV s5, v1
    vmv.v.v v1, v20
    FOLDV s5, v1
    vmv.v.x v1, a4
    FOLDV s5, v1
    vmv.v.i v1, 7
    FOLDV s5, v1

/* --- s6: multiply, divide ------------------------------------------ */
    vsetvli t0, a0, e8, m1, ta, ma
    vmul.vv v1, v16, v17
    FOLDV s6, v1
    vmulh.vv v1, v16, v17
    FOLDV s6, v1
    vmulhu.vv v1, v16, v17
    FOLDV s6, v1
    vmulhsu.vv v1, v16, v17
    FOLDV s6, v1
    vdiv.vv v1, v16, v17
    FOLDV s6, v1
    vdivu.vv v1, v16, v17
    FOLDV s6, v1
    vrem.vv v1, v16, v17
    FOLDV s6, v1
    vremu.vv v1, v16, v17
    FOLDV s6, v1
    vsetvli t0, a0, e32, m1, ta, ma
    li   a4, -1
    vdiv.vx v1, v18, a4          /* INT_MIN / -1 among them */
    FOLDV s6, v1
    vrem.vx v1, v18, a4
    FOLDV s6, v1
    vdivu.vx v1, v18, zero       /* by zero */
    FOLDV s6, v1
    vrem.vx v1, v18, zero
    FOLDV s6, v1
    li   a4, 0x80000001
    vmulh.vx v1, v18, a4
    FOLDV s6, v1
    vmulhsu.vx v1, v18, a4
    FOLDV s6, v1
    vsetvli t0, a0, e16, m1, ta, ma
    li   a4, -1
    vdiv.vx v1, v20, a4
    FOLDV s6, v1
    vrem.vx v1, v20, zero
    FOLDV s6, v1

/* --- s7: multiply-add ---------------------------------------------- */
    vsetvli t0, a0, e8, m1, ta, ma
    vmv.v.v v1, v16
    vmacc.vv v1, v17, v18
    FOLDV s7, v1
    vmv.v.v v1, v16
    vnmsac.vx v1, a4, v18
    FOLDV s7, v1
    vsetvli t0, a0, e32, m1, ta, ma
    vmv.v.v v1, v16
    vmadd.vv v1, v17, v18
    FOLDV s7, v1
    vmv.v.v v1, v16
    li   a4, 77
    vnmsub.vx v1, a4, v18
    FOLDV s7, v1

/* --- s8: masked ---------------------------------------------------- */
    vsetvli t0, a0, e8, m1, tu, mu
    vmsltu.vv v0, v16, v17
    vmv.v.v v1, v19
    vadd.vv v1, v16, v17, v0.t
    FOLDV s8, v1
    vmv.v.v v1, v19
    vsll.vi v1, v16, 3, v0.t
    FOLDV s8, v1
    vmv.v.v v1, v19
    la   a2, data_b
    vle8.v v1, (a2), v0.t
    FOLDV s8, v1
    la   a3, outbuf
    vmv.v.i v2, 0
    vse8.v v2, (a3)
    vse8.v v19, (a3), v0.t
    lw   t1, 0(a3)
    MIX  s8, t1
    lw   t1, 12(a3)
    MIX  s8, t1
    vsetvli t0, a0, e16, m1, tu, mu
    vmv.v.v v1, v19
    vmul.vx v1, v16, a4, v0.t
    FOLDV s8, v1
    vmv.v.v v1, v19
    vmseq.vi v1, v16, 0, v0.t
    FOLDV s8, v1

/* --- s9: reductions, moves, counts, mask ops ----------------------- */
    vsetvli t0, a0, e8, m1, ta, ma
    vredsum.vs v1, v16, v17
    vmv.x.s t1, v1
    MIX  s9, t1
    vredmax.vs v1, v16, v17
    vmv.x.s t1, v1
    MIX  s9, t1
    vredminu.vs v1, v16, v17
    vmv.x.s t1, v1
    MIX  s9, t1
    vsetvli t0, a0, e32, m2, ta, ma
    vredxor.vs v1, v16, v18
    vmv.x.s t1, v1
    MIX  s9, t1
    vredmin.vs v1, v16, v18
    vmv.x.s t1, v1
    MIX  s9, t1
    vredand.vs v1, v16, v18
    vmv.x.s t1, v1
    MIX  s9, t1
    vredor.vs v1, v16, v18
    vmv.x.s t1, v1
    MIX  s9, t1
    vredmaxu.vs v1, v16, v18
    vmv.x.s t1, v1
    MIX  s9, t1
    vsetvli t0, a0, e16, m1, ta, ma
    vmv.x.s t1, v20              /* sign-extended from SEW */
    MIX  s9, t1
    li   a4, 0xABCD1234
    vmv.v.i v1, 0
    vmv.s.x v1, a4
    FOLDV s9, v1
    vsetvli t0, a0, e8, m1, ta, ma
    vcpop.m t1, v16
    MIX  s9, t1
    vfirst.m t1, v17
    MIX  s9, t1
    vmv.v.i v2, 0
    vfirst.m t1, v2              /* none: -1 */
    MIX  s9, t1
    vid.v v1
    FOLDV s9, v1
    vsetvli t0, a0, e16, m2, ta, ma
    vid.v v2
    FOLDV s9, v2
    FOLDV s9, v3
    vsetvli t0, a0, e8, m1, ta, ma
    viota.m v1, v16
    FOLDV s9, v1
    vmsbf.m v1, v17
    FOLDV s9, v1
    vmsif.m v1, v17
    FOLDV s9, v1
    vmsof.m v1, v17
    FOLDV s9, v1
    vmand.mm v1, v16, v17
    FOLDV s9, v1
    vmnor.mm v1, v16, v17
    FOLDV s9, v1
    vmxnor.mm v1, v16, v17
    FOLDV s9, v1
    vmorn.mm v1, v16, v17
    FOLDV s9, v1
    vmandn.mm v1, v16, v17
    FOLDV s9, v1

/* --- s10: strided, indexed, mask, whole registers ------------------ */
    la   a2, data_a
    la   a3, outbuf
    li   a4, 6
    vsetvli t0, a0, e16, m1, ta, ma
    vlse16.v v1, (a2), a4
    FOLDV s10, v1
    li   a4, -4
    addi a5, a2, 60
    vsetvli t0, a0, e32, m1, ta, ma
    vlse32.v v1, (a5), a4
    FOLDV s10, v1
    li   a4, 8
    vsse32.v v19, (a3), a4
    lw   t1, 0(a3)
    MIX  s10, t1
    lw   t1, 24(a3)
    MIX  s10, t1
    vsetvli t0, a0, e8, m1, ta, ma
    vand.vi v2, v17, 15          /* byte indices 0..15 */
    vluxei8.v v1, (a2), v2
    FOLDV s10, v1
    vsetvli t0, a0, e32, m1, ta, ma
    vsetivli t0, 4, e32, m1, ta, ma
    vid.v v2
    vsll.vi v2, v2, 3            /* every other word */
    vloxei32.v v1, (a2), v2
    FOLDV s10, v1
    vsoxei32.v v18, (a3), v2
    lw   t1, 8(a3)
    MIX  s10, t1
    vsetvli t0, a0, e8, m1, ta, ma
    vlm.v v1, (a2)
    FOLDV s10, v1
    vsetivli t0, 11, e8, m1, ta, ma
    vmv.v.i v2, 0
    vsm.v v17, (a3)              /* ceil(11 / 8) = 2 bytes */
    lw   t1, 0(a3)
    MIX  s10, t1
    vl2re16.v v2, (a2)
    FOLDV s10, v2
    FOLDV s10, v3
    vs2r.v v18, (a3)
    lw   t1, 20(a3)
    MIX  s10, t1

/* --- s11: what is illegal ------------------------------------------ */
    li   a0, 7
    vsetvli t0, a0, e64, m1, ta, ma
    CASE 0, vadd.vv v1, v2, v3          /* vill */
    vsetvli t0, a0, e8, m2, ta, ma
    CASE 1, vadd.vv v1, v2, v4          /* vd not aligned to LMUL 2 */
    CASE 2, vadd.vv v2, v3, v4          /* vs2 not aligned */
    CASE 3, vadd.vv v0, v2, v4, v0.t    /* masked, writing v0 */
    CASE 4, vmseq.vv v0, v2, v4, v0.t   /* a mask may */
    CASE 5, .word 0x02067107            /* vle64.v v2, (a2): EEW past ELEN */
    vsetvli t0, a0, e8, m8, ta, ma
    CASE 6, vle32.v v8, (a2)            /* EMUL 32 */
    vsetvli t0, a0, e8, m1, ta, ma
    CASE 7, vredsum.vs v1, v2, v3       /* fine */
    CASE 8, .word 0x42860187            /* vl3re8.v v3, (a2): reserved */
    li   t1, 5
    csrw vstart, t1
    CASE 9, vcpop.m t1, v2              /* vstart not 0 */
    csrw vstart, zero
    CASE 10, vmand.mm v1, v2, v3        /* fine */
    li   t0, (3 << 9)
    csrc mstatus, t0                    /* VS Off */
    CASE 11, vadd.vv v1, v2, v3
    CASE 12, csrr t1, vl
    CASE 13, vsetvli t0, a0, e8, m1, ta, ma
    li   t0, (1 << 9)
    csrs mstatus, t0

    li   a1, 21
    la   t0, tohost
    li   t1, 1
    sw   t1, 0(t0)
2:  j    2b

/* Illegal instruction: case s0's bit in s11; anything else in its high
 * half. Then past the instruction. */
    .align 2
mtrap:
    csrr t4, mcause
    li   t3, 1
    sll  t3, t3, s0
    addi t2, t4, -2
    beqz t2, 3f
    slli t3, t3, 16
3:  or   s11, s11, t3
    csrr t4, mepc
    addi t4, t4, 4
    csrw mepc, t4
    mret

    .data
    .align 6
    .globl tohost
tohost: .quad 0
    .globl fromhost
fromhost: .quad 0
    .align 4
data_a:
    .word 0x00000000, 0x00000001, 0xFFFFFFFF, 0x7FFFFFFF
    .word 0x80000000, 0x7F7F8080, 0x0000FFFF, 0xFFFF0000
    .word 0x12345678, 0x9ABCDEF0, 0x00010001, 0x80008000
    .word 0x01020304, 0xF1F2F3F4, 0x55AA55AA, 0xC0FFEE00
    .word 0x00000003, 0xFFFFFFFD, 0x7FFF0001, 0x8001FFFF
    .word 0x0F0F0F0F, 0xF0F0F0F0, 0x000000FF, 0xFF000000
    .word 0x11111111, 0x22222222, 0x33333333, 0x44444444
    .word 0x80000001, 0xFFFFFFFE, 0x00000002, 0x7FFFFFFE
    .rept 96
    .word 0x5A5A5A5A
    .endr
data_b:
    .word 0xFFFFFFFF, 0x00000000, 0x80000000, 0x00000001
    .word 0x00000002, 0x80808080, 0xFFFF7FFF, 0x00007F80
    .word 0xDEADBEEF, 0x01234567, 0xFFFE0002, 0x7FFF8000
    .word 0x00FF00FF, 0x0F0E0D0C, 0xAA55AA55, 0x13579BDF
    .word 0x00000005, 0xFFFFFFFB, 0x80000000, 0x7FFFFFFF
    .word 0x00000000, 0x00000000, 0x00000001, 0x00000080
    .word 0x99999999, 0x66666666, 0xCCCCCCCC, 0x33333333
    .word 0x00000100, 0x00008000, 0x00800000, 0x80000000
    .rept 96
    .word 0xA5A5A5A5
    .endr
outbuf:
    .space 256
vbuf:
    .space 512
