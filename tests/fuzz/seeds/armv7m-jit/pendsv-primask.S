/* PendSV pended under PRIMASK, taken at CPSIE on both backends. */
    .syntax unified
    .thumb
    cpsid   i
    ldr     r0, =0xE000ED04     /* ICSR */
    ldr     r1, =0x10000000     /* PENDSVSET */
    str     r1, [r0]
    ldr     r2, [r0]
    cpsie   i
    adds    r3, #1
    bkpt    #0
    .ltorg
