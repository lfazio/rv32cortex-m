/*
 * Calls and returns, for the stub a return goes through (emu_jit_ibtc):
 * one function called from two sites, round a loop, so each return finds
 * its site's block in the table -- the only seed here whose returns land
 * on translated code; random words and the guests' text, run without
 * their data, made none (FUZZ_STATS: "computed jumps sent on").
 *
 * And the context the stub compares. One time round, the FP unit is off
 * for the call whose return lands on an f write (FS is part of a block's
 * context): the entry the earlier rounds left names the block built with
 * FS on, which must not be entered, and the write must take its illegal
 * instruction -- the handler counts it in a5 and steps over it. A stub
 * that matched the pc alone ran the write.
 *
 * fmv.w.x is a word: the seeds are assembled without F.
 */
    la      t0, handler
    csrw    mtvec, t0
    li      t4, 0x6000          /* mstatus.FS */
    li      t6, 0x2000
    csrs    mstatus, t6         /* FS Initial: f writes allowed */
    li      t3, 24
1:  li      t5, 12
    bne     t3, t5, 2f
    csrc    mstatus, t4         /* FS off, for this round's first return */
2:  jal     ra, f
    .word   0xF0058053          /* fmv.w.x f1, a1 */
    addi    a1, a1, 3
    csrs    mstatus, t6
    jal     ra, f
    addi    a2, a2, 5
    addi    t3, t3, -1
    bnez    t3, 1b
    ecall
f:  addi    a3, a3, 1
    jalr    x0, 0(ra)
    .balign 4
handler:
    csrr    t1, mepc
    addi    t1, t1, 4
    csrw    mepc, t1
    addi    a5, a5, 1
    mret
