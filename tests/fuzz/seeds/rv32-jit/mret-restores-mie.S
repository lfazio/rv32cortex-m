/* MRET with MPIE set restores MIE, and the pending interrupt is taken. */
    li      t0, 0x02000000
    li      t1, 1
    sw      t1, 0(t0)
    la      t2, 1f
    csrw    mepc, t2
    li      t3, 0x1880          /* MPP = M, MPIE */
    csrs    mstatus, t3
    mret
1:  addi    a1, a1, 1
    ecall
