; SPDX-License-Identifier: Apache-2.0
;
; crt0.asm - the vector table and C runtime start-up every G4MH guest
; gets, whether it asks for one or not.
;
; Extracted from scripts/g4mh-build-guest.sh, which used to carry it as a
; heredoc, once a second script needed the same thing. It is shared
; because every line of it was added after something failed without it.
;
; What is here and why:
;
;   * a 512-byte vector table, because **a trap only reports if
;     something catches it**. RBASE resets to the load address, so in a
;     flat image an unhandled exception lands on ordinary code twenty
;     bytes further on and the guest carries on producing plausible
;     output. That has cost this project three multi-session hunts on
;     two frontends.
;
;   * each slot records *which slot it is*, at 4 bytes of its 16. Every
;     FE cause reaches the same reporting code, so without it a vector
;     sent to the wrong handler prints identically to one sent to the
;     right handler -- the instrument could not measure the thing it
;     was for. Writing it that way immediately found three wrong
;     vectors in the emulator.
;
;   * the fault address, from MEA. A protection trap reporting only its
;     pc says an access was refused without saying what was reached
;     for, and the two are rarely near each other.
;
;   * gp and ep, which the C library addresses its own data through.
;
;   * .bss zeroed, because nothing else does it.
;
;   * PSW.CU0, or every floating-point instruction raises UCPOP.
;
; sp is *not* set here: the emulator's boot protocol already puts the top
; of guest RAM in r3, the core id in r6 and the RAM size in r7.

	.section .text_entry, text
	.public _entry
	.extern _main

; --- the table -----------------------------------------------------
; Each slot records *which slot it is* in r19 before branching. That
; costs 4 bytes of a 16-byte entry and is what makes the report able
; to tell a mis-mapped exception from a correctly mapped one: without
; it every FE cause prints identically, so a vector sent to the wrong
; handler reads exactly like a vector sent to the right one.
	.offset  0x000
_entry:
	jr   _start                 ; reset
; Renesas' board package puts a SYNCI here, citing technical update
; TN-RH8-B0183B/E: without it the lockstep checker core reads an
; uninitialised register. A no-op here, kept so the table is the
; shape a real guest has.
	synci
	.offset  0x010
	mov  0x010, r19
	jr   _fe                    ; SYSERR
	.offset  0x020
	mov  0x020, r19
	jr   _fe                    ; (reserved)
	.offset  0x030
	mov  0x030, r19
	jr   _fe                    ; FETRAP
	.offset  0x040
	mov  0x040, r19
	jr   _ei                    ; TRAP 0-15
	.offset  0x050
	mov  0x050, r19
	jr   _ei                    ; TRAP 16-31
	.offset  0x060
	mov  0x060, r19
	jr   _fe                    ; RIE
	.offset  0x070
	mov  0x070, r19
	jr   _fe                    ; FPE / FXE
	.offset  0x080
	mov  0x080, r19
	jr   _fe                    ; UCPOP
	.offset  0x090
	mov  0x090, r19
	jr   _fe                    ; MIP / MDP
	.offset  0x0A0
	mov  0x0A0, r19
	jr   _fe                    ; PIE
	.offset  0x0B0
	mov  0x0B0, r19
	jr   _fe                    ; (reserved: debug)
	.offset  0x0C0
	mov  0x0C0, r19
	jr   _fe                    ; MAE
	.offset  0x0D0
	mov  0x0D0, r19
	jr   _fe                    ; (reserved)
	.offset  0x0E0
	mov  0x0E0, r19
	jr   _fe                    ; FENMI
	.offset  0x0F0
	mov  0x0F0, r19
	jr   _fe                    ; FEINT
	.offset  0x100
	mov  0x100, r19
	jr   _ei                    ; EIINT priority 0
	.offset  0x110
	mov  0x110, r19
	jr   _ei                    ; EIINT priority 1
	.offset  0x120
	mov  0x120, r19
	jr   _ei                    ; EIINT priority 2
	.offset  0x130
	mov  0x130, r19
	jr   _ei                    ; EIINT priority 3
	.offset  0x140
	mov  0x140, r19
	jr   _ei                    ; EIINT priority 4
	.offset  0x150
	mov  0x150, r19
	jr   _ei                    ; EIINT priority 5
	.offset  0x160
	mov  0x160, r19
	jr   _ei                    ; EIINT priority 6
	.offset  0x170
	mov  0x170, r19
	jr   _ei                    ; EIINT priority 7
	.offset  0x180
	mov  0x180, r19
	jr   _ei                    ; EIINT priority 8
	.offset  0x190
	mov  0x190, r19
	jr   _ei                    ; EIINT priority 9
	.offset  0x1A0
	mov  0x1A0, r19
	jr   _ei                    ; EIINT priority 10
	.offset  0x1B0
	mov  0x1B0, r19
	jr   _ei                    ; EIINT priority 11
	.offset  0x1C0
	mov  0x1C0, r19
	jr   _ei                    ; EIINT priority 12
	.offset  0x1D0
	mov  0x1D0, r19
	jr   _ei                    ; EIINT priority 13
	.offset  0x1E0
	mov  0x1E0, r19
	jr   _ei                    ; EIINT priority 14
	.offset  0x1F0
	mov  0x1F0, r19
	jr   _ei                    ; EIINT priority 15  (16+ share this)
	.offset  0x200

; --- C runtime start-up --------------------------------------------
; sp already holds the top of guest RAM, from the emulator's boot
; protocol, so what is left is the two base registers and .bss.
;
; gp is what an access to a global in an sdata section goes through, and
; ep likewise for tdata/edata. A program with nothing in those sections
; does not notice they are unset; one linked against the C library does,
; and it presents as a wild address rather than a wrong value.
_start:
	mov  #__gp_data, gp
	mov  #__ep_data, ep

; Enable the FPU.
;
; PSW.CU0 gates coprocessor 0, which is where every floating-point
; instruction lives, and it is **clear out of reset**. Without it the
; first FP instruction raises UCPOP -- "coprocessor unusable" -- rather
; than computing anything, and CC-RH emits floating point for any C that
; uses a float or a double without being asked to.
;
; It cost a run that got as far as DOOM's R_InitPlanes, which is the
; first thing in that program to touch a float, and reported cause 0x80
; at slot 0x80 with MEA zero -- a trap with no address, because there is
; no address involved.
	stsr 5,   r6, 0             ; PSW
	movhi 0x0001, r6, r6        ; set CU0 (bit 16)
	ldsr r6,  5,  0

	mov  #__s.bss, r6
	mov  #__e.bss, r7
	jarl _zero, r31

	jarl _main, r31
	halt

; Zero [r6, r7). A word at a time: rlink aligns both ends of a section
; to 4, which is what lets this ignore a tail.
_zero:
	cmp  r7, r6
	bnl  _zero_done
	st.w r0, 0x00000000[r6]
	add  4, r6
	br   _zero
_zero_done:
	jmp  [r31]

; --- handlers ------------------------------------------------------
; FE and EI differ only in which pair of registers holds the cause and
; the return address; everything after that is common.
_fe:
	stsr 14, r6, 0              ; FEIC
	stsr 2,  r7, 0              ; FEPC
	jr   _report
_ei:
	stsr 13, r6, 0              ; EIIC
	stsr 0,  r7, 0              ; EIPC

; Print "!TRAP <cause> @<pc> #<slot>" and stop. Halting is the point: carrying on
; is what made these invisible.
_report:
	stsr 6,  r21, 2             ; MEA -- the address a memory fault touched
	mov  0x10000000, r20        ; NS16550 transmit holding register
	mov  0x21, r8               ; '!'
	st.b r8, 0x00000000[r20]
	mov  0x54, r8               ; 'T'
	st.b r8, 0x00000000[r20]
	mov  0x52, r8               ; 'R'
	st.b r8, 0x00000000[r20]
	mov  0x41, r8               ; 'A'
	st.b r8, 0x00000000[r20]
	mov  0x50, r8               ; 'P'
	st.b r8, 0x00000000[r20]
	mov  0x20, r8               ; ' '
	st.b r8, 0x00000000[r20]
	mov  r6, r18
	jarl _hex, r31
	mov  0x20, r8               ; ' '
	st.b r8, 0x00000000[r20]
	mov  0x40, r8               ; '@'
	st.b r8, 0x00000000[r20]
	mov  r7, r18
	jarl _hex, r31
	mov  0x20, r8               ; ' '
	st.b r8, 0x00000000[r20]
	mov  0x23, r8               ; '#'
	st.b r8, 0x00000000[r20]
	mov  r19, r18
	jarl _hex, r31
; The faulting address, which is the whole question for MIP and MDP: a
; protection trap that reports only its pc says an access was refused
; without saying what was reached for, and the two are rarely near each
; other. Meaningless for the other causes, and printed anyway rather
; than branching -- a table this size cannot afford a decision.
	mov  0x20, r8               ; ' '
	st.b r8, 0x00000000[r20]
	mov  0x21, r8               ; '!'
	st.b r8, 0x00000000[r20]
	mov  r21, r18
	jarl _hex, r31
	mov  0x0A, r8               ; '\n'
	st.b r8, 0x00000000[r20]
	halt

; r18 in, eight hex digits out, r20 the port. Clobbers r8, r9, r10.
_hex:
	mov  28, r9
_hex_loop:
	shr  r9, r18, r10
	andi 0x000F, r10, r10
	cmp  10, r10
	bge  _hex_af
	addi 0x0030, r10, r10       ; '0'
	br   _hex_out
_hex_af:
	addi 0x0037, r10, r10       ; 'A' - 10
_hex_out:
	st.b r10, 0x00000000[r20]
	add  -4, r9
	cmp  0, r9
	bge  _hex_loop
	jmp  [r31]

; --- dummy sections -------------------------------------------------
; Renesas' own start-up module ends with these. They make each section
; exist, so __s.bss and __e.bss resolve in a program whose C code
; happens to declare no zero-initialised global -- without them the
; link fails on an undefined symbol rather than zeroing nothing.
	.section ".data", data
.L.dummy.data:
	.section ".bss", bss
.L.dummy.bss:
	.section ".const", const
.L.dummy.const:
	.section ".text", text
.L.dummy.text:
