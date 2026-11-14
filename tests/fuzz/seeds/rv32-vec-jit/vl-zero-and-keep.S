/*
 * vl 0, and vsetvli zero, zero keeping it: an operation writes nothing,
 * then one under a vtype of the same VLMAX keeps the old vl -- and one
 * of another VLMAX, which the specification reserves and the helper
 * answers with vill.
 */
    vsetvli zero, zero, e8, m1, ta, ma   /* vill: no vl to keep yet */
    li      t0, 0
    vsetvli zero, t0, e8, m1, ta, ma
    vadd.vv v1, v2, v3
    li      t0, 6
    vsetvli zero, t0, e16, m2, ta, ma
    vadd.vv v2, v4, v6
    vsetvli zero, zero, e32, m4, ta, ma  /* VLMAX 16 again: keep 6 */
    vxor.vv v4, v8, v12
    vsetvli zero, zero, e8, m1, ta, ma   /* VLMAX 16: keep */
    vsub.vx v1, v2, t0
    vsetvli zero, zero, e8, m4, ta, ma   /* VLMAX 64: vill */
    vadd.vv v4, v8, v12
    ecall
