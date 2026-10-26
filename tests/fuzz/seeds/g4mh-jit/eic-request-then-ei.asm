; A channel requested while PSW.ID is set, taken at EI on both backends.
    .section ".text", text
    mov     0xFFFC0000, r10     ; INTC1, this PE
    mov     0x1002, r11         ; EIRF, EIP 2, unmasked
    st.h    r11, 6[r10]         ; EIC3
    ld.h    6[r10], r12
    ld.w    0xF0[r10], r13      ; IMR0: a view of the same bits
    ei                          ; taken here
    add     1, r14
    halt
