/* SPDX-License-Identifier: Apache-2.0 */
/*
 * vec-int2.S - the rest of Zve32x against Spike (rv_vec.c): carry,
 * fixed point under all four rounding modes, narrowing and widening,
 * integer extension, slides, gathers, compress, whole-register moves,
 * widening reductions and segment accesses -- each group folded into one
 * register by scalar code from a whole-register store, as vec-int.S
 * does, so the check does not lean on what it checks.
 *
 * probe-march: rv32imac_zicsr_zve32x
 * probe-models: spike
 * probe-spike-isa: rv32imac_zicsr_zve32x_zvl128b
 * probe-regs: s1 s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 a1 a2 a3
 *
 *   build/vec-host built -DRV32_EXT_ZVE32X=ON:
 *   scripts/run-probe.py --emu build/vec-host/emu-host \
 *       tests/arch-test/probes/vec-int2.S
 *
 *   s1  vadc, vmadc, vsbc, vmsbc: every form, with and without carry
 *   s2  vsaddu, vsadd, vssubu, vssub, and vxsat after each
 *   s3  vaaddu, vaadd, vasubu, vasub, vsmul under each vxrm; vcsr
 *   s4  vssrl, vssra under each vxrm, shifts 0 to SEW - 1
 *   s5  vnsrl, vnsra, vnclipu, vnclip: .wv, .wx, .wi, each vxrm
 *   s6  vwaddu ... vwsub.w, at e8 m1, e16 m2 and e8 mf2
 *   s7  vwmulu, vwmulsu, vwmul, vwmaccu, vwmacc, vwmaccus, vwmaccsu
 *   s8  vzext and vsext, vf2 and vf4
 *   s9  vslideup, vslidedown, vslide1up, vslide1down, masked and not
 *   s10 vrgather, vrgatherei16, vcompress, vmv<n>r.v
 *   a2  vwredsumu, vwredsum; segment loads and stores: unit-stride,
 *       strided, indexed, fault-only-first
 *   a3  a load resumed from a nonzero vstart
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
    li   a2, 0
    li   a3, 0

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

/* Fold register v (16 bytes) into acc. */
.macro FOLDV acc, v
    la   t5, vbuf
    vs1r.v \v, (t5)
    FOLDMEM \acc, vbuf, 4
.endm

/* Fold the pair of registers at v into acc. */
.macro FOLDV2 acc, v
    la   t5, vbuf
    vs2r.v \v, (t5)
    FOLDMEM \acc, vbuf, 8
.endm

/* Fold vxsat into acc and clear it. */
.macro FOLDSAT acc
    csrr t1, vxsat
    MIX  \acc, t1
    csrwi vxsat, 0
.endm

/* insn under each of the four rounding modes, its result v folded. */
.macro RM4 acc, v, insn:vararg
    csrwi vxrm, 0
    \insn
    FOLDV \acc, \v
    csrwi vxrm, 1
    \insn
    FOLDV \acc, \v
    csrwi vxrm, 2
    \insn
    FOLDV \acc, \v
    csrwi vxrm, 3
    \insn
    FOLDV \acc, \v
.endm

/* A case that may trap: s0 its number, s11's bit set if it was illegal. */
.macro CASE n, insn:vararg
    li   s0, \n
    \insn
.endm

/* Every register from the data; v0 a mask with a mix of bits. */
.macro RELOAD
    la   a0, data_a
    vl8re32.v v0, (a0)
    vl8re32.v v8, (a0)
    la   a0, data_b
    vl8re32.v v16, (a0)
    vl8re32.v v24, (a0)
    vsetivli zero, 1, e32, m1, ta, ma
    li   t0, 0xA5C3F05A
    vmv.s.x v0, t0
.endm

    RELOAD
    li   a4, 0x80000001          /* the scalar operand of every .vx */
    li   a5, 0x0000007F
    li   a6, -1

/* --- s1: carry ------------------------------------------------------ */
    vsetivli zero, 13, e8, m1, ta, ma
    vadc.vvm  v1, v16, v17, v0
    FOLDV s1, v1
    vadc.vxm  v1, v16, a4, v0
    FOLDV s1, v1
    vadc.vim  v1, v17, -3, v0
    FOLDV s1, v1
    vmadc.vvm v1, v16, v17, v0
    FOLDV s1, v1
    vmadc.vv  v1, v16, v17
    FOLDV s1, v1
    vmadc.vxm v1, v17, a6, v0
    FOLDV s1, v1
    vmadc.vx  v1, v17, a6
    FOLDV s1, v1
    vmadc.vim v1, v16, 15, v0
    FOLDV s1, v1
    vmadc.vi  v1, v16, -1
    FOLDV s1, v1
    vsbc.vvm  v1, v16, v17, v0
    FOLDV s1, v1
    vsbc.vxm  v1, v17, a4, v0
    FOLDV s1, v1
    vmsbc.vvm v1, v16, v17, v0
    FOLDV s1, v1
    vmsbc.vv  v1, v17, v16
    FOLDV s1, v1
    vmsbc.vxm v1, v16, a5, v0
    FOLDV s1, v1
    vmsbc.vx  v1, v16, a5
    FOLDV s1, v1
    vsetivli zero, 15, e16, m2, ta, ma
    vadc.vvm  v2, v16, v18, v0
    FOLDV2 s1, v2
    vmadc.vvm v1, v18, v20, v0
    FOLDV s1, v1
    vmsbc.vxm v1, v20, a4, v0
    FOLDV s1, v1
    vsetivli zero, 3, e32, m1, ta, ma
    vadc.vxm  v1, v19, a6, v0
    FOLDV s1, v1
    vmadc.vvm v1, v19, v20, v0
    FOLDV s1, v1
    vmsbc.vvm v1, v20, v19, v0
    FOLDV s1, v1
    vmadc.vvm v16, v16, v17, v0      /* vd the source itself */
    FOLDV s1, v16
    RELOAD

/* --- s2: saturating add and subtract -------------------------------- */
    csrwi vxsat, 0
    vsetivli zero, 16, e8, m1, ta, ma
    vsaddu.vv v1, v16, v17
    FOLDV s2, v1
    FOLDSAT s2
    vsaddu.vx v1, v16, a5
    FOLDV s2, v1
    FOLDSAT s2
    vsaddu.vi v1, v18, -2
    FOLDV s2, v1
    FOLDSAT s2
    vsadd.vv  v1, v16, v17
    FOLDV s2, v1
    FOLDSAT s2
    vsadd.vx  v1, v17, a5
    FOLDV s2, v1
    FOLDSAT s2
    vsadd.vi  v1, v18, -16
    FOLDV s2, v1
    FOLDSAT s2
    vssubu.vv v1, v16, v17
    FOLDV s2, v1
    FOLDSAT s2
    vssubu.vx v1, v17, a5
    FOLDV s2, v1
    FOLDSAT s2
    vssub.vv  v1, v17, v16
    FOLDV s2, v1
    FOLDSAT s2
    vssub.vx  v1, v16, a4
    FOLDV s2, v1
    FOLDSAT s2
    vsetivli zero, 8, e16, m1, ta, mu
    vsadd.vv  v1, v18, v19, v0.t
    FOLDV s2, v1
    FOLDSAT s2
    vssubu.vx v1, v19, a4
    FOLDV s2, v1
    FOLDSAT s2
    vsetivli zero, 4, e32, m1, ta, ma
    vsaddu.vv v1, v18, v19
    FOLDV s2, v1
    FOLDSAT s2
    vsadd.vv  v1, v19, v17
    FOLDV s2, v1
    FOLDSAT s2
    vssub.vx  v1, v19, a4
    FOLDV s2, v1
    FOLDSAT s2
    vsadd.vi  v1, v1, 1          /* nothing saturates: vxsat stays 0 */
    FOLDSAT s2

/* --- s3: averaging and vsmul, each rounding mode --------------------- */
    vsetivli zero, 16, e8, m1, ta, ma
    RM4 s3, v1, vaaddu.vv v1, v16, v17
    RM4 s3, v1, vaadd.vv v1, v16, v17
    RM4 s3, v1, vasubu.vv v1, v16, v17
    RM4 s3, v1, vasub.vx v1, v17, a4
    RM4 s3, v1, vsmul.vv v1, v16, v17
    FOLDSAT s3
    vsetivli zero, 8, e16, m1, ta, ma
    RM4 s3, v1, vaaddu.vx v1, v18, a6
    RM4 s3, v1, vasubu.vx v1, v18, a4
    RM4 s3, v1, vsmul.vv v1, v18, v19
    FOLDSAT s3
    vsetivli zero, 4, e32, m1, ta, ma
    RM4 s3, v1, vaadd.vv v1, v18, v19
    RM4 s3, v1, vasub.vv v1, v19, v18
    RM4 s3, v1, vsmul.vx v1, v18, a4
    FOLDSAT s3
    li   t0, 0x80000000
    vmv.v.x v2, t0
    vsmul.vv v1, v2, v2          /* min * min: the one overflow */
    FOLDV s3, v1
    csrwi vxrm, 2
    csrr t1, vcsr                /* vxrm 2, vxsat 1: 5 */
    MIX  s3, t1
    FOLDSAT s3

/* --- s4: scaling shifts, each rounding mode ------------------------- */
    vsetivli zero, 16, e8, m1, ta, ma
    vid.v v3
    RM4 s4, v1, vssrl.vv v1, v17, v3
    RM4 s4, v1, vssra.vv v1, v17, v3
    RM4 s4, v1, vssrl.vi v1, v16, 3
    RM4 s4, v1, vssra.vx v1, v16, a4
    vsetivli zero, 8, e16, m1, ta, ma
    vid.v v3
    vsll.vi v3, v3, 1
    RM4 s4, v1, vssrl.vv v1, v18, v3
    RM4 s4, v1, vssra.vi v1, v18, 15
    vsetivli zero, 4, e32, m1, ta, ma
    vid.v v3
    vsll.vi v3, v3, 3
    RM4 s4, v1, vssra.vv v1, v19, v3
    RM4 s4, v1, vssrl.vx v1, v19, a5
    RM4 s4, v1, vssrl.vi v1, v19, 0

/* --- s5: narrowing ------------------------------------------------- */
    csrwi vxsat, 0
    vsetivli zero, 16, e8, m1, ta, ma
    vid.v v3
    vnsrl.wv v1, v16, v3
    FOLDV s5, v1
    vnsra.wx v1, v18, a5
    FOLDV s5, v1
    vnsrl.wi v1, v20, 4
    FOLDV s5, v1
    vnsra.wi v1, v20, 9
    FOLDV s5, v1
    RM4 s5, v1, vnclipu.wv v1, v16, v3
    FOLDSAT s5
    RM4 s5, v1, vnclip.wv v1, v18, v3
    FOLDSAT s5
    RM4 s5, v1, vnclipu.wi v1, v20, 3
    FOLDSAT s5
    RM4 s5, v1, vnclip.wx v1, v22, a4
    FOLDSAT s5
    vsetivli zero, 4, e16, mf2, ta, ma
    RM4 s5, v1, vnclip.wi v1, v17, 7
    FOLDSAT s5
    vsetivli zero, 13, e16, m2, ta, mu
    vnclipu.wi v2, v24, 5, v0.t
    FOLDV2 s5, v2
    FOLDSAT s5
    vsetivli zero, 16, e8, m1, ta, ma
    vnsrl.wi v16, v16, 2          /* vd the lowest of vs2's group */
    FOLDV s5, v16
    RELOAD

/* --- s6: widening add and subtract --------------------------------- */
    vsetivli zero, 13, e8, m1, ta, ma
    vwaddu.vv v2, v16, v17
    FOLDV2 s6, v2
    vwadd.vv  v2, v16, v17
    FOLDV2 s6, v2
    vwsubu.vx v2, v16, a4
    FOLDV2 s6, v2
    vwsub.vx  v2, v17, a6
    FOLDV2 s6, v2
    vwaddu.wv v2, v18, v17
    FOLDV2 s6, v2
    vwadd.wx  v2, v20, a4
    FOLDV2 s6, v2
    vwsubu.wv v2, v22, v16
    FOLDV2 s6, v2
    vwsub.wx  v2, v24, a5
    FOLDV2 s6, v2
    vsetivli zero, 11, e16, m2, ta, mu
    vwadd.vv  v4, v16, v18, v0.t
    FOLDV2 s6, v4
    FOLDV2 s6, v6
    vwsubu.wv v4, v24, v20
    FOLDV2 s6, v4
    FOLDV2 s6, v6
    vsetivli zero, 7, e8, mf2, ta, ma
    vwadd.vx  v2, v17, a4
    FOLDV s6, v2
    vwsub.wv  v2, v18, v19
    FOLDV s6, v2
    vsetivli zero, 16, e8, m1, ta, ma
    vmv1r.v   v9, v17
    vwaddu.vv v8, v9, v16          /* a source the top of vd's group */
    FOLDV2 s6, v8
    RELOAD

/* --- s7: widening multiply and multiply-add ------------------------ */
    vsetivli zero, 16, e8, m1, ta, ma
    vwmulu.vv  v2, v16, v17
    FOLDV2 s7, v2
    vwmulsu.vv v2, v16, v17
    FOLDV2 s7, v2
    vwmul.vx   v2, v17, a4
    FOLDV2 s7, v2
    vwmulsu.vx v2, v17, a6
    FOLDV2 s7, v2
    vmv2r.v    v4, v24
    vwmaccu.vv v4, v16, v17
    FOLDV2 s7, v4
    vwmacc.vx  v4, a4, v17
    FOLDV2 s7, v4
    vwmaccus.vx v4, a6, v16
    FOLDV2 s7, v4
    vwmaccsu.vv v4, v17, v16
    FOLDV2 s7, v4
    vsetivli zero, 6, e16, m1, ta, mu
    vwmul.vv   v2, v18, v19, v0.t
    FOLDV2 s7, v2
    vwmaccsu.vx v4, a4, v18
    FOLDV2 s7, v4

/* --- s8: integer extension ------------------------------------------ */
    vsetivli zero, 8, e16, m1, ta, ma
    vzext.vf2 v1, v17
    FOLDV s8, v1
    vsext.vf2 v1, v17
    FOLDV s8, v1
    vsetivli zero, 4, e32, m1, ta, ma
    vzext.vf4 v1, v18
    FOLDV s8, v1
    vsext.vf4 v1, v18
    FOLDV s8, v1
    vsext.vf2 v1, v19
    FOLDV s8, v1
    vsetivli zero, 7, e32, m2, ta, mu
    vsext.vf2 v2, v20, v0.t
    FOLDV2 s8, v2
    vzext.vf4 v2, v21
    FOLDV2 s8, v2

/* --- s9: slides ----------------------------------------------------- */
    /* Each vslideup's vd starts nonzero: what lies below the offset must
     * survive, and a zero there would survive anything. */
    vsetivli zero, 13, e8, m1, ta, ma
    vmv.v.i v1, -7
    li   t0, 3
    vslideup.vx v1, v16, t0
    FOLDV s9, v1
    vslideup.vi v1, v17, 0
    FOLDV s9, v1
    li   t0, 20
    vmv1r.v v1, v18
    vslideup.vx v1, v16, t0      /* past vl: nothing written */
    FOLDV s9, v1
    vslideup.vx v1, v17, a6      /* an offset of 2^32 - 1 */
    FOLDV s9, v1
    vslidedown.vi v1, v16, 3
    FOLDV s9, v1
    vslidedown.vi v1, v16, 15    /* past the group: zeros */
    FOLDV s9, v1
    vslidedown.vx v1, v17, a6
    FOLDV s9, v1
    vmv1r.v v1, v18
    vslidedown.vi v1, v1, 1      /* in place */
    FOLDV s9, v1
    vslide1up.vx v1, v16, a4
    FOLDV s9, v1
    vslide1down.vx v1, v17, a4
    FOLDV s9, v1
    vsetivli zero, 13, e8, m1, ta, mu
    vmv.v.i v1, -7
    vslideup.vi v1, v18, 2, v0.t
    FOLDV s9, v1
    vslidedown.vi v1, v18, 2, v0.t
    FOLDV s9, v1
    vslide1down.vx v1, v19, a5, v0.t
    FOLDV s9, v1
    vsetivli zero, 7, e32, m2, ta, ma
    li   t0, 5
    vslidedown.vx v2, v16, t0
    FOLDV2 s9, v2
    vmv.v.i v2, -7
    vslideup.vi v2, v18, 4
    FOLDV2 s9, v2
    vslide1up.vx v2, v20, a4
    FOLDV2 s9, v2
    vsetivli zero, 3, e16, mf2, ta, ma
    vslidedown.vi v1, v17, 1
    FOLDV s9, v1
    vslide1down.vx v1, v17, a4
    FOLDV s9, v1

/* --- s10: gathers, compress, whole-register moves ------------------- */
    vsetivli zero, 16, e8, m1, ta, ma
    vid.v v3
    vrsub.vi v3, v3, 15          /* reversed */
    vrgather.vv v1, v16, v3
    FOLDV s10, v1
    vrgather.vv v1, v16, v17     /* indices past VLMAX read 0 */
    FOLDV s10, v1
    li   t0, 7
    vrgather.vx v1, v17, t0
    FOLDV s10, v1
    vrgather.vx v1, v17, a6
    FOLDV s10, v1
    vrgather.vi v1, v18, 31
    FOLDV s10, v1
    vrgather.vi v1, v18, 2
    FOLDV s10, v1
    vsetivli zero, 13, e8, m1, ta, mu
    vrgather.vv v1, v19, v3, v0.t
    FOLDV s10, v1
    vsetivli zero, 16, e16, m1, ta, ma
    vid.v v2
    vrsub.vi v2, v2, 9
    vsetivli zero, 16, e8, m1, ta, ma
    vrgatherei16.vv v1, v16, v2  /* EMUL 2: v2-v3 */
    FOLDV s10, v1
    vsetivli zero, 7, e32, m2, ta, ma
    vrgatherei16.vv v4, v16, v18
    FOLDV2 s10, v4
    vsetivli zero, 16, e8, m1, ta, ma
    vcompress.vm v1, v16, v17
    FOLDV s10, v1
    vcompress.vm v1, v18, v0
    FOLDV s10, v1
    vsetivli zero, 3, e32, m4, ta, ma
    vcompress.vm v4, v24, v17
    FOLDV2 s10, v4
    FOLDV2 s10, v6
    vsetivli zero, 0, e8, m1, ta, ma /* vl 0: whole moves still move */
    vmv1r.v v1, v18
    FOLDV s10, v1
    vmv2r.v v2, v20
    FOLDV2 s10, v2
    vmv4r.v v4, v24
    FOLDV2 s10, v4
    FOLDV2 s10, v6
    vmv8r.v v8, v16
    FOLDV2 s10, v8
    FOLDV2 s10, v14
    RELOAD

/* --- a2: widening reductions and segments --------------------------- */
    vsetivli zero, 16, e8, m1, ta, ma
    vwredsumu.vs v1, v16, v18
    FOLDV a2, v1
    vwredsum.vs v1, v16, v18
    FOLDV a2, v1
    vsetivli zero, 13, e16, m2, ta, ma
    vwredsum.vs v1, v16, v19, v0.t
    FOLDV a2, v1
    vwredsumu.vs v1, v18, v19
    FOLDV a2, v1

    la   a0, data_a
    la   t2, outbuf
    vsetivli zero, 7, e8, m1, ta, ma
    vlseg2e8.v v2, (a0)
    FOLDV a2, v2
    FOLDV a2, v3
    vsetivli zero, 5, e16, m1, ta, mu
    vlseg3e16.v v4, (a0), v0.t
    FOLDV a2, v4
    FOLDV a2, v5
    FOLDV a2, v6
    vsetivli zero, 3, e32, m2, ta, ma
    vlseg4e32.v v8, (a0)          /* four fields of two registers */
    FOLDV2 a2, v8
    FOLDV2 a2, v14
    vsetivli zero, 4, e32, m1, ta, ma
    vsseg2e32.v v16, (t2)
    FOLDMEM a2, outbuf, 8
    li   t0, 12
    vsetivli zero, 4, e16, m1, ta, ma
    vlsseg2e16.v v2, (a0), t0
    FOLDV a2, v2
    FOLDV a2, v3
    vsetivli zero, 4, e8, m1, ta, ma
    vid.v v1
    vsll.vi v1, v1, 3
    vsetivli zero, 4, e32, m1, ta, ma
    vluxseg2ei8.v v2, (a0), v1
    FOLDV a2, v2
    FOLDV a2, v3
    vsetivli zero, 4, e16, m1, ta, ma
    vid.v v1
    vsll.vi v1, v1, 4
    vsetivli zero, 4, e8, mf2, ta, ma
    vsuxseg3ei16.v v20, (t2), v1
    FOLDMEM a2, outbuf, 16
    vsetivli zero, 6, e16, m1, ta, ma
    vlseg2e16ff.v v2, (a0)
    csrr t1, vl
    MIX  a2, t1
    FOLDV a2, v2
    FOLDV a2, v3

/* --- a3: a load from a nonzero vstart ------------------------------- */
    RELOAD
    vsetivli zero, 13, e8, m1, ta, ma
    li   t0, 5
    csrw vstart, t0
    la   a0, data_b
    vle8.v v1, (a0)               /* elements 0-4 kept */
    FOLDV a3, v1
    csrr t1, vstart
    MIX  a3, t1
    csrr t1, mstatus
    srli t1, t1, 9
    andi t1, t1, 3                /* VS Dirty */
    MIX  a3, t1

/* --- s11: what is illegal ------------------------------------------- */
    vsetivli zero, 7, e8, m1, ta, ma
    CASE 0, vadc.vvm v0, v2, v3, v0      /* vadc into v0 */
    CASE 1, .word 0x422180D7             /* vadc.vvm with vm 1 */
    vsetivli zero, 3, e32, m1, ta, ma
    CASE 2, vnsrl.wv v1, v2, v3          /* 2 * SEW past ELEN */
    CASE 3, vwadd.vv v2, v4, v6          /* the same */
    vsetivli zero, 7, e8, m1, ta, ma
    CASE 4, vwadd.vv v2, v2, v6          /* vs2 the low half of vd */
    CASE 5, vwadd.vv v2, v3, v6          /* the high half: fine */
    CASE 6, vslideup.vx v2, v2, a4       /* vd == vs2 */
    CASE 7, vslidedown.vx v2, v2, a4     /* fine */
    CASE 8, vrgather.vv v2, v2, v3
    CASE 9, vcompress.vm v2, v2, v0
    CASE 10, vmv2r.v v3, v4              /* vd not aligned to 2 */
    vsetivli zero, 3, e32, m1, ta, ma
    CASE 11, vzext.vf8 v1, v2            /* from SEW 4 */
    vsetivli zero, 7, e8, m1, ta, ma
    li   t0, 1
    csrw vstart, t0
    CASE 12, vadd.vv v1, v2, v3          /* arithmetic from vstart 1 */
    csrw vstart, t0
    la   a0, data_a
    CASE 13, vle8.v v1, (a0)             /* a load from it: fine */
    vsetivli zero, 7, e32, m2, ta, ma
    CASE 14, vlseg8e32.v v8, (a0)        /* 8 fields of 2 registers */
    CASE 15, .word 0x22B60087            /* vlm.v with nf 1 */
    vsetivli zero, 7, e8, m2, ta, ma
    CASE 16, vmadc.vvm v5, v4, v6, v0    /* inside vs2's group */
    vsetivli zero, 7, e8, m1, ta, ma
    CASE 17, vnsrl.wv v3, v2, v1         /* the high half of vs2 */
    CASE 18, vnsrl.wv v2, v2, v1         /* the low half: fine */
    li   t0, 1
    csrw vstart, t0
    CASE 19, vmv.x.s t1, v2              /* vstart 1 */
    csrw vstart, zero
    CASE 20, vrgatherei16.vv v2, v4, v3  /* indices at EMUL 2, v3 */
    vsetivli zero, 7, e8, m1, ta, ma
    CASE 21, vwmaccus.vx v2, a4, v2      /* the low half of vd */

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
    li   t3, 0x80000000
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
