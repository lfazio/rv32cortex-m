/*
 * What the block cannot lower, between what it can: a masked operation,
 * vsetvl from a register (after which the block knows no vtype), a
 * compare into a mask and a reduction -- each the helper -- and lowered
 * operations on either side, which must see what the helper left.
 */
    li      t0, 11
    vsetvli zero, t0, e16, m1, ta, mu
    vadd.vv v1, v2, v3
    vmseq.vv v0, v1, v2
    vadd.vv v4, v1, v3, v0.t
    vsub.vv v5, v4, v1
    li      t1, 0x0A                 /* e32, m4 */
    li      t2, 9
    vsetvl  t3, t2, t1
    vadd.vv v8, v12, v16
    vredsum.vs v1, v8, v2
    vsetvli zero, t2, e32, m4, ta, ma
    vmul.vv v8, v12, v16
    vsetivli zero, 3, e32, m1, ta, ma
    vmacc.vv v20, v21, v22
    vadd.vv v20, v20, v21
    ecall
