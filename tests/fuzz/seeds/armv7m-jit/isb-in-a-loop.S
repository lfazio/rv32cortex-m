/*
 * ISB round a loop, PendSV pended on a late round: it must be taken
 * after that ISB and before the ADD -- the block ends there through the
 * dispatcher, its exit a computed one so that it cannot chain. By then
 * the earlier rounds have put the ADD's block in the computed jumps'
 * table (emu_jit_ibtc), so an ISB marked a plain jump (EMU_IR_EXIT_JUMP)
 * ran the ADD first. ccr-isb.S passes the ISB once and could not tell.
 */
    .syntax unified
    .thumb
    ldr     r0, =0xE000ED04     /* ICSR */
    ldr     r1, =0x10000000     /* PENDSVSET */
    movs    r2, #8
1:  cmp     r2, #2
    bne     2f
    str     r1, [r0]            /* pended, two rounds from the end */
2:  dsb
    isb
    adds    r3, #1              /* not yet, the round PendSV is pended */
    subs    r2, #1
    bne     1b
    bkpt    #0
    .ltorg
