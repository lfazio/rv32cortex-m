/* WFI with an interrupt pending and masked wakes at once and goes on. */
    li      t0, 0x02000000
    li      t1, 1
    sw      t1, 0(t0)
    wfi
    addi    a1, a1, 1
    sw      zero, 0(t0)
    csrsi   mstatus, 8
    wfi                         /* nothing pending now: parks */
