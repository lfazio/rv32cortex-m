/* SPDX-License-Identifier: Apache-2.0 */
/*
 * sys_stubs.S - the instructions sys.c cannot write in C, and the
 * vectors that record what they did.
 *
 * Every test here is a leaf called with `bl`. That is what makes a
 * fault recoverable without knowing how long the faulting instruction
 * is or where the compiler put anything: the handler returns to LR,
 * which is the C caller, with the interrupt's number as the stub's
 * return value. A stub that did not fault returns whatever it was
 * written to. (`&&label` is not a resume address -- this tree has
 * written that down once already.)
 *
 * The two interrupts that are meant to be returned *from* -- a system
 * call and the floating-point round interrupt, both of which name the
 * following instruction -- are let through when sys.c sets
 * g_trap.resume.
 */

#define SPR_SRR0   26
#define SPR_SRR1   27
#define SPR_DEAR   61
#define SPR_ESR    62
#define SPR_SPRG0  272
#define SPR_SPRG1  273
#define SPR_IVOR0  400
#define SPR_IVOR32 528

/* struct trap in sys.c */
#define T_IVOR   0
#define T_SRR0   4
#define T_SRR1   8
#define T_ESR    12
#define T_DEAR   16
#define T_MSR    20
#define T_COUNT  24
#define T_RESUME 28
#define T_CR     32
#define T_R6     36

	.text

	.macro	svec n
	.balign	16
sys_vec\n:
	mtspr	SPR_SPRG0, %r3
	li	%r3, \n
	b	sys_trap
	.endm

	svec	0
	svec	1
	svec	2
	svec	3
	svec	4
	svec	5
	svec	6
	svec	7
	svec	8
	svec	9
	svec	11
	svec	12
	svec	13
	svec	14
	svec	15
	svec	32
	svec	33
	svec	34
	svec	35

sys_trap:
	mtspr	SPR_SPRG1, %r4
	lis	%r4, g_trap@ha
	addi	%r4, %r4, g_trap@l
	stw	%r3, T_IVOR(%r4)
	mfspr	%r3, SPR_SRR0
	stw	%r3, T_SRR0(%r4)
	mfspr	%r3, SPR_SRR1
	stw	%r3, T_SRR1(%r4)
	mfspr	%r3, SPR_ESR
	stw	%r3, T_ESR(%r4)
	mfspr	%r3, SPR_DEAR
	stw	%r3, T_DEAR(%r4)
	mfmsr	%r3			/* what the interrupt left of MSR */
	stw	%r3, T_MSR(%r4)
	stw	%r6, T_R6(%r4)		/* the FP tests' destination */
	lwz	%r3, T_COUNT(%r4)
	addi	%r3, %r3, 1
	stw	%r3, T_COUNT(%r4)
	mfcr	%r3
	stw	%r3, T_CR(%r4)
	lwz	%r3, T_RESUME(%r4)
	cmpwi	%r3, 0
	bne	1f

	/* Abandon the stub: back to its caller, in supervisor state, with
	 * the interrupt's number where its result would have been. */
	mflr	%r3
	mtspr	SPR_SRR0, %r3
	mfspr	%r3, SPR_SRR1
	rlwinm	%r3, %r3, 0, 18, 16	/* clear PR */
	mtspr	SPR_SRR1, %r3
	lwz	%r3, T_IVOR(%r4)
	mfspr	%r4, SPR_SPRG1
	rfi

1:	/* Carry on where SRR0 says, with everything as it was. */
	lwz	%r3, T_CR(%r4)
	mtcrf	0xff, %r3
	mfspr	%r4, SPR_SPRG1
	mfspr	%r3, SPR_SPRG0
	rfi

/*
 * Point every vector but the decrementer's at the recorders above. An
 * IVOR is an offset inside IVPR's 64 KiB page, and these addresses are
 * in the page crt0 already named, so the low half of the address is the
 * offset.
 */
	.globl	sys_install
sys_install:
	.macro	ivor spr, n
	lis	%r3, sys_vec\n@ha
	addi	%r3, %r3, sys_vec\n@l
	mtspr	\spr, %r3
	.endm
	ivor	SPR_IVOR0 + 0, 0
	ivor	SPR_IVOR0 + 1, 1
	ivor	SPR_IVOR0 + 2, 2
	ivor	SPR_IVOR0 + 3, 3
	ivor	SPR_IVOR0 + 4, 4
	ivor	SPR_IVOR0 + 5, 5
	ivor	SPR_IVOR0 + 6, 6
	ivor	SPR_IVOR0 + 7, 7
	ivor	SPR_IVOR0 + 8, 8
	ivor	SPR_IVOR0 + 9, 9
	ivor	SPR_IVOR0 + 11, 11
	ivor	SPR_IVOR0 + 12, 12
	ivor	SPR_IVOR0 + 13, 13
	ivor	SPR_IVOR0 + 14, 14
	ivor	SPR_IVOR0 + 15, 15
	ivor	SPR_IVOR32 + 0, 32
	ivor	SPR_IVOR32 + 1, 33
	ivor	SPR_IVOR32 + 2, 34
	ivor	SPR_IVOR32 + 3, 35
	lis	%r3, sys_vec35@ha
	addi	%r3, %r3, sys_vec35@l
	blr				/* the highest one, for a range check */

/* ------------------------------------------------------------------ */
/* Supervisor stubs                                                    */
/* ------------------------------------------------------------------ */

	.macro	stub name
	.globl	\name
\name:
	.endm

stub stub_illegal
	.long	0x00000000
	li	%r3, 0xAA
	blr

stub stub_trap
	trap
	li	%r3, 0xAA
	blr

stub stub_twi_not			/* 7 < 5 is false: no trap */
	li	%r4, 7
	twlti	%r4, 5
	li	%r3, 0xAA
	blr

stub stub_twi_taken			/* 3 < 5 */
	li	%r4, 3
	twlti	%r4, 5
	li	%r3, 0xAA
	blr

stub stub_sc				/* r3 = the number, as r0 */
	mr	%r0, %r3
	sc
	li	%r3, 0xAA
	blr

stub stub_mfmsr
	mfmsr	%r3
	blr

stub stub_mtmsr
	mtmsr	%r3
	blr

	.macro	rd name, spr
stub \name
	mfspr	%r3, \spr
	blr
	.endm
	.macro	wr name, spr
stub \name
	mtspr	\spr, %r3
	li	%r3, 0xAA
	blr
	.endm

	rd	rd_pvr, 287
	rd	rd_svr, 1023
	rd	rd_l1cfg0, 515
	rd	rd_l1cfg1, 516
	rd	rd_tbl, 268
	rd	rd_tbl_w, 284		/* write-only: an invalid reference */
	rd	rd_dec, 22
	rd	rd_tsr, 336
	rd	rd_esr, 62
	rd	rd_dear, 61
	rd	rd_spefscr, 512
	rd	rd_sprg4, 276
	rd	rd_usprg0, 256
	rd	rd_hid0, 1008
	rd	rd_undef, 100		/* no such register, user-accessible number */
	rd	rd_undef_priv, 31	/* no such register, privileged number      */
	wr	wr_pvr, 287
	wr	wr_l1cfg0, 515
	wr	wr_tbl, 284
	wr	wr_dec, 22
	wr	wr_tsr, 336
	wr	wr_esr, 62
	wr	wr_dear, 61
	wr	wr_spefscr, 512
	wr	wr_sprg4, 276
	wr	wr_usprg0, 256
	wr	wr_l1csr0, 1010
	wr	wr_undef, 100

stub stub_wrteei0
	wrteei	0
	blr
stub stub_wrteei1
	wrteei	1
	blr

stub stub_load				/* r3 = address */
	lwz	%r3, 0(%r3)
	blr

stub stub_store				/* r3 = address, r4 = value */
	stw	%r4, 0(%r3)
	li	%r3, 0xAA
	blr

stub stub_jump				/* r3 = address; LR stays the caller's */
	mtctr	%r3
	bctr

stub stub_lwarx
	lwarx	%r4, 0, %r3
	li	%r3, 0xAA
	blr

/* Reserve and store in one go; returns CR, where CR0[EQ] says it took. */
stub stub_resv_w			/* r3 = address, r4 = value */
	lwarx	%r5, 0, %r3
	stwcx.	%r4, 0, %r3
	mfcr	%r3
	blr
stub stub_stwcx
	stwcx.	%r4, 0, %r3
	mfcr	%r3
	blr
/* lwarx at r3, then stwcx. at r5: for a misaligned store that has a
 * reservation to be refused under. */
stub stub_resv_other			/* r3 = reserve here, r4 = value, r5 = store here */
	lwarx	%r6, 0, %r3
	stwcx.	%r4, 0, %r5
	mfcr	%r3
	blr
/* The byte and halfword forms, which the e500 assembler does not know:
 * lbarx and lharx are XO 52 and 116, stbcx. and sthcx. 694 and 726. */
stub stub_resv_b
	.long	0x7CA01868		/* lbarx  r5,0,r3 */
	.long	0x7C801D6D		/* stbcx. r4,0,r3 */
	mfcr	%r3
	blr
stub stub_resv_h
	.long	0x7CA018E8		/* lharx  r5,0,r3 */
	.long	0x7C801DAD		/* sthcx. r4,0,r3 */
	mfcr	%r3
	blr
stub stub_lharx
	.long	0x7CA018E8
	li	%r3, 0xAA
	blr

/* stmw then lmw of three registers, and what came back. */
stub stub_multi				/* r3 = buffer */
	stwu	%r1, -32(%r1)
	stw	%r29, 8(%r1)
	stw	%r30, 12(%r1)
	stw	%r31, 16(%r1)
	lis	%r29, 0x1111
	ori	%r29, %r29, 0x2222
	lis	%r30, 0x3333
	ori	%r30, %r30, 0x4444
	lis	%r31, 0x5555
	ori	%r31, %r31, 0x6666
	stmw	%r29, 0(%r3)
	li	%r29, 0
	li	%r30, 0
	li	%r31, 0
	lmw	%r29, 0(%r3)
	xor	%r3, %r29, %r30
	add	%r3, %r3, %r31
	lwz	%r29, 8(%r1)
	lwz	%r30, 12(%r1)
	lwz	%r31, 16(%r1)
	addi	%r1, %r1, 32
	blr

stub stub_lmw				/* r3 = address; faults before it loads */
	lmw	%r31, 0(%r3)
	li	%r3, 0xAA
	blr

stub stub_dcbz
	dcbz	0, %r3
	li	%r3, 0xAA
	blr

stub stub_wait
	.long	0x7C00007C		/* wait */
	li	%r3, 0xAA
	blr

stub stub_evaddw			/* an SPE vector instruction */
	.long	0x10642A00		/* evaddw r3,r4,r5 */
	li	%r3, 0xAA
	blr

/* r6 = r3 op r4 under SPEFSCR r5, with r6 marked first so that "not
 * written" is something a caller can see. Returns r6. */
	.macro	fp name, insn
stub \name
	mtspr	512, %r5
	lis	%r6, 0x1234
	ori	%r6, %r6, 0x5678
	\insn	%r6, %r3, %r4
	mr	%r3, %r6
	blr
	.endm
	fp	fp_div, efsdiv
	fp	fp_add, efsadd
	fp	fp_mul, efsmul

/* ------------------------------------------------------------------ */
/* User mode                                                           */
/* ------------------------------------------------------------------ */

/*
 * Run a routine in problem state. It does not come back by returning:
 * every routine below ends in a system call, which is the only door
 * out, and the recorder then abandons it to this stub's caller.
 */
stub stub_user				/* r3 = routine, r5 = an argument for it */
	mtspr	SPR_SRR0, %r3
	mfmsr	%r4
	ori	%r4, %r4, 0x4000	/* MSR[PR] */
	mtspr	SPR_SRR1, %r4
	rfi

	.macro	leave
	lis	%r4, g_uval@ha
	stw	%r3, g_uval@l(%r4)
	li	%r0, 0x7777
	sc
2:	b	2b			/* a system call the host answered */
	.endm

	.macro	user name, insn:vararg
stub \name
	\insn
	leave
	.endm

	user	u_mfmsr, mfmsr %r3
	user	u_mtmsr, mtmsr %r3
	user	u_rd_srr0, mfspr %r3, 26
	user	u_rd_undef, mfspr %r3, 100
	user	u_rd_undef_priv, mfspr %r3, 31
	user	u_wr_l1cfg0, mtspr 515, %r3
	user	u_rd_tbl, mfspr %r3, 268
	user	u_rd_sprg4, mfspr %r3, 260
	user	u_rd_xer, mfspr %r3, 1
	user	u_wrteei, wrteei 1
	user	u_rfi, rfi
	user	u_dcbi, dcbi 0, %r3
	user	u_nothing, li %r3, 0x55

stub u_usprg0				/* r5 = value: write, read back */
	mtspr	256, %r5
	mfspr	%r3, 256
	leave

/* write(1, ...) as the host's semihosting numbers it. From problem
 * state it belongs to the guest's own handler, not to the host. */
stub u_host_write
	li	%r0, 64
	li	%r3, 1
	lis	%r4, g_uval@ha
	addi	%r4, %r4, g_uval@l
	li	%r5, 4
	sc
3:	b	3b
