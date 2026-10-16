/* SPDX-License-Identifier: Apache-2.0 */
/*
 * crt0.S - start-up for the compiled e200z7 guests.
 *
 * Classic Book E, not VLE: GCC has no VLE code generator, so C for this
 * core is 32-bit PowerPC and the image says so by *not* carrying
 * PF_PPC_VLE -- which is what the frontend reads to choose the decoder.
 * Load these as ELF; a raw binary cannot say which encoding it is in
 * and is taken as VLE.
 *
 * **The vectors come first, and every one of them reports.** With IVPR
 * and the IVORs left at zero an interrupt vectors to address 0, and a
 * guest that takes one presents as a hang with no output -- the lesson
 * this project has now written down for four frontends. Here every
 * interrupt this guest does not expect prints which IVOR it was, where,
 * and the syndrome, and exits 99.
 *
 * The one it does expect is the decrementer, which this file sets
 * ticking every DEC_PERIOD instructions. Its handler only counts, but
 * that is the point: it lands in the middle of compiled code thousands
 * of times a run, and the benchmark's own checksum has to survive it --
 * on the interpreter and on translated code alike.
 */

#define SPR_SRR0   26
#define SPR_SRR1   27
#define SPR_DEC    22
#define SPR_DECAR  54
#define SPR_DEAR   61
#define SPR_ESR    62
#define SPR_IVPR   63
#define SPR_TSR    336
#define SPR_TCR    340
#define SPR_IVOR0  400
#define SPR_IVOR32 528
#define SPR_HID0   1008

#define DEC_PERIOD 10007        /* prime, so it drifts across any loop */

#define UART_THR   0x10000000

	.section .text.start, "ax"
	.globl	_start
_start:
	b	_reset

/*
 * One 16-byte slot per interrupt: the IVOR number into r3, and off to
 * the reporter. r3 is clobbered, which is fine for a guest that is
 * about to exit and is why the decrementer does not come through here.
 */
	.macro	vector n
	.balign	16
_vec\n:
	li	%r3, \n
	b	_trap
	.endm

	vector	0
	vector	1
	vector	2
	vector	3
	vector	4
	vector	5
	vector	6
	vector	7
	vector	8
	vector	9
	.balign	16
_vec10:
	b	_decrementer
	vector	11
	vector	12
#ifdef PPC_CRT0_TLBMISS
	.balign	16
_vec13:
	b	_tlbmiss
	.balign	16
_vec14:
	b	_tlbmiss
#else
	vector	13
	vector	14
#endif
	vector	15
	vector	32
	vector	33
	vector	34
	vector	35

/* ------------------------------------------------------------------ */
/* The decrementer: count, acknowledge, return                         */
/* ------------------------------------------------------------------ */

	.balign	16
_decrementer:
	stwu	%r1, -16(%r1)
	stw	%r3, 8(%r1)
	stw	%r4, 12(%r1)
#ifdef PPC_CRT0_TLBMISS
	/*
	 * The counter's page may be unmapped, and the TLB miss that finds
	 * out is the same class of interrupt as this one: it would take
	 * SRR0 and SRR1 for its own return. Saved on the stack, which is
	 * in a protected entry and cannot miss.
	 */
	stwu	%r1, -16(%r1)
	mfspr	%r3, SPR_SRR0
	stw	%r3, 8(%r1)
	mfspr	%r3, SPR_SRR1
	stw	%r3, 12(%r1)
#endif
	lis	%r3, ppc_dec_ticks@ha
	lwz	%r4, ppc_dec_ticks@l(%r3)
	addi	%r4, %r4, 1
	stw	%r4, ppc_dec_ticks@l(%r3)
#ifdef PPC_CRT0_TLBMISS
	lwz	%r3, 8(%r1)
	mtspr	SPR_SRR0, %r3
	lwz	%r3, 12(%r1)
	mtspr	SPR_SRR1, %r3
	addi	%r1, %r1, 16
#endif
	lis	%r3, 0x0800		/* TSR[DIS], write one to clear */
	mtspr	SPR_TSR, %r3
	lwz	%r4, 12(%r1)
	lwz	%r3, 8(%r1)
	addi	%r1, %r1, 16
	rfi

#ifdef PPC_CRT0_TLBMISS
/* ------------------------------------------------------------------ */
/* A TLB miss: map the page and return to the instruction that missed */
/* ------------------------------------------------------------------ */

/*
 * Both TLB error interrupts, instruction and data, come here. The core
 * has already set the MAS registers up for the missing page (table
 * 10-15): MAS1 valid with the access's TID and TS and MAS4's size, MAS2
 * the page's EPN with MAS4's attributes, MAS3 empty. What is left is
 * the real address -- the effective one, since everything here is
 * flat -- the permissions, and which entry to replace, round robin over
 * TLBMISS_WAYS entries from TLBMISS_FIRST, the victim kept in SPRG3.
 *
 * No memory is touched, so this cannot miss itself, and SPRG0-2 hold
 * what it borrows. It is the operating system's half of a software-
 * managed TLB, and every refill is a tlbwe -- which is what makes it
 * the guest that measures what a TLB write costs translated code.
 */
/*
 * Two, which is fewer than CoreMark's working set of pages -- the code
 * beyond the first 4 KB, the constants and the data -- so it misses all
 * the way through rather than once per page: 26,652 refills in a run.
 * With four or more everything fits after the first touch and there is
 * nothing to measure.
 */
#ifndef TLBMISS_WAYS
#define TLBMISS_WAYS 2
#endif
#define TLBMISS_FIRST 4
#define SPR_SPRG0 272
#define SPR_SPRG1 273
#define SPR_SPRG2 274
#define SPR_SPRG3 275
#define SPR_MAS0 624
#define SPR_MAS1 625
#define SPR_MAS2 626
#define SPR_MAS3 627
#define SPR_MAS4 628

_tlbmiss:
	mtspr	SPR_SPRG0, %r3
	mtspr	SPR_SPRG1, %r4
	mfcr	%r3
	mtspr	SPR_SPRG2, %r3
	mfspr	%r3, SPR_MAS2
	rlwinm	%r3, %r3, 0, 0, 19	/* the 4 KB page's EPN */
	ori	%r3, %r3, 0x3F		/* RPN = EPN; UX SX UW SW UR SR */
	mtspr	SPR_MAS3, %r3
	mfspr	%r4, SPR_SPRG3
	addi	%r4, %r4, 1
	cmplwi	%r4, TLBMISS_FIRST + TLBMISS_WAYS
	blt	1f
	li	%r4, TLBMISS_FIRST
1:	mtspr	SPR_SPRG3, %r4
	slwi	%r4, %r4, 16		/* ESEL */
	oris	%r4, %r4, 0x1000	/* TLBSEL1 */
	mtspr	SPR_MAS0, %r4
	tlbwe
	isync
	mfspr	%r3, SPR_SPRG2
	mtcr	%r3
	mfspr	%r4, SPR_SPRG1
	mfspr	%r3, SPR_SPRG0
	rfi
#endif

/* ------------------------------------------------------------------ */
/* Anything else: say what, and stop                                   */
/* ------------------------------------------------------------------ */

_trap:
	/*
	 * The syndrome first, before anything that could take another
	 * interrupt -- with the TLB filled on demand, reading the message
	 * below can miss, and the miss would write SRR0 for its own return.
	 */
	mfspr	%r29, SPR_SRR0
	mfspr	%r28, SPR_ESR
	mfspr	%r27, SPR_DEAR
	mr	%r31, %r3
	lis	%r30, UART_THR@h
	lis	%r4, _trap_msg@ha
	addi	%r4, %r4, _trap_msg@l
	bl	_puts
	mr	%r3, %r31
	bl	_puthex
	lis	%r4, _at_msg@ha
	addi	%r4, %r4, _at_msg@l
	bl	_puts
	mr	%r3, %r29
	bl	_puthex
	lis	%r4, _esr_msg@ha
	addi	%r4, %r4, _esr_msg@l
	bl	_puts
	mr	%r3, %r28
	bl	_puthex
	lis	%r4, _dear_msg@ha
	addi	%r4, %r4, _dear_msg@l
	bl	_puts
	mr	%r3, %r27
	bl	_puthex
	li	%r3, '\n'
	stb	%r3, 0(%r30)
	li	%r3, 99
	b	_exit

/* r4 -> NUL-terminated string. r30 is the UART. */
_puts:
	lbz	%r5, 0(%r4)
	cmpwi	%r5, 0
	beqlr
	stb	%r5, 0(%r30)
	addi	%r4, %r4, 1
	b	_puts

/* r3 as eight hex digits. */
_puthex:
	li	%r6, 8
	mtctr	%r6
1:	rlwinm	%r3, %r3, 4, 0, 31
	andi.	%r5, %r3, 15
	cmpwi	%r5, 10
	blt	2f
	addi	%r5, %r5, 'a' - 10 - '0'
2:	addi	%r5, %r5, '0'
	stb	%r5, 0(%r30)
	bdnz	1b
	blr

/* ------------------------------------------------------------------ */
/* Reset                                                               */
/* ------------------------------------------------------------------ */

/*
 * IVPR and the IVORs, which every core has its own of: core 1 installs
 * the same table for itself (_start1).
 */
	.macro	setivor spr, n
	li	%r3, _vec\n - _start
	mtspr	\spr, %r3
	.endm
	.macro	install_vectors
	lis	%r3, _start@h
	mtspr	SPR_IVPR, %r3
	setivor	SPR_IVOR0 + 0, 0
	setivor	SPR_IVOR0 + 1, 1
	setivor	SPR_IVOR0 + 2, 2
	setivor	SPR_IVOR0 + 3, 3
	setivor	SPR_IVOR0 + 4, 4
	setivor	SPR_IVOR0 + 5, 5
	setivor	SPR_IVOR0 + 6, 6
	setivor	SPR_IVOR0 + 7, 7
	setivor	SPR_IVOR0 + 8, 8
	setivor	SPR_IVOR0 + 9, 9
	setivor	SPR_IVOR0 + 10, 10
	setivor	SPR_IVOR0 + 11, 11
	setivor	SPR_IVOR0 + 12, 12
	setivor	SPR_IVOR0 + 13, 13
	setivor	SPR_IVOR0 + 14, 14
	setivor	SPR_IVOR0 + 15, 15
	setivor	SPR_IVOR32 + 0, 32
	setivor	SPR_IVOR32 + 1, 33
	setivor	SPR_IVOR32 + 2, 34
	setivor	SPR_IVOR32 + 3, 35
	.endm

_reset:
	/*
	 * r1 is the loader's: the top of whatever RAM this machine has,
	 * which is a megabyte on a host and a fifth of that on a board.
	 * A link script that named it would be right on one of them.
	 */
	li	%r0, 0
	stwu	%r0, -16(%r1)		/* a terminated back chain */

	/* The vectors: IVPR is the 64 KiB page, each IVOR an offset in it. */
	install_vectors

#ifdef PPC_CRT0_TLB
	/*
	 * The TLB programmed as an MPC57xx start-up does it (mmu.c does the
	 * same with C around it): RAM -- code, data, stack, the vectors --
	 * in one protected megabyte with every permission, the console in a
	 * protected 4 KB page, cache-inhibited and guarded, and the reset
	 * entry overwritten. From here translation is on for the whole run,
	 * which is what a guest built with this measures: the same program
	 * as without, under the MMU. MAS fields as tables 10-9 to 10-12.
	 */
	.macro	tlbent esel, mas1, mas2, mas3
	lis	%r3, (0x10000000 | (\esel << 16))@h	/* TLBSEL1, ESEL */
	mtspr	624, %r3
	lis	%r3, (\mas1)@h
	ori	%r3, %r3, (\mas1)@l
	mtspr	625, %r3
	lis	%r3, (\mas2)@h
	ori	%r3, %r3, (\mas2)@l
	mtspr	626, %r3
	lis	%r3, (\mas3)@h
	ori	%r3, %r3, (\mas3)@l
	mtspr	627, %r3
	tlbwe
	isync
	.endm
	/* V | IPROT | TSIZE 1 MB; EPN; RPN | UX SX UW SW UR SR */
	tlbent	1, 0xC0000500, 0x80000000, 0x8000003F
	/* V | IPROT | TSIZE 4 KB; EPN | I | G; RPN | SW SR */
	tlbent	2, 0xC0000100, 0x1000000A, 0x10000005
	/* entry 0, the reset entry: invalid */
	tlbent	0, 0x00000000, 0x00000000, 0x00000000
#endif

#ifdef PPC_CRT0_TLBMISS
	/*
	 * The TLB filled on demand. Three protected entries hold what the
	 * miss handler cannot fault on -- the image's first 4 KB, which has
	 * the vectors and the handler; the 16 KB under the stack top; the
	 * console -- and everything else is mapped a 4 KB page at a time by
	 * _tlbmiss, as it is touched: the rest of the code, the constants,
	 * the data. The reset entry goes last, because until then it is
	 * what this code is running through.
	 */
	.macro	tlbreg esel, mas1, mas2, mas3
	lis	%r3, (0x10000000 | (\esel << 16))@h
	mtspr	624, %r3
	lis	%r3, (\mas1)@h
	ori	%r3, %r3, (\mas1)@l
	mtspr	625, %r3
	mtspr	626, \mas2
	mtspr	627, \mas3
	tlbwe
	isync
	.endm
	/* V | IPROT | TSIZE 4 KB; EPN; RPN | UX SX UW SW UR SR */
	lis	%r4, _start@h
	ori	%r4, %r4, _start@l
	ori	%r5, %r4, 0x3F
	tlbreg	1, 0xC0000100, %r4, %r5
	/* V | IPROT | TSIZE 16 KB: the stack's, below the top */
	addi	%r4, %r1, -1
	rlwinm	%r4, %r4, 0, 0, 17
	ori	%r5, %r4, 0x3F
	tlbreg	2, 0xC0000200, %r4, %r5
	/* V | IPROT | TSIZE 4 KB; EPN | I | G; RPN | SW SR */
	lis	%r4, 0x1000
	ori	%r4, %r4, 0x000A
	lis	%r5, 0x1000
	ori	%r5, %r5, 0x0005
	tlbreg	3, 0xC0000100, %r4, %r5
	/* MAS4: TLBSEL1, 4 KB, TID from PID0, no attributes (table 10-13) */
	lis	%r3, 0x1000
	ori	%r3, %r3, 0x0100
	mtspr	628, %r3
	li	%r3, TLBMISS_FIRST + TLBMISS_WAYS - 1
	mtspr	275, %r3		/* SPRG3: the last victim */
	li	%r4, 0
	li	%r5, 0
	tlbreg	0, 0x00000000, %r4, %r5
#endif

	/* .bss, which nothing else will zero for a guest loaded as one blob. */
	lis	%r3, __bss_start@ha
	addi	%r3, %r3, __bss_start@l
	lis	%r4, __bss_end@ha
	addi	%r4, %r4, __bss_end@l
	li	%r0, 0
3:	cmplw	%r3, %r4
	bge	4f
	stw	%r0, 0(%r3)
	addi	%r3, %r3, 4
	b	3b
4:
	/*
	 * The time base on the processor clock -- HID0[TBEN] with
	 * SEL_TBCLK clear -- which in this emulator is one tick per
	 * instruction: a clock that is a property of the guest binary and
	 * reads the same on every run and both backends. Then the
	 * decrementer, auto-reloading, and interrupts on.
	 */
	lis	%r3, 0x0000
	ori	%r3, %r3, 0x4000	/* HID0[TBEN] */
	mtspr	SPR_HID0, %r3
	lis	%r3, DEC_PERIOD@h
	ori	%r3, %r3, DEC_PERIOD@l
	mtspr	SPR_DECAR, %r3
	mtspr	SPR_DEC, %r3
	lis	%r3, 0x0440		/* TCR[DIE] | TCR[ARE] */
	mtspr	SPR_TCR, %r3
	wrteei	1

	li	%r3, 0
	li	%r4, 0
	bl	main
	/* fall through: main's result is the exit status */

	.globl	_exit
_exit:
	li	%r0, 93
	sc
5:	b	5b

#ifdef PPC_CRT0_SMP
/* ------------------------------------------------------------------ */
/* Core 1                                                              */
/* ------------------------------------------------------------------ */

/*
 * Where core 0 points SIU_RSTVEC1 (smp.c). Core 1 comes out of reset
 * with nothing set up -- the loader gave core 0 a stack and no one gave
 * this core anything -- so it takes its own stack, installs the same
 * vectors in its own IVPR and IVORs, and runs main1. Everything else
 * crt0 does is core 0's: the .bss core 1 relies on was zeroed before it
 * was started.
 *
 * main1 returning parks the core: wait with MSR[EE] clear, which
 * nothing can end, and which the runner counts as finished.
 */
#ifndef PPC_CORE1_STACK
#define PPC_CORE1_STACK 4096		/* smp.c's ppc_core1_stack */
#endif
	.balign	16
	.globl	_start1
_start1:
	lis	%r1, (ppc_core1_stack + PPC_CORE1_STACK)@ha
	addi	%r1, %r1, (ppc_core1_stack + PPC_CORE1_STACK)@l
	li	%r0, 0
	stwu	%r0, -16(%r1)
	install_vectors
	bl	main1
	wrteei	0
6:	.long	0x7C00007C		/* wait */
	b	6b
#endif

	.section .rodata
_trap_msg:	.asciz	"\nTRAP ivor="
_at_msg:	.asciz	" srr0="
_esr_msg:	.asciz	" esr="
_dear_msg:	.asciz	" dear="

	.section .bss
	.balign	4
	.globl	ppc_dec_ticks
ppc_dec_ticks:
	.space	4
