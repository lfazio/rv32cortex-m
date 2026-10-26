/*
 * Enabled and masked again each time round, with flags set in the loop;
 * PendSV pended, while masked, half way. By then the loop is a
 * translated block, so it is the JIT's dispatcher that takes the
 * exception after CPSIE -- with the flags still in translated code's
 * split form, which entry has to pack before it stacks xPSR. The other
 * three targets have a seed of this shape.
 */
    .syntax unified
    .thumb
    cpsid   i
    ldr     r0, =0xE000ED04     /* ICSR */
    ldr     r1, =0x10000000     /* PENDSVSET */
    movs    r2, #16
    movs    r3, #0
1:  cpsie   i
    adds    r3, #7              /* flags, live into the next block */
    subs    r4, r3, #40
    cpsid   i
    cmp     r2, #8
    bne     2f
    str     r1, [r0]
2:  subs    r2, #1
    bne     1b
    bkpt    #0
    .ltorg
