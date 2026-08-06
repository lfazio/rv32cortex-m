; SPDX-License-Identifier: Apache-2.0
;
; hello.asm - the smallest RH850 guest that proves the toolchain path.
;
; Built with the real Renesas CC-RH assembler and linker, not with an
; assembled-by-hand instruction array, which is the point: the unit tests
; encode their own opcodes and so agree with whatever the frontend already
; believes. This does not -- and the first time it ran it found two things
; the unit tests could not: CC-RH emits e_machine 36 (EM_V800) where the
; frontend accepted only 87, and the syscall number cannot come from the
; five-bit TRAP vector.
;
;   docker run --rm -v "$PWD":/w -w /w --entrypoint /bin/bash ccrh:latest -c '\
;     B=/usr/local/Renesas/CC-RH/V2.08.00/bin; \
;     $B/asrh -Xcommon=rh850 -Xcpu=g4mh hello.asm && \
;     $B/rlink hello.obj -start=.text,.rodata/80000000 \
;             -output=hello.abs -form=absolute -entry=__start'
;
;   ./build/g4mh/rv32-host --frontend g4mh hello.abs
;
; The host's syscall ABI: number in r11, arguments in r6-r9, result in r10,
; entered with TRAP. The number is *not* the trap vector -- that field is
; five bits and cannot reach newlib's 64 and 93.
;
    .section .text, text
    .align 4
    .public __start
__start:
    mov     1, r6
    mov     #_msg, r7
    mov     22, r8
    mov     64, r11
    trap    0
    mov     0, r6
    mov     93, r11
    trap    0
_spin:
    br      _spin

    .section .rodata, const
    .align 4
    .public _msg
_msg:
    .db "hello from RH850 G4MH", 0x0A
