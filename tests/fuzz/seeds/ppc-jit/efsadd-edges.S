/* efsadd and efssub at the edges of the translated path's fast case
 * (ppc_fpu.c, add_fast): the interpreter's are ppc_fpu_exec's general
 * path, so the two are held against each other here. As in
 * efsmul-edges.S each operation is followed by an mfspr of SPEFSCR,
 * which the translator declines, so every one starts a block of its own
 * and its FG, FX and status are read before the next replaces them, and
 * both fold into r7 and r8. Each case is padded to 64 bytes and there
 * is no prologue, so fuzz.sh's 256-byte cuts leave four whole in each
 * input.
 *
 * In the fast case: exponent differences d of 0 (a carry out), 1, 7
 * (the last that loses nothing), 8 (the first that does), 23, 24, 25,
 * 31, 32 and 100; a difference whose sticky bit is a borrow, its
 * leading bit dropping a place; deep cancellation at d 0 and 1, where
 * the shift is exact; ties to odd and to even; the smaller operand
 * first (the swap); the larger negative; an all-ones significand rounded up; an exponent
 * of 253 rounded into 254. To the general path: an exact zero, a
 * cancellation below the normal range, a sum carried out to 254 and
 * one rounded into overflow, a denormal, an infinity and a NaN. Under a
 * configuration word whose FRMC or FINXE is set, all of them. */
	.machine e500
	.macro	LI32 r, v
	lis	\r, (\v)@h
	ori	\r, \r, (\v)@l
	.endm
	.macro	ADD a, b
	LI32	%r3, \a
	LI32	%r4, \b
	efsadd	%r5, %r3, %r4
	mfspr	%r6, 512		/* SPEFSCR */
	rotlwi	%r7, %r7, 5
	xor	%r7, %r7, %r5
	rotlwi	%r8, %r8, 3
	xor	%r8, %r8, %r6
	.balign	64			/* a case a slot: see the top */
	.endm
	.macro	SUB a, b
	LI32	%r3, \a
	LI32	%r4, \b
	efssub	%r5, %r3, %r4
	mfspr	%r6, 512
	rotlwi	%r7, %r7, 5
	xor	%r7, %r7, %r5
	rotlwi	%r8, %r8, 3
	xor	%r8, %r8, %r6
	.balign	64			/* a case a slot: see the top */
	.endm

	ADD	0x3FC00000, 0x3FA00000	/* d 0: 1.5 + 1.25, a carry out */
	ADD	0x3F800001, 0x3F000001	/* d 1: exact shift, a tie to odd */
	ADD	0x3F800000, 0x3C000001	/* d 7: the shift still exact */
	ADD	0x3F800000, 0x3B800001	/* d 8: the first lossy shift */
	ADD	0x3F800000, 0x34000001	/* d 23 */
	ADD	0x3F800000, 0x33800001	/* d 24: just over half an ulp */
	ADD	0x3F800001, 0x33800000	/* d 24, exactly half: tie to odd, up */
	ADD	0x3F800002, 0x33800000	/* ...and to even, down */
	ADD	0x3F800000, 0x33000001	/* d 25 */
	ADD	0x3F800000, 0x30000001	/* d 31 */
	ADD	0x3F800000, 0x2F800001	/* d 32: only a sticky bit */
	ADD	0x3F800000, 0x0D800001	/* d 100 */
	SUB	0x3F800000, 0x33800001	/* d 24: a borrow, leading bit down one */
	SUB	0x3F800000, 0x3B800001	/* d 8, the borrow */
	SUB	0x40400000, 0x3FC00000	/* d 1: down one, exact */
	SUB	0x3F800001, 0x3F800000	/* d 0: deep cancellation */
	SUB	0x3F800000, 0x3F7FFFFF	/* d 1: deep cancellation */
	SUB	0x40000000, 0x3F7FFFFF	/* d 2: down one, a tie to even */
	ADD	0x3C000001, 0x3F800000	/* the smaller first: the swap */
	ADD	0xBF800000, 0x3C000001	/* the larger negative */
	SUB	0x3C000001, 0x3F800000	/* the smaller first, subtracted */
	ADD	0x3FFFFFFF, 0x33800000	/* all ones, a tie to odd: up to 2.0 */
	ADD	0x7EFFFFFF, 0x72800000	/* exponent 253 rounded into 254 */
	SUB	0x3F800000, 0x3F800000	/* an exact zero: general path */
	SUB	0x00800001, 0x00800000	/* below the normal range */
	ADD	0x7E800000, 0x7E800000	/* a carry out to 254: general */
	ADD	0x7F7FFFFF, 0x73800000	/* carried into overflow */
	ADD	0x7F7FFFFF, 0x73000000	/* 254, a tie rounded into overflow */
	ADD	0x00400000, 0x3F800000	/* a denormal */
	ADD	0x7F800000, 0x3F800000	/* an infinity */
	SUB	0x7FC00000, 0x3F800000	/* a NaN */
	.long	0x7C00007C		/* wait */
