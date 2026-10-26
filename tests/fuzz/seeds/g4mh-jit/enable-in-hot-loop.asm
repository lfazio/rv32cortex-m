; Enabled and masked again each time round; requested, while masked,
; half way. By then the code after EI is a translated block, so the
; JIT's own copy of the interrupt path decides when it is taken.
    .section ".text", text
    mov     0xFFFC0000, r10
    mov     0x1002, r11
    mov     16, r12
again:
    ei
    add     1, r13
    add     2, r14
    di
    add     -1, r12
    cmp     8, r12
    bne     skip
    st.h    r11, 6[r10]
skip:
    cmp     0, r12
    bne     again
    halt
