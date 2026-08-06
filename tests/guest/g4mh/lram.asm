; SPDX-License-Identifier: Apache-2.0
;
; lram.asm - the U2B memory map, checked from the guest side.
;
; Local RAM is mapped twice: at the SELF alias, which means *this* PE's
; RAM, and at each PE's absolute address, which means the same memory
; whichever PE is looking. This checks both halves, and the second one is
; what makes it a real test -- writing and reading back through SELF alone
; passes just as well against three copies of one buffer.
;
; Cores run in order within a round and this program is 22 instructions
; against a 4096-instruction quantum, so PE0 has finished before PE1
; starts: reading PE0's window from PE1 is deterministic.
;
; A/B'd by pointing every SELF alias at PE0's buffer, which makes PE1 and
; PE2 report NG. The first version did *not* discriminate -- it read
; HTCFG0 with the wrong register id, so every PE tagged its RAM with the
; same value and the collapse was invisible.
;
;   docker run --rm -v "$PWD":/w -w /w --entrypoint /bin/bash ccrh:latest -c '\
;     B=/usr/local/Renesas/CC-RH/V2.08.00/bin; \
;     $B/asrh -Xcommon=rh850 -Xcpu=g4mh lram.asm && \
;     $B/rlink lram.obj -start=.text,.rodata/0 \
;             -output=lram.abs -form=absolute -entry=__start'
;
;   ./build/g4mh3/rv32-host --frontend g4mh lram.abs
;
    .section .text, text
    .align 4
    .public __start
__start:
    ; Which PE am I? HTCFG0 (SR2, selID 2) reports it -- the same register
    ; real startup code reads to pick its own stack.
    stsr    0, r19, 2              ; r19 = HTCFG0 = PE number

    ; Tag this PE's own local RAM through the SELF alias with a value only
    ; this PE would write.
    mov     0xFDE00000, r20
    mov     0x1000, r12
    add     r19, r12               ; 0x1000 + peid
    st.w    r12, 0[r20]

    ; Read it straight back: SELF must be *this* PE's memory.
    ld.w    0[r20], r13
    cmp     r12, r13
    bne     _fail

    ; And PE0's absolute window must hold PE0's tag, whichever PE asks.
    ; That is what separates a real alias from three copies of one buffer:
    ; on PE1 and PE2 this address is another core's RAM, not their own.
    ; Cores run in order within a round, so PE0 has already written.
    mov     0xFDC00000, r21
    ld.w    0[r21], r14
    mov     0x1000, r15
    cmp     r15, r14
    bne     _fail

    mov     1, r6
    mov     #_ok, r7
    mov     8, r8
    mov     64, r11
    trap    0
    mov     0, r6
    br      _exit
_fail:
    mov     1, r6
    mov     #_bad, r7
    mov     8, r8
    mov     64, r11
    trap    0
    mov     1, r6
_exit:
    mov     93, r11
    trap    0
_spin:
    br      _spin

    .section .rodata, const
    .align 4
    .public _ok
_ok:
    .db "lram ok", 0x0A
    .public _bad
_bad:
    .db "lram NG", 0x0A
