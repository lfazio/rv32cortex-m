; SPDX-License-Identifier: Apache-2.0
;
; mmiobench.handlers.asm - a vector table that steps over one refused
; store, for mmiobench.c's checks of what the window refuses.
;
; The shared crt0 reports any trap and halts, which is right for every
; guest but this one: here a refused store is the question, asked eight
; times before the kernels and eight after, once the JIT has inlined the
; window. So the guest points EBASE at this table and sets PSW.EBV
; (mdp_install). Every slot forwards to the same slot of crt0's table,
; which records the slot and reports as it always has, except MIP/MDP's:
; an MDP at the one store in refused_store is recorded -- FEIC, MEA, a
; count -- and stepped over. An MIP, or an MDP anywhere else, still
; halts with crt0's report.
;
; Linked after the C by scripts/g4mh-build-guest.sh, which takes a
; <guest>.handlers.asm beside <guest>.c. The table is in .text, whose
; alignment becomes 512 for every object (rlink says "Section alignment
; mismatch" and aligns the whole section): EBASE ignores its low 9 bits.

	.section .text, text
	.extern _entry
	.extern _g_mdp_count
	.extern _g_mdp_feic
	.extern _g_mdp_mea
	.public _mdp_install
	.public _refused_store

	.align 512
_vectors:
	jr   _entry                 ; reset: RBASE's, never this table's
	.align 16
	jr   _entry + 0x010         ; SYSERR
	.align 16
	jr   _entry + 0x020
	.align 16
	jr   _entry + 0x030         ; FETRAP
	.align 16
	jr   _entry + 0x040         ; TRAP 0-15
	.align 16
	jr   _entry + 0x050         ; TRAP 16-31
	.align 16
	jr   _entry + 0x060         ; RIE
	.align 16
	jr   _entry + 0x070         ; FPE / FXE
	.align 16
	jr   _entry + 0x080         ; UCPOP
	.align 16
	jr   _mdp_step              ; MIP / MDP
	.align 16
	jr   _entry + 0x0A0         ; PIE
	.align 16
	jr   _entry + 0x0B0
	.align 16
	jr   _entry + 0x0C0         ; MAE
	.align 16
	jr   _entry + 0x0D0
	.align 16
	jr   _entry + 0x0E0         ; FENMI
	.align 16
	jr   _entry + 0x0F0         ; FEINT
	.align 16
	jr   _entry + 0x100         ; EIINT priority 0
	.align 16
	jr   _entry + 0x110
	.align 16
	jr   _entry + 0x120
	.align 16
	jr   _entry + 0x130
	.align 16
	jr   _entry + 0x140
	.align 16
	jr   _entry + 0x150
	.align 16
	jr   _entry + 0x160
	.align 16
	jr   _entry + 0x170
	.align 16
	jr   _entry + 0x180
	.align 16
	jr   _entry + 0x190
	.align 16
	jr   _entry + 0x1A0
	.align 16
	jr   _entry + 0x1B0
	.align 16
	jr   _entry + 0x1C0
	.align 16
	jr   _entry + 0x1D0
	.align 16
	jr   _entry + 0x1E0
	.align 16
	jr   _entry + 0x1F0         ; EIINT priority 15 and above
	.align 16

; MDP is resumable: FEPC is the store, and nothing it would have written
; was written. Only the store at _refused_store_at is stepped over --
; four bytes, st.w reg2, disp16[reg1] -- so an access refused anywhere
; else cannot be skipped by mistake. PSW comes back from FEPSW at FERET,
; so the compare does not reach the interrupted code.
_mdp_step:
	add  -8, sp
	st.w r6, 0x00000000[sp]
	st.w r7, 0x00000004[sp]
	stsr 2,  r6, 0              ; FEPC
	mov  #_refused_store_at, r7
	cmp  r6, r7
	bne  _mdp_other
	add  4, r6
	ldsr r6, 2, 0               ; FEPC: past the store
	stsr 14, r6, 0              ; FEIC
	mov  #_g_mdp_feic, r7
	st.w r6, 0x00000000[r7]
	stsr 6,  r6, 2              ; MEA
	mov  #_g_mdp_mea, r7
	st.w r6, 0x00000000[r7]
	mov  #_g_mdp_count, r7
	ld.w 0x00000000[r7], r6
	add  1, r6
	st.w r6, 0x00000000[r7]
	ld.w 0x00000000[sp], r6
	ld.w 0x00000004[sp], r7
	add  8, sp
	feret
_mdp_other:
	ld.w 0x00000000[sp], r6
	ld.w 0x00000004[sp], r7
	add  8, sp
	jr   _entry + 0x090         ; crt0's report, which halts

; void mdp_install(void): this table, from now on.
_mdp_install:
	mov  #_vectors, r10
	ldsr r10, 3, 1              ; EBASE
	stsr 5,  r10, 0             ; PSW
	ori  0x8000, r10, r10       ; EBV
	ldsr r10, 5, 0
	jmp  [r31]

; unsigned refused_store(volatile unsigned *ok, unsigned v,
;                        volatile unsigned *addr)
;
; Stores v to ok, which the window permits, then to addr, which it must
; refuse, and returns v + 1, computed after the refused store: the
; instructions past it are seen to run. The refused store is second in
; its block, because a fault at a block's first instruction cannot see a
; translator's mistake about the middle.
_refused_store:
	st.w r7, 0x00000000[r6]
_refused_store_at:
	st.w r7, 0x00000000[r8]
	addi 1, r7, r10
	jmp  [r31]
