; SPDX-License-Identifier: Apache-2.0
;
; disp23.asm - the Format XIV long-displacement loads and stores, as
; CC-RH encodes them.
;
; The unit tests for these build their own halfwords, so they agree with
; whatever this frontend already believes about the field split -- which
; is the failure mode the whole g4mh-check-encodings.sh story is about.
; This guest does not: Renesas' assembler produces the bytes and the
; emulator executes them, so a disagreement about where disp[22:7] lives
; shows up as a wrong value rather than as two encoders being wrong
; together.
;
;   docker run --rm -v "$PWD":/w -w /w --entrypoint /bin/bash ccrh:latest -c '\
;     B=/usr/local/Renesas/CC-RH/V2.08.00/bin; \
;     $B/asrh -Xcommon=rh850 -Xcpu=g4mh disp23.asm && \
;     $B/rlink disp23.obj -start=.text,.rodata/80000000 \
;             -output=disp23.abs -form=absolute -entry=__start'
;
;   ./build/host/emu-host --frontend g4mh disp23.abs
;   ./build/host/emu-host --frontend g4mh --jit disp23.abs
;
; Both backends must print the same thing; the JIT ends its block at any
; 48-bit form, so what it proves is that the fallback runs, not that it
; translates these.
;
; r20 counts failures, so "checks=... fails=00000000" is the pass.
;
    .section .text, text
    .align 4
    .public __start
__start:
    mov     0, r20                  ; failures
    mov     0, r21                  ; checks

    ; A base register far enough below the buffer that the displacement
    ; needs all three fields. 0x123456 has bits in disp[22:7], in
    ; disp[6:1] and -- for the +1 cases -- in disp[0].
    mov     #_buf, r11
    add     -0x123456, r11          ; r11 = buf - 0x123456

    ; ---- ST.W / LD.W through the full displacement ------------------
    mov     0x5A5A, r12
    st.w    r12, 0x123456[r11]
    ld.w    0x123456[r11], r13
    cmp     r12, r13
    bz      .Lw_ok
    add     1, r20
.Lw_ok:
    add     1, r21

    ; and read the same cell back with a *short* displacement, which
    ; decodes by an entirely different path. If the disp23 write went
    ; somewhere else, this is what says so.
    mov     #_buf, r14
    ld.w    0[r14], r15
    cmp     r12, r15
    bz      .Lw2_ok
    add     1, r20
.Lw2_ok:
    add     1, r21

    ; ---- the byte forms, where disp[0] is a displacement bit --------
    ; Plant two different bytes with short displacements, then pick one
    ; out with a long odd displacement.
    mov     0xA5, r16
    st.b    r16, 0[r14]
    mov     0x5B, r17
    st.b    r17, 1[r14]

    ld.bu   0x123457[r11], r18      ; the odd one: must be 0x5B
    cmp     r17, r18
    bz      .Lb_ok
    add     1, r20
.Lb_ok:
    add     1, r21

    ld.bu   0x123456[r11], r18      ; the even one: must be 0xA5
    cmp     r16, r18
    bz      .Lb2_ok
    add     1, r20
.Lb2_ok:
    add     1, r21

    ; ---- a negative displacement, which is the sign extension -------
    mov     #_buf, r11
    add     0x40, r11               ; r11 = buf + 0x40
    mov     0x3C3C, r12
    st.w    r12, -0x40[r11]         ; back to buf
    ld.w    0[r14], r13
    cmp     r12, r13
    bz      .Ln_ok
    add     1, r20
.Ln_ok:
    add     1, r21

    ; ---- LD.DW / ST.DW, the register pair ---------------------------
    mov     0x1111, r22
    mov     0x2222, r23
    mov     #_buf, r11
    add     8, r11
    st.dw   r22, -8[r11]            ; writes r22 at buf, r23 at buf+4
    ld.dw   -8[r11], r24            ; reads them into r24 and r25

    cmp     r22, r24
    bz      .Ld_lo_ok
    add     1, r20
.Ld_lo_ok:
    add     1, r21

    cmp     r23, r25
    bz      .Ld_hi_ok
    add     1, r20
.Ld_hi_ok:
    add     1, r21

    ; the halves really are in that order: read the high word back on
    ; its own, so a swapped pair cannot pass by symmetry.
    ld.w    4[r14], r26
    cmp     r23, r26
    bz      .Ld_ord_ok
    add     1, r20
.Ld_ord_ok:
    add     1, r21

    ; ---- report -----------------------------------------------------
    ;
    ; Two digits on the stack: checks run, then failures. Both, because
    ; "0 failures" from a guest that ran no checks is the null result
    ; this project keeps being caught by -- a trap on the first disp23
    ; instruction would leave r20 at zero and look like a pass.
    add     -8, r3
    mov     0x30, r6
    add     r21, r6
    st.b    r6, 0[r3]
    mov     0x30, r6
    add     r20, r6
    st.b    r6, 1[r3]
    mov     0x0A, r6
    st.b    r6, 2[r3]

    mov     1, r6
    mov     #_msg, r7
    mov     9, r8
    mov     64, r11
    trap    0

    mov     1, r6
    mov     r3, r7
    mov     3, r8
    mov     64, r11
    trap    0

    mov     r20, r6                 ; exit(failures)
    mov     93, r11
    trap    0
_spin:
    br      _spin

    .section .rodata, const
    .align 4
    .public _msg
_msg:
    .db "G4MH-D23 "

; The buffer these loads and stores address. In .text because this guest
; is linked as one flat image loaded into RAM, so .text is writable here
; -- which is the same arrangement the other guests in this directory
; use, and is why they need no .data section at all.
    .section .text, text
    .align 4
    .public _buf
_buf:
    .ds 64
