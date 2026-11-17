/*
 * The vector unit's floating point (Zve32f) under the JIT, where every
 * one is the helper: FS turned on, frm changed between blocks, special
 * values made by integer moves, the arithmetic, a fused multiply-add,
 * compares into v0 and an operation under it, conversions both ways and
 * at SEW 16, a reduction, vfmv.f.s into an f register, fflags read back
 * -- and last an invalid frm, which every vector FP instruction refuses.
 */
    li      t0, 0x2000                   /* FS Initial */
    csrs    mstatus, t0
    li      t1, 0x7F800001               /* a signalling NaN */
    li      t2, 0x3FC00000               /* 1.5 */
    fmv.w.x ft0, t2
    li      t0, 9
    vsetvli zero, t0, e32, m2, ta, mu
    vmv.v.x v2, t2
    vmv.v.x v4, t1
    vfadd.vv v6, v2, v4
    vfmul.vf v8, v2, ft0
    vfmacc.vv v8, v2, v6
    vmflt.vf v0, v8, ft0
    vfsub.vv v10, v8, v2, v0.t
    csrwi   frm, 2
    vfcvt.x.f.v v12, v8
    vfcvt.f.x.v v14, v12
    vfredosum.vs v16, v8, v2
    vfmv.f.s ft1, v16
    vsetivli zero, 7, e16, m1, ta, ma
    vfncvt.x.f.w v18, v8
    vfwcvt.f.x.v v20, v18
    csrr    a5, fflags
    vsetivli zero, 4, e32, m1, ta, ma
    vfrec7.v v22, v2
    vfrsqrt7.v v23, v2
    csrwi   frm, 6                       /* invalid: refused */
    vfadd.vv v24, v2, v2
    ecall
