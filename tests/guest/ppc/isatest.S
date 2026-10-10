/* SPDX-License-Identifier: Apache-2.0 */
/*
 * isatest.S - the first e200z7 guest that runs.
 *
 * Why this exists, and why it is assembly
 * ---------------------------------------
 *
 * Until this file the PowerPC frontend had never executed a program.
 * Its whole test suite was hand-assembled halfword arrays in
 * tests/unit/test_ppc.c -- the *same* arrangement G4MH had, and
 * CLAUDE.md records what that cost: the unit tests could not find three
 * consecutive decode defects, and one compiled 90-line C guest found all
 * three. A suite that shares an author with the thing it tests agrees
 * with it.
 *
 * The escape is a second, independent encoder. For G4MH that was Renesas
 * CC-RH; here it is **GNU binutils**. Every mnemonic below is assembled
 * by powerpc-linux-gnu-as, which has its own opinion about the encoding,
 * so a disagreement with this emulator's decoder shows up as a wrong
 * answer or a desynchronised instruction stream rather than as a passing
 * test.
 *
 * It is assembly rather than C because Debian's powerpc-linux-gnu-gcc is
 * **not built with VLE support** -- `-mvle` is rejected outright -- while
 * the assembler and objdump handle VLE fine. So the compiler half of the
 * G4MH arrangement is unavailable, and that limit is worth stating
 * rather than papering over: hand-written assembly still reflects what
 * its author thought to write. This catches encoding and semantic
 * errors; it does not catch "the form a compiler actually emits", which
 * is precisely the class that cost G4MH three defects.
 *
 * Writing it already paid for itself twice, before it ran at all. The
 * assembler rejected `sc` (VLE spells it se_sc/e_sc), rejected `rlwinm`
 * (e_rlwinm), and rejected every se_ form naming r8-r23 -- because the
 * compressed register field maps 0-15 to r0-r7 and r24-r31. The decoder
 * already had that right, with a comment saying it had been confirmed
 * against this same assembler. That is what a second encoder is for.
 *
 * Protocol
 * --------
 * The host harness's newlib subset, reached through `se_sc`:
 *
 *   r0 = 64, r3 = fd, r4 = buf, r5 = len    write
 *   r0 = 93, r3 = code                      exit
 *
 * Each failing check prints its name, and the exit status is the number
 * of failures -- so a harness needs no output parsing to know.
 */

	.text
	.globl _start

/* ------------------------------------------------------------------ */
/* Vectors                                                             */
/* ------------------------------------------------------------------ */

/*
 * **A trap only reports if something catches it.**
 *
 * This is the lesson that cost G4MH three sessions and is written at the
 * top of ppc_interp.c, so this guest does not repeat it. With IVPR and
 * the IVORs left at zero an unimplemented encoding vectors to address 0
 * -- which in a flat image is this file's own entry point, so the
 * program silently restarts and presents as a hang.
 *
 * That is not a hypothetical here: while this guest was being written it
 * hit three separate decode gaps, and *every one of them* showed up as
 * "runs to the instruction limit with no output". Finding which
 * instruction took the trap needed a trace build and a manual bisection
 * each time.
 *
 * With the handler below the same failure prints the faulting pc and
 * exits 99. Book E computes the vector as IVPR[0:15] || IVORn[16:27] ||
 * 0b0000, so IVPR is the 64 KB page and each IVOR a 16-byte-aligned
 * offset inside it -- which is why the handler sits at a fixed .org
 * rather than wherever the assembler would have put it.
 */
_reset:
	e_b	_start

	.org	0x100
_trap:
	mfspr	%r6, 26			/* SRR0: the faulting pc */
	e_lis	%r7, trapmsg@ha
	e_add16i %r7, %r7, trapmsg@l
	e_add16i %r7, %r7, 18		/* the last hex digit; see trapmsg */
	e_li	%r8, 8
1:
	e_rlwinm %r9, %r6, 0, 28, 31	/* the low nibble */
	e_cmp16i %r9, 10
	e_blt	2f
	e_add16i %r9, %r9, 87		/* 'a' - 10 */
	e_b	3f
2:
	e_add16i %r9, %r9, 48		/* '0' */
3:
	e_stb	%r9, 0(%r7)
	e_add16i %r7, %r7, -1
	se_srwi	%r6, 4
	e_add16i %r8, %r8, -1
	e_cmp16i %r8, 0
	e_bgt	1b

	e_lis	%r4, trapmsg@ha
	e_add16i %r4, %r4, trapmsg@l
	e_li	%r5, (trapmsg_end - trapmsg)
	e_li	%r3, 1
	e_li	%r0, 64
	se_sc

	e_li	%r3, 99			/* a code no check count can be */
	e_li	%r0, 93
	se_sc
4:	e_b	4b

	.org	0x200

/* ------------------------------------------------------------------ */
/* Helpers                                                             */
/* ------------------------------------------------------------------ */

/*
 * CHECK reg, expected, name -- compare rN against a 32-bit constant and
 * count a failure.
 *
 * r30 is the failure count and r31 the comparison scratch, so no check
 * may use either. Deliberately *not* a subroutine: a call would need lr
 * saved and restored around every check, and a defect in the link
 * register is exactly what this file should be able to find -- so the
 * checking must not depend on lr working.
 *
 * The constant is built with e_lis/e_or2i rather than @h/@l, which are
 * for symbols; the halves are taken explicitly so a negative expectation
 * (0xFFFF:FFF2 for -14) is written the same way as a positive one.
 */
	.macro CHECK reg, want, name
	e_lis	%r31, ((\want) >> 16) & 0xFFFF
	e_or2i	%r31, (\want) & 0xFFFF
	cmpw	%r\reg, %r31
	e_beq	9998f
	e_add16i %r30, %r30, 1
	e_lis	%r4, msg_\name@ha
	e_add16i %r4, %r4, msg_\name@l
	e_li	%r5, (msg_\name\()_end - msg_\name)
	e_li	%r3, 1
	e_li	%r0, 64
	se_sc
9998:
	.endm

/* A string in .rodata, with an end label so CHECK can size it. */
	.macro MSG name, text
	.section .rodata
msg_\name:
	.ascii "\text\n"
msg_\name\()_end:
	.text
	.endm

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

_start:
	e_li	%r30, 0			/* failure count */

	/*
	 * Arm the vectors before anything can fault. IVPR is the 64 KB
	 * page and each IVOR the offset within it, so all four of the
	 * interrupts a decode gap can raise point at the one handler:
	 * data storage (2), instruction storage (3), alignment (5) and
	 * program (6) -- the last being what an unimplemented encoding
	 * takes. System call (8) is deliberately *not* redirected: the
	 * host harness consumes `se_sc` before it ever becomes an
	 * interrupt, and pointing it here would only matter if that
	 * stopped being true.
	 */
	e_lis	%r3, 0x8000		/* IVPR = the image's 64 KB page */
	mtspr	63, %r3
	e_li	%r3, 0x100		/* every IVOR -> _trap */
	mtspr	402, %r3		/* IVOR2  data storage        */
	mtspr	403, %r3		/* IVOR3  instruction storage */
	mtspr	405, %r3		/* IVOR5  alignment           */
	mtspr	406, %r3		/* IVOR6  program             */

/* ---- 32-bit arithmetic, the e_* forms -------------------------- */

	e_li	%r3, 40
	e_add16i %r3, %r3, 2
	CHECK	3, 42, add16i

	e_lis	%r4, 0x1234
	e_or2i	%r4, 0x5678
	CHECK	4, 0x12345678, lis_or2i

	/*
	 * e_add2is adds to the *upper* half, and its field is signed --
	 * so a negative addend is the awkward input, because a decoder
	 * that zero-extends is right for every positive one.
	 */
	e_lis	%r5, 0x0002
	e_add2is %r5, -1
	CHECK	5, 0x00010000, add2is_neg

/* ---- the 16-bit se_* forms ------------------------------------- */
/* Only r0-r7 and r24-r31 are encodable here; the assembler enforces it. */

	e_li	%r6, 7
	e_li	%r7, 5
	se_add	%r6, %r7
	CHECK	6, 12, se_add

	se_sub	%r6, %r7
	CHECK	6, 7, se_sub

	/* se_li is 7-bit unsigned, so 127 is where a narrow field shows. */
	se_li	%r5, 127
	CHECK	5, 127, se_li_max

	e_li	%r24, 0
	se_not	%r24
	CHECK	24, 0xFFFFFFFF, se_not

	/* r24-r31 are the other half of the compressed field's range. */
	e_li	%r25, 1
	se_slwi	%r25, 31
	CHECK	25, 0x80000000, se_slwi_31

	se_srwi	%r25, 31
	CHECK	25, 1, se_srwi_31

	/*
	 * Shift by zero. Every shift encoder in this project has had a
	 * defect at an end of its range -- the Thumb-2 one twice -- so
	 * both ends are checked rather than a typical value.
	 */
	e_li	%r26, 0x5A5A
	se_slwi	%r26, 0
	CHECK	26, 0x5A5A, se_slwi_0

	/* Arithmetic shift keeps the sign where the logical one does not. */
	e_lis	%r27, 0x8000
	se_srawi %r27, 4
	CHECK	27, 0xF8000000, se_srawi_sign

/* ---- rotate and mask ------------------------------------------- */

	/*
	 * e_rlwinm is what every PowerPC compiler reaches for, and its
	 * three fields interact: a mask that *wraps* (MB > ME) is the
	 * case a naive implementation gets backwards, and it is the only
	 * one of these three the assembler does not rewrite to a
	 * simpler mnemonic.
	 */
	e_lis	%r3, 0x1234
	e_or2i	%r3, 0x5678
	e_rlwinm %r4, %r3, 8, 0, 31	/* pure rotate */
	CHECK	4, 0x34567812, rlwinm_rot

	e_rlwinm %r4, %r3, 0, 24, 31	/* low byte */
	CHECK	4, 0x00000078, rlwinm_mask

	e_rlwinm %r4, %r3, 0, 28, 3	/* wrapping mask */
	CHECK	4, 0x10000008, rlwinm_wrap

/* ---- compare and the condition register -------------------------- */

	e_li	%r3, -1
	e_li	%r4, 1
	cmpw	%r3, %r4		/* signed: -1 < 1 */
	e_blt	1f
	e_add16i %r30, %r30, 1
1:
	cmplw	%r3, %r4		/* unsigned: 0xFFFFFFFF > 1 */
	e_bgt	2f
	e_add16i %r30, %r30, 1
2:

	/*
	 * CR fields are numbered from the *left*, so cr7 is the low
	 * nibble. Putting cr0 where cr7 belongs is a trap ppc_interp.c
	 * documents, and only a compare into a non-zero field catches it:
	 * every test that uses cr0 passes either way.
	 */
	e_li	%r4, 1
	e_li	%r5, 1
	cmpw	%cr7, %r4, %r5
	mfcr	%r6
	e_and2i. %r6, 0x000F
	CHECK	6, 0x2, cr7_eq_bit

/* ---- load and store, and the byte order ------------------------- */

	/*
	 * The first big-endian guest in this tree, so this is the check
	 * that matters most here: a word store read back a byte at a
	 * time says which end the emulator put the high byte at, and it
	 * is wrong in a way no register-only test can see.
	 */
	e_lis	%r3, scratch@ha
	e_add16i %r3, %r3, scratch@l

	e_lis	%r4, 0x1122
	e_or2i	%r4, 0x3344
	e_stw	%r4, 0(%r3)
	e_lbz	%r5, 0(%r3)
	CHECK	5, 0x11, be_stw_byte0
	e_lbz	%r5, 3(%r3)
	CHECK	5, 0x44, be_stw_byte3

	e_lhz	%r5, 0(%r3)
	CHECK	5, 0x1122, be_lhz

	/* Sign extension: e_lha does it, e_lhz must not. */
	e_lis	%r4, 0x8000
	e_stw	%r4, 0(%r3)
	e_lhz	%r5, 0(%r3)
	CHECK	5, 0x00008000, lhz_no_sign
	e_lha	%r5, 0(%r3)
	CHECK	5, 0xFFFF8000, lha_sign

	/*
	 * se_ load/store take a *scaled* immediate: SD4 counts words for
	 * se_lwz/se_stw, not bytes. An unscaled decoder is right for
	 * offset 0 and nothing else, so offset 0 must not be the only
	 * one tested -- `se_stw r4,4(r3)` encodes SD4 = 1.
	 */
	e_lis	%r4, 0xAABB
	e_or2i	%r4, 0xCCDD
	se_stw	%r4, 4(%r3)
	e_lwz	%r5, 4(%r3)
	CHECK	5, 0xAABBCCDD, se_stw_sd4
	se_lwz	%r6, 4(%r3)
	CHECK	6, 0xAABBCCDD, se_lwz_sd4

/* ---- branches, the link register and ctr ------------------------ */

	e_li	%r3, 0
	e_bl	subr
	CHECK	3, 0x99, bl_and_blr

	/*
	 * e_bdnz decrements ctr and branches while it is non-zero, so a
	 * count of 3 runs the body three times. An implementation that
	 * tests before decrementing runs it four.
	 */
	e_li	%r3, 3
	mtctr	%r3
	e_li	%r4, 0
3:
	e_add16i %r4, %r4, 1
	e_bdnz	3b
	CHECK	4, 3, bdnz_count

/* ---- multiply and divide ---------------------------------------- */

	e_li	%r3, 1000
	e_li	%r4, 1000
	mullw	%r5, %r3, %r4
	CHECK	5, 1000000, mullw

	/* The high half is a separate instruction and a common gap. */
	e_lis	%r3, 0x0001
	e_li	%r4, 0x10
	mulhwu	%r5, %r3, %r4
	CHECK	5, 0x0, mulhwu

	e_li	%r3, 100
	e_li	%r4, 7
	divw	%r5, %r3, %r4
	CHECK	5, 14, divw

	e_li	%r3, -100
	divw	%r5, %r3, %r4
	CHECK	5, -14, divw_neg

	/*
	 * -100 read unsigned is 0xFFFFFF9C = 4294967196, and that over 7
	 * is 613566742 = 0x24924916. Computed, not guessed: the first
	 * version of this line said 0x24924924 -- a plausible-looking
	 * repeating pattern that is 14 too high -- and it was the only
	 * check of the twenty-nine that failed once the interpreter was
	 * right. An expectation written from the shape of a number is an
	 * expectation that will disagree with a correct implementation.
	 */
	divwu	%r5, %r3, %r4
	CHECK	5, 0x24924916, divwu

/* ---- what only VLE has, and no compiler emits -------------------- */

	/*
	 * The volatile context save and restore (manual 3.14): one
	 * instruction for an interrupt handler's whole prologue. Each
	 * names a fixed set of registers, so the test is that the right
	 * ones went, in the right order, to the right words -- and came
	 * back. r26 is a frame below the stack pointer the loader set.
	 */
	e_add16i %r26, %r1, -128

	e_li	%r0, 0x100
	e_li	%r3, 0x103
	e_li	%r4, 0x104
	e_li	%r5, 0x105
	e_li	%r6, 0x106
	e_li	%r7, 0x107
	e_li	%r8, 0x108
	e_li	%r9, 0x109
	e_li	%r10, 0x10A
	e_li	%r11, 0x10B
	e_li	%r12, 0x10C
	e_stmvgprw 0(%r26)
	e_li	%r0, 0
	e_li	%r3, 0
	e_li	%r4, 0
	e_li	%r5, 0
	e_li	%r6, 0
	e_li	%r7, 0
	e_li	%r8, 0
	e_li	%r9, 0
	e_li	%r10, 0
	e_li	%r11, 0
	e_li	%r12, 0
	e_lmvgprw 0(%r26)
	/* r0 first and r3 second: r1 and r2 are not volatile and have no
	 * slot, so r3 is the *second* word, not the fourth. */
	CHECK	12, 0x10C, lmv_r12
	CHECK	8, 0x108, lmv_r8
	CHECK	3, 0x103, lmv_r3
	e_lwz	%r20, 0(%r26)
	CHECK	20, 0x100, stmv_word0
	e_lwz	%r20, 4(%r26)
	CHECK	20, 0x103, stmv_word1
	e_lwz	%r20, 40(%r26)
	CHECK	20, 0x10C, stmv_word10

	/* CR, LR, CTR and XER, in that order. */
	e_lis	%r3, 0x1234
	e_or2i	%r3, 0x5678
	mtcrf	0xff, %r3
	e_li	%r3, 0x1110
	mtspr	8, %r3
	e_li	%r3, 0x2220
	mtspr	9, %r3
	e_lis	%r3, 0x2000		/* XER[CA] */
	mtspr	1, %r3
	e_stmvsprw 48(%r26)
	e_li	%r3, 0
	mtcrf	0xff, %r3
	mtspr	8, %r3
	mtspr	9, %r3
	mtspr	1, %r3
	e_lmvsprw 48(%r26)
	mfcr	%r20
	CHECK	20, 0x12345678, lmv_cr
	mfspr	%r20, 8
	CHECK	20, 0x1110, lmv_lr
	mfspr	%r20, 9
	CHECK	20, 0x2220, lmv_ctr
	mfspr	%r20, 1
	CHECK	20, 0x20000000, lmv_xer
	/*
	 * Every word, not the ends: a save and a restore that agree on the
	 * wrong order round-trip perfectly, and the handler that reads the
	 * frame is the one that finds out.
	 */
	e_lwz	%r20, 48(%r26)
	CHECK	20, 0x12345678, stmv_cr_first
	e_lwz	%r20, 52(%r26)
	CHECK	20, 0x1110, stmv_lr_second
	e_lwz	%r20, 56(%r26)
	CHECK	20, 0x2220, stmv_ctr_third
	e_lwz	%r20, 60(%r26)
	CHECK	20, 0x20000000, stmv_xer_last

	/* The four save/restore pairs. */
	e_li	%r3, 0x4410
	mtspr	26, %r3
	e_li	%r3, 0x5510
	mtspr	27, %r3
	e_stmvsrrw 64(%r26)
	e_li	%r3, 0
	mtspr	26, %r3
	mtspr	27, %r3
	e_lmvsrrw 64(%r26)
	mfspr	%r20, 26
	CHECK	20, 0x4410, lmv_srr0
	mfspr	%r20, 27
	CHECK	20, 0x5510, lmv_srr1
	e_li	%r3, 0x4420
	mtspr	58, %r3
	e_li	%r3, 0x5520
	mtspr	59, %r3
	e_stmvcsrrw 64(%r26)
	e_li	%r3, 0
	mtspr	58, %r3
	mtspr	59, %r3
	e_lmvcsrrw 64(%r26)
	mfspr	%r20, 58
	CHECK	20, 0x4420, lmv_csrr0
	mfspr	%r20, 59
	CHECK	20, 0x5520, lmv_csrr1
	e_li	%r3, 0x4430
	mtspr	574, %r3
	e_li	%r3, 0x5530
	mtspr	575, %r3
	e_stmvdsrrw 64(%r26)
	e_li	%r3, 0
	mtspr	574, %r3
	mtspr	575, %r3
	e_lmvdsrrw 64(%r26)
	mfspr	%r20, 574
	CHECK	20, 0x4430, lmv_dsrr0
	mfspr	%r20, 575
	CHECK	20, 0x5530, lmv_dsrr1
	e_li	%r3, 0x4440
	mtspr	570, %r3
	e_li	%r3, 0x5540
	mtspr	571, %r3
	e_stmvmcsrrw 64(%r26)
	e_li	%r3, 0
	mtspr	570, %r3
	mtspr	571, %r3
	e_lmvmcsrrw 64(%r26)
	mfspr	%r20, 570
	CHECK	20, 0x4440, lmv_mcsrr0
	mfspr	%r20, 571
	CHECK	20, 0x5540, lmv_mcsrr1

	/* e_stmw and e_lmw: rS up to r31, which takes r30 and r31 with it
	 * -- the failure count and the scratch -- and puts them back. */
	e_li	%r27, 0x777
	e_li	%r28, 0x888
	e_li	%r29, 0x999
	e_li	%r31, 0x31F
	e_stmw	%r27, 80(%r26)
	/* The last register of the five is the one a count that is one
	 * short leaves out, and it is r31 -- so look at its slot, then
	 * change the slot and see that the load brings the change back. */
	e_lwz	%r20, 96(%r26)
	CHECK	20, 0x31F, stmw_r31_slot
	e_li	%r20, 0xABC
	e_stw	%r20, 96(%r26)
	e_li	%r27, 0
	e_li	%r28, 0
	e_li	%r29, 0
	e_lmw	%r27, 80(%r26)
	or	%r20, %r31, %r31
	CHECK	20, 0xABC, lmw_r31
	CHECK	27, 0x777, lmw_r27
	CHECK	29, 0x999, lmw_r29

	/*
	 * An interrupt taken from VLE says so: ESR[VLEMI] beside the cause
	 * (manual table 7-3). The handler is this guest's own for the one
	 * instruction, steps over it -- se_illegal is two bytes -- and
	 * comes back with se_rfi.
	 */
	e_lis	%r3, vle_handler@ha
	e_add16i %r3, %r3, vle_handler@l
	mtspr	406, %r3
	e_li	%r20, 0
	e_li	%r21, 0
ill_at:
	se_illegal
	CHECK	20, 0x08000020, esr_vlemi
	e_lis	%r22, ill_at@ha
	e_add16i %r22, %r22, ill_at@l
	subf	%r21, %r22, %r21
	CHECK	21, 0, illegal_srr0
	e_li	%r3, 0x100
	mtspr	406, %r3

	/* e_sc is the 32-bit spelling of the same system call: a write of
	 * no bytes, which the harness answers and nothing else notices. */
	e_li	%r5, 0
	e_li	%r3, 1
	e_li	%r0, 64
	e_sc

/* ---- done -------------------------------------------------------- */

	e_lis	%r4, msg_done@ha
	e_add16i %r4, %r4, msg_done@l
	e_li	%r5, (msg_done_end - msg_done)
	e_li	%r3, 1
	e_li	%r0, 64
	se_sc

	se_mr	%r3, %r30		/* exit(failures) */
	e_li	%r0, 93
	se_sc
4:	e_b	4b			/* the harness halts us; never reached */

/*
 * A leaf reached by e_bl, so lr must survive the call and se_blr must
 * return through it.
 */
subr:
	e_li	%r3, 0x99
	se_blr

/*
 * IVOR6 for the one instruction above. Sixteen-byte aligned, because
 * an IVOR's low four bits are not part of the address.
 */
	.balign	16
vle_handler:
	mfspr	%r20, 62		/* ESR  */
	mfspr	%r21, 26		/* SRR0 */
	e_add16i %r22, %r21, 2
	mtspr	26, %r22
	se_rfi

/* ------------------------------------------------------------------ */
/* Data                                                                */
/* ------------------------------------------------------------------ */

	MSG	add16i,        "FAIL e_add16i"
	MSG	lis_or2i,      "FAIL e_lis/e_or2i"
	MSG	add2is_neg,    "FAIL e_add2is negative"
	MSG	se_add,        "FAIL se_add"
	MSG	se_sub,        "FAIL se_sub"
	MSG	se_li_max,     "FAIL se_li 127"
	MSG	se_not,        "FAIL se_not"
	MSG	se_slwi_31,    "FAIL se_slwi 31"
	MSG	se_srwi_31,    "FAIL se_srwi 31"
	MSG	se_slwi_0,     "FAIL se_slwi 0"
	MSG	se_srawi_sign, "FAIL se_srawi sign"
	MSG	rlwinm_rot,    "FAIL e_rlwinm rotate"
	MSG	rlwinm_mask,   "FAIL e_rlwinm mask"
	MSG	rlwinm_wrap,   "FAIL e_rlwinm wrapping mask"
	MSG	cr7_eq_bit,    "FAIL cr7 field numbering"
	MSG	be_stw_byte0,  "FAIL big-endian stw byte 0"
	MSG	be_stw_byte3,  "FAIL big-endian stw byte 3"
	MSG	be_lhz,        "FAIL e_lhz"
	MSG	lhz_no_sign,   "FAIL e_lhz sign extended"
	MSG	lha_sign,      "FAIL e_lha did not sign extend"
	MSG	se_stw_sd4,    "FAIL se_stw SD4 scaling"
	MSG	se_lwz_sd4,    "FAIL se_lwz SD4 scaling"
	MSG	bl_and_blr,    "FAIL e_bl/se_blr"
	MSG	bdnz_count,    "FAIL e_bdnz count"
	MSG	mullw,         "FAIL mullw"
	MSG	mulhwu,        "FAIL mulhwu"
	MSG	divw,          "FAIL divw"
	MSG	divw_neg,      "FAIL divw negative"
	MSG	divwu,         "FAIL divwu"
	MSG	stmv_lr_second, "FAIL stmv lr second"
	MSG	stmv_ctr_third, "FAIL stmv ctr third"
	MSG	stmw_r31_slot, "FAIL stmw r31 slot"
	MSG	lmw_r31, "FAIL lmw r31"
	MSG	lmv_r12, "FAIL lmv r12"
	MSG	lmv_r8, "FAIL lmv r8"
	MSG	lmv_r3, "FAIL lmv r3"
	MSG	stmv_word0, "FAIL stmv word0"
	MSG	stmv_word1, "FAIL stmv word1"
	MSG	stmv_word10, "FAIL stmv word10"
	MSG	lmv_cr, "FAIL lmv cr"
	MSG	lmv_lr, "FAIL lmv lr"
	MSG	lmv_ctr, "FAIL lmv ctr"
	MSG	lmv_xer, "FAIL lmv xer"
	MSG	stmv_cr_first, "FAIL stmv cr first"
	MSG	stmv_xer_last, "FAIL stmv xer last"
	MSG	lmv_srr0, "FAIL lmv srr0"
	MSG	lmv_srr1, "FAIL lmv srr1"
	MSG	lmv_csrr0, "FAIL lmv csrr0"
	MSG	lmv_csrr1, "FAIL lmv csrr1"
	MSG	lmv_dsrr0, "FAIL lmv dsrr0"
	MSG	lmv_dsrr1, "FAIL lmv dsrr1"
	MSG	lmv_mcsrr0, "FAIL lmv mcsrr0"
	MSG	lmv_mcsrr1, "FAIL lmv mcsrr1"
	MSG	lmw_r27, "FAIL lmw r27"
	MSG	lmw_r29, "FAIL lmw r29"
	MSG	esr_vlemi, "FAIL esr vlemi"
	MSG	illegal_srr0, "FAIL illegal srr0"

	.section .rodata
msg_done:
	.ascii "PPC-ISATEST-END\n"
msg_done_end:

/*
 * Writable, because the handler patches the eight hex digits in place.
 * The offset is load-bearing: "TRAP at pc=" is 11 characters, so the
 * digits occupy bytes 11..18 and _trap starts at +18 and writes
 * backwards. Changing the text without changing that constant puts the
 * pc somewhere in the middle of the words.
 */
	.section .data
trapmsg:
	.ascii "TRAP at pc=00000000\n"
trapmsg_end:

	.section .data
	.align 4
scratch:
	.space 32
