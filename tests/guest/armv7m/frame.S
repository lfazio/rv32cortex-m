/* SPDX-License-Identifier: Apache-2.0 */
/*
 * frame.S - does an exception give back the registers it took?
 *
 * **In assembly because inline asm could not express it.** The C
 * version set r0-r3 and r12 to known values and compared them
 * afterwards, and it passed with the restore of r0 and of r12
 * deliberately broken -- because gcc needs registers for the output
 * operands and for the address of the flag being polled, and the ones
 * it chose were the ones under test. The test was measuring the
 * compiler's allocation.
 *
 * Here nothing is allocated: every register is named, the poll uses
 * r6 alone, and the result is a bitmask so the caller can say *which*
 * register did not come back.
 */

    .syntax unified
    .thumb

    .text

/*
 * r0 = bytes to bias the stack by, 0 or 4.
 *
 * The bias is the point of the second call. Exception entry has to keep
 * the frame 8-byte aligned, so from a stack that is 4 mod 8 it inserts
 * a padding word and records that in bit 9 of the stacked xPSR; the
 * return has to undo exactly that. Getting it wrong leaks four bytes
 * per exception, which a single interrupt cannot show -- the stack is
 * simply four bytes lower afterwards and everything still works. Only a
 * caller that checks sp, or one that takes thousands, ever notices.
 *
 * Returns a bitmask: bit 0 r0, 1 r1, 2 r2, 3 r3, 4 r12, 5 sp.
 */
    .global armv7m_frame_check
    .thumb_func
armv7m_frame_check:
    push    {r4, r5, r6, r7, lr}

    mov     r7, r0              /* remember the bias to undo it     */
    sub     sp, sp, r0          /* and apply it                      */
    mov     r4, sp              /* the sp an exception must preserve */

    ldr     r6, =g_ticks
    movs    r5, #0
    str     r5, [r6]            /* g_ticks = 0: wait for a fresh one */

    /* The values the handler is going to write over. */
    movs    r0, #0x11
    movs    r1, #0x22
    movs    r2, #0x33
    movs    r3, #0x44
    movs    r5, #0x55
    mov     r12, r5

    /* Poll with r6 and r5 only -- nothing under test is touched. */
1:  ldr     r5, [r6]
    cmp     r5, #0
    beq     1b

    movs    r5, #0

    cmp     r0, #0x11
    beq     2f
    orr     r5, r5, #1
2:  cmp     r1, #0x22
    beq     3f
    orr     r5, r5, #2
3:  cmp     r2, #0x33
    beq     4f
    orr     r5, r5, #4
4:  cmp     r3, #0x44
    beq     5f
    orr     r5, r5, #8
5:  mov     r0, r12
    cmp     r0, #0x55
    beq     6f
    orr     r5, r5, #16

    /*
     * And the stack pointer, which is what the alignment bit is about:
     * an entry that padded and a return that did not leaves sp four
     * bytes below where the exception found it.
     */
6:  mov     r0, sp
    cmp     r0, r4
    beq     7f
    orr     r5, r5, #32

7:  mov     r0, r5
    add     sp, sp, r7
    pop     {r4, r5, r6, r7, pc}
