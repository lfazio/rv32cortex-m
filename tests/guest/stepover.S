/* SPDX-License-Identifier: Apache-2.0 */
/*
 * stepover.S - a fault stepped over, 100 times, second in its block.
 *
 * Each pass stores the counter to RAM, then loads from an address no
 * region covers -- an access fault, whose handler moves mepc past the
 * load and returns -- then counts. The run's total must be the same on
 * both backends (tests/guest/same-on-both-backends.py): the interpreter
 * charges a trap one unit of the budget, and a block that trapped
 * partway was charged only what it had retired: one short under the
 * JIT for each fault (emu_jit_hot_t::charged). The fault is second in
 * its block; first, the dispatcher's floor of one would charge it
 * anyway.
 *
 * Prints STEPOVER-OK if the loop ran all 100 passes and the last store
 * before the last fault landed, STEPOVER-BAD otherwise. The word is at a
 * fixed address in RAM: on a board the image is served read-only, and a
 * store into it faults too.
 */
    .section .text
    .globl _start
_start:
    la      t0, handler
    csrw    mtvec, t0
    li      a0, 0x70000000      /* no region: an access fault */
    li      s1, 0
    li      s2, 100
    li      s3, 0x80001000      /* a RAM word: on a board the image is ROM */
loop:
    sw      s1, 0(s3)           /* permitted */
    lw      a2, 0(a0)           /* faults, stepped over */
    addi    s1, s1, 1
    blt     s1, s2, loop

    la      a1, okmsg
    lw      t2, 0(s3)
    li      t3, 99
    beq     t2, t3, print
    la      a1, badmsg
print:
    li      t0, 0x10000000      /* the NS16550's transmit register */
1:  lbu     t1, 0(a1)
    beqz    t1, 2f
    sb      t1, 0(t0)
    addi    a1, a1, 1
    j       1b
2:  li      a0, 0
    li      a7, 93              /* exit */
    ecall
3:  j       3b

    .align  4                   /* mtvec, direct mode */
handler:
    csrr    t4, mepc
    addi    t4, t4, 4           /* lw is four bytes */
    csrw    mepc, t4
    mret

    /* Read only, so in .text: the default link script would give .data
     * an address the flat image does not load. */
okmsg:  .string "STEPOVER-OK\n"
badmsg: .string "STEPOVER-BAD\n"
