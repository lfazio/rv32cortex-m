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
	; The FXU (R01US0209EJ0220 2.5): wreg operands, PE0 only, CU1.
	absf.s4 wr6, wr10
	negf.s4 wr6, wr10
	sqrtf.s4 wr6, wr10
	recipf.s4 wr6, wr10
	rsqrtf.s4 wr6, wr10
	roundf.sw4 wr6, wr10
	roundf.suw4 wr6, wr10
	trncf.sw4 wr6, wr10
	trncf.suw4 wr6, wr10
	ceilf.sw4 wr6, wr10
	ceilf.suw4 wr6, wr10
	floorf.sw4 wr6, wr10
	floorf.suw4 wr6, wr10
	cvtf.sw4 wr6, wr10
	cvtf.suw4 wr6, wr10
	cvtf.ws4 wr6, wr10
	cvtf.uws4 wr6, wr10
	cvtf.hs4 wr6, wr10
	cvtf.sh4 wr6, wr10
	flpv.s4 0, wr6, wr10
	movv.w4 wr6, wr10
	trfsrv.w4 0, wr6
	addf.s4 wr7, wr6, wr10
	subf.s4 wr7, wr6, wr10
	mulf.s4 wr7, wr6, wr10
	maxf.s4 wr7, wr6, wr10
	minf.s4 wr7, wr6, wr10
	divf.s4 wr7, wr6, wr10
	addrf.s4 wr7, wr6, wr10
	subrf.s4 wr7, wr6, wr10
	mulrf.s4 wr7, wr6, wr10
	maxrf.s4 wr7, wr6, wr10
	minrf.s4 wr7, wr6, wr10
	addxf.s4 wr7, wr6, wr10
	subxf.s4 wr7, wr6, wr10
	mulxf.s4 wr7, wr6, wr10
	addsubf.s4 wr7, wr6, wr10
	subaddf.s4 wr7, wr6, wr10
	addsubxf.s4 wr7, wr6, wr10
	subaddxf.s4 wr7, wr6, wr10
	addsubnf.s4 wr7, wr6, wr10
	subaddnf.s4 wr7, wr6, wr10
	addsubnxf.s4 wr7, wr6, wr10
	subaddnxf.s4 wr7, wr6, wr10
	fmaf.s4 wr7, wr6, wr10
	fmsf.s4 wr7, wr6, wr10
	fnmaf.s4 wr7, wr6, wr10
	fnmsf.s4 wr7, wr6, wr10
	cmpf.s4 0, wr7, wr6, wr10
	ldv.w 5, 8[r5], wr10
	ldv.dw 1, 8[r5], wr10
	ldv.qw -16[r5], wr10
	ldvz.h4 24[r5], wr10
	stv.w 0, wr10, 12[r5]
	stv.dw 0, wr10, -24[r5]
	stv.qw wr10, 32[r5]
	stvz.h4 wr10, 40[r5]
	cmovf.w4 wr12, wr7, wr6, wr10
	shflv.w4 0x000, wr7, wr6, wr10
