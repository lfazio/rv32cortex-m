/* SPDX-License-Identifier: Apache-2.0 */
/*
 * start.S - the IR replay as native Nucleo-F746ZG firmware.
 *
 * Reset at HSI 16 MHz with the caches off, as tests/armv7m-diff's
 * harness runs: what the replay asks is what the Thumb-2 lowering
 * computes, and with nothing cached what was emitted is what is fetched.
 * The FPU is on, since blocks with floats in them use it.
 *
 * Every fault vector reports, with the case it was running: a block the
 * lowering got wrong enough to fault is a finding, and a hang would read
 * as nothing at all.
 */
    .syntax unified
    .thumb

    .section .vectors, "a"
    .word   __stack_top
    .word   Reset_Handler + 1
    .rept   14
    .word   Fault_Handler + 1
    .endr

    .text
    .thumb_func
    .global Reset_Handler
Reset_Handler:
    ldr     r0, =__data_lma
    ldr     r1, =__data_start
    ldr     r2, =__data_end
1:  cmp     r1, r2
    bhs     2f
    ldr     r3, [r0], #4
    str     r3, [r1], #4
    b       1b
2:  ldr     r1, =__bss_start
    ldr     r2, =__bss_end
    movs    r3, #0
3:  cmp     r1, r2
    bhs     4f
    str     r3, [r1], #4
    b       3b
    /* CP10 and CP11 full access. */
4:  ldr     r0, =0xE000ED88
    ldr     r1, [r0]
    orr     r1, r1, #(0xF << 20)
    str     r1, [r0]
    dsb
    isb
    bl      main
5:  b       5b

    .thumb_func
Fault_Handler:
    tst     lr, #4
    ite     eq
    mrseq   r0, msp
    mrsne   r0, psp
    mrs     r1, ipsr
    b       ir_board_fault
    .ltorg
