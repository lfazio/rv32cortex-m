/* Enabled and masked again each time round; raised, while masked, half
 * way, so the code after wrteei is a translated block by then. */
	lis	%r10, 0xFFF4
	ori	%r10, %r10, 0x8000
	li	%r11, 0
	stw	%r11, 8(%r10)
	li	%r12, 5
	stb	%r12, 0x40(%r10)
	li	%r21, 2
	li	%r20, 16
1:	wrteei	1
	addi	%r13, %r13, 1
	addi	%r14, %r14, 2
	wrteei	0
	addi	%r20, %r20, -1
	cmpwi	%r20, 8
	bne	2f
	stb	%r21, 0x20(%r10)
2:	cmpwi	%r20, 0
	bne	1b
	wait
