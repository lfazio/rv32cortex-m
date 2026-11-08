/* SPDX-License-Identifier: Apache-2.0 */
/*
 * tcm.S - the M7's map, as an ARMv7-M guest sees it
 * (include/armv7m/armv7m_memmap.h).
 *
 * The image runs at the flash's 0x0800_0000: this table is read from
 * there. The routine below is copied into the ITCM at 0x0000_0100 and
 * called there, twice -- the second time after rewriting its immediate,
 * which a JIT that kept the first translation would not see -- and a
 * word is written and read back at the DTCM's first address and at its
 * last, then at the SRAM just above it. Prints TCM-OK if all of that
 * held, TCM-BAD otherwise.
 */
    .syntax unified
    .thumb

    .section .vectors, "a"
    .word   0x20010000          /* initial SP: the DTCM's top */
    .word   _start + 1

    .text
    .thumb_func
    .global _start
_start:
    ldr     r7, =badmsg

    /* The image is where the flash is. */
    adr     r0, _start
    lsrs    r0, r0, #24
    cmp     r0, #0x08
    bne     print

    /* Copy `routine` into the ITCM and call it there. */
    ldr     r0, =0x00000100
    adr     r1, routine
    ldr     r2, [r1]
    str     r2, [r0]
    dsb
    isb
    ldr     r3, =0x00000101     /* the Thumb bit */
    blx     r3
    cmp     r0, #42
    bne     print

    /* Rewrite its immediate and call it again. */
    ldr     r0, =0x00000100
    ldrh    r2, [r0]
    bic     r2, r2, #0xFF
    orr     r2, r2, #7          /* movs r0, #7 */
    strh    r2, [r0]
    dsb
    isb
    blx     r3
    cmp     r0, #7
    bne     print

    /* The DTCM's first and last words, and the SRAM above it. */
    ldr     r4, =0x5A5AA5A5
    ldr     r0, =0x20000000
    str     r4, [r0]
    ldr     r1, =0x2000FFFC
    str     r4, [r1]
    ldr     r2, =0x20010000
    str     r4, [r2]
    ldr     r5, [r0]
    cmp     r5, r4
    bne     print
    ldr     r5, [r1]
    cmp     r5, r4
    bne     print
    ldr     r5, [r2]
    cmp     r5, r4
    bne     print

    ldr     r7, =okmsg
print:
    ldr     r2, =0x10000000     /* the UART's transmit register */
1:  ldrb    r3, [r7]
    cbz     r3, 2f
    strb    r3, [r2]
    adds    r7, #1
    b       1b
2:  bkpt    #0

    .align  2
routine:
    movs    r0, #42
    bx      lr

okmsg:  .asciz "TCM-OK\n"
badmsg: .asciz "TCM-BAD\n"
    .align  2
    .ltorg
