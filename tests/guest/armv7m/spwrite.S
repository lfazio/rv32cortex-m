/* SPDX-License-Identifier: Apache-2.0 */
/*
 * spwrite.S - every way an instruction writes sp, as a Cortex-M7 does it.
 *
 * Two answers the F746 gave (docs/fuzzing.md, the corpus on the board):
 *
 *   - sp's bits [1:0] are always zero: an odd value written through MOV,
 *     ADD, ADDW, SUB, a load or a load or store's writeback lands with
 *     them cleared;
 *   - ADD.W and SUB.W sp, sp, Rm execute with any shift, flags or not,
 *     where the manual allows only LSL #0-#3.
 *
 * Each case saves sp in r1, writes sp, copies it to r0, puts sp back,
 * and compares r0 with r1 plus what the board's sp moved by. The
 * encodings are written out: the assembler refuses some of the forms
 * the M7 executes. Eight passes, so the JIT translates them too.
 *
 * Prints SPWRITE-OK, or SPWRITE-BAD after the letter of the case.
 */
    .syntax unified
    .thumb

    .section .vectors, "a"
    .word   0x20010000          /* initial SP: the DTCM's top */
    .word   _start + 1

    .macro  EXPECT  delta
    mov     r0, sp
    mov     sp, r1
    ldr     r2, =\delta
    add     r2, r1, r2
    cmp     r0, r2
    bne     bad
    .endm

    .text
    .thumb_func
    .global _start
_start:
    movs    r6, #8
pass:
    movs    r5, #'A'            /* mov sp, r3: sp + 1 */
    mov     r1, sp
    adds    r3, r1, #1
    .inst.n 0x469d
    EXPECT  0

    movs    r5, #'B'            /* add sp, r3, r3 = 1 */
    mov     r1, sp
    movs    r3, #1
    .inst.n 0x449d
    EXPECT  0

    movs    r5, #'C'            /* add.w sp, sp, #1 */
    mov     r1, sp
    .inst.w 0xf10d0d01
    EXPECT  0

    movs    r5, #'D'            /* addw sp, sp, #1 */
    mov     r1, sp
    .inst.w 0xf20d0d01
    EXPECT  0

    movs    r5, #'E'            /* sub.w sp, sp, #1 */
    mov     r1, sp
    .inst.w 0xf1ad0d01
    EXPECT  -4

    movs    r5, #'F'            /* mov.w sp, r3: sp + 1 */
    mov     r1, sp
    adds    r3, r1, #1
    .inst.w 0xea4f0d03
    EXPECT  0

    movs    r5, #'G'            /* add.w sp, sp, r3, ror #2, r3 = 0x41 */
    mov     r1, sp
    movs    r3, #0x41
    .inst.w 0xeb0d0db3
    EXPECT  0x40000010

    movs    r5, #'H'            /* subs.w sp, sp, r3, ror #2 */
    mov     r1, sp
    movs    r3, #0x41
    .inst.w 0xebbd0db3
    EXPECT  -0x40000010

    movs    r5, #'I'            /* add.w sp, sp, r3, ror #2, r3 = 4: + 1 */
    mov     r1, sp
    movs    r3, #4
    .inst.w 0xeb0d0db3
    EXPECT  0

    movs    r5, #'J'            /* ldr.w sp, [sp, #-8] of sp + 1 */
    mov     r1, sp
    adds    r3, r1, #1
    .inst.w 0xf84d3c08          /* str r3, [sp, #-8] */
    .inst.w 0xf85ddc08
    EXPECT  0

    movs    r5, #'K'            /* ldr r3, [sp, #1]! */
    mov     r1, sp
    .inst.w 0xf85d3f01
    EXPECT  0

    movs    r5, #'L'            /* ldr r3, [sp], #1 */
    mov     r1, sp
    .inst.w 0xf85d3b01
    EXPECT  0

    movs    r5, #'M'            /* str r3, [sp, #-5]! */
    mov     r1, sp
    .inst.w 0xf84d3d05
    EXPECT  -8

    movs    r5, #'N'            /* ldrb r3, [sp], #1 */
    mov     r1, sp
    .inst.w 0xf81d3b01
    EXPECT  0

    movs    r5, #'O'            /* add.w sp, sp, r3, lsl #2: exact */
    mov     r1, sp
    movs    r3, #1
    .inst.w 0xeb0d0d83
    EXPECT  4

    subs    r6, #1
    beq     ok
    b       pass

ok:
    ldr     r7, =okmsg
    b       print
bad:
    ldr     r2, =0x10000000     /* the UART's transmit register */
    strb    r5, [r2]
    ldr     r7, =badmsg
print:
    ldr     r2, =0x10000000
1:  ldrb    r3, [r7]
    cbz     r3, 2f
    strb    r3, [r2]
    adds    r7, #1
    b       1b
2:  bkpt    #0

okmsg:  .asciz "SPWRITE-OK\n"
badmsg: .asciz " SPWRITE-BAD\n"
    .align  2
    .ltorg
