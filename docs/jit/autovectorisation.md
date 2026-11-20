# Autovectorising the JIT: a plan

**Status, 2026-11-19: P0 (the census) and P1 (recognition) done; P2 and
P3 built -- four kernels -- and measured: strcpy's copy to the NUL kept,
Dhrystone 19-20% faster for RV32 on the F746 and 19% on the N6, 15% for
ARMv7-M; the dot product on since 2026-11-20, emitted in the block on
Thumb-2 (RV32 CoreMark 0.89% faster); the copy and the fill not paying
on the F746, off by default.** Every phase ends
in a measurement that decides whether the next one starts, because this
is the kind of feature that reads as obviously worth it and then is not
(the loop register cache, [../backend/thumb2.md](../backend/thumb2.md#a-loops-guest-registers-in-host-ones-measured-not-kept),
and three earlier register caches before it).

## What it means here

A guest's scalar loop -- RV32 `lh; lh; mul; add; bne`, ARMv7-M's
`ldrsh; ldrh; smlabb; bne` -- run so that one host SIMD instruction
does the work of several iterations. Not the guest's own vector
instructions, which RV32's V already lowers to SIMD
([../frontend/rv32-vector.md](../frontend/rv32-vector.md)): the host
SIMD is there, the IR has vector operations, and the guests that would
gain most -- every CoreMark, DSP code built for a plain Cortex-M -- do
not use either.

What the hosts have:

| host | SIMD | what fits a guest's integer loop |
|---|---|---|
| F746 (M7) | the DSP extension, SIMD within a 32-bit register | `SMLAD`: two 16-bit products added to a 32-bit sum in one instruction; `UADD16`/`USUB16`, `SADD8` -- two or four lanes |
| N6 (M55) | Helium, 128-bit Q0-Q3 in a block | `VMLADAVA.S16`: eight 16-bit products into a sum; loads that widen (`VLDRH.S32`) and gather (`VLDRH` with a vector of offsets) |
| x86-64 | SSE2 at least | `PMADDWD`: eight 16-bit products in pairs; `PADDW` |

## What the data says, before anything is built

From the board dumps of 2026-11-18
([../backend/thumb2.md](../backend/thumb2.md#what-the-dumps-found-and-what-came-of-it)),
CoreMark under the JIT on the F746:

- Blocks that loop on themselves hold 22% of RV32's time in translated
  code, 33% of ARMv7-M's, 14% of PowerPC's and none of G4MH's -- whose
  CC-RH loops are not single blocks.
- RV32's are almost all CoreMark's matrix kernels, 12% of the run:

  | | of the run | the loop |
  |---|---|---|
  | `matrix_mul_matrix_bitextract` | 5.5% | `lh; lh` (one a column, stride `sh1add`), `mul`, shifts and masks, `mul`, `add` |
  | `matrix_mul_matrix` | 4.2% | a 16-bit dot product, one operand a column |
  | `matrix_mul_vect`, `matrix_test` | 2.1% | a unit-stride dot product; `lhu; sub; sh` -- a map |

  Two shapes, a reduction of products and an element-wise map, at 16
  bits: exactly what `SMLAD` and `VMLADAVA` are.
- **How many iterations an entry is the open question.** The loop
  cache lost on PowerPC because its loops ran few iterations an entry;
  a vectorised loop pays its checks once an entry and wins only past a
  handful of iterations. CoreMark's matrices are N x N with N about 10
  at the default size: ten iterations an entry, two `SMLAD` pairs and
  a remainder.

### The census (P0)

`scripts/loop-census.py` runs guests on a host runner built with
`-DEMU_JIT_LOOP_CENSUS=ON`: the x86-64 lowering counts, for every
block that branches back to its own start, its entries, the back edges
it takes and the loop cap's exits, and the script puts the counts
beside the guest's code. Shares are of the run's retired guest
instructions -- not of its time, which the board's profile gives -- and
a trip is iterations an entry from outside the loop:

| guest | in self-loops | the loops worth a kernel | trip |
|---|---|---|---|
| RV32 CoreMark x120 | 32.1% | `matrix_mul_matrix_bitextract` 14.0%, `matrix_mul_matrix` 8.1% | 9 |
| ARMv7-M CoreMark x200 | 24.0% | the same two, 9.4% and 6.3% | 9 |
| PowerPC CoreMark x200 | 23.4% | the same two, 8.6% and 5.8% | 9 |
| RV32 Dhrystone | 33.0% | `strcpy`'s byte loop, 32.9% | 31 |
| ARMv7-M Dhrystone | 26.7% | `strcpy`'s, 26.5% | 30 |
| RV32 crypto | 26.3% | `memcpy`'s byte loop 11.7%, `memset`'s 6.1% | 15, 69 |
| ARMv7-M crypto | 24.3% | `memcpy` 11.6%, `memset` 7.2% | 16, 69 |
| PowerPC crypto | 21.6% | `memcpy`, unrolled by two, 11.1% | 7 |
| PowerPC Whetstone | 4.7% | none: a loop of register moves | -- |

PowerPC Dhrystone has none (its C library copies by word), Whetstone
on RV32 1.9%, and `tc_aes_encrypt`'s loops look tables up by a loaded
byte, a gather no host here does cheaply. **The gate is met**: three
kernels, each over 10% of some benchmark, at trips of 9, 15 and 30.
Nine is the thin one -- four `SMLAD` pairs and the last iteration
scalar -- so the dot product is where P2 must show its checks are paid
for.

So the kernels, in the order P2 and P3 take them:

1. **A 16-bit dot product**, one operand at a stride (a matrix's
   column): `matrix_mul_matrix`, and with the shifts and masks applied
   to each product, `_bitextract` -- a map of each product, then the
   sum.
2. **A byte copy to a NUL** (`strcpy`): a word at a time while no byte
   of it is zero, the classic `(w - 0x01010101) & ~w & 0x80808080`; the
   word with the NUL left to the scalar body, which stores up to it and
   no further.
3. **A byte copy and a byte fill of a known length** (`memcpy`,
   `memset`), a word at a time where both ends allow it.

## The design

### Kernels, not a general vectoriser -- first

Two ways to say a vectorised loop in this IR:

1. **General**: rewrite the loop's IR into the existing vector
   operations (`EMU_IR_VMEM`, `EMU_IR_VOP`, `EMU_IR_VRED`) on a scratch
   vector file, with a loop inside the block, a preheader and a
   remainder. Needs a loop construct the IR does not have (its only
   back edge is a block's exit to its own start), scratch vector state
   for frontends that have no vector file, and every backend's vector
   lowering made to work on it.
2. **Kernels**: recognise a small set of loop shapes and replace the
   loop's work with one operation, `EMU_IR_VLOOP`, whose `aux` names the
   kernel (a 16-bit dot product, a 16-bit map by a constant, a copy, a
   fill) and whose operands are the pointers, strides, trip count and
   scalar. Each backend emits the kernel as one tight loop, as it
   already emits `EMU_IR_VMEM`'s copy (`emit_vmem_loop`); the IR
   interpreter's C is its meaning.

**Kernels first**: the census above is two shapes; a kernel is a
backend's loop the size of `emit_vmem_loop`, its reference twenty lines
of C, and fuzz-ir can build one directly. The general form waits for a
census that shows shapes no handful of kernels covers.

### The block a kernel goes in

The scalar block stays exactly as it is: it is the remainder, the
fallback and the meaning. Ahead of its body (where the loop cache put
its loads, before `g_body`), the vectorised block gets:

1. **The checks**, each a branch to the scalar body when it fails:
   - the trip count, from the exit compare and the induction step
     (`bne t1, a5` with `a5 += 2`: `n = (t1 - a5) / 2`), positive and
     exact -- an inexact division means the scalar loop does not stop
     where the formula says;
   - every byte the kernel reads or writes in the block's RAM window,
     and for a write the store guard clear -- the same conditions the
     inlined accesses check one at a time, so no access can fault or
     reach a device;
   - for a write, no overlap with a source at a distance shorter than
     the vector (the scalar loop would read what it wrote);
   - and, where the frontend watches stores into code (ARMv7-M's
     `smc_mark`), no translated code in the written range.
2. **The kernel** over `min(n - 1, cap)` iterations: the last iteration
   is always left to the scalar body, so every register the loop
   writes -- the loaded values, the products, the flags on ARMv7-M,
   G4MH and PowerPC -- ends exactly as the scalar loop leaves it,
   without the kernel computing any of it. `cap` bounds the latency an
   interrupt sees, as `EMU_JIT_LOOP_CAP` does a back edge.
3. **The fix-up**: induction registers and pointers advanced by the
   iterations done, the accumulator written, and the retired count
   charged what the interpreter would have charged -- iterations times
   the body's instructions (`emu_jit_hot_t::charged`).

Then the scalar body runs the rest, from the state the fix-up left.

### Recognising a loop

A pass over a block that exits to its own start, after the existing
passes and before lowering, and in the shared IR so every frontend's
loops reach it:

- **induction registers**: a guest register `r` whose only write is
  `PUT r = ADDI(GET r, k)` -- after `pass_reg_traffic`, the GET is the
  block's first read of `r`;
- **affine accesses**: a `LOAD` or `STORE` whose address is an
  induction register plus a constant;
- **the exit**: an `EXIT_IF` to the start on a compare of an induction
  register with a value the loop does not write;
- **the work**: what is left must match a kernel's shape exactly --
  the products, the sum, the map's operation, its width and signedness
  (`lh` sign-extends, `lhu` does not; CoreMark's `mul` wraps at 32 bits,
  which `SMLAD` and `PMADDWD` also do, past sixteen-bit products).
  Anything else -- a helper, a second store, an access not affine, a
  register written twice -- and the block is left alone.

The pass reports what it saw (`-- ir --`: loops seen, matched by kernel,
refused by reason), so that a kernel nothing reaches shows as a zero,
not as a speed-up nobody can find.

## Phases

Each phase ends in a measurement on the board, against a worktree at
the commit before.

**P0 -- census. Done** (above). A host runner option that counts, for
every block that loops on itself, its entries and iterations, and a
script that ranks the loops by share with the guest's code beside them.
Still to count: G4MH, whose CoreMark is a CC-RH binary the script does
not load yet, DOOM and a Linux boot. **Gate**: kernels reaching at least
10% of some benchmark's run, at a median of at least 8 iterations an
entry -- met by three.

**P1 -- recognition only. Done**: `emu_ir_loop_shape`, run at the end
of `emu_ir_optimise` (`-DEMU_IR_LOOP_SHAPE`; off on the boards until P3
gave it something to do there, on everywhere since), changing
nothing by itself. Every temp of the body gets a kind -- invariant, a carried
register plus a constant or plus an invariant, this iteration's, or
carried other than affinely -- every register the body writes is
per-iteration, an induction, a sum or a sticky bit ORed in, and a loop
is one of five kernels or refused for one of seven reasons, counted in
`loop_shape[]`. The census labels each loop with it:

| | RV32 | ARMv7-M | PowerPC |
|---|---|---|---|
| `matrix_mul_matrix` | dot16 | dot16 | dot16 |
| `matrix_mul_matrix_bitextract` | dot16-f | dot16-f | dot16-f |
| `strcpy` (Dhrystone) | copy-nul | copy-nul | -- |
| `memcpy`, `memset` (crypto) | copy, fill | copy, fill | affine: unrolled by 2 and 4 |

ARMv7-M's needed two things RV32's did not: an element is a halfword
sign-extended by the multiply (`smlabb` reads the bottom half of what
`ldrh` loaded), and a sticky Q ORed in from the sum's overflow every
iteration. PowerPC's is a halfword byte-swapped then sign-extended, and
its bit extraction `rlwinm`, a rotate and a mask. Unit tests cover each
kernel and each refusal (a store, an address loaded, an exit on a sum, a
register multiplied by itself, a helper, a side exit, an unsigned
element, a copy to NUL on EQ), two mutants of the rules fail them, and
fuzz-ir runs the analysis on its 21,799 loops in a minute under the
sanitizers -- which found a negation of INT_MIN in it, the offsets
unsigned now. None of fuzz-ir's random loops has a kernel's shape:
P2's generator has to build them.

**P2 -- the 16-bit dot product. On, emitted in the block on Thumb-2:
RV32 CoreMark on the F746 0.89% faster.** First not as planned -- no
backend emitted the kernel. `EMU_IR_KERNEL` is a call,
ahead of the block's body so a back edge does not repeat it, to one C
function, `emu_ir_kernel_run` (src/emu/emu_ir_kernel.c), which reads the
descriptor the pass made (`emu_ir_kernel_add`: registers resolved to
offsets, the block's RAM window, a small table shared by equal loops),
checks, runs, writes the registers back and returns the instructions it
retired. GCC makes it SMLAD on the M7 and plain C on x86-64; the IR
interpreter calls the same function, so fuzz-ir compares the kernel with
the scalar loop as built. Two rules make that comparison exact:

- **It runs only what the scalar loop would have run this entry**, less
  the last: the iterations before the loop cap's back edge leaves
  (`retired + j * insns >= cap`), which bounds an interrupt's wait as
  before and leaves the block in the very state it would have left.
- **The iteration it leaves must complete**: its elements are checked
  in the window too. fuzz-ir found this one -- an array ending one
  element short of the window: the scalar iteration trapped before
  writing the register each iteration writes, which the kernel had not
  written either (`regressions/ir/kernel-last-iteration-traps`).

On the F746 (RV32 CoreMark x120, 62,270 calls running 325,673
iterations) it was **2.2% slower** -- 9.42 against 9.22 -- and a firmware
whose kernel returned at once measured the same 9.42: the call, about
100 cycles a block entry, cost what six iterations of a 16-bit dot
product save. In flash, before `EMU_HOT_TEXT`, it was 9.56. And the
first board run lost every kernel block to the interpreter (RV32 78%
slower, "declined 2") because the Thumb-2 backend's `bisect_allows`, a
third list of what it lowers, did not name the operation while its
capability list said yes (docs/lessons.md).

That figure was the call's, not the kernel's: the C loop runs an
iteration in about 14 instructions where the translated one takes about
40 (each load through the window check, each register stored and
reloaded), and what it saved went on the call and on reading a 120-byte
descriptor from SRAM each time. So the Thumb-2 backend now emits the
loop itself (`emit_dot_inline`): the descriptor's offsets, window and
constants baked in, the trip and the window checked in a few compares on
the cpu's DTCM words, then LDRSH, LDRSH, MLA (not SMLABB, which would
set Q) and a count-down, and the registers written back in the C
kernel's order. It takes the shape CoreMark has -- x's stride 2, neither
element swapped -- and y's stride within 0x8000, so nothing overflows
32 bits where the C kernel uses 64; anything else the scalar body runs,
which ends in the same state. The pass asks first (`EMU_IR_KERNEL_PLAIN`
in the op's aux), so a dot product the backend would neither emit nor
call -- PowerPC's, byte-swapped -- gets no kernel, no table entry and no
op.
x86-64 still calls the C function, the IR interpreter too, so the
board's replay of fuzz-ir's corpus compares the emitted loop with it.

| F746, against df58293 | before | after | |
|---|---|---|---|
| RV32 CoreMark, pad 0 / 64 | 9.02 / 9.02 | 8.94 / 8.94 | **-0.89%** (325,673 iterations in loops emitted inline) |
| PowerPC CoreMark, pad 0 / 64 | 11.44 / 11.45 | 11.49 / 11.51 | +0.44% / +0.52%, no kernel made: layout |
| ARMv7-M, G4MH CoreMark | 11.27, 14.40 | 11.34, 14.39 | no dot kernel there: layout |

Less than the dot loops' share suggested (matrix_mul_matrix and
matrix_mul_vect, 4.2% of the samples): six iterations an entry still
leave the checks a large part, and the largest of CoreMark's matrix
loops, `matrix_mul_matrix_bitextract` (5.8%), maps its product through
two bit-field extracts -- a DOT16_F, with no kernel at all.

**P3 -- copies and fills. strcpy's kept.** The byte loops get the same
call: copy to the NUL (strcpy), copy and fill counted (memcpy, memset),
a byte at a time in the scalar loop's order -- so a destination over its
source reads what it wrote -- with memcpy and memset where the two cannot
meet; the source may be the image (Dhrystone copies string literals),
and the RAM window's store guard is the inlined stores' (RV32's
reservation, ARMv7-M's `smc_watch`). Measured each on its own, F746:

| | without | with | |
|---|---|---|---|
| RV32 Dhrystone (strcpy) | 13.34 | 10.75 | **-19.4%** (576,193 iterations in 39,737 calls) |
| RV32 crypto (memcpy, memset) | 20.49 | 20.58 | +0.4% |

So `emu_ir_kernel_kinds` gives a kernel to the copy to the NUL alone;
fuzz-ir turns every kind on, so the dot product, the copy and the fill
stay tested. The maps (`matrix_add_const`) were not built: they share
the dot product's trip counts.

As committed -- copy to the NUL on, a table of 16 (`EMU_IR_KERNELS`;
at 64 it and the recogniser's arrays were 10 KB of `.bss`, a third of
the F446's guest RAM), on everywhere but the F446 -- against 330775a:

| | layout | without | with | |
|---|---|---|---|---|
| RV32 Dhrystone, F746 | pad 0 / 64 | 13.34 / 13.30 | 10.70 / 10.77 | **-19.8% / -19.0%** |
| ARMv7-M Dhrystone, F746 | pad 0 / 64 | 18.30 / 18.28 | 15.53 / 15.55 | **-15.1% / -14.9%** (556,047 iterations) |
| RV32 Dhrystone, N6 | | 18.17 | 14.73 | **-18.9%** |
| CoreMark, F746: RV32, ARMv7-M, PowerPC, G4MH | pad 0 | 9.22, 11.29, 11.36, 14.35 | 9.02, 11.28, 11.44, 14.40 | -2.2%, -0.1%, +0.7%, +0.4% |

No CoreMark makes a kernel, so its rows are layout: PowerPC's, the
largest, did not move with the flash pad, nor with four ITCM pads or
four guest-RAM pads on the old tree (11.36-11.40), and moving the old
tree's `.bss` by the 10 KB the first tables added made it 11.67, by
4 KB 11.41. The fuzz-ir corpus replayed on the F746 (16,171 inputs, no
difference), isatest, and CoreMark x2000 at 32 KB (55 s across three
wraps of the cycle counter, its CRC valid, 516 compactions) with the
old tree's translations, compactions and interpreted count exactly.

**P4 -- the other frontends.** ARMv7-M's matrix loops are the same
kernels with the guest's own `smlabb` (one product an instruction, where
the kernel does two); PowerPC's after its flags work
([../TODO.md](../TODO.md)); G4MH's only once a CC-RH loop is a single
block. The last iteration left to the scalar body is what makes their
flags right; a census per frontend first.

**P5 -- general vectorisation**, only if P0's census or a later one
shows loop shapes that no handful of kernels covers.

## How it is checked

The project's own rules, applied to this:

- **A reference that is code**: the kernel's C in `emu_ir_interp` is
  its meaning, and fuzz-ir's comparison of a block as built against the
  block as optimised is the check of the pass -- it already runs loops
  (71,456 looped of 362,727 in three minutes). fuzz-ir builds the loop
  shapes directly, with the parameters where the behaviours differ:
  trip counts of 0, 1, 2 and odd ones; a source and destination
  overlapping at every distance below the vector; arrays ending at the
  window's last byte and one past it; strides of 0, negative ones, and
  2 against 4.
- **The board replays it**: the corpus through Thumb-2's DSP and
  Helium kernels (`ir-board.sh`), and a mutant of each check
  (`ir-board-mutant.sh`) -- the remainder off by one, the overlap test
  dropped, the window's end checked at the start only.
- **A guest that asserts by value**: a C guest of the kernels' loops
  for RV32 and ARMv7-M, odd sizes and aliasing included, its checksum
  the host's, run on both backends and on both boards.
- **The retired count**: each guest's under the JIT equal to the
  interpreter's, which is what shows the fix-up charged the iterations
  the kernel did.
- **The instrument first**: the pass's counters on every board run
  before the figure, since a kernel that never fires changes nothing
  (CLAUDE.md, "Identical counters mean the code never ran").

## What would make it not worth it

- Few iterations an entry (P0 answers this before anything else).
- Bytes: a kernel site with its checks is a hundred bytes or more, and
  on the F746 a working set that stops fitting its buffer costs more
  than any kernel saves (ARMv7-M CoreMark, the image window in line:
  42% slower). The 32 KB rows are part of every gate.
- A shape that matches but means something else: a 16-bit product
  summed into 16 bits, a sign-extension that is not one. The pass's
  refusals are tested as carefully as its matches.
