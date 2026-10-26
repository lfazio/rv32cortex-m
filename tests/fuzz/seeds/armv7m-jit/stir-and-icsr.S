/* STIR, ICSR's clear bits and VECTPENDING, with PRIMASK set throughout. */
    .syntax unified
    .thumb
    cpsid   i
    ldr     r0, =0xE000E100
    movs    r1, #0x08
    str     r1, [r0]
    ldr     r2, =0xE000EF00     /* STIR */
    movs    r3, #3
    str     r3, [r2]
    ldr     r4, =0xE000ED04
    ldr     r5, [r4]            /* VECTPENDING, ISRPENDING */
    ldr     r6, =0x08000000     /* PENDSVCLR */
    str     r6, [r4]
    ldr     r7, [r0, #0x100]
    str     r1, [r0, #0x180]    /* ICPR */
    ldr     r8, [r0, #0x100]
    bkpt    #0
    .ltorg
