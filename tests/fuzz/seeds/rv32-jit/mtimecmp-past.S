/* mtimecmp written below mtime, masked, then enabled: a timer interrupt. */
    li      t0, 0x02004000      /* CLINT mtimecmp */
    li      t1, 0x7ff8
    add     t2, t0, t1          /* t2 = mtime, at 0x0200bff8 */
    lw      a2, 0(t2)
    lw      a3, 4(t2)
    sw      zero, 4(t0)
    sw      a2, 0(t0)           /* mtimecmp = mtime: MTIP */
    csrr    a4, mip
    csrsi   mstatus, 8
    addi    a1, a1, 1
    ecall
