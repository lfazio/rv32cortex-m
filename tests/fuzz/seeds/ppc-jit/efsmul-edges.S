/* efsmul at the edges of the translated path's fast case (ppc_fpu.c,
 * ppc_fpu_mul): the interpreter's efsmul is ppc_fpu_exec's general
 * path, so the two are held against each other here. Each product is
 * followed by an mfspr of SPEFSCR, which the translator declines, so
 * every efsmul starts a block of its own and its FG, FX and status are
 * read before the next replaces them. Both are folded into r7 and r8,
 * so a wrong result or bit anywhere shows.
 *
 * In the fast case: ties to odd (up) and to even (down), a guard with
 * and without a sticky bit, a sticky bit only in the product's low 16
 * bits with the leading bit at 46 and at 47, the carry out of an
 * all-ones significand, the exponent before rounding at 1 and at 253,
 * a carry from 253 into 254, operands at the ends of the normal range,
 * negative signs, and rd = ra. To the general path: an exponent of 0
 * (underflow) and of 254 -- one of them rounded into overflow -- an
 * overflow, a denormal, an infinity, a NaN and a zero. Under a
 * configuration word whose FRMC or FINXE is set -- fuzz.sh adds one
 * with FINXE, and the fuzzer mutates the rest -- all of them take the
 * general path. Six mutants of the fast case, each found. */
	.machine e500
	.macro	LI32 r, v
	lis	\r, (\v)@h
	ori	\r, \r, (\v)@l
	.endm
	.macro	MUL a, b
	LI32	%r3, \a
	LI32	%r4, \b
	efsmul	%r5, %r3, %r4
	mfspr	%r6, 512		/* SPEFSCR */
	rotlwi	%r7, %r7, 5
	xor	%r7, %r7, %r5
	rotlwi	%r8, %r8, 3
	xor	%r8, %r8, %r6
	.endm

	/* No prologue: fuzz.sh cuts a seed into 256-byte inputs, and each
	 * case is 32 bytes, so every input holds eight whole ones. r7 and
	 * r8 start as the harness leaves them. */
	MUL	0x3F800001, 0x3FC00000	/* tie, odd: up */
	MUL	0x3F800003, 0x3FC00000	/* tie, even: down */
	MUL	0xBF800001, 0x3FC00000	/* the same, negative */
	MUL	0xBF800003, 0xBFC00000
	MUL	0x40400000, 0x40155555	/* 3 * 7/3: guard and sticky */
	MUL	0x3F8CCCCD, 0x3F8CCCCD	/* 1.1 * 1.1 */
	MUL	0x3F800001, 0x3F800001	/* sticky only in the low half, bit 46 */
	MUL	0x3FFFFFFF, 0x3FFFFFFF	/* ...and with the leading bit at 47 */
	MUL	0x3FFFFFFE, 0x3F800001	/* all ones, rounded up: 2.0 */
	MUL	0x20000000, 0x20000000	/* 2^-126: exponent 1 */
	MUL	0x1F800000, 0x20000000	/* 2^-127: exponent 0, the helper */
	MUL	0x5F000000, 0x5F000000	/* 2^126: exponent 253 */
	MUL	0x5EFFFFFE, 0x5F800001	/* 253, rounded into 254 */
	MUL	0x5F800000, 0x5F000000	/* 2^127: exponent 254, the helper */
	MUL	0x5F800000, 0x5F800000	/* overflow */
	MUL	0x5EFFFFFE, 0x60000001	/* 254, rounded into overflow */
	MUL	0x7F7FFFFF, 0x3F000000	/* the largest normal, halved */
	MUL	0x00800000, 0x3F800000	/* the smallest normal */
	MUL	0x00400000, 0x3F800000	/* a denormal: FINV */
	MUL	0x7F800000, 0x3F800000	/* an infinity */
	MUL	0x7FC00000, 0x40000000	/* a NaN */
	MUL	0x00000000, 0x3F800000	/* a zero */
	LI32	%r3, 0x3F8CCCCD		/* rd = ra */
	LI32	%r4, 0x3F8CCCCD
	efsmul	%r3, %r3, %r4
	mfspr	%r6, 512
	xor	%r7, %r7, %r3
	xor	%r8, %r8, %r6
	.long	0x7C00007C		/* wait */
