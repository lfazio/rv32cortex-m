/*
 * The narrowing shifts and clips: from e32 to e16 and e16 to e8, LMUL 1
 * and 2, .wx and .wi with counts of 0, 1 and past the width, vl short so
 * a tail element that would saturate must not set vxsat -- read after
 * each clip and cleared -- vd at vs2's own start, which the
 * specification allows, and the rdn clip the block leaves to the helper.
 */
    li      t1, 0x7FFF0123
    vsetivli zero, 4, e32, m2, ta, ma
    vmv.v.x v8, t1
    vadd.vx v8, v8, t1
    li      t0, 3
    vsetvli zero, t0, e16, m1, ta, ma
    vnclip.wi v1, v8, 0
    csrr    a5, vxsat
    csrwi   vxsat, 0
    vnclip.wi v2, v8, 4
    vnclipu.wx v3, v8, t0
    vnsra.wi v4, v8, 31
    vnsrl.wx v5, v8, t1
    li      t0, 13
    vsetvli zero, t0, e8, m1, ta, ma
    vnclip.wx v6, v10, t0
    vnclipu.wi v7, v12, 1
    vsetvli zero, t0, e16, m2, ta, ma
    vnclip.wi v8, v8, 2                  /* vd at vs2's start */
    csrr    a6, vxsat
    csrwi   vxrm, 2
    vnclip.wi v12, v16, 3                /* rdn: the helper */
    ecall
