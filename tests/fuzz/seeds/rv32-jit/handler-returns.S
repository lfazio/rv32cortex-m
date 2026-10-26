/*
 * A handler of the program's own: it clears msip and returns, and the
 * program goes round again, so interrupts are taken from translated
 * code's successors and returned into it.
 */
    la      t2, handler
    csrw    mtvec, t2
    li      t0, 0x02000000
    li      t1, 1
    li      t3, 6
1:  sw      t1, 0(t0)           /* masked: MIE is clear */
    addi    a1, a1, 1
    csrsi   mstatus, 8          /* taken; the handler returns after it */
    csrci   mstatus, 8
    addi    t3, t3, -1
    bnez    t3, 1b
    ecall
    .balign 4
handler:
    sw      zero, 0(t0)
    addi    a2, a2, 1
    csrr    a3, mcause
    mret
