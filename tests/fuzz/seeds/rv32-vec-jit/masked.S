/*
 * Masked operations (v0.t) and vmerge, as if-converted loops compile:
 * a compare writes v0, then arithmetic under it at e8, e16 and e32,
 * LMUL 1 and 4, .vv, .vx and .vi; vmerge of every form; a mask of all
 * ones, of none, alternating; and a masked write of v0 itself, which is
 * reserved and must reach the helper's illegal instruction.
 */
    li      t1, 0x5A5A5A5A
    li      t0, 13
    vsetvli zero, t0, e8, m1, ta, mu
    vmslt.vx v0, v2, t1
    vadd.vv v1, v2, v3, v0.t
    vsub.vx v4, v5, t1, v0.t
    vrsub.vi v6, v7, 3, v0.t
    vmaxu.vv v8, v9, v10, v0.t
    vmerge.vvm v11, v12, v13, v0
    vmerge.vxm v14, v15, t1, v0
    vmerge.vim v16, v17, -7, v0
    li      t0, 29
    vsetvli zero, t0, e16, m4, ta, mu
    vmseq.vi v0, v8, 0
    vmul.vv v4, v8, v12, v0.t
    vsra.vx v16, v20, t0, v0.t
    vsetivli zero, 3, e32, m1, ta, mu
    vmv.v.i v0, -1
    vxor.vv v1, v2, v3, v0.t
    vmv.v.i v0, 0
    vor.vx v4, v5, t1, v0.t
    vmv.v.i v0, 5
    vmacc.vv v6, v7, v8, v0.t
    vadd.vv v0, v2, v3, v0.t             /* masked write of v0: reserved */
    ecall
