/*
 * A handler of the program's own: IVPR moved onto the code, IVOR4 at
 * the handler. It acknowledges through IACKR, clears the source, ends
 * through EOIR and returns with rfi, and the program goes on.
 */
	.globl	_start
_start:
	lis	%r5, 0x2000
	mtspr	63, %r5			/* IVPR = 0x2000_0000 */
	li	%r6, handler - _start
	mtspr	404, %r6		/* IVOR4 */
	lis	%r10, 0xFFF4
	ori	%r10, %r10, 0x8000
	li	%r11, 0
	stw	%r11, 8(%r10)
	li	%r12, 4
	stb	%r12, 0x42(%r10)	/* PSR2: priority 4 */
	li	%r13, 2
	stb	%r13, 0x22(%r10)	/* SSCIR2: set */
	wrteei	1			/* taken; the handler returns here */
	addi	%r14, %r14, 1
	lwz	%r16, 8(%r10)		/* CPR, restored by EOIR */
	wait
	.balign	16
handler:
	lwz	%r20, 0x10(%r10)	/* IACKR: the acknowledgement */
	li	%r21, 1
	stb	%r21, 0x22(%r10)	/* SSCIR2: clear */
	mbar
	stw	%r0, 0x18(%r10)		/* EOIR */
	addi	%r22, %r22, 1
	rfi
