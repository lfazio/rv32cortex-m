; The table reference method: the handler's address read through INTBP,
; a handler in the program, and EIRET back into it.
    .section ".text", text
    mov     0x80000000, r10
    mov     #handler, r11
    st.w    r11, 12[r10]        ; INTBP + 4 * 3
    ldsr    r10, 4, 1           ; INTBP
    mov     0xFFFC0000, r12
    mov     0x1042, r13         ; EIRF, EITB, EIP 2
    st.h    r13, 6[r12]         ; EIC3
    ei
    add     1, r14
    halt
    .align  4
handler:
    add     1, r15
    stsr    13, r16, 0          ; EIIC
    eiret
