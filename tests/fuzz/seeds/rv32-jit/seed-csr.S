/*
 * Zkr's seed in a hot loop, both backends: the translator declines every
 * CSR instruction but a read of time, so each poll is the interpreter's,
 * reached from the middle of translated code and from the dispatcher.
 * The harness's source (fuzz_harness.h) answers WAIT one poll in seven
 * and restarts for each side, so the two read the same values in the
 * same order -- or diverge, which a source restarted once would show.
 * mseccfg and the read-only form, whose trap ends the run, are there
 * for the fuzzer to mutate.
 */
    li      t2, 24
    li      a1, 0
1:  csrrw   t0, 0x015, x0       # seed
    srli    t1, t0, 30
    addi    t1, t1, -1          # WAIT?
    beqz    t1, 1b
    slli    a2, a1, 5
    srli    a1, a1, 27
    or      a1, a1, a2
    xor     a1, a1, t0
    addi    t2, t2, -1
    bnez    t2, 1b
    li      t4, 0x300
    csrw    0x747, t4           # mseccfg: USEED, SSEED
    csrr    a3, 0x747
    csrrs   t3, 0x015, x0       # read-only: illegal, and the run ends
    ecall
