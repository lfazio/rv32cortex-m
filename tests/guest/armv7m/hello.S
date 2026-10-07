/* SPDX-License-Identifier: Apache-2.0 */
/*
 * hello.S - the first program the ARMv7-M frontend ran.
 *
 * **Assembled, not hand-encoded.** This project has had three defects
 * from hand-built instruction arrays that encoded what the author
 * believed rather than what the assembler produces, so every halfword
 * here comes from arm-none-eabi-as.
 *
 * **16-bit encodings only**, because that is all the interpreter
 * decodes so far. Which is also why this is written in assembly and not
 * C: a compiler reaches for movw/movt and the wide data-processing forms
 * immediately, and the frontend would fault on the first one -- with the
 * halfword reported, which is the point, but it would not print
 * anything.
 *
 * It exercises the four things a frontend has to get right before any
 * of the rest matters: the reset table, a store to a device, flags from
 * an arithmetic instruction, and a backward conditional branch.
 */

    .syntax unified
    .thumb

    .section .vectors, "a"
    .word   0x80010000          /* initial SP: top of the guest RAM window */
    .word   _start + 1          /* reset, with the Thumb bit the core needs */

    .text
    .thumb_func
    .global _start
_start:
    /*
     * The UART at 0x1000_0000, built with a shift because a 16-bit MOVS
     * carries only eight bits of immediate. 0x10 << 24 is the address,
     * and forming it this way keeps the whole program inside the subset.
     */
    movs    r0, #0x10
    lsls    r0, r0, #24

    movs    r1, #'O'
    str     r1, [r0]
    movs    r1, #'K'
    str     r1, [r0]

    /*
     * Three 'A's from a counted loop. SUBS sets the flags and BNE reads
     * them, so a frontend with a broken Z flag prints the wrong number
     * of characters rather than merely the wrong ones -- a failure that
     * names its own cause.
     */
    movs    r2, #3
.Lloop:
    movs    r1, #'A'
    str     r1, [r0]
    subs    r2, r2, #1
    bne     .Lloop

    movs    r1, #'\n'
    str     r1, [r0]

    /* Done. BKPT is how a guest stops without a semihosting call. */
    bkpt    #0
