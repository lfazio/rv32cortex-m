# RV32: the vector unit

`-DRV32_EXT_ZVE32X=ON` builds it (`src/frontend/rv32/rv_vec.c`): the
whole of **Zve32x**, VLEN 128 (Zvl128b), ELEN 32, named in the ISA
string as `zve32x_zvl128b` when on. It has no `misa` bit -- that is V's.
**Off by default**: 512 bytes of registers a hart, and its code in every
board's firmware, where the N6 has a few hundred bytes to spare.

## The target, and the order

V on RV32 needs ELEN 64 and floating point. The embedded subsets are the
way there, each a whole extension a guest can be told about:

1. **Zve32x** -- integer, ELEN 32, VLEN 128 (Zvl128b). *Done.*
2. **Zve32f** -- single-precision FP on top, through the scalar unit's
   SoftFloat as F's are (`rv_hart_fp`).
3. **Zve64x, Zve64f, Zve64d** -- 64-bit elements.
4. **V**, and the `misa` bit.

VLEN is 128 on every platform: 512 bytes of registers a hart, which the
boards can afford, and what Spike calls `zvl128b`.

## What is in

| | |
|---|---|
| state | 32 registers of 16 bytes, adjacent; `vl`, `vtype` (top bit vill), `vstart`, `vxrm`, `vxsat`; `mstatus.VS`, with SD summarising it beside FS, VS Initial at reset as FS is |
| CSRs | `vstart`, `vxsat`, `vxrm`, `vcsr`, `vl`, `vtype`, `vlenb`: every one refused while VS is Off |
| configuration | `vsetvli`, `vsetivli`, `vsetvl`: the three AVL rules; vill with vl 0 for SEW past ELEN, LMUL 1/16, a fractional LMUL too small for SEW (mf8 holds nothing at ELEN 32) and reserved bits; and for rs1 and rd both x0 with a new VLMAX, which the specification reserves and Spike sets vill for |
| loads and stores | unit-stride, strided, indexed (ordered and unordered), mask (`vlm`, `vsm`), whole-register, fault-only-first, and the segment form of each but mask and whole-register; EEW 8, 16, 32; masked |
| integer | `vadd`, `vsub`, `vrsub`, `vand`, `vor`, `vxor`, `vsll`, `vsrl`, `vsra`, `vminu`, `vmin`, `vmaxu`, `vmax`; the eight compares into masks; `vmerge`, `vmv.v.*`; `vadc`, `vmadc`, `vsbc`, `vmsbc`; `vmul`, `vmulh`, `vmulhu`, `vmulhsu`, `vdiv`, `vdivu`, `vrem`, `vremu` (the scalar rules for zero and overflow); `vmacc`, `vnmsac`, `vmadd`, `vnmsub`; `vzext` and `vsext` (vf2, vf4; vf8 has no source width at ELEN 32) |
| widening and narrowing | `vwaddu` ... `vwsub.w`, `vwmulu`, `vwmulsu`, `vwmul`, `vwmaccu`, `vwmacc`, `vwmaccus`, `vwmaccsu`; `vnsrl`, `vnsra` -- SEW 8 and 16, where 2 * SEW fits ELEN |
| fixed point | `vsaddu`, `vsadd`, `vssubu`, `vssub`, `vaaddu`, `vaadd`, `vasubu`, `vasub`, `vsmul`, `vssrl`, `vssra`, `vnclipu`, `vnclip`: the four `vxrm` modes, `vxsat` sticky |
| permutation | `vslideup`, `vslidedown`, `vslide1up`, `vslide1down`, `vrgather`, `vrgatherei16`, `vcompress`, `vmv1r` / `2r` / `4r` / `8r` |
| masks and moves | the eight mask-logical operations, `vcpop`, `vfirst`, `vmsbf`, `vmsif`, `vmsof`, `viota`, `vid`, `vmv.x.s`, `vmv.s.x` |
| reductions | the eight single-width integer ones, `vwredsumu`, `vwredsum` |

Elements run from `vstart` to `vl`; tail and masked-off elements are left
undisturbed, which the agnostic policies also permit.

**Where the specification lets an implementation choose, this one
chooses as Spike does**, which is what the probes compare against:

- **arithmetic from a nonzero `vstart` is refused** (illegal
  instruction), `vmv.x.s` and `vcpop` included. Only an access can be
  stopped part way, so only an access resumes, from the element or the
  segment that faulted;
- **every vector instruction that runs marks VS Dirty** -- a store and
  a scalar result too -- because each writes `vstart`;
- **a segment stopped by a fault keeps the fields before the one that
  faulted**, in that segment, and fault-only-first shortens `vl` at a
  fault in any field past segment 0;
- the register-group overlaps the specification reserves are refused
  as Spike's `require_noover` and `is_overlapped_widen` refuse them:
  a widening destination may hold a narrower source only as its
  highest-numbered part, of a whole register or more, and a narrowing
  destination may be the lowest register of its wider source.

## The two backends

**One implementation, `rv_hart_vec`, for both.** The interpreter calls
it for OP-V and for the vector widths (0, 5, 6, 7) of LOAD-FP and
STORE-FP, before anything asks about FS -- the vector unit is gated by
VS. The translator emits a call to it for every vector instruction
(`rv_ir_vec_helper`, through EMU_IR_HELPER_TRAP), ahead of the FP
group's FS gate. It used to decline OP-V, which ended the block at each
vector instruction: a vector loop was a block per instruction, and
vecbench interpreted 67,390 instructions and entered 54,386 blocks under
the JIT. With the helper it interprets 6 and enters 4,307, and runs in
9.3% fewer host instructions (248.5M to 225.4M, callgrind). Nothing a
block is specialised on is vector state: vtype, vl and vstart are read
by the helper at run time.

**Accesses that only touch plain RAM are copied** (`vmem_direct`):
unit-stride, strided, segment, mask and whole-register accesses whose
bytes all lie in one RAM region, with no PMP, no translation and no
trigger armed -- where nothing can refuse an element, so the semantics
left are which bytes move. One `memcpy` where the elements are
contiguous and all active, an element at a time where not; indexed
accesses keep the per-element path, whose extent would need a pass over
the indices. Anything else -- PMP on, a range past RAM's end -- goes an
element at a time through `rv_hart_load` and `rv_hart_store`, which say
what refused and where. A store breaks an LR reservation on any word of
its range. **Elements are one host load each** on a little-endian host,
which every host here is, where they were a byte loop. Together, on
vecbench: interpreted 278.5M host instructions to 122.9M, under the JIT
225.4M to 69.7M -- 3.6x fewer than the JIT that declined.

**The arithmetic is lowered with the host's SIMD** where the block
knows vtype: after a `vsetvli` or `vsetivli` it lowered itself, which
puts vl, vtype and a zero vstart in the hart and vl in rd as IR
operations (`rv_ir_vset`; vl = AVL less `(AVL - VLMAX) & -(AVL >
VLMAX)`, RISC-V having no select). From there to the block's end
nothing can change vtype -- it has no CSR write, a CSR write ends the
block, and the helper's `vsetvl` makes the block forget it -- and
vstart stays 0. Unmasked `vadd`, `vsub`, `vrsub`, `vand`, `vor`,
`vxor`, `vminu`, `vmin`, `vmaxu`, `vmax`, `vmul`, `vsll`, `vsrl`,
`vsra`, `vmv.v`, `vmacc`, `vnmsac`, `vmadd`, `vnmsub`, `vaaddu` and
`vaadd` while vxrm is rnu, and the widening `vwmaccu`, `vwmacc`,
`vwmaccsu`, `vwmaccus` -- at LMUL 1 to 4, vd's group disjoint from the
sources -- and the narrowing `vnsrl`, `vnsra`, and `vnclipu` and
`vnclip` under rnu, by one count (.wx, .wi), vd disjoint from vs2's
group or at its start, become `EMU_IR_VOP` when the backend says it
lowers that form (`emu_ir_can_lower`), and VS is marked Dirty at the
block's first; the rest stays the helper. vxrm is in the block's
context (bits 42:41), as frm is: the averages' rounding is resolved at
translation, and a vxrm write is a CSR write, which ends the block. VS off is in the block's context
(`rv_jit_ctx_key` bit 40), as FS off is, because the inline vsetvli has
no test of its own. The x86-64 lowering is SSE2 and SSE4.1
([backend/x86_64.md](../backend/x86_64.md#the-guests-vector-registers-sse2-and-sse41));
Thumb-2's is the DSP extension's SIMD within a register
([backend/thumb2.md](../backend/thumb2.md#the-guests-vector-registers-the-dsp-extension)).

**Masked, the single-width forms are inline too** (`EMU_IR_VOPM`): each
element's bit in v0, read from the register as the operation runs,
keeps vd's old element where it is clear -- mask undisturbed, which
`ma` permits -- and `vmerge.vvm`, `.vxm` and `.vim` are its merge, the
source where the bit is set and vs2 where not. A masked operation
whose vd is v0 is reserved, and declined; the masked widening and
narrowing forms stay the helper. vecbench has no masked operation, and
on the F746 its figure moved only with the layout: 27.85 with the same
translated code, 28.02 and 27.67 with the code moved 1 KB and 2.8 KB.

**Reductions and strided accesses are inline too.** The eight
single-width reductions and the two widening sums, masked or not, become
`EMU_IR_VRED` wherever the block knows vtype: vd's element 0 from vs1's
and every active element of vs2's group, vd any register -- v0 under a
mask included, which a reduction may write -- and nothing written when
vl is 0. A widening sum from e32 (its 64 bits are past ELEN) and a
misaligned vs2 stay the helper's illegal instruction. Unmasked strided
loads and stores of one field become `EMU_IR_VMEMS`: inline when the
stride is under 2^24 either way and the first and last elements lie in
the RAM window -- every element between lies on the line between them
-- and the helper otherwise, which rebuilds the instruction from the
op's imm (vd, rs1, rs2), so a fault is rv_hart_vec's. vecbench under the
JIT on the host: 12.9M to 11.9M host instructions, the vector helper
1.26M to 0.14M of them; on the F746 27.85 to 25.44, the checksum the
host's.

**On the F746**, vecbench, host cycles a guest instruction, the checksum
the host's in every row:

| | interpreted | JIT |
|---|---|---|
| 03044e9: vector instructions declined, a byte loop an element | 424.22 | 441.55 |
| 0b2d2d1: the helper under the JIT, plain-RAM accesses copied | 226.46 | 141.01 |
| the arithmetic lowered with the DSP extension | | 134.22 |
| unit-stride loads and stores inline (`EMU_IR_VMEM`) | | 74.29 |
| the averaging adds under rnu, the multiply-adds | | 48.21 |
| the widening multiply-adds | | 37.68 |
| the narrowing shifts and clip | | 27.54 |
| masked operations (vecbench has none); 27.67 to 28.02 at three layouts | | 27.85 |
| reductions and strided accesses (`EMU_IR_VRED`, `EMU_IR_VMEMS`); the same at two layouts | | 25.44 |

**Unit-stride loads and stores are inline too** (`rv_ir_vmem`): an
unmasked access of one field, in a block that knows vtype and has the
RAM window, becomes `EMU_IR_VMEM`. Where vl's bytes fill the group --
VLMAX elements of EEW, 16 to 128 bytes -- and the whole group lies in
the window, the backend copies it between RAM and the registers; where
not -- the short last trip of a strip-mined loop, an address outside,
a store while an LR reservation is outstanding -- it calls the vector
helper, which faults as the interpreter does. A fractional EMUL and a
build that traps misaligned elements keep the helper. vecbench under
the JIT on the host: 59.0M host instructions to 39.6M.

vecbench under the JIT, one binary, callgrind: the helper alone
(`EMU_HOST_SIMD=0`) 67.6M host instructions, SSE2 64.6M, SSE4.1 59.0M,
the same checksum each; 39.6M with the accesses inline (below), 22.2M
with the averages and the multiply-adds, 17.7M with the widening
multiply-adds, 12.9M with the narrowing clip. A clip that saturates an
active element sets vxsat through the target's saturation word
(`emu_ir_target_t::vsat_offset`). What was left by a profile on the
F746 (`-DVECBENCH_ROUNDS=800 -DEMU_PC_PROFILE=8000`) after the widening
step was the clip, first; now it is the reductions and the strided
load ([TODO](../TODO.md)).

**Whole-register accesses are inline too** -- `vl<n>re<eew>.v` and
`vs<n>r.v`, which a compiler spills and reloads through. They move a
fixed group whatever vtype and vl are, so they need no vset in the
block, but they do need vstart 0, which a block cannot assume at its
start: it can begin at an access resumed after a fault. So the block
tracks it (`g_vstart0`): known after an inline vset or any vector helper
that returned. `vec-ramend.S`'s `s8` resumes a whole-register load at
vstart 2, and an inline copy that took vstart for 0 fails it.

**On code a compiler wrote** (`coremark-v`: CoreMark with
`-march=..._zve32x`, 56 vector instructions -- 16-bit unit-stride loads
and stores, whole-register spills, `vmv.v.x`, `vadd`, `vmul.vx`, `vid`)
the CRCs are the scalar build's, 0x72be at 2 iterations and 0xd340 at
120, on both backends on the host and on the F746, and
`guest-coremark-v-both` holds the two backends to one output. It is not
faster: on the F746 at 120 iterations, 0.25% more cycles interpreted and
4.3% under the JIT, and 2.9% more host instructions under the JIT on the
host -- with the helper all but idle (4,654 host instructions). Its
vectors are a register or two a trip, and an inline vector operation
with its vsetvli and tail masks costs about what the scalar instructions
it replaced did.

**Built only where something emits them**: the IR's vector operations,
their reference and both lowerings are under `EMU_IR_VECTOR`, which CMake
sets on every host build and on a board only with the vector unit.
Without it the N6's image was 384 bytes past its ROM.

`fuzz-rv32-jit` built with the unit (`scripts/fuzz.sh SECS rv32-vec`)
compares the vector registers and CSRs; its first four minutes found an
OPFVV instruction lowered as `vadd.vx` (above, in
[fuzzing.md](../fuzzing.md#what-it-found)), and ten more minutes, 242,141
programs compared, nothing. Its seeds find three mutants in seconds:
the SSE tail mask a byte late, vl left unclamped to VLMAX, a `.vi`
immediate taken unsigned (`scripts/fuzz-mutant.sh`).

## How it is checked

No arch-test suite at the current pin has a vector directory, so four
probes, each against Spike (`zve32x_zvl128b`), interpreted and under the
JIT. Each folds every result into a scalar register by a whole-register
store and scalar rotate-xor, so the check does not lean on what it
checks:

- `vec-int.S`: configuration, the accesses but segments, the
  single-width ALU, multiply, divide, masks, reductions, and a set of
  illegal encodings;
- `vec-int2.S`: carries, saturation and `vxsat`, averaging, `vsmul`,
  the scaling shifts and narrowing clips under each `vxrm`, widening,
  extension, slides (offsets past `vl` and of 2^32 - 1, in place,
  masked), gathers (indices past VLMAX), `vrgatherei16`, `vcompress`,
  whole-register moves at vl 0, the widening reductions, segment loads
  and stores of every kind, a load from a nonzero `vstart`, and 22
  illegal cases -- the reserved overlaps, `vadc` with vm 1, arithmetic
  from vstart 1, `vlm` with nf 1;
- `vec-fault.S`: a locked PMP entry makes a 16-byte hole, and loads and
  stores whose elements fall in it -- unit-stride, strided, indexed,
  masked with the first faulting element off, segments faulting at a
  first and at a second field -- report the cause, the address and the
  `vstart`, and restart from it with the base moved; fault-only-first
  shortens `vl` past element 0 and traps at it;
- `vec-ramend.S`: accesses at the end of RAM with no PMP, where the
  direct path is open: one that ends exactly at the end copies, one
  that runs past it faults at the first element outside, a store having
  written the ones before, a segment its first field.

ctest registers them only where the option is on, and the matrix's
`host-zve32x` row is where it is on.

The first runs found two defects of this unit's own: `vsetvl` kept vl
where Spike sets vill, for the case the specification reserves, and
`vzext`/`vsext` read their factor one place off, so vf2 copied its
source. **17 mutants of `rv_vec.c` are each found** by the probes
(`scripts/probe-mutant.sh`), in the register for their group: the first
stage's four (an arithmetic shift made logical, a signed minimum
reduction made unsigned, a masked load ignoring its mask, `vmerge`
choosing the other way), and this stage's thirteen -- `rne` and `rod`
rounding broken, the carry-in dropped, a widening multiply's signs
swapped, segment fields interleaved wrongly, fault-only-first keeping
`vl`, a fault leaving `vstart` 0, a gather bounded by vl instead of
VLMAX, `vcompress` writing in place, `vxsat` never set by `vnclip`,
`vslideup` writing below its offset, and the widening and narrowing
overlap exceptions each refused. Two survived on the first try: `vslideup` writing below
its offset, because every element it zeroed was already zero in the
probe's data -- each `vslideup` destination now starts at 0xF9 -- and an
overlap mutant that turned out equivalent, replaced by one that is not.
Five more hold the direct path: ignoring `vstart`, ignoring the mask,
a strided access taken as unit-stride, a segment's field from the
wrong register, the region check 8 bytes loose -- each found. Refusing
an access that ends exactly at the region's end is equivalent: the
per-element path gives the same answer, slower.

`guest-vecbench-both` runs `tests/guest/vecbench.c` -- integer vector
kernels written with the RVV intrinsics: add, multiply-add, a widening
dot product, a byte copy, rounding averages, a narrowing clip, a
max reduction, a strided gather -- on both backends and requires the
same output and count.
