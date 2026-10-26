/*
 * A vector table in RAM naming a handler in the program: PendSV is taken,
 * the handler pends an external interrupt and returns, and the return
 * tail-chains into it -- whose vector is still the default BKPT.
 */
    .syntax unified
    .thumb
    cpsid   i
    ldr     r0, =0x80000000     /* RAM: the new table */
    ldr     r1, =0x20010401     /* the default BKPT stub, Thumb */
    movs    r2, #0
1:  str     r1, [r0, r2, lsl #2]
    adds    r2, #1
    cmp     r2, #32
    bne     1b
    adr     r3, handler
    adds    r3, #1
    str     r3, [r0, #56]       /* PendSV, exception 14 */
    ldr     r4, =0xE000ED08     /* VTOR */
    str     r0, [r4]
    ldr     r5, =0xE000E100
    movs    r6, #1
    str     r6, [r5]            /* IRQ 0 enabled */
    ldr     r4, =0xE000ED04
    ldr     r7, =0x10000000
    str     r7, [r4]            /* PendSV pending, masked */
    cpsie   i
    adds    r8, #1
    bkpt    #1
    .balign 4
handler:
    str     r6, [r5, #0x100]    /* pend IRQ 0 while PendSV is active */
    adds    r9, #1
    bx      lr
    .ltorg
