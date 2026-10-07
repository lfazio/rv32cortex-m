/* SPDX-License-Identifier: Apache-2.0 */
/*
 * start.S - the ARMv7-M guests' start-up.
 *
 * The vector table, .data installed, .bss zeroed, main called. Doing
 * that here rather than in the loader is the conclusion this tree
 * reached for its RISC-V guests the hard way: a guest that cannot
 * initialise itself is a core image rather than a program, and the
 * split it forces on the loader was wrong in three bytes wherever
 * .rodata ended unaligned.
 *
 * **The table is addresses, not instructions**, which is the thing an
 * ARMv7-M vector table gets right compared with every other
 * architecture in this tree: offset 0 is the initial stack pointer and
 * offset 4 is the reset address with its Thumb bit set. A guest that
 * gets it wrong faults before its first instruction rather than twenty
 * bytes later.
 */

    .syntax unified
    .thumb

    .section .vectors, "a"
    .word   0x80100000          /* initial SP: top of the RAM window   */
    .word   _start + 1          /* reset, Thumb bit set                */

    .text
    .thumb_func
    .global _start
_start:
    /* .data from its load address in ROM to its run address in RAM. */
    ldr     r0, =__data_lma
    ldr     r1, =__data_start
    ldr     r2, =__data_end
.Lcopy:
    cmp     r1, r2
    beq     .Lzero
    ldr     r3, [r0]
    str     r3, [r1]
    adds    r0, r0, #4
    adds    r1, r1, #4
    b       .Lcopy

.Lzero:
    ldr     r0, =__bss_start
    ldr     r1, =__bss_end
    movs    r2, #0
.Lbss:
    cmp     r0, r1
    beq     .Lmain
    str     r2, [r0]
    adds    r0, r0, #4
    b       .Lbss

.Lmain:
    bl      main
    bkpt    #0
