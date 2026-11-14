/*
 * Every width and grouping the lowering has: e8 m1 with vl 13, e16 m4,
 * e8 mf2, e32 m8 -- each a vsetvli then operations on its own groups,
 * register (.vv), scalar (.vx) and immediate (.vi) forms, with the
 * scalar's high bits set to see that it is taken at the element width.
 */
    li      t0, 13
    li      t1, 0xFFFF8081
    vsetvli zero, t0, e8, m1, ta, ma
    vadd.vx v1, v2, t1
    vminu.vv v3, v1, v2
    vmax.vx v4, v3, t1
    vand.vi v5, v4, -2
    vor.vv v6, v5, v1
    vmv.v.x v7, t1
    vmv.v.i v8, -9
    vmv.v.v v9, v6
    vsetivli zero, 29, e16, m4, ta, ma
    vadd.vv v12, v16, v20
    vmul.vx v16, v12, t1
    vsrl.vx v20, v16, t1
    vmin.vv v24, v20, v12
    vsetivli zero, 5, e8, mf2, ta, ma
    vsub.vx v1, v2, t1
    vxor.vi v3, v1, 7
    li      t2, 1000
    vsetvli t3, t2, e32, m8, ta, ma
    vadd.vi v8, v16, 15
    vmaxu.vx v24, v8, t3
    vsetvli zero, zero, e32, m8, ta, ma
    vor.vv v16, v24, v8
    ecall
