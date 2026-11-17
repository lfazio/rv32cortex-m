/* SPDX-License-Identifier: Apache-2.0 */
/*
 * vec-fp.S - the vector unit's single-precision floating point (Zve32f,
 * rv_vec.c's opf through rv_fpu.c) against Spike: every operation on 32
 * pairs of special values -- zeros of both signs, normals, subnormals,
 * infinities, quiet and signalling NaNs, the edges of the integer
 * ranges -- under each rounding mode where it rounds, with fflags read
 * after each group; masks, the scalar moves, the reductions and the
 * slides; and what is refused: SEW 16, FS off, frm invalid, the widening
 * and half-precision forms.
 *
 * probe-march: rv32imafdc_zicsr_zve32f
 * probe-models: spike
 * probe-spike-isa: rv32imafdc_zicsr_zve32f_zvl128b
 * probe-regs: s1 s2 s3 s4 s5 s6 s7 s8 s9 s10 s11 a1
 *
 *   build/vfp-host built -DRV32_EXT_ZVE32X=ON -DRV32_EXT_ZVE32F=ON:
 *   scripts/run-probe.py --emu build/vfp-host/emu-host \
 *       tests/arch-test/probes/vec-fp.S
 *
 *   s1  vfadd, vfsub, vfrsub, vfmul, vfdiv, vfrdiv, .vv and .vf, rne
 *   s2  the same under rtz, rdn, rup and rmm
 *   s3  vfmin, vfmax, the sign injections, vfsqrt, vfclass
 *   s4  vfmacc, vfnmacc, vfmsac, vfnmsac, vfmadd, vfnmadd, vfmsub,
 *       vfnmsub, .vv and .vf
 *   s5  the compares into masks, .vv and .vf
 *   s6  vfcvt (all six), vfwcvt.f.x(u).v and vfncvt.x(u).f.w (and rtz)
 *       at SEW 16, under rne and rdn
 *   s7  vfredusum, vfredosum, vfredmin, vfredmax; vfmv.f.s, vfmv.s.f,
 *       vfmv.v.f, vfmerge, vfslide1up, vfslide1down
 *   s8  masked (v0.t) arithmetic, compares and conversions
 *   s9  vfrec7, vfrsqrt7 on the data (vec-fp2.S has every entry)
 *   s10 mstatus.FS after the vector instructions that do and do not
 *       raise a flag, and after vfmv.f.s
 *   s11 illegal instruction: a bit per case that raised it
 *   a1  set once every case has run
 */
    .option norvc
    .section .text.init
    .globl rvtest_entry_point
rvtest_entry_point:
    la   t0, mtrap
    csrw mtvec, t0
    li   t0, (1 << 13) | (1 << 9)  /* FS and VS Initial: Spike resets them Off */
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
    csrw fflags, zero
    csrwi frm, 0

/*
 * acc = (rotl(acc, 5) ^ x) * an odd constant. Without the multiply the
 * fold is linear over XOR: a constant error in every element of a run
 * cancels -- over 32 words whenever it flips an even number of bits, over
 * 128 always -- and vec-fp2.S's s1 and s2 folded equal from different
 * results.
 */
.macro MIX acc, x
    slli t3, \acc, 5
    srli \acc, \acc, 27
    or   \acc, \acc, t3
    xor  \acc, \acc, \x
    li   t3, 0x9E3779B1          /* not linear: see the note above */
    mul  \acc, \acc, t3
.endm

.macro FOLDBUF acc, n
    la   t5, vbuf
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
    FOLDBUF \acc, 4
.endm

/* Fold the group at v (8 registers) into acc. */
.macro FOLDV8 acc, v
    la   t5, vbuf
    vs8r.v \v, (t5)
    FOLDBUF \acc, 32
.endm

/* fflags into acc, then cleared. */
.macro FFL acc
    csrr t1, fflags
    MIX  \acc, t1
    csrw fflags, zero
.endm

/* A case that may trap: s0 its number, s11's bit set if it was illegal. */
.macro CASE n, insn:vararg
    li   s0, \n
    \insn
.endm

/* The data: a in v8-v15, b in v16-v23, 32 elements at e32 m8. */
.macro LOAD
    li   t0, 32
    vsetvli zero, t0, e32, m8, ta, ma
    la   a0, fa
    vle32.v v8, (a0)
    la   a0, fb
    vle32.v v16, (a0)
.endm

    /* Scalar operands for .vf: 1.5, a signalling NaN, -0, +inf. */
    li   t0, 0x3FC00000
    fmv.w.x ft0, t0
    li   t0, 0x7F800001
    fmv.w.x ft1, t0
    li   t0, 0x80000000
    fmv.w.x ft2, t0
    li   t0, 0x7F800000
    fmv.w.x ft3, t0
    LOAD

/* --- s1: the arithmetic, rne ----------------------------------------- */
.macro ARITH acc
    vfadd.vv  v24, v8, v16
    FOLDV8 \acc, v24
    vfadd.vf  v24, v8, ft0
    FOLDV8 \acc, v24
    vfsub.vv  v24, v8, v16
    FOLDV8 \acc, v24
    vfsub.vf  v24, v8, ft3
    FOLDV8 \acc, v24
    vfrsub.vf v24, v8, ft0
    FOLDV8 \acc, v24
    vfmul.vv  v24, v8, v16
    FOLDV8 \acc, v24
    vfmul.vf  v24, v8, ft2
    FOLDV8 \acc, v24
    vfdiv.vv  v24, v8, v16
    FOLDV8 \acc, v24
    vfdiv.vf  v24, v8, ft0
    FOLDV8 \acc, v24
    vfrdiv.vf v24, v8, ft0
    FOLDV8 \acc, v24
    FFL  \acc
.endm
    ARITH s1
    vfadd.vf  v24, v8, ft1         /* a signalling NaN scalar */
    FOLDV8 s1, v24
    FFL  s1

/* --- s2: the same under the other rounding modes ---------------------- */
    csrwi frm, 1
    ARITH s2
    csrwi frm, 2
    ARITH s2
    csrwi frm, 3
    ARITH s2
    csrwi frm, 4
    ARITH s2
    csrwi frm, 0

/* --- s3: min, max, sign injection, sqrt, class ------------------------ */
    vfmin.vv   v24, v8, v16
    FOLDV8 s3, v24
    vfmax.vv   v24, v8, v16
    FOLDV8 s3, v24
    FFL  s3
    vfmin.vf   v24, v8, ft1
    FOLDV8 s3, v24
    vfmax.vf   v24, v8, ft2
    FOLDV8 s3, v24
    FFL  s3
    vfsgnj.vv  v24, v8, v16
    FOLDV8 s3, v24
    vfsgnjn.vv v24, v8, v16
    FOLDV8 s3, v24
    vfsgnjx.vf v24, v8, ft2
    FOLDV8 s3, v24
    vfsqrt.v   v24, v8
    FOLDV8 s3, v24
    FFL  s3
    csrwi frm, 3
    vfsqrt.v   v24, v16
    FOLDV8 s3, v24
    csrwi frm, 0
    FFL  s3
    vfclass.v  v24, v8
    FOLDV8 s3, v24
    vfclass.v  v24, v16
    FOLDV8 s3, v24
    FFL  s3

/* --- s4: the fused multiply-adds -------------------------------------- */
.macro FMA op, acc
    vmv8r.v v24, v16
    \op\().vv v24, v8, v16
    FOLDV8 \acc, v24
    vmv8r.v v24, v8
    \op\().vf v24, ft0, v16
    FOLDV8 \acc, v24
    FFL  \acc
.endm
    FMA vfmacc, s4
    FMA vfnmacc, s4
    FMA vfmsac, s4
    FMA vfnmsac, s4
    FMA vfmadd, s4
    FMA vfnmadd, s4
    FMA vfmsub, s4
    FMA vfnmsub, s4
    csrwi frm, 2
    FMA vfmacc, s4
    FMA vfnmsub, s4
    csrwi frm, 0

/* --- s5: the compares ------------------------------------------------- */
.macro CMP op, acc
    vmv.v.i v0, 0
    \op v0, v8, v16
    FOLDV \acc, v0
    FFL  \acc
.endm
.macro CMPF op, f, acc
    vmv.v.i v0, 0
    \op v0, v8, \f
    FOLDV \acc, v0
    FFL  \acc
.endm
    CMP vmfeq.vv, s5
    CMP vmfne.vv, s5
    CMP vmflt.vv, s5
    CMP vmfle.vv, s5
    CMPF vmfeq.vf, ft0, s5
    CMPF vmfne.vf, ft1, s5
    CMPF vmflt.vf, ft2, s5
    CMPF vmfle.vf, ft3, s5
    CMPF vmfgt.vf, ft0, s5
    CMPF vmfge.vf, ft2, s5
    CMPF vmfgt.vf, ft1, s5

/* --- s6: the conversions ---------------------------------------------- */
    vfcvt.xu.f.v v24, v8
    FOLDV8 s6, v24
    FFL  s6
    vfcvt.x.f.v v24, v8
    FOLDV8 s6, v24
    FFL  s6
    vfcvt.rtz.xu.f.v v24, v16
    FOLDV8 s6, v24
    FFL  s6
    vfcvt.rtz.x.f.v v24, v16
    FOLDV8 s6, v24
    FFL  s6
    la   a0, ints
    vle32.v v24, (a0)
    vfcvt.f.xu.v v24, v24
    FOLDV8 s6, v24
    FFL  s6
    vle32.v v24, (a0)
    vfcvt.f.x.v v24, v24
    FOLDV8 s6, v24
    FFL  s6
    csrwi frm, 2
    vfcvt.x.f.v v24, v8
    FOLDV8 s6, v24
    vle32.v v24, (a0)
    vfcvt.f.x.v v24, v24
    FOLDV8 s6, v24
    FFL  s6
    csrwi frm, 0
    /* SEW 16: int16 to single, single to int16 */
    vsetivli zero, 16, e16, m2, ta, ma
    la   a0, ints
    vle16.v v4, (a0)
    vfwcvt.f.x.v v24, v4
    FOLDV8 s6, v24
    vfwcvt.f.xu.v v24, v4
    FOLDV8 s6, v24
    FFL  s6
    vsetivli zero, 16, e16, m2, ta, ma
    vmv8r.v v24, v8
    vfncvt.x.f.w v4, v24
    FOLDV s6, v4
    FOLDV s6, v5
    FFL  s6
    vfncvt.xu.f.w v4, v24
    FOLDV s6, v4
    FOLDV s6, v5
    FFL  s6
    vfncvt.rtz.x.f.w v4, v16
    FOLDV s6, v4
    FOLDV s6, v5
    FFL  s6
    vfncvt.rtz.xu.f.w v4, v16
    FOLDV s6, v4
    FOLDV s6, v5
    FFL  s6
    csrwi frm, 2
    vfncvt.x.f.w v4, v16
    FOLDV s6, v4
    FOLDV s6, v5
    FFL  s6
    csrwi frm, 0
    LOAD

/* --- s7: reductions, moves, merge, slides ------------------------------ */
    li   t0, 32
    vsetvli zero, t0, e32, m8, ta, ma
    vfmv.s.f v4, ft0
    vfredusum.vs v5, v8, v4
    FOLDV s7, v5
    FFL  s7
    vfredosum.vs v5, v16, v4
    FOLDV s7, v5
    FFL  s7
    vfredmin.vs v5, v8, v4
    FOLDV s7, v5
    FFL  s7
    vfredmax.vs v5, v16, v4
    FOLDV s7, v5
    FFL  s7
    vsetivli zero, 12, e32, m4, ta, ma
    vfredosum.vs v5, v16, v4       /* the ordinary values only */
    FOLDV s7, v5
    vfredusum.vs v5, v20, v4
    FOLDV s7, v5
    FFL  s7
    li   t0, 32
    vsetvli zero, t0, e32, m8, ta, ma
    vfmv.f.s ft4, v16
    fmv.x.w t1, ft4
    MIX  s7, t1
    vfmv.v.f v24, ft2
    FOLDV8 s7, v24
    li   t0, 0x5A5AA5A5
    vsetivli zero, 4, e32, m1, ta, ma
    vmv.v.x v0, t0
    li   t0, 32
    vsetvli zero, t0, e32, m8, ta, ma
    vfmerge.vfm v24, v8, ft0, v0
    FOLDV8 s7, v24
    vfslide1up.vf v24, v8, ft3
    FOLDV8 s7, v24
    vfslide1down.vf v24, v16, ft1
    FOLDV8 s7, v24
    FFL  s7
    vsetivli zero, 0, e32, m1, ta, ma
    vfmv.s.f v4, ft3               /* vl 0: nothing */
    FOLDV s7, v4

/* --- s8: masked ------------------------------------------------------- */
    li   t0, 32
    vsetvli zero, t0, e32, m8, ta, mu
    li   t0, 0x936C5AA5
    vsetivli zero, 4, e32, m1, ta, mu
    vmv.v.x v0, t0
    li   t0, 32
    vsetvli zero, t0, e32, m8, ta, mu
    vmv8r.v v24, v16
    vfadd.vv v24, v8, v16, v0.t
    FOLDV8 s8, v24
    vfsqrt.v v24, v8, v0.t
    FOLDV8 s8, v24
    vfmacc.vf v24, ft0, v16, v0.t
    FOLDV8 s8, v24
    vfcvt.x.f.v v24, v8, v0.t
    FOLDV8 s8, v24
    FFL  s8
    vmv.v.i v4, 0
    vmflt.vv v4, v8, v16, v0.t
    FOLDV s8, v4
    vfredosum.vs v5, v8, v16, v0.t
    FOLDV s8, v5
    FFL  s8

/* --- s9: the estimates ------------------------------------------------- */
    vfrec7.v v24, v8
    FOLDV8 s9, v24
    vfrec7.v v24, v16
    FOLDV8 s9, v24
    FFL  s9
    vfrsqrt7.v v24, v8
    FOLDV8 s9, v24
    vfrsqrt7.v v24, v16
    FOLDV8 s9, v24
    FFL  s9

/* --- s10: mstatus.FS -------------------------------------------------- */
.macro FSCLEAN
    li   t0, 0x4000
    csrs mstatus, t0
    li   t0, 0x2000
    csrc mstatus, t0
.endm
.macro FSREAD acc
    csrr t1, mstatus
    srli t1, t1, 13
    andi t1, t1, 3
    MIX  \acc, t1
.endm
    csrw fflags, zero
    FSCLEAN
    vfadd.vv v24, v16, v16          /* ordinary values: no flag */
    FSREAD s10
    vfadd.vv v24, v8, v16           /* NaNs among them: NV */
    FSREAD s10
    FSCLEAN
    vfmv.f.s ft5, v8                /* writes an f register */
    FSREAD s10
    FSCLEAN
    vfmv.s.f v4, ft0
    FSREAD s10
    csrr t1, fflags
    MIX  s10, t1

/* --- s11: what is refused ---------------------------------------------- */
    vsetivli zero, 4, e16, m1, ta, ma
    CASE 0, vfadd.vv v24, v8, v16    /* SEW 16: half precision */
    vsetivli zero, 4, e32, m1, ta, ma
    CASE 1, vfwadd.vv v24, v8, v16   /* widening: a 64-bit result */
    CASE 2, vfwcvt.f.x.v v24, v8     /* SEW 32 to a 64-bit float */
    vsetivli zero, 4, e16, m1, ta, ma
    CASE 3, vfncvt.f.f.w v4, v8      /* to half precision */
    CASE 4, vfwcvt.f.f.v v24, v4     /* from half precision */
    vsetivli zero, 4, e32, m1, ta, ma
    CASE 5, vfadd.vv v0, v8, v16, v0.t  /* masked, vd v0 */
    CASE 6, vfslide1up.vf v8, v8, ft0   /* vd overlapping vs2 */
    csrwi frm, 5
    CASE 7, vfadd.vv v24, v8, v16    /* frm invalid */
    CASE 8, vfmv.f.s ft6, v8         /* rounds nothing, refused anyway */
    CASE 9, vfmv.s.f v4, ft0
    CASE 10, vmfeq.vv v4, v8, v16    /* a compare reads it too */
    csrwi frm, 0
    li   t0, 0x2000                  /* FS Off */
    csrc mstatus, t0
    li   t0, 0x4000
    csrc mstatus, t0
    CASE 11, vfadd.vv v24, v8, v16
    CASE 12, vfmv.f.s ft6, v8
    li   t0, 0x2000
    csrs mstatus, t0
    CASE 13, vfredusum.vs v4, v8, v16
    CASE 14, .word 0xA0203353        /* fle.s funct3 3: reserved */
    li   t0, 0x7FC00000
    fmv.w.x ft7, t0
    CASE 15, .word 0xA003B353        /* the same with a NaN operand */

    li   a1, 1
    la   t0, tohost
    li   t1, 1
    sw   t1, 0(t0)
2:  j    2b

/* Any trap: s11's bit s0 for an illegal instruction, s0 + 16 otherwise. */
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
fa:
    .word 0x00000000, 0x80000000, 0x3F800000, 0xBF800000
    .word 0x3FC00000, 0x40000000, 0x3F000000, 0x7F7FFFFF
    .word 0x00800000, 0x00000001, 0x7F800000, 0xFF800000
    .word 0x7FC00000, 0x7F800001, 0x7FC12345, 0x3EAAAAAB
    .word 0x46FFFE00, 0xC7000000, 0x47000000, 0x477FFF00
    .word 0x4F000000, 0xCF000000, 0x4F800000, 0x3F7FFFFF
    .word 0x40490FDB, 0xC0490FDB, 0x3F000001, 0x3FA00000
    .word 0x40200000, 0xC0200000, 0x007FFFFF, 0x80000001
fb:
    .word 0x3F800000, 0x00000000, 0x80000000, 0x40400000
    .word 0x7F800000, 0xFF800000, 0x40000000, 0x3F000000
    .word 0x7F7FFFFF, 0x3EAAAAAB, 0x3F7FFFFF, 0x0DA24260
    .word 0x7149F2CA, 0x40E00000, 0xC1200000, 0x00800001
    .word 0x3FC00001, 0xBFC00000, 0x4B000001, 0xCB7FFFFF
    .word 0x3F400000, 0xBF400000, 0x3EFFFFFF, 0x3F200000
    .word 0x47000080, 0xC7000080, 0x46FFFF00, 0x477FFF80
    .word 0x00400000, 0x80400000, 0x42F60000, 0x3DCCCCCD
ints:
    .word 0x00000000, 0x00000001, 0xFFFFFFFF, 0x7FFFFFFF
    .word 0x80000000, 0x00FFFFFF, 0x01000001, 0xFF000001
    .word 0x7FFF8000, 0x00007FFF, 0x0000FFFF, 0xFFFF8000
    .word 0x12345679, 0xEDCBA987, 0x00000003, 0x80000001
    .word 0x7FFFFFC0, 0x7FFFFFBF, 0x00010000, 0xFFFE0001
    .word 0x5A5A5A5A, 0xA5A5A5A5, 0x00000100, 0x00800001
    .word 0x00000080, 0x0000FF80, 0x8000FFFF, 0x7FFF0001
    .word 0x00000040, 0x3F800000, 0x40000000, 0xC0000000
vbuf:
    .space 512
