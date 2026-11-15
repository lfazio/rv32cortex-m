/*
 * The averages and the clips under each of vxrm's four modes -- rnu,
 * rne, rdn, rod -- a CSR write between them ending each block, so each
 * is translated under its own mode. The values make ties: odd sums for
 * the averages (0x7F + 0x80, -1 + 0), and for the clips a remainder of
 * exactly half, more than half and less, both parities of the quotient,
 * and one that saturates. vxsat is read after each clip and cleared.
 */
    li      t1, 0x80FF7F01
    li      t2, 0x00017FFE
    li      t0, 12
    vsetvli zero, t0, e8, m1, ta, ma
    vmv.v.x v1, t1
    vmv.v.x v2, t2
    vsetvli zero, t0, e32, m2, ta, ma
    li      t3, 0x00018006
    vmv.v.x v4, t3
    li      t3, 0xFFFE7FF9
    vmv.v.x v6, t3
    csrwi   vxrm, 0
    vsetvli zero, t0, e8, m1, ta, ma
    vaaddu.vv v10, v1, v2
    vaadd.vx v11, v1, t2
    vsetvli zero, t0, e16, m1, ta, ma
    vnclip.wi v12, v4, 3
    vnclipu.wi v13, v6, 4
    csrr    a5, vxsat
    csrwi   vxsat, 0
    csrwi   vxrm, 1
    vsetvli zero, t0, e8, m1, ta, ma
    vaaddu.vv v14, v1, v2
    vaadd.vx v15, v1, t2
    vsetvli zero, t0, e16, m1, ta, ma
    vnclip.wi v16, v4, 3
    vnclipu.wi v17, v6, 4
    csrr    a6, vxsat
    csrwi   vxsat, 0
    csrwi   vxrm, 2
    vsetvli zero, t0, e8, m1, ta, ma
    vaaddu.vv v18, v1, v2
    vaadd.vx v19, v1, t2
    vsetvli zero, t0, e16, m1, ta, ma
    vnclip.wi v20, v4, 3
    vnclipu.wi v21, v6, 4
    csrr    a7, vxsat
    csrwi   vxsat, 0
    csrwi   vxrm, 3
    vsetvli zero, t0, e8, m1, ta, ma
    vaaddu.vv v22, v1, v2
    vaadd.vx v23, v1, t2
    vsetvli zero, t0, e32, m1, ta, ma
    vaadd.vv v24, v4, v6
    vsetvli zero, t0, e16, m1, ta, ma
    vnclip.wi v25, v4, 3
    vnclipu.wi v26, v6, 4
    vnclip.wi v27, v4, 0
    csrr    s2, vxsat
    ecall
