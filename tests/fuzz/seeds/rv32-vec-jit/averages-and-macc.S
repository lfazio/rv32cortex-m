/*
 * The averaging adds, lowered only under vxrm rnu -- which the block is
 * specialised on -- and the multiply-adds, which read vd: a loop that
 * changes vxrm on its second trip, so a block built under rnu must not
 * be entered under rdn, and vmacc/vnmsac/vmadd/vnmsub at e16 and e32,
 * .vv and .vx, accumulating into registers the loop does not reset.
 */
    li      t4, 2
    li      t1, 0xFFFF8001
1:  csrwi   vxrm, 0
    bne     t4, zero, 2f
    csrwi   vxrm, 2
2:  li      t0, 13
    vsetvli zero, t0, e8, m1, ta, ma
    vaaddu.vv v1, v2, v3
    vaadd.vx v4, v1, t1
    vsetvli zero, t0, e16, m2, ta, ma
    vaadd.vv v2, v4, v6
    vmacc.vv v8, v4, v6
    vnmsac.vx v10, t1, v8
    vsetivli zero, 4, e32, m1, ta, ma
    vaaddu.vx v1, v2, t1
    vmadd.vv v12, v13, v14
    vnmsub.vx v15, t1, v12
    vmacc.vx v16, t0, v15
    addi    t4, t4, -1
    bge     t4, zero, 1b
    ecall
