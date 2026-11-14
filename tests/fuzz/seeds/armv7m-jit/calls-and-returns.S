/*
 * Calls and returns, for the stub a computed jump goes through
 * (emu_jit_ibtc): BX LR, POP {pc} and a TBB, each round a loop, so each
 * finds its target's block in the table. Random words made none that
 * landed on translated code (FUZZ_STATS: "computed jumps sent on"), so
 * nothing told whether the translator marked the right exits
 * (EMU_IR_EXIT_JUMP) or the stub sent them to the right block.
 */
    .syntax unified
    .thumb
    movs    r3, #24
1:  bl      f                   /* BX LR */
    adds    r1, #3
    bl      g                   /* POP {pc} */
    adds    r2, #5
    and     r6, r3, #1
    tbb     [pc, r6]
.Ltbl:
    .byte   (.Lc0 - .Ltbl) / 2, (.Lc1 - .Ltbl) / 2
    .balign 2
.Lc0:
    adds    r7, #1
    b       2f
.Lc1:
    adds    r7, #2
2:  subs    r3, #1
    bne     1b
    bkpt    #0
f:  adds    r4, #1
    bx      lr
g:  push    {lr}
    adds    r5, #2
    pop     {pc}
