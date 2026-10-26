; The core interrupts itself through the IPIR: EIINT0, while masked.
    .section ".text", text
    mov     0xFFFB9000, r10     ; IPIR
    mov     1, r11
    st.b    r11, 0[r10]         ; IPIENS0: accept channel 0 from PE0
    mov     0xFFFC0000, r12
    mov     0x0001, r13         ; EIC0: EIP 1, unmasked
    st.h    r13, 0[r12]
    st.b    r11, 0x10[r10]      ; IPIREQS0: request to PE0
    ld.b    4[r10], r14         ; IPIFLGS0
    ei
    add     1, r15
    halt
