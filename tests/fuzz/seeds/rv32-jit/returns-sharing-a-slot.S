/*
 * Two return sites in one entry of the computed jumps' table
 * (emu_jit_ibtc): at 0x2000_0020 and 0x2000_00B0, which the index's
 * multiply sends to the same slot (57). Each return finds the other
 * site's entry there, so the stub must compare the pc it was given with
 * the entry's and go back to the dispatcher; one that did not ran the
 * other site's block -- a1 counted for a2 -- and calls-and-returns.S,
 * whose sites have slots of their own, could not tell.
 *
 * norvc and .org keep the addresses: the multiply decides the slots.
 */
    .option norvc
    li      t3, 24              /* 0x00 */
    j       1f
    .org    0x1c
1:  jal     ra, f               /* 0x1c: returns to 0x20 */
    addi    a1, a1, 3           /* 0x20, slot 57 */
    j       2f
    .org    0xac
2:  jal     ra, f               /* 0xac: returns to 0xb0 */
    addi    a2, a2, 5           /* 0xb0, slot 57 */
    addi    t3, t3, -1
    bnez    t3, 1b
    ecall
f:  addi    a3, a3, 1
    jalr    x0, 0(ra)
