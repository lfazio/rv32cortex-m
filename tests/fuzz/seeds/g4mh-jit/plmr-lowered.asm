; Refused by PLMR after EI, then taken when LDSR lowers the ceiling.
    .section ".text", text
    mov     0xFFFC0000, r10
    mov     0x1005, r11         ; EIRF, EIP 5
    st.h    r11, 8[r10]         ; EIC4
    mov     3, r12
    ldsr    r12, 14, 2          ; PLMR 3: priority 5 refused
    ei
    add     1, r13
    mov     16, r12
    ldsr    r12, 14, 2          ; PLMR 16: taken after this
    add     1, r14
    halt
