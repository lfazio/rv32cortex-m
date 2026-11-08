/* SPDX-License-Identifier: Apache-2.0 */
/*
 * faulthead.S - a precise fault at a block's first instruction, 100
 * times, and then one that cannot be taken.
 *
 * The loop's head is a load from an address no region covers: a precise
 * BusFault, escalated to HardFault, whose handler steps over the load.
 * Each pass is a block that faults before it retires anything, and the
 * interpreter charges such a fault nothing; the JIT's dispatcher charged
 * one whenever it entered the block itself rather than through a chain,
 * and the run's total differed, 804 against 806
 * (emu_jit_ops_t::fault_not_charged, same-on-both-backends.py).
 *
 * Then the part that makes charging nothing safe: VTOR moves to a table
 * whose HardFault vector is an address no region covers, and the load
 * faults again. The handler cannot be fetched at HardFault's priority,
 * so the core locks up and the run ends -- on both backends, where a
 * frontend that could fault at its own vector for ever would spin the
 * JIT's loop uncharged.
 *
 * Prints FAULTHEAD-OK after the loop if all 100 passes ran.
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
    movs    r6, #0
    movs    r7, #100
    b       loop
loop:
    ldr.w   r8, [r4]            /* precise BusFault: the block's first */
    adds    r6, #1
    cmp     r6, r7
    blt     loop

    ldr     r1, =okmsg
    ldr     r2, =0x10000000     /* the UART's transmit register */
1:  ldrb    r3, [r1]
    cbz     r3, 2f
    strb    r3, [r2]
    adds    r1, #1
    b       1b
2:
    /* A table in RAM whose HardFault vector is nowhere, then a fault. */
    ldr     r0, =0x20002000
    ldr     r1, =0x20010000
    str     r1, [r0]
    ldr     r1, =_start + 1
    str     r1, [r0, #4]
    ldr     r1, =0x60000001
    str     r1, [r0, #8]
    str     r1, [r0, #12]
    ldr     r1, =0xE000ED08     /* VTOR */
    str     r0, [r1]
    dsb
    isb
    ldr.w   r8, [r4]            /* locks up */
    bkpt    #0                  /* not reached */

    /* Step over the load: ldr.w is four bytes. */
    .thumb_func
fault:
    ldr     r1, [sp, #24]
    adds    r1, #4
    str     r1, [sp, #24]
    bx      lr

okmsg:  .asciz "FAULTHEAD-OK\n"
    .align  2
    .ltorg
