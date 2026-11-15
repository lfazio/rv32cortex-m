/*
 * The widening multiply-adds, which write a vd of twice the width in
 * twice the registers: every sign at e8 and e16, LMUL 1, 2 and 4, .vv
 * and .vx, with vl short of the group so the wide tail is kept -- and the
 * forms the block must leave to the helper: a source that is vd's high
 * half, which the specification allows, and a fractional LMUL.
 */
    li      t1, 0xFFFF8F7F
    li      t0, 13
    vsetvli zero, t0, e8, m1, ta, ma
    vwmaccu.vv v2, v16, v17
    vwmacc.vx v4, t1, v18
    vwmaccsu.vv v6, v19, v20
    vwmaccus.vx v8, t1, v21
    li      t0, 11
    vsetvli zero, t0, e16, m2, ta, ma
    vwmacc.vv v8, v16, v18
    vwmaccu.vx v12, t1, v20
    vsetvli zero, t0, e16, m4, ta, ma
    vwmacc.vv v24, v16, v20
    vsetivli zero, 6, e16, m1, ta, ma
    vwmacc.vv v2, v3, v20                /* vs1 is vd's high half */
    vsetivli zero, 3, e16, mf2, ta, ma
    vwmacc.vx v1, t1, v2                 /* fractional: the helper */
    ecall
