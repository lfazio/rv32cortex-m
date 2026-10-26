/*
 * Enabled and masked again on every iteration; the line is raised, while
 * masked, half way. The instructions after the enable are a hot block by
 * then, so the dispatcher, not the interpreter, decides when it is taken.
 */
    li      t0, 0x02000000
    li      t1, 1
    li      t2, 16
    li      t3, 8
1:  csrsi   mstatus, 8
    addi    a1, a1, 1
    addi    a2, a2, 2
    csrci   mstatus, 8
    addi    t2, t2, -1
    bne     t2, t3, 2f
    sw      t1, 0(t0)
2:  bnez    t2, 1b
    ecall
