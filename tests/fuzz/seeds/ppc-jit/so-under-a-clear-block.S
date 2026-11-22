/*
 * XER[SO] in a compare's CR field, where a block built with SO clear
 * says 0 (PPC_CTX_SO, ppc_cpu_ctx). One block runs every round, its
 * compare's field read back each time: with SO clear (round 1), then set
 * (rounds 2-3 and 5-6) -- found again under the old context, it would
 * read SO as 0. And both writers of SO, each with a compare after it
 * in its block, under a context that says clear: an o-form (round 2)
 * and mtxer (round 4) end the block there or the compare reads 0. The
 * mfmsr is what derives the context again after mtxer cleared SO
 * (round 3): it is interpreted. Random words never set SO and compare
 * in one block, and run the same block on both sides of it.
 */
	.machine e500
	li	%r9, 6			/* six rounds, counted down */
	lis	%r10, 0x7FFF
	ori	%r10, %r10, 0xFFFF	/* INT_MAX: an addo of it and 1 overflows */
	li	%r11, 1
1:	cmpw	%r9, %r11		/* every round: cr0, SO from XER */
	mfcr	%r5
	add	%r6, %r6, %r5
	cmpwi	%cr1, %r9, 5
	bne	%cr1, 2f
	addo	%r12, %r10, %r11	/* round 2: SO set by an o-form ... */
	cmpw	%cr7, %r9, %r11		/* ... and read in its block */
	mfcr	%r5
	add	%r7, %r7, %r5
2:	cmpwi	%cr1, %r9, 4
	bne	%cr1, 3f
	li	%r0, 0
	mtxer	%r0			/* round 3: SO clear again */
	mfmsr	%r0			/* interpreted: the context follows */
3:	cmpwi	%cr1, %r9, 3
	bne	%cr1, 4f
	lis	%r0, 0x8000
	mtxer	%r0			/* round 4: SO set by mtxer ... */
	cmpw	%cr7, %r9, %r11		/* ... and read in its block */
	mfcr	%r5
	add	%r8, %r8, %r5
4:	addi	%r9, %r9, -1
	cmpwi	%cr1, %r9, 0
	bne	%cr1, 1b
	.long	0x7C00007C		/* wait */
