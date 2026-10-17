/* SPDX-License-Identifier: Apache-2.0 */
/*
 * intc_stubs.S - the handlers intc.c installs, and the accesses it
 * cannot write in C.
 *
 * Two external-input handlers, one per INTC handshake (MPC5675K
 * Reference Manual, Rev. 10, 32.5.2):
 *
 *   - ext_sw, software vector mode, Example 32-1: save what the EABI
 *     lets a C function clobber and SRR0/SRR1, read INTC_IACKR -- the
 *     acknowledgement -- enable interrupts so that a higher priority can
 *     preempt, call isr(iackr), disable, write INTC_EOIR, restore, rfi;
 *   - hwvec_page, hardware vector mode, Example 32-2: the core branches
 *     to IVPR[0:15] || vector << 4, so each 16-byte entry names its own
 *     vector and joins one body that calls hw_isr(vector).
 *
 * The hardware table needs a 64 KiB page of its own. crt0's vectors fill
 * the first 0x250 bytes of the image's page, and in hardware vector mode
 * vector 30 would land on crt0's slot for IVOR29 -- so intc.c points
 * IVPR at hwvec_page while it tests that mode, with the decrementer's
 * interrupt off, since every other IVOR names an offset in that page
 * too. This file is linked last, so .text.hwvec comes after every
 * handler that must stay in the first page.
 */

#define SPR_SRR0   26
#define SPR_SRR1   27
#define SPR_IVPR   63
#define SPR_TCR    340
#define SPR_IVOR4  404

#define INTC_IACKR 0xFFF48010
#define INTC_EOIR  0xFFF48018

/* 4-byte slots below the back chain and the LR save word. */
#define F_SIZE 96

	.macro	save_volatile
	stwu	%r1, -F_SIZE(%r1)
	stw	%r0, 8(%r1)
	stw	%r3, 12(%r1)
	stw	%r4, 16(%r1)
	stw	%r5, 20(%r1)
	stw	%r6, 24(%r1)
	stw	%r7, 28(%r1)
	stw	%r8, 32(%r1)
	stw	%r9, 36(%r1)
	stw	%r10, 40(%r1)
	stw	%r11, 44(%r1)
	stw	%r12, 48(%r1)
	mfcr	%r0
	stw	%r0, 52(%r1)
	mflr	%r0
	stw	%r0, 56(%r1)
	mfctr	%r0
	stw	%r0, 60(%r1)
	mfxer	%r0
	stw	%r0, 64(%r1)
	mfspr	%r0, SPR_SRR0
	stw	%r0, 68(%r1)
	mfspr	%r0, SPR_SRR1
	stw	%r0, 72(%r1)
	.endm

	/* The epilogue both handlers share: lower the priority, return --
	 * and give back `extra` bytes the entry pushed below the frame. */
	.macro	eoi_restore_rfi extra=0
	wrteei	0
	mbar				/* the flag's clear lands first, 32.4.3.1.2 */
	lis	%r3, INTC_EOIR@ha
	li	%r4, 0
	stw	%r4, INTC_EOIR@l(%r3)
	lwz	%r0, 72(%r1)
	mtspr	SPR_SRR1, %r0
	lwz	%r0, 68(%r1)
	mtspr	SPR_SRR0, %r0
	lwz	%r0, 64(%r1)
	mtxer	%r0
	lwz	%r0, 60(%r1)
	mtctr	%r0
	lwz	%r0, 56(%r1)
	mtlr	%r0
	lwz	%r0, 52(%r1)
	mtcr	%r0
	lwz	%r12, 48(%r1)
	lwz	%r11, 44(%r1)
	lwz	%r10, 40(%r1)
	lwz	%r9, 36(%r1)
	lwz	%r8, 32(%r1)
	lwz	%r7, 28(%r1)
	lwz	%r6, 24(%r1)
	lwz	%r5, 20(%r1)
	lwz	%r4, 16(%r1)
	lwz	%r3, 12(%r1)
	lwz	%r0, 8(%r1)
	addi	%r1, %r1, F_SIZE + \extra
	rfi
	.endm

	.text

	.balign	16
	.globl	ext_sw
ext_sw:
	save_volatile
	lis	%r3, INTC_IACKR@ha
	lwz	%r3, INTC_IACKR@l(%r3)	/* the acknowledgement */
	wrteei	1
	bl	isr
	eoi_restore_rfi

/* IVOR4 at ext_sw, an offset in the image's page as crt0's are. */
	.globl	install_ext_sw
install_ext_sw:
	lis	%r3, ext_sw@ha
	addi	%r3, %r3, ext_sw@l
	mtspr	SPR_IVOR4, %r3
	blr

	.macro	stub name
	.globl	\name
\name:
	.endm

stub rd_ivpr
	mfspr	%r3, SPR_IVPR
	blr
stub wr_ivpr
	mtspr	SPR_IVPR, %r3
	blr
stub rd_tcr
	mfspr	%r3, SPR_TCR
	blr
stub wr_tcr
	mtspr	SPR_TCR, %r3
	blr

/* Narrow accesses, which the STM refuses; a fault returns the IVOR. */
stub ld_b
	lbz	%r3, 0(%r3)
	blr
stub ld_h
	lhz	%r3, 0(%r3)
	blr
stub st_h
	sth	%r4, 0(%r3)
	blr

/*
 * The counter either side of `n` decrements of CTR, `n` >= 1: n + 2
 * instructions apart on the processor clock, which the STM counts at
 * CPS 0. Returns the difference.
 *
 * Each read follows a time-base read, which the translator declines, so
 * that translated each load begins a block of its own: a block's clock
 * is exact at its start and a chain's is not (docs/frontend/ppc.md), and
 * this measures the STM's rate, not the JIT's bookkeeping.
 */
stub stm_span
	lis	%r5, 0xFFF3C004@ha
	mtctr	%r3
	mfspr	%r7, 268		/* TBL */
	lwz	%r6, 0xFFF3C004@l(%r5)
1:	bdnz	1b
	mfspr	%r7, 268
	lwz	%r3, 0xFFF3C004@l(%r5)
	subf	%r3, %r6, %r3
	blr

/* ------------------------------------------------------------------ */
/* Hardware vector mode                                                */
/* ------------------------------------------------------------------ */

	.section .text.hwvec, "ax"
	.balign	65536
	.globl	hwvec_page
hwvec_page:
	.macro	hwent n
	.balign	16
	stwu	%r1, -16(%r1)
	stw	%r3, 8(%r1)
	li	%r3, \n
	b	hw_common
	.endm
	hwent	0
	hwent	1
	hwent	2
	hwent	3
	hwent	4
	hwent	5
	hwent	6
	hwent	7
	hwent	8
	hwent	9
	hwent	10
	hwent	11
	hwent	12
	hwent	13
	hwent	14
	hwent	15
	hwent	16
	hwent	17
	hwent	18
	hwent	19
	hwent	20
	hwent	21
	hwent	22
	hwent	23
	hwent	24
	hwent	25
	hwent	26
	hwent	27
	hwent	28
	hwent	29
	hwent	30
	hwent	31
	hwent	32
	hwent	33

/*
 * The entry pushed 16 bytes holding the interrupted r3 and put its
 * vector in r3. The vector goes beside it in that frame, r3 comes back,
 * and from there it is ext_sw -- except that the take was the
 * acknowledgement, so nothing reads IACKR first -- and the epilogue
 * gives the entry's 16 bytes back too.
 */
hw_common:
	stw	%r3, 12(%r1)
	lwz	%r3, 8(%r1)
	save_volatile
	lwz	%r3, F_SIZE + 12(%r1)
	wrteei	1
	bl	hw_isr
	eoi_restore_rfi 16
