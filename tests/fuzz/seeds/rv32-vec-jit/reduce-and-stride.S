/*
 * Reductions and strided accesses, as a dot product, a maximum and a
 * gather compile: every single-width reduction at e8, e16 and e32,
 * LMUL 1 and 4, masked and not, vd overlapping vs1 and vs2 and v0 under
 * a mask; the widening sums from e8 and e16; vl 0, where vd keeps every
 * byte. Then vlse and vsse with strides of 16, -12, 0 and 3 at each
 * width, from a0-a2, which point into data RAM from reset -- and last,
 * since a trap ends the run, one from far outside it, which faults.
 * (The widening sum from e32, reserved, is wide-sum-e32.S's.)
 */
    li      t0, 13
    vsetvli zero, t0, e8, m1, ta, ma
    vredsum.vs v1, v2, v3
    vredand.vs v4, v5, v6
    vredor.vs v7, v8, v9
    vredxor.vs v10, v11, v12
    vredminu.vs v13, v14, v15
    vredmin.vs v16, v17, v18
    vredmaxu.vs v19, v20, v21
    vredmax.vs v22, v23, v23
    vwredsumu.vs v24, v2, v3
    vwredsum.vs v25, v5, v6
    li      t1, 0x0F0F0F0F
    vmslt.vx v0, v2, t1
    vredsum.vs v0, v2, v3, v0.t
    vredmax.vs v26, v26, v4, v0.t
    li      t0, 29
    vsetvli zero, t0, e16, m4, ta, ma
    vredsum.vs v1, v4, v1
    vredmin.vs v2, v8, v3
    vredmaxu.vs v3, v12, v2, v0.t
    vwredsum.vs v5, v16, v6
    vwredsumu.vs v6, v20, v7, v0.t
    vsetivli zero, 3, e32, m1, ta, ma
    vredsum.vs v1, v2, v3
    vredminu.vs v4, v4, v5
    vredmax.vs v6, v7, v6, v0.t
    vsetivli zero, 0, e32, m1, ta, ma
    vredsum.vs v11, v12, v13             /* vl 0: vd as it was */
    li      t0, 8
    vsetvli zero, t0, e32, m2, ta, ma
    li      t2, 16
    vlse32.v v2, (a0), t2
    li      t2, -12
    vlse32.v v4, (a1), t2
    vadd.vv v6, v2, v4
    vsse32.v v6, (a2), t2
    li      t0, 17
    vsetvli zero, t0, e8, m2, ta, ma
    li      t2, 3
    vlse8.v v8, (a0), t2
    vsse8.v v8, (a1), zero
    li      t0, 9
    vsetvli zero, t0, e16, m2, ta, ma
    vlse16.v v10, (a2), t2
    li      a3, 0x7FFFFFF0
    vlse16.v v12, (a3), t2               /* outside RAM: faults */
    ecall
