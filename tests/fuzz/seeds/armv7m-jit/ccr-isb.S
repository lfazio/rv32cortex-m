/* CCR's divide trap set, then ISB: the next divide by zero traps. */
    .syntax unified
    .thumb
    ldr     r0, =0xE000ED14     /* CCR */
    ldr     r1, [r0]
    orr     r1, r1, #0x10       /* DIV_0_TRP */
    str     r1, [r0]
    dsb
    isb
    movs    r2, #0
    sdiv    r3, r4, r2
    bkpt    #0
    .ltorg
