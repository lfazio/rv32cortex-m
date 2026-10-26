/* A software interrupt raised while masked, taken when mstatus.MIE is set. */
    li      t0, 0x02000000      /* CLINT msip */
    li      t1, 1
    sw      t1, 0(t0)
    lw      a0, 0(t0)
    csrsi   mstatus, 8          /* taken here, on both backends */
    addi    a1, a1, 1
    ecall
