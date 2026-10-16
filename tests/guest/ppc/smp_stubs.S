/* SPDX-License-Identifier: Apache-2.0 */
/*
 * smp_stubs.S - what smp.c cannot write in C: a TLB write, two pages of
 * code to map at one address, and an entry point in VLE.
 */

#define SPR_MAS0 624
#define SPR_MAS1 625
#define SPR_MAS2 626
#define SPR_MAS3 627

	.text

/* r3 = entry, r4 = MAS1, r5 = MAS2, r6 = MAS3 */
	.globl	smp_tlbwe
smp_tlbwe:
	slwi	%r3, %r3, 16		/* ESEL */
	oris	%r3, %r3, 0x1000	/* TLBSEL1 */
	mtspr	SPR_MAS0, %r3
	mtspr	SPR_MAS1, %r4
	mtspr	SPR_MAS2, %r5
	mtspr	SPR_MAS3, %r6
	tlbwe
	isync
	blr

/*
 * Two pages, the same offset in each, different answers: one virtual
 * address mapped to the first on core 0 and to the second on core 1
 * says which core's mapping a call went through.
 */
	.section .text.smp_pages, "ax"
	.balign	4096
	.globl	smp_page_a
smp_page_a:
	li	%r3, 0x5A
	blr
	.balign	4096
	.globl	smp_page_b
smp_page_b:
	li	%r3, 0xA2
	blr
	.balign	4096

/*
 * Core 1 started in VLE (SSCM_DPMBOOT[DVLE]): it stores 0x56 to g_vle_ran
 * and waits. The address is a literal after the code, reached from the
 * link register, because VLE has nothing pc-relative a store can use.
 * Encodings from binutils' -mvle.
 */
	.text
	.balign	4
	.globl	core1_vle
core1_vle:
	.long	0x78000005		/* e_bl    1f            */
	.short	0x0084			/* 1: se_mflr r4         */
	.long	0x50840010		/* e_lwz   r4, 16(r4)    */
	.short	0x4D63			/* se_li   r3, 0x56      */
	.short	0xD034			/* se_stw  r3, 0(r4)     */
	.long	0x7C00007C		/* wait                  */
	.short	0xE800			/* se_b    .             */
	.long	g_vle_ran		/* at 1f + 16            */
