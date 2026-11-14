; Calls and returns, for the stub a computed jump goes through
; (emu_jit_ibtc): JMP [lp] after JARL, JARL [reg1] and DISPOSE's return,
; round a loop, so each finds its target's block in the table. Random
; words made none that landed on translated code (FUZZ_STATS: "computed
; jumps sent on"), so nothing told whether the translator marked the
; right exits (EMU_IR_EXIT_JUMP) or the stub sent them to the right
; block.
    .section ".text", text
    mov     24, r12
    mov     #g, r10
again:
    jarl    f, lp
    add     3, r13
    jarl    [r10], lp
    add     5, r14
    jarl    h, lp
    add     7, r18
    add     -1, r12
    cmp     0, r12
    bne     again
    halt
f:
    add     1, r15
    jmp     [lp]
g:
    add     2, r16
    jmp     [lp]
h:
    prepare lp, 0
    add     4, r17
    dispose 0, lp, [lp]
