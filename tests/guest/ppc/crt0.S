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
	vector	13
	vector	14
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
	lis	%r3, ppc_dec_ticks@ha
	lwz	%r4, ppc_dec_ticks@l(%r3)
	addi	%r4, %r4, 1
	stw	%r4, ppc_dec_ticks@l(%r3)
	lis	%r3, 0x0800		/* TSR[DIS], write one to clear */
	mtspr	SPR_TSR, %r3
	lwz	%r4, 12(%r1)
	lwz	%r3, 8(%r1)
	addi	%r1, %r1, 16
	rfi

/* ------------------------------------------------------------------ */
/* Anything else: say what, and stop                                   */
/* ------------------------------------------------------------------ */

_trap:
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
	mfspr	%r3, SPR_SRR0
	bl	_puthex
	lis	%r4, _esr_msg@ha
	addi	%r4, %r4, _esr_msg@l
	bl	_puts
	mfspr	%r3, SPR_ESR
	bl	_puthex
	lis	%r4, _dear_msg@ha
	addi	%r4, %r4, _dear_msg@l
	bl	_puts
	mfspr	%r3, SPR_DEAR
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

_reset:
	lis	%r1, __stack_top@ha
	addi	%r1, %r1, __stack_top@l
	li	%r0, 0
	stwu	%r0, -16(%r1)		/* a terminated back chain */

	/* The vectors: IVPR is the 64 KiB page, each IVOR an offset in it. */
	lis	%r3, _start@h
	mtspr	SPR_IVPR, %r3
	.macro	setivor spr, n
	li	%r3, _vec\n - _start
	mtspr	\spr, %r3
	.endm
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
