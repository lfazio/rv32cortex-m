/* SPDX-License-Identifier: Apache-2.0 */
/*
 * isbpend.S - PendSV pended before an ISB, round a loop: taken after the
 * ISB and before the instruction that follows it, every time.
 *
 * The architecture lets the instruction after the store that pends an
 * exception run first, and not the one after an ISB; the translator ends
 * the block at the ISB with a computed exit so that the dispatcher looks
 * for the exception there. Under the computed jumps' stub
 * (emu_jit_ibtc) that holds only while the ISB's exit is not marked a
 * plain jump (EMU_IR_EXIT_JUMP): the rounds before the first pend put
 * the ADD's block in the table, and an ISB sent on through it ran the
 * ADD before PendSV -- which the fuzzer cannot see, since it sets aside
 * every run whose interrupt may land at different places, and which
 * the board's system tests could not either, each passing its ISB once.
 *
 * **The store is conditional, in an IT block, so that it and the ISB
 * are one block.** Branched round, the store was a block of its own,
 * whose exit went back to the dispatcher, which took PendSV there: the
 * ISB's exit decided nothing, and the mutant above passed.
 *
 * The handler checks r3 against r6, the ADDs counted before the pend,
 * and counts the entries that agreed in r5. Prints ISBPEND-OK if all
 * four did, ISBPEND-BAD if any did not.
 */
    .syntax unified
    .thumb

    .section .vectors, "a"
    .word   0x20010000          /* initial SP: the DTCM's top */
    .word   _start + 1
    .word   halt + 1            /* NMI */
    .word   halt + 1            /* HardFault */
    .word   halt + 1            /* MemManage */
    .word   halt + 1            /* BusFault */
    .word   halt + 1            /* UsageFault */
    .word   0, 0, 0, 0
    .word   halt + 1            /* SVCall */
    .word   halt + 1            /* DebugMonitor */
    .word   0
    .word   pendsv + 1          /* PendSV */
    .word   halt + 1            /* SysTick */

    .text
    .thumb_func
    .global _start
_start:
    ldr     r0, =0xE000ED04     /* ICSR */
    ldr     r1, =0x10000000     /* PENDSVSET */
    movs    r3, #0              /* ADDs after the ISB */
    movs    r5, #0              /* PendSV entries that found r3 == r6 */
    movs    r2, #16
loop:
    cmp     r2, #4
    itt     ls
    movls   r6, r3
    strls   r1, [r0]            /* pended: the last four rounds */
    dsb
    isb
    adds    r3, #1              /* after PendSV, the rounds it is pended */
    subs    r2, #1
    bne     loop

    ldr     r1, =okmsg
    cmp     r5, #4
    beq     2f
    ldr     r1, =badmsg
2:  ldr     r2, =0x10000000     /* the UART's transmit register */
3:  ldrb    r3, [r1]
    cbz     r3, 4f
    strb    r3, [r2]
    adds    r1, #1
    b       3b
4:  bkpt    #0

    .thumb_func
pendsv:
    cmp     r3, r6
    bne     5f
    adds    r5, #1
5:  bx      lr

    .thumb_func
halt:
    bkpt    #1

okmsg:  .asciz "ISBPEND-OK\n"
badmsg: .asciz "ISBPEND-BAD\n"
    .align  2
    .ltorg
