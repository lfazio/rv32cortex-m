/* efsdiv at the edges of the translated path's fast case (ppc_fpu.h,
 * ppc_fpu_div_fast): the interpreter's efsdiv is ppc_fpu_exec's general
 * path, so the two are held against each other here. As in
 * efsmul-edges.S each quotient is followed by an mfspr of SPEFSCR,
 * which the translator declines, so every one starts a block of its own
 * and its FG, FX and status are read before the next replaces them, and
 * both fold into r7 and r8. Each case is padded to 64 bytes and there
 * is no prologue, so fuzz.sh's 256-byte cuts leave four whole in each
 * input.
 *
 * In the fast case: exact quotients with the leading bit at 25 and at
 * 24, inexact ones both ways, a dividend just under and just over the
 * divisor, exponents of 1 and 253, and the signs. No quotient of normals
 * rounds up out of its binade -- it would have to lie within 2^-25 of a
 * power of two, and 24-bit significands keep it 2^-24 away -- so unlike
 * the product and the sum there is no carry case to have. To the
 * general path: an exponent of 0 and of 254, an overflow, an underflow
 * far below, a zero dividend and divisor, a denormal, an infinity and a
 * NaN. Under a configuration word whose FRMC or FINXE is set, all of
 * them. No case is a tie: a binary quotient never is. */
	.machine e500
	.macro	LI32 r, v
	lis	\r, (\v)@h
	ori	\r, \r, (\v)@l
	.endm
	.macro	DIV a, b
	LI32	%r3, \a
	LI32	%r4, \b
	efsdiv	%r5, %r3, %r4
	mfspr	%r6, 512		/* SPEFSCR */
	rotlwi	%r7, %r7, 5
	xor	%r7, %r7, %r5
	rotlwi	%r8, %r8, 3
	xor	%r8, %r8, %r6
	.balign	64			/* a case a slot: see the top */
	.endm

	DIV	0x3F800000, 0x3F800000	/* 1/1: exact, leading bit 25 */
	DIV	0x40400000, 0x40000000	/* 3/2: exact */
	DIV	0x40000000, 0x40400000	/* 2/3: leading bit 24, up */
	DIV	0x3F800000, 0x40400000	/* 1/3 */
	DIV	0x40E00000, 0x40400000	/* 7/3 */
	DIV	0x3F800000, 0x40E00000	/* 1/7 */
	DIV	0x41B00000, 0x40E00000	/* 22/7 */
	DIV	0x3F800000, 0x3F800001	/* just under: leading bit 24 */
	DIV	0x3F800001, 0x3F800000	/* just over: exact */
	DIV	0x3FFFFFFF, 0x3F7FFFFF	/* exactly 2.0, from two odd operands */
	DIV	0x01000000, 0x40000000	/* 2^-126: exponent 1 */
	DIV	0x7E800000, 0x3F800000	/* 2^126: exponent 253 */
	DIV	0x7EFFFFFF, 0x3F7FFFFF	/* exactly 2^127: exponent 254, general */
	DIV	0xC0C00000, 0x40400000	/* -6/3 */
	DIV	0x40C00000, 0xC0400000	/* 6/-3 */
	DIV	0xC0C00000, 0xC0400000	/* -6/-3 */
	DIV	0x00800000, 0x40000000	/* 2^-127: exponent 0, general */
	DIV	0x7F000000, 0x3F800000	/* 2^127: exponent 254, general */
	DIV	0x7F7FFFFF, 0x3F000000	/* overflow */
	DIV	0x00800000, 0x7F000000	/* far below: the exponent wraps */
	DIV	0x00000000, 0x40000000	/* a zero dividend */
	DIV	0x40000000, 0x00000000	/* a zero divisor: FDBZ */
	DIV	0x00400000, 0x40000000	/* a denormal */
	DIV	0x7F800000, 0x40000000	/* an infinity */
	DIV	0x7FC00000, 0x40000000	/* a NaN */
	.long	0x7C00007C		/* wait */
