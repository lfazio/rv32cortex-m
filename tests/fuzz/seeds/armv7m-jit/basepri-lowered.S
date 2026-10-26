/* Masked by BASEPRI, then BASEPRI lowered by MSR: taken after it. */
    .syntax unified
    .thumb
    movs    r0, #0x40
    msr     basepri, r0
    ldr     r1, =0xE000E100
    movs    r2, #0x02           /* IRQ 1 */
    str     r2, [r1]
    ldr     r3, =0xE000E401
    movs    r4, #0x80
    strb    r4, [r3]
    str     r2, [r1, #0x100]
    movs    r0, #0
    msr     basepri, r0
    adds    r5, #1
    bkpt    #0
    .ltorg
