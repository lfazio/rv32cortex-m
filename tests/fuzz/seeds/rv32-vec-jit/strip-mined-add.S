/*
 * A strip-mined loop as compiled vector code is: vsetvli from the count
 * at the head -- which is what lets the block lower what follows -- two
 * loads, the arithmetic, a store, the pointers stepped by vl. 37
 * elements at e32 m2: four trips of 8, then 5 with a tail. a0-a2 point
 * into data RAM from reset.
 */
    li      t0, 37
1:  vsetvli t1, t0, e32, m2, ta, ma
    vle32.v v2, (a0)
    vle32.v v4, (a1)
    vadd.vv v6, v2, v4
    vsub.vv v8, v6, v2
    vxor.vv v6, v6, v8
    vmul.vv v8, v4, v6
    vmaxu.vv v6, v6, v8
    vmin.vx v8, v6, t0
    vsll.vi v6, v8, 3
    vsra.vx v8, v6, t1
    vrsub.vi v6, v8, -5
    vse32.v v6, (a2)
    slli    t2, t1, 2
    add     a0, a0, t2
    add     a1, a1, t2
    add     a2, a2, t2
    sub     t0, t0, t1
    bnez    t0, 1b
    ecall
