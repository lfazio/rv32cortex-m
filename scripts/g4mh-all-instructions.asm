	.section .text, text
	.public _enc
_enc:
_lbl:
	ld.b 0x10[r6],r8
	ld.b 0x1000[r6],r10
	ld.b [r6]+,r10
	ld.b [r6]-,r10
	ld.bu 0x10[r6],r8
	ld.bu 0x1000[r6],r10
	ld.bu [r6]+,r10
	ld.bu [r6]-,r10
	ld.dw 0x1000[r6],r10
	ld.h 0x10[r6],r8
	ld.h 0x1000[r6],r10
	ld.h [r6]+,r10
	ld.h [r6]-,r10
	ld.hu 0x10[r6],r8
	ld.hu 0x1000[r6],r10
	ld.hu [r6]+,r10
	ld.hu [r6]-,r10
	ld.w 0x10[r6],r8
	ld.w 0x1000[r6],r10
	ld.w [r6]+,r10
	ld.w [r6]-,r10
	sld.b 0x8[ep],r8
	sld.bu 0x4[ep],r8
	sld.h 0x8[ep],r8
	sld.hu 0x4[ep],r8
	sld.w 0x8[ep],r8
	st.b r8,0x10[r6]
	st.b r10,0x1000[r6]
	st.b r10,[r6]+
	st.b r10,[r6]-
	st.dw r10,0x1000[r6]
	st.h r8,0x10[r6]
	st.h r10,0x1000[r6]
	st.h r10,[r6]+
	st.h r10,[r6]-
	st.w r8,0x10[r6]
	st.w r10,0x1000[r6]
	st.w r10,[r6]+
	st.w r10,[r6]-
	sst.b r8,0x8[ep]
	sst.h r8,0x8[ep]
	sst.w r8,0x8[ep]
	set1 3,0x10[r6]
	set1 r8,[r6]
	tst1 3,0x10[r6]
	tst1 r8,[r6]
	caxi [r6],r8,r10
	ldl.bu [r6],r10
	ldl.hu [r6],r10
	ldl.w [r6],r10
	prepare 0x3,4
	prepare 0x3,4,sp
	prepare 0x3,4,0x10
	prepare 0x3,4,0x10<<16
	prepare 0x3,4,0x10000
	stc.b r10,[r6]
	stc.h r10,[r6]
	stc.w r10,[r6]
	mul r6,r8,r10
	mul 8,r8,r10
	mulh r6,r8
	mulh 4,r8
	mulhi 0x10,r6,r8
	mulu r6,r8,r10
	mulu 8,r8,r10
	mac r6,r8,r10,r12
	macu r6,r8,r10,r12
	add r6,r8
	add 4,r8
	addi 0x10,r6,r8
	cmp r6,r8
	cmp 4,r8
	mov r6,r8
	mov 4,r8
	mov 0x10000,r6
	movea 0x10,r6,r8
	movhi 0x10,r6,r8
	sub r6,r8
	subr r6,r8
	adf 0x1,r6,r8,r10
	sbf 0x1,r6,r8,r10
	satadd r6,r8
	satadd 4,r8
	satadd r6,r8,r10
	satsub r6,r8
	satsub r6,r8,r10
	satsubi 0x10,r6,r8
	satsubr r6,r8
	and r6,r8
	andi 0x10,r6,r8
	not r6,r8
	or r6,r8
	ori 0x10,r6,r8
	tst r6,r8
	xor r6,r8
	xori 0x10,r6,r8
	bsh r8,r10
	bsw r8,r10
	clip.b r6,r8
	clip.bu r6,r8
	clip.h r6,r8
	clip.hu r6,r8
	cmov 0x1,r6,r8,r10
	cmov 0x1,4,r8,r10
	hsh r8,r10
	hsw r8,r10
	rotl 4,r8,r10
	rotl r6,r8,r10
	sar r6,r8
	sar 4,r8
	sar r6,r8,r10
	sasf 0x1,r8
	setf 0x1,r8
	shl r6,r8
	shl 4,r8
	shl r6,r8,r10
	shr r6,r8
	shr 4,r8
	shr r6,r8,r10
	sxb r6
	sxh r6
	zxb r6
	zxh r6
	sch0l r8,r10
	sch0r r8,r10
	sch1l r8,r10
	sch1r r8,r10
	div r6,r8,r10
	divh r6,r8
	divh r6,r8,r10
	divhu r6,r8,r10
	jarl _lbl,r6
	jarl [r6],r10
	jmp [r6]
	jmp 0x10[r6]
	ctret
	eiret
	feret
	trap 1
	switch r6
	syscall 1
	di
	ei
	halt
	ldsr r8,0,0
	nop
	snooze
	stsr 0,r8,0
	synce
	synci
	syncm
	syncp
	cache 0x0, [r6]
	pref 0x0, [r6]
	absf.s r8,r10
	addf.s r6,r8,r10
	ceilf.sl r8,r10
	ceilf.sul r8,r10
	ceilf.suw r8,r10
	ceilf.sw r8,r10
	cmovf.s 0,r6,r8,r10
	cmpf.s 0x1,r6,r8,0
	cvtf.hs r8,r10
	cvtf.ls r8,r10
	cvtf.sh r8,r10
	cvtf.sl r8,r10
	cvtf.sul r8,r10
	cvtf.suw r8,r10
	cvtf.sw r8,r10
	cvtf.uls r8,r10
	cvtf.uws r8,r10
	cvtf.ws r8,r10
	divf.s r6,r8,r10
	floorf.sl r8,r10
	floorf.sul r8,r10
	floorf.suw r8,r10
	floorf.sw r8,r10
	fmaf.s r6,r8,r10
	fmsf.s r6,r8,r10
	fnmaf.s r6,r8,r10
	fnmsf.s r6,r8,r10
	maxf.s r6,r8,r10
	minf.s r6,r8,r10
	mulf.s r6,r8,r10
	negf.s r8,r10
	recipf.s r8,r10
	roundf.sl r8,r10
	roundf.sul r8,r10
	roundf.suw r8,r10
	roundf.sw r8,r10
	rsqrtf.s r8,r10
	sqrtf.s r8,r10
	subf.s r6,r8,r10
	trfsr 0
	trncf.sl r8,r10
	trncf.sul r8,r10
	trncf.suw r8,r10
	trncf.sw r8,r10
	absf.d r8,r10
	addf.d r6,r8,r10
	ceilf.dl r8,r10
	ceilf.dul r8,r10
	ceilf.duw r8,r10
	ceilf.dw r8,r10
	cmovf.d 0,r6,r8,r10
	cmpf.d 0x1,r6,r8,0
	cvtf.dl r8,r10
	cvtf.ds r8,r10
	cvtf.dul r8,r10
	cvtf.duw r8,r10
	cvtf.dw r8,r10
	cvtf.ld r8,r10
	cvtf.sd r8,r10
	cvtf.uld r8,r10
	cvtf.uwd r8,r10
	cvtf.wd r8,r10
	divf.d r6,r8,r10
	floorf.dl r8,r10
	floorf.dul r8,r10
	floorf.duw r8,r10
	floorf.dw r8,r10
	maxf.d r6,r8,r10
	minf.d r6,r8,r10
	mulf.d r6,r8,r10
	negf.d r8,r10
	recipf.d r8,r10
	roundf.dl r8,r10
	roundf.dul r8,r10
	roundf.duw r8,r10
	roundf.dw r8,r10
	rsqrtf.d r8,r10
	sqrtf.d r8,r10
	subf.d r6,r8,r10
	trncf.dl r8,r10
	trncf.dul r8,r10
	trncf.duw r8,r10
	trncf.dw r8,r10
	bins r6, 4, 8, r8
	callt 5
	clr1 3, 0x10[r6]
	clr1 r8, [r6]
	not1 3, 0x10[r6]
	not1 r8, [r6]
	dispose 4, 0x3
	dispose 4, 0x3, [r31]
	divq r6, r8, r10
	divqu r6, r8, r10
	divu r6, r8, r10
	fetrap 1
	resbank
	jr 0x10
