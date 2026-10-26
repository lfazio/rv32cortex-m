; Two PEs, each reading its stack in a loop. The stack is in local RAM
; through the SELF window, which is a different memory on each PE, so the
; two sums differ -- unless a block one PE built reads the other's.
    .section ".text", text
    mov     16, r10
again:
    ld.w    0[sp], r11
    add     r11, r12
    ld.w    4[sp], r13
    add     r13, r12
    add     -1, r10
    cmp     0, r10
    bne     again
    halt
