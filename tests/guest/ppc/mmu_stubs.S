/* SPDX-License-Identifier: Apache-2.0 */
/*
 * mmu_stubs.S - what mmu.c cannot write in C: the MMU's registers and
 * instructions, accesses made in address space 1, and code reached
 * through a virtual address other than the one it was linked at.
 *
 * Linked with sys_stubs.S, whose vectors and recorder catch every
 * interrupt here and return to the C caller with the IVOR number --
 * so every stub is a leaf called with `bl`, as there.
 */

#define SPR_PID0    48
#define SPR_SRR0    26
#define SPR_SRR1    27
#define SPR_MAS0    624
#define SPR_MAS1    625
#define SPR_MAS2    626
#define SPR_MAS3    627
#define SPR_MAS4    628
#define SPR_MAS6    630
#define SPR_TLB0CFG 688
#define SPR_TLB1CFG 689
#define SPR_MMUCSR0 1012
#define SPR_MMUCFG  1015

#define MSR_IS 0x20
#define MSR_DS 0x10

	.text

	.macro	stub name
	.globl	\name
\name:
	.endm

	.macro	rd name, spr
stub \name
	mfspr	%r3, \spr
	blr
	.endm
	.macro	wr name, spr
stub \name
	mtspr	\spr, %r3
	blr
	.endm

	rd	rd_mas0, SPR_MAS0
	rd	rd_mas1, SPR_MAS1
	rd	rd_mas2, SPR_MAS2
	rd	rd_mas3, SPR_MAS3
	rd	rd_mas4, SPR_MAS4
	rd	rd_mas6, SPR_MAS6
	rd	rd_pid0, SPR_PID0
	rd	rd_mmucfg, SPR_MMUCFG
	rd	rd_tlb0cfg, SPR_TLB0CFG
	rd	rd_tlb1cfg, SPR_TLB1CFG
	rd	rd_mmucsr0, SPR_MMUCSR0
	wr	wr_mas0, SPR_MAS0
	wr	wr_mas1, SPR_MAS1
	wr	wr_mas2, SPR_MAS2
	wr	wr_mas3, SPR_MAS3
	wr	wr_mas4, SPR_MAS4
	wr	wr_mas6, SPR_MAS6
	wr	wr_pid0, SPR_PID0
	wr	wr_mmucsr0, SPR_MMUCSR0

stub stub_tlbre
	tlbre
	blr
stub stub_tlbwe
	tlbwe
	isync
	blr
stub stub_tlbsx				/* r3 = EA */
	tlbsx	0, %r3
	blr
stub stub_tlbivax			/* r3 = EA, with TLBSEL and INV_ALL */
	tlbivax	0, %r3
	tlbsync
	blr

/* The same, from problem state: each must be refused (10.5). */
	.macro	leave
	lis	%r4, g_uval@ha
	stw	%r3, g_uval@l(%r4)
	li	%r0, 0x7777
	sc
2:	b	2b
	.endm

stub u_tlbwe
	tlbwe
	leave
stub u_tlbsx
	tlbsx	0, %r5
	leave
stub u_load				/* r5 = address */
	lwz	%r3, 0(%r5)
	leave

/* A load in address space 1: MSR[DS] for one instruction. */
stub stub_load_ds			/* r3 = address */
	mfmsr	%r5
	ori	%r6, %r5, MSR_DS
	mtmsr	%r6
	lwz	%r3, 0(%r3)
	mtmsr	%r5
	blr

/* An instruction fetch in address space 1. */
stub stub_jump_is			/* r3 = target */
	mtspr	SPR_SRR0, %r3
	mfmsr	%r4
	ori	%r4, %r4, MSR_IS
	mtspr	SPR_SRR1, %r4
	rfi

/*
 * Two pages of code, reached only through mappings mmu.c makes. The
 * first is called from Book E and VLE pages alike, so it holds both
 * encodings; its last halfword starts a 32-bit VLE instruction whose
 * second half is the first halfword of the next page.
 */
	.section .text.mmu_pages, "ax"
	.balign	4096
	.globl	mmu_page0
mmu_page0:
	li	%r3, 0x5A			/* Book E */
	blr

	.org	mmu_page0 + 0x100
	.short	0x4D53				/* se_li  r3, 0x55 */
	.short	0x0004				/* se_blr          */

	.org	mmu_page0 + 0xFFE
	.short	0x7060				/* e_li   r3, 0x77, first half */
	.short	0x0077				/* ...second half: next page   */
	.short	0x0004				/* se_blr                      */
	.org	mmu_page0 + 0x2000
