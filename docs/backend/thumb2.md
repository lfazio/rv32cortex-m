# The Thumb-2 backend

`src/backend/thumb2/` — ARMv7E-M, which is the whole point of the
project: the emulator's own host is a Cortex-M.

It is a lowering of the shared IR and nothing else. `encode.c` spells
instructions, `ir_lower.c` turns one IR block into one host block, and
neither knows what guest it is working for: everything guest-specific
arrives through `emu_ir_target_t`. All four frontends reach it through
the one macro in `src/emu/emu_ir_jit.c`.

| frontend | run on a board under this backend |
|---|---|
| RV32 | yes — `isatest` (748 checks, every B instruction among them), CoreMark, `bench`, on the Nucleo-F746ZG |
| PowerPC e200z7 | yes — `alu`, `sys`, CoreMark, crypto and 600 generated cases, on the F746 |
| G4MH | yes — its small guest on the F746, the board and the host agreeing to the digit; three-PE `smp.bin` on the N6, its line exactly the model's |
| ARMv7-M | yes — all eight guests, both backends, on the F746, each with the host's retired count ([../frontend/armv7m.md](../frontend/armv7m.md#firmware)) |

---

## How it is checked

**No host suite exercises the lowering.** It compiles only for ARM, so
the architecture suites and `ctest` say nothing about it. Validate by
flashing a guest and reading the UART. That has caught real defects that
every host suite passed: an inlined store that skipped the LR/SC
reservation break, a `CMP` that assembled as a different instruction, a
shift by zero that assembled as a shift by 32, and a compaction that
moved blocks with their chained jumps intact (below).

The *encoders* are a different matter, and are checked on a host twice:

- `scripts/t2-check-encodings.sh` compiles `encode.c` for the host —
  it is plain C that writes bytes — and compares what it emits with
  `arm-none-eabi-as`. `scripts/check-thumb2-vfp.sh` does the same for
  the FP forms.
- `tests/guest/armv7m/t2exec.c` compiles the same encoder into a
  Cortex-M guest, emits instructions into guest RAM and **branches to
  them**, on the ARMv7-M frontend. That frontend exists because of this.

Neither reaches `ir_lower.c`'s decisions — which registers, which
frame slot, which branch gets patched.

**`emu_ir_can_lower` answers yes by default, and every yes is a
promise.** It has been broken three times, each found when a frontend
started asking: the rotations and the four value bit operations, then
the memory bit operations (`EMU_IR_BITOP_*`), which `lower_one` has never
lowered, then CLMUL and AESSB, which an M7 has no instruction for. G4MH
emitted the bit operations anyway and lost each block containing one at
emission; RV32 believed the yes for `clmul`, which GCC makes of
CoreMark's CRC loops, and CoreMark under the JIT was 32% slower here
with 910,195 instructions interpreted. Every operation `lower_one`
refuses is named now -- SELECT and POPCNT too, which nothing asks about
yet -- and a refusal of a promised one is counted and printed as
`unkept` ([../jit/README.md](../jit/README.md)), so the next one shows in
the run's own report.

## Registers and the frame

| | |
|---|---|
| `r4` | the `emu_cpu_t` pointer, callee-saved so it survives a helper |
| `r5` | the retired count the block returns |
| `r11` | `t2_mem_stub`, the slow path every load and store shares -- and, `ADDW r12, r11, #delta` away in the same ITCM section, `t2_ibtc_stub`, where a marked computed exit goes ([jit/README.md](../jit/README.md#computed-jumps-the-stub-emu_jit_ibtc)); it returns with `pop {r4-r11, pc}`, which is why every block's save set is the same, and `emu_ir_lower` checks it before sending an exit there |
| `sp` | the temp frame: every IR value has a home in it |
| `r0`–`r3`, `r12` | scratch, and the AAPCS argument registers |
| `r6`–`r10` | handed out by `emu_ir_regalloc`, on top of the frame |

**The guest register file stays in memory.** A guest register is one
`LDR.W`/`STR.W` off `r4`, and guest state is coherent at every point a
fault can be taken — a trap, an interrupt or a debugger read needs no
unwinding. Data processing and memory access use the 32-bit forms
throughout: allocated registers are frequently above `r7`, and a 16-bit
encoding handed one does not fail.

A **register cache** in `r8`–`r10` was built and measured **15.5%
slower**. Reads per block said it should win; it did not, because a
cached read is `MOV` where an uncached one is `LDR` — one instruction
either way — while write-through adds an instruction per write and three
more registers hit every PUSH/POP. The histogram that would have
predicted this exists now (`EMU_JIT_HOT_REG_STATS`): 88–93% of (block,
register) pairs are referenced once or twice, and pinning breaks even at
two.

The **register allocator** is a different thing and does pay, but only
when the value is used *in place*. The first version routed allocated
temps through `ld_operand`/`st_slot` exactly as x86-64 does and measured
worse: 6040 translations against 5943, 894 compactions against 828, and
410 buffer overflows where there had been 309. On x86-64 that
substitution turns an eight-byte `[rsp + disp32]` into a three-byte
`mov`; on Thumb-2 `LDR.W` and `MOV.W` are four bytes each and it is pure
cost. Computing *into* the allocated register is what pays, and pays more
than on x86-64, because Thumb-2 is three-address: an ADD with both
operands and its destination allocated is **one** instruction where the
frame needs four.

## Encoding hazards

An encoder whose wrong answers are other valid instructions needs its
**boundary** values tested, not its typical ones. Three defects, all at
an end of a range, none of which computed a wrong answer that any test
was looking at:

- **A register that does not fit assembles as a different instruction.**
  The 16-bit `CMP`/data-processing form encodes r0–r7;
  `emit_dp_reg(DP_CMP, R8, R1)` set a bit belonging to `rm` and became
  `CMP r0, r1`, so the loop cap never applied and chained loops ran
  unbounded — 3700 guest instructions per block entry where 64 was
  intended. It looked like extra throughput and was the interrupt-latency
  bound being thrown away.
- **`imm5 == 0` does not mean "no shift", and only `LSL` reads it that
  way.** `LSR #0` **is** `LSR #32`, `ASR #0` is `ASR #32`, `ROR #0` is
  `RRX`. RISC-V spells a move as `srli rd, rs, 0`, which assembled as a
  shift by 32. Rewriting the type to `LSL` is the fix; *skipping* the
  emit is not, because `rd` and `rm` differ and the move still has to
  happen. Invisible to x86-64, whose `shr r32, 0` is a genuine no-op.
- **Branch range is a silent cliff.** Loop chaining was once emitted only
  when the back edge fitted the 16-bit conditional branch (±254 bytes); a
  larger block stopped chaining rather than widening the encoding,
  costing 2.4× on the loops that crossed the line. Every branch this
  backend emits is the wide form now.
- **A VFP register's low bit sits apart from the rest, in a field of
  its own.** `t2_vfp3` put the destination's (D) at bit 6 of the second
  halfword, which is `sub` -- VADD with an odd destination was VSUB into
  the even register below. Every caller passed s0 until the vector
  lowering wanted more; `t2-check-encodings.sh` now asks the assembler
  with odd registers in all three fields, and the old placement fails
  it.

## The guest's vector registers: the DSP extension

`EMU_IR_VOP` (an operation on whole guest vector registers, in the cpu
state) is lowered a word at a time with the DSP extension's SIMD within
a register: vs2's word in r0, vs1's in r1 or the scalar in r12 --
masked to the element and repeated across the word by two `ADD ... LSL`
-- the operation, then vd's bytes past vl kept. r3 holds a 256-byte
window of ones then zeros, less vl's bytes, so each word's mask is one
LDR from it (unaligned, which the M7 reads from normal memory) and the
keep is `AND`, `BIC`, `ORR`. At 8 and 16 bits: `UADD8`/`UADD16`,
`USUB8`/`USUB16` (RSUB with the operands swapped), `USUB` or `SSUB`
setting GE by lane and `SEL` for the minimum and maximum, `SMULBB` and
`SMULTT` packed by `PKHBT` for a 16-bit multiply. At 32: the ALU, a
compare and an `IT` for the minimum and maximum -- NZCV written, which
no IR instruction here carries to the next. No 8-bit multiply and no
shift narrower than a word: the frontend's helper. The averages under
rnu are the halving add (`UHADD8`/`16`, `SHADD8`/`16`, which truncate)
plus each lane's lost bit, `(a ^ b) & 1`, added with `UADD8`/`16` -- a
lane cannot carry out -- and at 32 bits `(a | b) - ((a ^ b) >> 1)`; the
multiply-adds are `MLA` and `MLS` at 32 bits, vd's word loaded first,
and narrower ones keep the helper: the 16-bit product needs the
register vd's word is in. vecbench on the F746 74.29 to 48.21; the
replay 5,604 compared clean, and with the lost bit dropped 64 different.

`vwmacc` from signed halfwords is what this host has instructions for:
a source word holds two elements whose products go to vd's next two
words, `SMLABB` and `SMLATT` each accumulating into the word it
replaces. Both products are formed before either is kept, in r2 and lr
-- lr is free inside a block, since every block pushes it and nothing in
the sequence calls -- leaving r0 and r1 for the masks and the old words.
The encodings, lr and r12 in each position, were checked against the
assembler. vecbench 48.21 to 37.68; the corpus replay clean in both its
halves, and the high element multiplied by SMLABB 77 different.

The narrowing shifts and the signed clip to halfwords go a word of vd
at a time from two source words: shifted by r12, for the clip rounded
up through lr, and packed by `PKHBT`. The clip saturates with
`SSAT #16`, and **Q is its vxsat**: cleared first (`MSR APSR_nzcvq`), the
tail's inputs zeroed by the word's mask -- a halfword's 0xFFFF
sign-extended by `SXTH` and `ASR #16` -- before the SSAT so that only an
active element can set it, read after into the saturation word and
cleared again. NZCV go with it, which no IR instruction carries. The
unsigned clip keeps the helper: USAT reads its 32-bit input as signed.
vecbench 37.68 to 27.54; the replay 7,900 clean, and with the tail not
zeroed 16 different.

**The other rounding modes**: an average's from the halving add (the
floor) and the lost bit -- at 32 bits the floor is one `ADD` with
`LSR`/`ASR #1` of a ^ b -- rne adding lb & floor, rdn nothing, rod an
`ORR`; the signed clip's per element, rod ORing in `CLZ`-tested "a bit
shifted out was set", rne adding v[d-1] where the bits below or the
shifted value's low bit are set. **The corpus could not test the clip's
modes**: a random word shifted by a few bits saturates to 16, which
hides its rounding, and no input of 12,003 told rod from rdn there --
the rod mutant ran clean on all three slices, and fuzzing for 300 s at
an x86 mutant of the same case found nothing either. `vecround.c`
(ties, small values) printed the host's checksum on the F746, and
`c8878003` and `6a173d2c` with rod's ORR or rne's low bit dropped. The
average's rne made rnu, 13 and 8 different in the replay.

Masked (`EMU_IR_VOPM`), a word's mask bits go to lr: the mask
register's byte holding the word's first element, shifted and cut to
the word's 1, 2 or 4 bits, then spread to its lanes -- negated for a
word; for halfwords and bytes multiplied by 0x8001 or 0x00204081, which
puts each bit at its own lane's bottom, ANDed with 0x00010001 or
0x01010101 and multiplied by 0xFFFF or 0xFF. ANDed into the word's
tail mask, it keeps a masked-off element's old value as the tail does;
a merge is `BIC`, `AND` and `ORR` between vs2's word and the source.
The replay 8,658 inputs clean in two halves. With the byte spread's
multiplier a bit off, 72 different in the second half -- the first
half's count was lost: `ir-board.sh` printed sixty differing cases and
cut the summary after them, and prints it always now -- and with the
mask not ANDed in, 306 and 317.

The narrowing forms take the same mask, after the shifts, where r2 and
lr are free. The widening one (signed halfwords) has its products in r2
and lr, so each 32-bit element's bit is tested in place -- `LDRB`, `TST`,
and under `IT EQ` a `MOV` clearing r0, that word's tail mask. **The
corpus never reached it**: no input of 12,667 differed with the `MOV`
made a `NOP`, in any slice. `tests/guest/vecmask.c` did: the host's
checksum on the F746, `0e09b928` with the `NOP`. The narrowing mask
dropped, 9 different in the replay.

**The 16-bit multiply-adds and the unsigned widening ones** are lowered
too. A 16-bit `vmacc` family's products are each lane's low 16 bits,
signed or not the same: `SMULBB` and `SMULTT`, packed by `PKHBT`, added
or subtracted by `UADD16`/`USUB16`. They need lr, so a masked
operation's element mask now comes after the operation (before only for
the merge, which chooses by it) -- which a 16-bit one needed anyway, the
mask's expansion using r2 where d is. `vwmaccu`, `vwmaccsu` and
`vwmaccus` from halfwords extend each with `UXTH`/`SXTH` (low) or
`LSR`/`ASR #16` (high) and `MLA`, through whichever of r1 and r12 holds
no operand. The replay, 12,667 inputs, clean; the high product from the
wrong half, 11 different in a slice, the unsigned operand sign-extended,
2. Still the helper's: the widening forms from bytes (four lanes
extended and two products a halfword is more registers than a block
has free), narrowing to bytes (no lane shift), and the unsigned clip
(`USAT` reads its input as signed).

A reduction (`EMU_IR_VRED`) goes a word of vs2's group at a time into r0,
its inactive lanes set to the identity -- cleared, set, or r12's signed
maximum or minimum -- and combined into r1 with the operation's
`vop_word`; then r1 rotated by 16 and by 8 and combined again, which
leaves lane 0 the whole reduction; then vs1's element 0; then lane 0
stored with a store of the result's width, skipped when vl is 0. The
widening sums add each lane extended with `SXTAB16`/`UXTAB16` from
bytes and `SXTAH`/`UXTAH` from halfwords, a word twice (unrotated, and
by 8 or 16). A strided access (`EMU_IR_VMEMS`) is a loop of
register-offset moves, r12 the window's host address and r3 the
element's offset in it, r2 its offset in the cpu, lr the end; the bounds
are x86-64's. A unit-stride access whose vl is short of its group took the same
loop at first, its stride the element's size: vector CoreMark on the
F746 from 4.3% more cycles than scalar to 0.3% fewer, but vecbench 0.5%
slower (25.44 to 25.57) -- about 90 bytes at every access, and an e8
group's last trip, up to 127 bytes, a byte a turn where the helper it
replaced had copied with `memcpy`. It is the bounds checks and a
`memcpy` call now: vecbench 24.98 at both layouts, 61,264 bytes
translated, vector CoreMark 0.1% under scalar; the replay, 12,920
inputs, clean.

**The first replay with them differed in 263 of 3,456 inputs**: the
loop's exit for vl 0 was a `B<c>.W`, and it was patched with `B.W`'s
fields alongside the loop's unconditional end -- which made it a `B.W`,
and every inline strided access jumped straight past itself. Each
branch is patched by its own kind now. With that, the corpus, 10,370
inputs in three slices, ran clean; the signed identity not set, 5
different in one slice, the stride never added, 44. The encodings --
these and the DSP ones the vector lowering had been checked against by
hand -- are `scripts/t2-check-encodings.sh`'s, a ctest now (72 match).

**The unrolled words cost vecbench nothing yet.** Its translations reach
53 KB of the 64 KB buffer, which read as a reason to loop over an m4 or
m8 group's words rather than unroll them. At a 32 KB buffer it runs at
27.43 cycles a guest instruction against 27.54, with 2 compactions, 15
blocks evicted and nothing given up on; at 48 KB, 26.64. The hot set
fits, and most of the 53 KB runs once. A PC profile of it
(`-DVECBENCH_ROUNDS=2000`) puts two thirds of the guest's time in
translated code, a sixth in the vector helper -- the strided load and
the last trip of each loop -- and a sixth in the console. The fifteen
encodings, r12 in every position, were checked against
arm-none-eabi-as before the first flash; one byte of the comment
beside them was not.

**The first board run lost 95 blocks** (`unkept`): `bisect_allows`, the
gate that allows one group of operations at a time for bisection,
answers false for anything it does not list, after `emu_ir_can_lower`
had promised the operation. Listed at its own level, 11.

On the F746, vecbench (`tests/guest/vecbench.c`, `-DRV32_EXT_ZVE32X=ON`)
under the JIT: 141.01 host cycles a guest instruction with every vector
operation the helper's, 134.22 lowered, the checksum the host's; the
code it translated 28,520 bytes to 36,280 -- a word is ten
instructions, unrolled, so an m8 operation is over a kilobyte. Before
this round's vector work (03044e9: vector instructions declined, a byte
loop an element) it was 441.55, slower than the interpreter's 424.22;
the interpreter is 226.46 now.

### Single precision: VFP

`EMU_IR_VFOP` is an element loop: r12 the vector file, r3 vl's bytes
(clamped to the group), r2 the element's offset, and for a masked
operation r1 the mask register's first word -- 32 elements at most --
shifted out a bit an element (`LSRS`, `BCC` past an inactive one); each
element `VLDR`s its sources, computes in s0 and `VSTR`s it. Under the
block's FPSCR -- round to nearest, default NaN -- the NaN is RISC-V's
canonical one and the flags are harvested at the exit with the scalar
FP's. All fifteen operations under RNE; the fused ones as the scalar
FMA: NEG_ADD picks `VFNMS`/`VFNMA`, NEG_MUL is `sub`, the addend in s0
(vd for `vfmacc`'s kin, vs2 for `vfmadd`'s). vfbench on the F746 125.63
to 33.26 host cycles a guest instruction, the same at a second layout
([frontend/rv32-vector.md](../frontend/rv32-vector.md#single-precision-under-the-jit)).
The corpus replay finds the product's sign inverted, a reversed
subtract and the mask's sense. Underflow is computed as for the scalar
operations (below).

### Vector loads and stores

`EMU_IR_VMEM`, as on x86-64: vl's bytes against the group's (`CMP`
r3), the window offset against its size less the group's, the guard
byte for a store, then a word at a time through r12 -- unaligned, which
the M7 reads and writes for normal memory -- and the helper, with r0
the cpu and then the instruction, the address and vl, wherever not.
Listed in `bisect_allows` from the first: vecbench on the F746 under the
JIT 134.22 to 74.29, the code it translated 36,280 bytes to 47,424 --
an m8 access is 32 word moves, unrolled. The corpus replay compared
4,938 blocks with none different; the window check 4 bytes loose, 21
different, every one a VMEM block. With the vector unit off the RV32
CoreMark firmware is unchanged (10.75 both sides, two layouts), and the
code is not built (`EMU_IR_VECTOR`).

### Helium (MVE) on the N6

The N6's Cortex-M55 has MVE: Q0-Q7, 128 bits, which are S0-S31 -- so a
block uses Q0-Q3, Q4-Q7 being the callee-saved S16-S31. Built where the
compiler targets it (`__ARM_FEATURE_MVE`) and used where MVFR1 says the
core implements it, read once; the M7's reads 0 there. Where it is, two
lowerings take MVE's road and the rest keeps the DSP extension's:

- **Unmasked single-width `EMU_IR_VOP`**, a register of the group at a
  time: vs2 in Q0, vs1 in Q1 or the scalar duplicated into it once
  (`VDUP`), vd's old value in Q2, the operation into Q0, then the tail's
  byte mask from the same window the DSP path reads (`VLDRB` into Q3)
  and Q0 = old ^ ((new ^ old) & mask) before `VSTRW`. Every width for
  the multiply and the shifts -- which the DSP path lacks at 8 bits, and
  for shifts below 32 -- a shift's count masked to the element and
  negated for a right one, `VSHL` shifting by a signed count; the
  averages under rnu and rdn (`VRHADD`, `VHADD`; rne and rod keep the
  DSP path); the multiply-adds as `VMUL` and `VADD`/`VSUB`.
- **`EMU_IR_VMEM`'s whole-group copy**, 16 bytes a pair: `VLDRB`/`VSTRB`
  on the RAM side, which takes any alignment, `VLDRW`/`VSTRW` on the
  register file's, from a register at vd.

The encoders (`t2_vldrw`, `t2_mve3` and the rest) are held to the
assembler for the M55 by `t2-check-encodings.sh`, Q7 and negative
offsets among them: 35 instructions, all right first time. vecbench on
the N6 under the JIT, 400 rounds, 25.02 and 24.99 host cycles an
instruction to **20.45 and 20.41** at two layouts, 18% fewer, the code it
translated 62,084 bytes to 47,700 and the checksum the host's. The baseline was
measured again from a worktree given the same ROM/RAM line: the first,
at 256 KiB of ROM, read 25.05 and 25.01.

**The reductions** (`lower_vred_mve`) are predicated across-vector
instructions into a general register that starts as vs1's element 0:
`VADDVA` adds the lanes in 32 bits -- the widening sums' arithmetic too
-- and `VMINV`/`VMAXV` take the extreme of the register's element and the
lanes. P0 is the register's active elements: `VCTP` of what vl leaves
it, and under a mask ANDed with the mask's bits, spread to P0's byte
bits by a 16-entry table at 16 and 32 bits (`mve_active`). Inactive
lanes take no part, so there is no identity to fill. AND, OR and XOR
have no MVE across-vector form and keep the DSP path. On vecbench they
are worth 0.2% (20.45 to 20.41, 20.41 to 20.39), because its
reductions run once a call, after the loop: the kernels' cost was the
loops, which a measurement of each kernel alone said
([frontend/rv32-vector.md](../frontend/rv32-vector.md#on-the-n6-by-kernel)).

**`tests/guest/vecops.c` checks it on the board**, and its first
version did not: every operation this covers, .vv and .vx, the
reductions plain and masked, masked operations and `vmerge`, at e8,
e16 and e32, m1, m2 and m8, vl at its edges, each vxrm, a line a group.
Run once a case, it printed the host's checksum on both boards and four
MVE mutants each changed it -- but on a board the buffer has compacted
before the run, so a block is translated on its eighth entry in one
context, vxrm is part of the context, and a case met its block six
times a context: only the first group ran translated. A mutant of the
16-bit mask spread changed nothing at all, which is how it showed; the
host's buffer never compacts, so its count of helper calls could not.
Each case runs ten times now, and the mutants hit exactly their groups:
the 16-bit spread all 12 e16 lines, the mask not applied all 36, the
signed widening sum made unsigned the 24 at e8 and e16, and the
reversed subtract, the tail window, a right shift not negated, the
store copy's register and the signed minimum each all 36. The N6 then
translated 3,529 blocks and interpreted 5.1M of 95.5M instructions.
**When vl fills the group there is no tail to keep**, which is every
trip of a strip-mined loop but its last: one compare sets Z, and each
register's blend -- vd's old value, the window's mask and three logic
operations, five instructions of nine -- is branched over on it, MVE's
operations writing no flags. vecbench 20.41 to 19.83 at both layouts.
Deciding "full" four bytes early is found, in the 12 lines whose vl
falls one 32-bit element short. The same skip on the F746's DSP path --
a compare a word against lr, since its operations write the flags --
gained nothing (17.99 to 17.98 and 17.99) for 2.5 KB more code: two
loads from DTCM and three ALU operations cost the M7 what a compare and
a taken branch do. Not kept.

**The widening multiply-adds and the narrowing shifts and clips** go
through the register file's memory, which MVE's widening loads and
narrowing stores read and write element for element in order:
`VLDRB.<S|U>16` and `VLDRH.<S|U>32` take 8 bytes of a source group to a
register of the wide width, and `VSTRB.16`/`VSTRH.32` store a wide
register's low halves -- where `VMULLB`/`VMULLT` and the narrowing
shifts work on even and odd lanes, not RISC-V's order. A widening
multiply-add is then `VMUL` and `VADD` at the wide width a register of
vd at a time, with the tail kept as above; a narrowing one shifts a
register of vs2 at the wide width (`VSHL` by the negated count, or
`VRSHL`, which rounds half up, for a clip under rnu), clamps a clip
with `VMIN`/`VMAX`, finds its saturated active lanes by comparing with
the unclamped value (`VCMP` into P0, ANDed with `VCTP`'s), and stores
the active elements by a predicated narrowing store. From and to 8 and
16 bits, every form; the clips under rnu and rdn, the DSP path keeping
rne and rod where it has them. vecbench 19.83 to 18.72 and 18.69; dot
13.5M cycles to 11.5M, clip 16.9M to 13.9M. Since the first MVE
lowering, 25.02 to 18.72: 25% fewer cycles.

The first board run of it differed in the e16 lines under rnu and rdn
alone: `t2_movs_imm8(T2_LR, 0)` -- the accumulator of saturated lanes,
zeroed -- emitted nothing, since MOVS's 16-bit form has no lr and the
encoder says so by returning false, which `(void)` threw away. lr kept
what it held, and vxsat read it as set. The 8-bit lines passed, so this
is the file's own trap a third time: an encoder whose refusal is not an
error. Four mutants of the new paths are each found: saturation never
recorded, the signed clip's maximum one low, `vwmaccsu`'s vs1 unsigned,
the count masked to the narrow width.

Not yet in MVE: the masked single-width operations (`VPSEL` on the same
P0), and the vector FP -- which MVE computes flushing subnormals to
zero whatever FPSCR says (`vfmul` of 0x3F7FFFFF and 0x00800000 gave 0
and UF), and which was at most 5.6% of vfbench even without the guard
that would need ([TODO](../TODO.md)).

## The guest pc, stored lazily

**What a hot block spends was read off the board** (`-DEMU_JIT_DUMP=ON`
with `-DEMU_PC_PROFILE`, `scripts/board-jit-dump.py`, in
[../performance.md](../performance.md#what-the-hot-translated-code-is)):
vecbench's scalar checksum loop on the N6 -- `lw`, `rori`, `addi`, `xor`,
`bne` -- was about thirty Thumb-2 instructions an iteration, and six of
them stored the guest pc: `MOVW`/`MOVT`/`STR` at the loop's head, for
the load's slow path, and again before the loop bound's compare, for
the exit to the dispatcher it rarely takes.

So a constant `SETPC` emits nothing now. Its value is pending
(`g_pc_pending`), and goes to the cpu only where something can read it:
a helper's call, a memory access's slow path (`emit_mem_stub`, through
r0, which the stub ignores on entry), the vector accesses' helpers, a
GET of the register that is the pc -- and a PUT to that register drops
it, or a later store would overwrite what the PUT wrote. An exit stores
its own. A block begins with its own pc pending, which holds on every
way in (the dispatcher and a chained exit store it), so a loop's back
edge stores nothing: the bound's exit stores the start pc on its way
out, after the compare (`loop_back`).

On the F746, CoreMark under the JIT at two layouts, retired counts and
CRCs unchanged: RV32 10.81 to 10.27 (-5.0%), ARMv7-M 13.66 to 13.38
(-2.0%), G4MH 18.98 to 18.69 (-1.5%), PowerPC 17.68 to 17.65 (-0.1%).
On the N6 vecbench's scalar kernel 12.42 to 10.86, and vecbench 18.72
to 17.35. isatest passes; CoreMark x650 at 32 KB validates
(`0x25e3`, 239 compactions, 20.3 s). The corpus replay found two
mutants -- no store on a memory slow path (405 inputs of 3,615), none
before a helper (67) -- and two not at all: a block that does not start
with its pc pending, and a PUT to the pc that leaves an older one
pending. No input looped back into a block that reads the pc before
writing it, or wrote r15 while a pc was pending; frontends do neither,
and the IR allows both. Two inputs written for them
(`tests/fuzz/regressions/ir/lazy-pc-*`) find each.

## Underflow, as RISC-V raises it

VFP detects tininess before rounding and RISC-V after, so a product
that rounds up to +-2^-126 raised UF under the JIT and not interpreted
or on the host (`tests/guest/fpedge.c`, flags 3 against 1). Only that
result can differ, and at it either answer can be right -- 2^-126 -
2^-150, a tie on the subnormal grid, is tiny after rounding too -- so
the result alone cannot settle it.

So where the target can redo an operation in the guest's arithmetic
(its `vfop`, SoftFloat's for RV32, which needs F now and not Zve32f),
UF is not taken from FPSCR at all: the block's harvest masks UFC out
(`emit_fp_harvest`), and UF is computed where it can arise. VFP raises
it only from a multiply, a divide or a fused multiply-add -- a tiny sum
is exact -- and only with a result of magnitude 2^-126 or less, where
RISC-V's UF lies too. Each of those three now keeps its operands in
s0-s2 and its result in s3, compares the result with 2^-126 (shifted
left one, so the sign drops out, against 0x01000000), and at or below
it calls `t2_uf_fix` with a, b and c where the hard-float ABI passes
them: the operation redone, its UF handed to the guest through the
target's `fp_flags`, the result -- the same either way -- returned. The
vector element loop does the same, keeping its registers and the
scalar in s3 across the call. Targets whose guest detects tininess
before rounding (ARMv7-M, G4MH) set no `vfop` and keep FPSCR's UFC.

The first version read FPSCR before each of the three and rebuilt it
on the slow path: RV32 Whetstone on the F746 26.05 and 26.15 host
cycles an instruction to 27.14 and 26.90. Without that read, and with
the check on the result's core register: **26.67 and 26.72, 2.4% and
2.2%** -- the price of the exact flag; the module output and the
retired count unchanged. vfbench 34.45 to 36.16 (5.0%), the check in
its element loop. fpedge and fpedge-v print the host's flags on the
F746 and the N6 under the JIT; fptest's five kernels and vecfp are the
host's; the corpus replay clean.

## Calling convention

Cortex-M selects instruction set with the low bit of a branch target, so
a block address needs bit 0 set before it is called. That belongs in the
**framework**, which knows the host, not in the frontend, which knows the
guest. Getting it wrong faults on the first block entry rather than
computing anything wrong: the banner prints and nothing else.

## Committed code is not emission

**A compaction caused by a buffer overflow unlinked nothing**, and the
board hard-faulted on a branch to 0x7A4 bytes *below* the code buffer.

The emitter's branch patcher declines to write while the buffer has
overflowed — rightly, during emission, because the slot it was handed
may lie past the end. But the hooks the framework calls to link, unlink
and retire blocks went through that same patcher, and an overflow is
exactly when the framework compacts: the flag was still set, `unlink_all`
marked every chained exit unlinked and rewrote none of them, and the
survivors were then moved with jumps aimed at blocks that had just been
evicted. A relative branch moves with its block, so the target moved
too — out of the buffer.

It needs a block larger than `EMU_JIT_BLOCK_RESERVE`, because otherwise
the reserve triggers the compaction first and the flag is clear. That is
8192 bytes on a host, which no block reaches, so no host run had ever
overflowed — and 512 here, where every guest that fills the cache does.
A host runner built with this backend's reserve segfaults on the same
guest (x86-64's patcher carried the same guard), so it was never a
Thumb-2 defect, only one that could not be reached anywhere else.

**It was not a PowerPC defect either, though PowerPC found it.** RV32
CoreMark at 120 iterations overflows 982 times on the F746. Built from
the commit before the fix, in a worktree, it prints its banner and never
finishes; with the one fix applied to that tree it completes with
`crcfinal 0xd340`. The defect arrived with block chaining and was there
for three weeks. `isatest`, which is what a board run usually is, never
overflows the buffer and so could not meet it.

Two fixes, each sufficient alone and each reverted alone to show it:

- the framework reports an overflow by value and clears the emitter's
  flag when the attempt ends;
- the hooks use `t2_write_branch`, which does not consult it.
  `t2_patch_branch` is for a branch in the block being emitted and
  nothing else.

`emu_jit_invalidate_page` already cleared the flag by hand before
calling the same hooks, with a comment saying why. `compact()` called
them too and had no such line. **A fix applied at one of two call sites
is a note about where the other one is.**

How it was found is the reusable part: the stacked pc was in `.bss`, the
range checks on every block pointer and link patch had not fired, and
dumping the code buffer over gdb showed the last block entered ending in
`b.w` to an address outside it — while the block table said that exit
was *unlinked*. The table and the bytes disagreeing is the whole
diagnosis.

## Caches

The JIT writes instructions as data and branches to them. On a Cortex-M7
that needs a real clean-to-PoU and I-cache invalidate by address, not the
DSB/ISB that sufficed with no caches.

The framework calls `emu_jit_ops_t.sync` after every translation, after
a compaction and after patching a link; on this host that is
`t2_sync_code`, which issues the barriers itself and delegates the
maintenance to `board_sync_icache`. That split is deliberate: the
barriers are a property of the *host*, and whether there are caches to
maintain is a property of the **platform** — nothing in the compiler
flags distinguishes the parts, since `-mcpu=cortex-m4` and
`-mcpu=cortex-m7` both define `__ARM_ARCH_7EM__`. So `board_sync_icache`
is weak and a no-op, and the F746 overrides it.

**It was missing entirely, and this section described it anyway.** When
the hand-written translator was replaced by the IR backend, the new one
inherited `.sync = NULL` from a macro shared with x86-64 — whose caches
*are* coherent with instruction fetch, so the comment justifying it was
true where it was written and false where it was copied.

**It has not been shown to fail without the maintenance.** CoreMark on
the F746 with `.sync` back at `NULL` — 25 iterations, 55,452 translations
into a 12 KB buffer with 55,441 evictions — came out identical to the
fixed build. The change is kept because the architecture requires it,
**not** because a failure was observed. Do not read "the A/B showed
nothing" as "the maintenance is unnecessary"; read it as "this workload
cannot see it".

## Sizes

On a microcontroller the framework's tables are the guest's memory, so
they follow the target (`EMU_HOST_JIT_THUMB2`):

| | here | on a host |
|---|---|---|
| blocks, hash buckets | 256, 256 | 65,536 each |
| block reserve | 512 bytes | 8192 |
| code buffer | `EMU_JIT_CODE_BYTES`: 64 KB on the F746 (96 with PowerPC or ARMv7-M, 64 with G4MH), 128 on the N6, 32 elsewhere -- see `CMakeLists.txt` | 64 MB |
| IR instructions, temps per block | 512, 256 | 2048, 1024 |

On the F746 an RV32 JIT build at the 64 KB default has 111 KiB less
guest RAM than an interpreter one, 193 KiB against 304 at ce02bd0. At
32 KB the gap is 79 KiB: the buffer, plus about 47 KiB of tables and IR
storage that a JIT build pays at any size.

## Measured

Nucleo-F746ZG, 216 MHz, `RelWithDebInfo`, host cycles per guest
instruction. Each figure is the firmware's own count and agrees with a
wall clock on the host end of the UART.

RV32, CoreMark at 120 iterations (30,027,635 instructions) and `bench`,
at ce02bd0:

| | CoreMark | translations | guest RAM | `bench` |
|---|---|---|---|---|
| interpreter | **102.27** | — | 304 KiB | **91.87** |
| JIT, 32 KB | 107.00 | 41,989 | 225 KiB | 15.01 |
| JIT, 48 KB | 49.49 | 17,103 | 209 KiB | |
| JIT, 64 KB (RV32's default) | **14.55** | 333 | 193 KiB | |
| JIT, 96 KB | 14.59 | 297 | 161 KiB | 14.50 |

**CoreMark's translated working set fits 64 KB now**, and from there up
the JIT is 7.0× the interpreter. At 32 KB the cache is still a queue,
41,989 translations for a guest with a few hundred blocks, and the JIT
is 1.05× slower than interpreting. When this table was first written it
was 1.57× slower there, and needed 96 KB to fit at all (588
translations at 53.1). The cache did not change; the code that goes into
it shrank, as [Where the bytes go](#where-the-bytes-go) describes.
`bench` fits everywhere: 14,648 bytes. **64 KB is the default
because of this table**: it gives up nothing here and gives the guest
32 KiB that 96 KB took. PowerPC CoreMark is what it costs -- see
[../frontend/ppc.md](../frontend/ppc.md#what-it-is-worth).

From 48 KB up, 120 iterations finish inside CoreMark's ten seconds,
so the guest calls those runs invalid. The figure is the firmware's
count, the CRC is `0xd340` in every row, and the host's clock agrees
(2.00 s against the firmware's 2.03 at 96 KB). PowerPC on the same board
has the same shape with larger guests: see
[../frontend/ppc.md](../frontend/ppc.md) and
[../performance.md](../performance.md#on-the-board-at-ce02bd0).

**Any board figure from a run longer than 19.9 seconds, measured before
the counter was fixed, is a wrap short.** DWT CYCCNT is 32 bits and the
firmware subtracted an epoch from it, for the guest's clock and for the
performance figure alike. A PowerPC CoreMark run printed 42.0 cycles per
instruction — and the JIT 5× ahead of the interpreter — while taking
21.8 s against the interpreter's 12.3; the true figure is 393.4.
`emu_cycles.h` accumulates wrap-correct deltas into 64 bits now. The
tell, before the wall clock settled it, was a profile that priced a
translation at 105,000 cycles in one configuration and 865 in another.

## Where the bytes go

`-DEMU_JIT_BYTE_STATS=ON` prints emitted bytes per IR operation after a
run, on the board as on a host (`include/emu/emu_ir_bytes.h`). It had
been measured once on x86-64 and not kept, and nobody had measured this
backend at all -- while code size is the thing the cache, which is the
guest's RAM, pays for.

RV32 CoreMark x120 on the F746 at 96 KB, per translation:

| | bytes / guest insn | LOAD | STORE | GET | PUT | EXIT_IF |
|---|---|---|---|---|---|---|
| x86-64, for comparison | 59.3 | 100 | 104 | 7 | 7 | 42 |
| Thumb-2, as it was | **60.8** | 92 | 105 | 5 | 4 | 38 |
| Thumb-2, short encodings | **44.6** | 77 | 86 | 3 | 2 | 30 |
| ...and the one-compare window guard | **36.0** | 54 | 59 | 3 | 2 | 27 |
| ...and one RETIRE per stretch | **33.9** | 54 | 59 | 3 | 2 | 27 |
| ...and one shared slow path | **31.1** | 43 | 51 | 3 | 2 | 27 |
| ...and the host pointer from a pool | | 38 | 47 | | | |

**The encoders always chose the 32-bit form.** `MOV`, `CMP`, and the
loads and stores of the frame and of the guest register file were
`MOV.W`, `CMP.W`, `LDR.W` and `STR.W` whatever their operands, though
Thumb-2 has 16-bit encodings for all four: `MOV` and `CMP` for any pair
of registers but SP and PC, `LDR`/`STR` for a low register at a word
offset from SP (to 1020) or from a low base (to 124) -- which is every
frame slot a small block has and the head of every frontend's register
file, reached through r4. They choose by operand now, and an access's
displacement is one `ADDW`/`SUBW` rather than `MOVW` and an `ADD`, and
a trap check compares with an immediate zero rather than materialising
one.

**CMP is the one with a trap in it**: T1 holds two low registers and T2
any pair, UNPREDICTABLE when both are low, and T1 handed r8 is the
defect this file's encoding hazards already record. `t2exec` executes
every pairing with values for which a compare of the wrong registers
gives other flags, and checks each form's *length* as well as its
answer, so a fall back to 32 bits is seen too; a mutant using T1 for
every pair fails three cases by name, and one scaling the 16-bit
offset wrongly fails three more. `t2-check-encodings.sh` holds the new
forms to `arm-none-eabi-as`.

27% fewer bytes, and **CoreMark's working set went from not fitting the
96 KB default to fitting it with room** -- which is a cliff, not a
slope. CoreMark x650, the firmware's count, each against a worktree at
the commit before and agreeing with the host's clock:

| | translations | compactions | cycles / insn | wall |
|---|---|---|---|---|
| 96 KB, before | 29,862 | 816 | 26.86 | 19.92 s |
| 96 KB, after | **294** | **2** | **15.71** | **11.65 s** |
| 32 KB, before | 346,178 | 19,453 | 151.68 | 112.56 s |
| 32 KB, after | 299,402 | 15,273 | 135.82 | 100.73 s |

**1.71x at the default** and 1.12x at 32 KB, where the hot set still
does not fit and the JIT still loses to the interpreter. `isatest` on the
board: 748 checks, 1,129 of 65,829 interpreted, as before; `crcfinal
0x25e3` and "Correct operation validated" in every row.

**Then the window guard, which was a third of every access.** It
subtracted the window's base, compared with the limit, tested the
alignment and branched twice, with the base and the limit each a
`MOVW`/`MOVT`: 34 bytes on every load and store. Now the offset is
rotated right by log2(size) after the subtraction, which leaves an
aligned offset as offset >> k and carries a misaligned one's low bits to
the top, so one unsigned compare against size >> k is the bounds test and
the alignment test together -- and the access undoes the rotation with
`[host, r2, LSL #k]`. The base and the limit are modified immediates
when they encode (RV32's 0x80000000 and a 164 KiB window both do), the
branch into the slow path and the one over it are the 16-bit forms
because each target is a few instructions away, the frame is set up and
given back with 16-bit `SUB/ADD SP`, and the slow path loads its
arguments with `MOVS` and `ADD rd, SP, #imm` -- the flags being dead
before a call. 16 bytes of guard where there were 34.

x650 against a worktree at the commit before:

| | translations | cycles / insn | wall |
|---|---|---|---|
| 96 KB, before | 294 | 15.70 | 11.66 s |
| 96 KB, after | 281 | **14.29** | **10.61 s** |
| 32 KB, before | 299,402 | 135.82 | 100.62 s |
| 32 KB, after | 264,568 | **117.63** | **87.18 s** |
| interpreter, same tree | | 111.22 | 82.42 s |

At 32 KB the JIT is now within 6% of the interpreter, from 22% behind.
`t2exec` runs the guard sequence at every edge -- the first and last
aligned slot, one past the end, below the base, each misalignment, at
all three sizes -- and the shifted access; it is a copy of
`emit_fast_guard`'s sequence built from the same encoders, not the
function itself, so the board's `isatest` (748 checks, 1,129
interpreted, unchanged) is what holds the lowering to it.

**Then `RETIRE`, 11% of what was left**: one 4-byte add per guest
instruction. `pass_merge_retire` keeps one per stretch between
observation points, carrying the count of the ones it absorbed -- see
[../jit/README.md](../jit/README.md). 3,291 adds became 1,429; x650
14.29 → 13.94 at 96 KB and 117.63 → 115.24 at 32 KB, against a worktree
at the commit before.

**On the N6's Cortex-M55**, where CoreMark already fitted the 128 KB
buffer, the same changes are the executed code getting shorter: x1500
19.76 → 17.09 cycles an instruction, 12.41 s → 10.73 s, against the same
baseline ([../jit/tuning.md](../jit/tuning.md#the-n6)).

**The peripheral window is inlined too, once armed**: the same guard
with the base as the host address, plus a test of each read-only hole
on a store. `mmiobench` 2.5-7x per peripheral access; see
[../performance.md](../performance.md#driver-performance-the-passthrough-window).

**Then the slow path, shared.** Every access carried its own call into
the checked accessor -- the cpu into r0, the spec into r2, an
out-pointer into r3, `MOVW`/`MOVT`/`BLX`, a compare for the verdict --
22 bytes of a load's 54. It is one `t2_mem_stub` now, in the hot
section, reached by `BLX r11`: an access puts its spec in r2 (bit 7 for
a store), calls, and takes the exit on `NE`; the stub returns a load's
value in r0, which is where the inlined path leaves it too, so the
dead-store rule now holds across a load as it does for anything else.
r11 comes from the block's second argument (`emu_jit_block_arg`) in a
`MOV` before the chain entry -- two bytes a block, inherited by a
chained one -- and is the allocator's sixth register given up; on
x86-64 the fourth was already worth only 0.6%, and the 32 KB figures
below include the loss.

x650 against a worktree at the commit before, under two function
layouts, because 7% is inside what layout alone does on this part:

| | translations | cycles / insn, default | `-falign-functions=32` |
|---|---|---|---|
| 32 KB, before | 256,320 | 117.31 | 115.06 |
| 32 KB, after | **239,432** | **109.17** | **109.30** |
| 96 KB, before / after | 286 / 296 | 14.02 / 14.02 | |
| interpreter, same tree | | 110.46 | |

The translation count does not depend on layout, and is what says the
change is real. **At 32 KB the JIT is level with the interpreter** -- it
was 22% behind before the short encodings -- and at 96 KB, where
CoreMark fits, a slow path that is rarely taken changes nothing. On
ARMv7-M CoreMark at 32 KB, 238.19 → 226.09 (one layout). isatest, mmiobench and
every ARMv7-M, G4MH and PowerPC board guest pass through it.

**Then the host pointer, from a pool per block.** Every inlined access
built the window's host address with `MOVW`/`MOVT`, eight bytes. It
cannot come from a shared slot, because a frontend may change the
window between translations without a flush -- ARMv7-M's is wherever
the stack is -- and each block's guard is baked against its own. So
each block carries its own copy after its last instruction, and an
access loads it with `LDR.W rt, [pc, #imm12]`: four bytes, and four a
block for the word plus two of alignment half the time. The same pool
takes the window's base and limit when they are not modified
immediates, which is ARMv7-M's stack window. A value that needs only
`MOVW` stays one, and nothing else is pooled: a constant used once
costs eight bytes either way, and a load where `MOVW`/`MOVT` had none.
A block with a single access pays that too, which is why the pool is
worth 4 bytes an access less 4 to 6 a block rather than 4 an access.

Two things it relies on. **Block starts stay word aligned**, at
translation and through compaction: `Align(pc, 4)` rounds a load at
2 mod 4 differently from one at 0 mod 4, so a block moved by a halfword
would load its neighbour's bytes. And **`LDR.W` reaches 4095 bytes
forward**, which the end of a block may not be within; once the oldest
load waiting on the pool is 3 KB back, the pool is laid down in line
behind a branch, and a load still out of reach refuses the block rather
than loading the wrong word. No guest here has a block that long.
`t2exec` checks the load from both halfword alignments with a decoy
word beside the literal, and the patcher's refusals; a mutant that
forgets the alignment loads `0x5678bd00` and fails by name.

RV32 CoreMark x650 at 32 KB, `-DEMU_JIT_BYTE_STATS=ON`, against a
worktree at the commit before -- the table above is 96 KB x120, so
these are not a row of it:

| | bytes / guest insn | LOAD | STORE | translations | cycles / insn | wall |
|---|---|---|---|---|---|---|
| 32 KB, before | 32.12 | 43 | 51 | 239,432 | 109.40 | 81.00 s |
| 32 KB, after | **31.17** | **38** | **47** | **226,794** | **106.01** | **78.48 s** |
| 96 KB, before / after | | | | 296 / 296 | 14.16 / 13.95 | 10.48 / 10.31 s |

The translation count is the layout-free figure, and the wall clock
agrees with the cycle counter. At 96 KB, where CoreMark fits, nothing
is retranslated and the difference is within layout: what that says is
that a literal load is not slower than `MOVW`/`MOVT` on the M7. ARMv7-M
CoreMark x40 at 32 KB, where the window's base is pooled as well:
226.94 → 223.46, 22,779 → 22,357 translations. isatest, mmiobench,
`t2exec`, `itblock`, PowerPC `alu` and `mmu` and the G4MH guest all pass
through it on the board.

### What a block shares with the blocks already there

ARMv7-M's and PowerPC's CoreMark do not fit the 96 KB buffer, and the
question after the bytes per operation was whether a block entered
part-way through another holds a second copy of its tail. The stats
build answers it too: each block's guest end is the largest pc its
frontend wrote after an instruction, taken before the passes elide the
writes, and as each block enters the cache the framework finds the
largest stretch of its guest code already in a resident block of the
same context (`overlap` in the `-- bytes --` report). On the F746 at
96 KB:

| CoreMark | bytes / guest insn | blocks sharing code with a resident one | of them begun inside one | guest bytes already resident | host bytes, prorated |
|---|---|---|---|---|---|
| ARMv7-M x200 | 31.23 | 1,739 of 2,551 (68%) | 1,455 | 83,306 of 117,008 (71%) | ~888 KB of 1.34 MB (66%) |
| PowerPC x40 | 42.10 | 643 of 1,609 (40%) | 476 | 20,700 of 67,752 (31%) | ~239 KB of 753 KB (32%) |
| PowerPC x200 | 41.95 | 2,022 of 4,659 (43%) | 1,452 | 63,856 of 197,776 (32%) | ~741 KB of 2.19 MB (34%) |

**Two thirds of what ARMv7-M CoreMark translates is a copy of code
already in the buffer**, and a third of PowerPC's -- mostly blocks that
begin inside another and run through its tail. The bytes per operation
are not the lever there: LOAD is 21% of ARMv7-M's at 39 bytes each,
STORE 9% at 48, and no operation is a third. ARMv7-M's blocks are long
(16.5 guest instructions on average, 516 bytes) and entered at many
points -- loop heads, return addresses -- each entry a new block to its
end.

**So a block stops before a known entry** (`emu_jit_entry_known`): the
translator asks, before every instruction after its first, whether a
block begins at that pc in this context, or did until compaction
evicted it, and if so ends with a constant exit to it, which chains.
Not before a pc the thrash detector has given up on, which is
interpreted to its next jump and cannot be chained to. A block
translated before an entry inside it was seen keeps its copy; the next
translation of it stops there. On the F746 at 96 KB, the same stats
builds:

| CoreMark | translations | compactions | host cycles / insn | wall |
|---|---|---|---|---|
| ARMv7-M x200 | 2,596 to **577** | 32 to **2** | 30.78 to **17.62** | 7.82 to 4.46 s |
| PowerPC x40 | 1,659 to **412** | 14 to **2** | 32.37 to **20.94** | 1.79 to 1.16 s |
| PowerPC x200 | 4,780 to **412** | 40 to **2** | 34.40 to **19.10** | 9.51 to 5.29 s |

Both now fit. **But the stop splits a block a hot path ran straight
through**, and where the buffer already holds the working set that is
all it does. Four rules, against a worktree at dfb2362, CoreMark under
the JIT on the F746 at two `.text` offsets, the retired count and CRC
the same in every pair:

| stop before a known entry | RV32 (64 KB) | ARMv7-M | PowerPC | G4MH (64 KB) |
|---|---|---|---|---|
| always | +4.3% | **-42%** | **-35%** | +3.3% |
| only translating a pc evicted before | -0.4% | -17% | -34% | +13.7% |
| ...or anything once a pc was given up on | -0.4% | -18% | -34% | +13.7% |
| ...and never before a pc given up on | -0.4% | -38% | -34% | +13.7% |

RV32's and G4MH's CoreMarks fit their buffers, and the gate cost G4MH
35% more block entries. **The reason was the dispatcher's linking**, not
the stop: it linked only the exits of the block it had called, and when
that block chained into others the exit taken was a later one's, which
then went through the dispatcher on every pass -- the gate's split put a
G4MH loop in three blocks, each dispatched 92,000 times. Searching the
chain for the exit after every return found them, and doubled RV32's
and ARMv7-M's CoreMark times (about 550 cycles a dispatch); searching
only after a return through an unlinked exit, which the block now says
(`EMU_JIT_RET_UNLINKED`), made every frontend faster, the stop rules
unchanged, at both offsets and with the retired count and CRC the same:

| CoreMark under the JIT, F746 | before | the linking fixed | and the stop in all four |
|---|---|---|---|
| RV32 x120 | 13.90 | 12.79 (**-8.0%**) | 13.30 (-4.3%) |
| ARMv7-M x200 | 17.53 | 16.49 (**-5.9%**) | 16.49 |
| PowerPC x40 | 20.72 | 19.88 (**-4.1%**) | 19.88 |
| G4MH (CC-RH) | 21.91 | 21.13 (**-3.6%**) | 21.92 (+0.1%) |

Host cycles per guest instruction; block entries fell 5-12%. With the
linking right, the stop in RV32 and G4MH still costs about 4% on these
CoreMarks, which fit their 64 KB: G4MH's translations halved and its
entries stayed level, so what is lost is inside the split -- a chained
hop, and the register traffic the passes can no longer remove across
it. **Where the buffer does not hold the working set the stop is what
counts**, and the linking fix alone is worse than none -- wall clock on
the F746, the retired count and CRC the same across each row:

| at a smaller buffer | before | the linking fixed | and the stop in all four |
|---|---|---|---|
| RV32 CoreMark x650, 32 KB | 41.48 s | 46.66 s (+12.5%) | **33.70 s (-18.8%)** |
| G4MH CoreMark, 32 KB | 6.44 s | 6.81 s (+5.7%) | **5.87 s (-8.9%)** |
| RV32 Whetstone, 32 KB | 3.80 s | 4.47 s (+18%) | **2.08 s (-45%)** |
| RV32 Whetstone, 16 KB | 8.11 s | 8.15 s | 7.93 s (-2.2%) |
| RV32 Whetstone, 8 KB | 16.92 s | 12.88 s | 13.24 s (-22%) |

**So all four translators stop, always**: the one rule never slower
than no stop on any row measured, at the price of RV32's and G4MH's
4% on a CoreMark that fits. Why the linking alone costs RV32 and G4MH
under pressure -- more links (894 against 548 in Whetstone at 32 KB),
twice the evictions, twice the pcs given up on -- is open
([TODO.md](../TODO.md)); with the stop, compactions fall instead
(1,335 to 606 on RV32 CoreMark x650, 253 to 178 on G4MH's).

The scan adds a field to each block entry, which the 72-byte assertion
allows in a stats build only: the build measures bytes, not time.

## The guest-RAM window

A frontend may offer a window (`emu_ir_fastmem_t`) that a `LOAD` or
`STORE` can reach without calling a helper: one unsigned compare against
the window and an alignment test, then a plain access, with the checked
accessor as the slow path. **The frontend refuses whenever an access
could mean more than a move** — RV32 offers it only with PMP, paging and
Sdtrig all idle, because the inlined path writes guest RAM without
asking anyone.

That is the rule the hand-written backend learned on hardware and it
still holds: anything `rv_hart_load`/`rv_hart_store` does beyond the
access itself — a reservation, a permission, a translation — has to be
answered by whoever inlines the access. The peripheral window is the
second one (`emu_ir_ptwin_t`), offered only once a guest has made enough
passthrough accesses to pay for the bytes; see "Where the bytes go"
above.

**The image window is the third (`emu_ir_rom_window`), and here it is
not in line.** A load that misses the RAM window ends in the shared
memory stub, as before, and the block marks it `T2_MEM_ROM_OK` when it
was lowered with the window. The stub then tries the image first --
inside it and aligned, a move from the image's address in the board's
flash -- and calls the accessor otherwise. The mark is per access
because the window is only sound where the RAM window is: a block lowered
under PMP, paging or an MPU has its image reads checked as before.

It was in line first, as on x86-64 ([x86_64.md](x86_64.md#the-image-window)),
and was measured both ways. Host cycles per guest instruction, against a
worktree at the commit before, guest output identical in every row:

| | no window | in line | in the stub |
|---|---|---|---|
| F746 RV32 Dhrystone | 27.84 | 14.54 | **16.41** |
| F746 RV32 Whetstone | 40.41 | 32.94 | **32.96** |
| F746 RV32 CoreMark x120 | 14.66 | 14.51 | **14.56** |
| F746 ARMv7-M Dhrystone | 36.78 | 22.58 | **22.73** |
| F746 ARMv7-M CoreMark | 34.65 | 49.19 | **33.51** |
| F746 ARMv7-M crypto | 30.27 | 27.54 | **26.81** |
| F446 RV32 Dhrystone | 38.97 | 30.15 | **32.95** |

**In line is 9-13% faster on Dhrystone and 42% slower on ARMv7-M
CoreMark**, which reads a constant table in `.text` often enough to arm
and was already near the edge of its 96 KB cache: 26 bytes more on
every load took it from 1,653 translations to 3,615. In the stub a load
is 38 bytes armed or not -- CoreMark armed from the start is 31.38
bytes a guest instruction, as unarmed -- so the window cannot push a
working set out of the cache. The stub was kept for that.

`isatest` passes its 896 checks. CoreMark x650 at 32 KB still validates
(`crcfinal 0x25e3`, 8,485 compactions, 78.7 s by the host against 79.8
by the firmware). ARMv7-M's `t2exec`, `itblock` and `nvic` print what
they printed before.

**And the flush that arms it is worth more than the arming.** With the
window from the start, so that there is nothing to arm, RV32 Dhrystone
runs at 19.19. With one flush forced after start-up and nothing else
changed, 15.96. Arming by use flushes at about that point, and the
steady-state loop is retranslated into an empty buffer, in order; that
is most of the difference between 16.41 and 19.19. It was a question
about layout, not about the window: the start of the buffer was the 9 KB
of it in DTCM, and with the buffer placed first in DTCM the flush no
longer matters (14.33 from the start, 14.41 armed;
[../performance.md](../performance.md#the-jit-buffer-in-dtcm)).

**An access may be predicated** (`emu_ir_mem_if`, the `c` operand), for
ARMv7-M's loads and stores inside an IT block: `CMP` of the predicate
against zero and a `BEQ.W` round everything the access emits -- guard,
inlined path and helper call alike -- placed after the address is in r1
so the reload elision still reaches operand `a`. Six bytes, and the
predicate's load where it has no register -- only where a frontend asks
for it.

## The data TLB probe

Below M-mode the frontend offers a table instead of a window
(`emu_ir_dtlb_t`; [x86_64.md](x86_64.md#the-data-tlb-probe) has the
idea). With the address in r1, and r0, r2 and r12 free until the slow
path:

    tst.w r1, #size-1 ; bne slow
    (store) ldrb r0, [r4, #guard] ; cmp r0, #0 ; bne slow
    lsr.w r2, r1, #12 ; and.w r0, r2, #mask
    add.w r0, r4, r0, lsl #4
    ldr.w r12, [r0, #table + tag] ; cmp r12, r2 ; bne slow
    ldr.w r12, [r0, #table + addend]
    ldr rt, [r12, r1]

The window's rotate-and-compare cannot serve here, because the tag has
to be compared whole; alignment is its own `TST`. On the F746, measured
on sv32bench with `-DEMU_JIT_BYTE_STATS=ON`: a load 40 bytes against
the window's 35, a store 49 against 44. The hart's table sits about
1.5 KB in, inside `LDR.W`'s 4095 -- the probe moves the base into r0
when it does not, so a larger hart costs two instructions rather than
a wrong offset. `add.w r0, r4, r0, lsl #4` is `eb04 1000` by the
assembler, which is what `t2_add_lsl` emits: its shift splits imm3:imm2
like every other here.

What it bought, with the paged kernel's 4.2x gap on the host as the
figure to beat:

| sv32bench, translated | parent | data TLB |
|---|---|---|
| F746, paged | 142.15 cycles/insn, 5.58 s | **9.47**, 0.36 s |
| F746, bare | 8.18 | 8.22 |
| N6, paged | 108.70, 1.56 s | **13.92**, 0.18 s |
| N6, bare | 12.09 | 12.09 |

The seconds are from the host end of the UART. Bare does not use the
table, and 8.18 against 8.22 is inside what moving `.bss` alone does
to the parent (13.88-14.17 on CoreMark, `-DEMU_LAYOUT_PAD_BSS`). Three
mutants of this probe -- no alignment test, no reservation guard, the
store reading the load tag -- each fail isatest on the board by name.

## History: the framework port

Porting onto the shared IR framework cost 4× at first, in four separate
ways, while every suite passed throughout: CoreMark went 79,502 ticks to
321,035 with `isatest` still passing. The causes, in order of size, were
a gate copied from the x86-64 backend that sent everything after the
first PMP entry to the interpreter, overflowed blocks being treated as
declined, a direct-mapped hash, and six indirect calls per dispatch.
Each is written up in [../jit/README.md](../jit/README.md).

Inlining the emitters was **neutral** (LTO already did it). Do not
re-try it.
