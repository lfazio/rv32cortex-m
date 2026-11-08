/* SPDX-License-Identifier: Apache-2.0 */
/*
 * stepover.S - a store's imprecise BusFault, 100 times, second in its
 * block.
 *
 * Each pass stores the counter to RAM, then stores to an address no
 * region covers -- a bus error, imprecise on the M7, so the store
 * retires and the BusFault (escalated to HardFault: it is not enabled)
 * is taken after it; the handler returns to the next instruction --
 * then counts. The run's total must be the same on both backends
 * (tests/guest/same-on-both-backends.py): the interpreter counts the
 * store it retired, and under the JIT the helper that finishes it after
 * the block has stopped counted it into the core but not into the
 * budget (emu_jit_hot_t::charged).
 *
 * Twice: STR, which the translator lowers (finish_imprecise), and STRT,
 * which it hands to the interpreter from inside the block (a7_ir_step)
 * -- the two places a helper finishes such a store. Each refused store
 * is second in its block: first, the dispatcher's floor of one would
 * charge it anyway.
 *
 * No start-up, so no .data: the words are at fixed addresses in RAM.
 * Prints STEPOVER-OK if both loops ran all 100 passes and the last store
 * before each one's last fault landed, STEPOVER-BAD otherwise.
 */
    .syntax unified
    .thumb

    .section .vectors, "a"
    .word   0x20010000          /* initial SP: the DTCM's top */
    .word   _start + 1
    .word   fault + 1           /* NMI */
    .word   fault + 1           /* HardFault */

    .text
    .thumb_func
    .global _start
_start:
    ldr     r4, =0x60000000     /* no region */
    ldr     r5, =0x20001000     /* a RAM word, one for each loop */
    movs    r7, #100
    movs    r6, #0
loop:
    str     r6, [r5]            /* permitted */
    str     r6, [r4]            /* imprecise BusFault, taken after it */
    adds    r6, #1
    cmp     r6, r7
    blt     loop

    movs    r6, #0
loop_t:
    str     r6, [r5, #4]        /* permitted */
    strt    r6, [r4]            /* the same, from the interpreter */
    adds    r6, #1
    cmp     r6, r7
    blt     loop_t

    ldr     r1, =badmsg
    ldr     r0, [r5]
    cmp     r0, #99
    bne     print
    ldr     r0, [r5, #4]
    cmp     r0, #99
    bne     print
    ldr     r1, =okmsg
print:
    ldr     r2, =0x10000000     /* the UART's transmit register */
1:  ldrb    r3, [r1]
    cbz     r3, 2f
    strb    r3, [r2]
    adds    r1, #1
    b       1b
2:  bkpt    #0                  /* the emulator's "finished" */

    /* The store has retired; the stacked pc is the next instruction. */
    .thumb_func
fault:
    bx      lr

okmsg:  .asciz "STEPOVER-OK\n"
badmsg: .asciz "STEPOVER-BAD\n"
    .align  2
    .ltorg
