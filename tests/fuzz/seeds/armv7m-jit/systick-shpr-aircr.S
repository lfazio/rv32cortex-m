/* SysTick programmed (its enable never lands), priorities and PRIGROUP. */
    .syntax unified
    .thumb
    ldr     r0, =0xE000E010     /* SYST_CSR */
    ldr     r1, =0x100
    str     r1, [r0, #4]        /* RVR */
    movs    r2, #7
    str     r2, [r0]            /* ENABLE, TICKINT, CLKSOURCE */
    ldr     r3, [r0]
    ldr     r4, [r0, #8]        /* CVR */
    ldr     r5, =0xE000ED20     /* SHPR3: PendSV and SysTick */
    ldr     r6, =0x80C00000
    str     r6, [r5]
    ldr     r7, =0xE000ED0C     /* AIRCR */
    ldr     r8, =0x05FA0500
    str     r8, [r7]
    ldr     r9, [r7]
    bkpt    #0
    .ltorg
