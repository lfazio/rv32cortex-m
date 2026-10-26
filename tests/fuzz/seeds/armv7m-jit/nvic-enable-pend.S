/* An external interrupt enabled, prioritised and pended, then unmasked. */
    .syntax unified
    .thumb
    cpsid   i
    ldr     r0, =0xE000E100     /* NVIC_ISER0 */
    movs    r1, #0x20           /* IRQ 5 */
    str     r1, [r0]
    ldr     r2, =0xE000E405     /* NVIC_IPR, IRQ 5's byte */
    movs    r3, #0x80
    strb    r3, [r2]
    str     r1, [r0, #0x100]    /* NVIC_ISPR0 */
    ldr     r4, [r0, #0x200]    /* NVIC_IABR0 */
    cpsie   i
    adds    r5, #1
    bkpt    #0
    .ltorg
