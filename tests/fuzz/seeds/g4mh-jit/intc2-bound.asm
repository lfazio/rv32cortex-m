; An INTC2 channel bound to this PE through EIBD, requested through EEIC.
    .section ".text", text
    mov     0xFFF80000, r10     ; INTC2
    mov     0, r11
    st.w    r11, 0x20A0[r10]    ; EIBD40: PE0
    mov     0x10000007, r12     ; EIRF, EIP 7, unmasked
    st.w    r12, 0x40A0[r10]    ; EEIC40
    ld.w    0x40A0[r10], r13
    ld.h    0x50[r10], r14      ; EIC40, the 16-bit view
    ei
    add     1, r15
    halt
