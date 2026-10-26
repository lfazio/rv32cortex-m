; Two PEs: PE0 halts with channel 0 enabled, and PE1 wakes it through the
; IPIR. PE0 takes EIINT0 on its next turn.
    .section ".text", text
    stsr    0, r10, 2           ; HTCFG0: this PE's number
    mov     0xFFFC0000, r12     ; INTC1, this PE
    mov     0x0001, r13         ; EIC0: EIP 1, unmasked
    st.h    r13, 0[r12]
    mov     0xFFFB9000, r14     ; IPIR
    cmp     0, r10
    bne     pe1
    mov     2, r11
    st.b    r11, 0[r14]         ; IPIENS0: PE0 accepts channel 0 from PE1
    ei
    halt                        ; until PE1 asks
    add     1, r15
    halt
pe1:
    mov     1, r11
    st.b    r11, 0x10[r14]      ; IPIREQS0: to PE0
    add     1, r16
    halt
