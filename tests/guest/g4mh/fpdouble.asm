; SPDX-License-Identifier: Apache-2.0
;
; fpdouble.asm - double precision, as CC-RH encodes it.
;
; The unit tests for this build their own halfwords from a table this
; project wrote down, so they agree with whatever the frontend already
; believes. This guest does not: Renesas' assembler produces the bytes
; and the emulator executes them, so a disagreement about which
; sub-opcode is ADDF.D -- or about which half of a register pair holds
; the high word -- shows up as a wrong value rather than as two encoders
; being wrong together.
;
;   docker run --rm -v "$PWD":/w -w /w --entrypoint /bin/bash ccrh:latest -c '\
;     B=/usr/local/Renesas/CC-RH/V2.08.00/bin; \
;     $B/asrh -Xcommon=rh850 -Xcpu=g4mh fpdouble.asm && \
;     $B/rlink fpdouble.obj -start=.text,.rodata/80000000 \
;             -output=fpdouble.abs -form=absolute -entry=__start'
;
;   ./build/host/emu-host --frontend g4mh fpdouble.abs
;   ./build/host/emu-host --frontend g4mh --jit fpdouble.abs
;
; r20 counts failures and r21 counts checks, printed as two digits, so
; "0 failures" from a guest that ran nothing cannot pass for a result.
;
    .section .text, text
    .align 4
    .public __start
__start:
    mov     0, r20                  ; failures
    mov     0, r21                  ; checks

    ; PSW.CU0, or every instruction below is a coprocessor-unusable
    ; exception rather than an answer.
    mov     0x00010000, r10
    ldsr    r10, 5

    ; ---- 3.0 + 1.0 = 4.0, and 3.0 - 1.0 = 2.0 -----------------------
    ; A double is a register pair: low word in rN, high in rN+1.
    mov     0x00000000, r6
    mov     0x3FF00000, r7          ; r6:r7 = 1.0
    mov     0x00000000, r8
    mov     0x40080000, r9          ; r8:r9 = 3.0

    addf.d  r6, r8, r10             ; r10:r11 <- r8 + r6
    mov     0x40100000, r12         ; 4.0, high word
    cmp     r12, r11
    bz      .Ladd_hi
    add     1, r20
.Ladd_hi:
    add     1, r21
    cmp     r0, r10                 ; low word must be zero
    bz      .Ladd_lo
    add     1, r20
.Ladd_lo:
    add     1, r21

    subf.d  r6, r8, r10             ; 3.0 - 1.0, not 1.0 - 3.0
    mov     0x40000000, r12         ; +2.0
    cmp     r12, r11
    bz      .Lsub
    add     1, r20
.Lsub:
    add     1, r21

    ; ---- the pair really is two registers ---------------------------
    ; 2.0 and 2 + 2^-50 differ only in the *low* word, so a subtract
    ; that read one register would give zero here.
    mov     0x00000000, r6
    mov     0x40000000, r7          ; 2.0
    mov     0x00000002, r8
    mov     0x40000000, r9          ; 2 + 2^-50
    subf.d  r6, r8, r10
    mov     0x3CD00000, r12         ; 2^-50
    cmp     r12, r11
    bz      .Lulp
    add     1, r20
.Lulp:
    add     1, r21

    ; ---- precision conversions --------------------------------------
    mov     0x40400000, r6          ; single 3.0
    cvtf.sd r6, r10                 ; -> double 3.0
    mov     0x40080000, r12
    cmp     r12, r11
    bz      .Lsd
    add     1, r20
.Lsd:
    add     1, r21

    cvtf.ds r10, r14                ; and back to single
    mov     0x40400000, r12
    cmp     r12, r14
    bz      .Lds
    add     1, r20
.Lds:
    add     1, r21

    ; ---- the 64-bit integer conversions -----------------------------
    ; 2^40 + 5 needs more than 32 bits, so anything keeping only the low
    ; word answers 5.
    mov     0x00000005, r6
    mov     0x00000100, r7          ; r6:r7 = 2^40 + 5
    cvtf.ld r6, r10                 ; long -> double
    cvtf.dl r10, r14                ; and back
    cmp     r6, r14
    bz      .Ll_lo
    add     1, r20
.Ll_lo:
    add     1, r21
    cmp     r7, r15
    bz      .Ll_hi
    add     1, r20
.Ll_hi:
    add     1, r21

    ; ---- report -----------------------------------------------------
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

    mov     r20, r6
    mov     93, r11
    trap    0
_spin:
    br      _spin

    .section .rodata, const
    .align 4
    .public _msg
_msg:
    .db "G4MH-FPD "
