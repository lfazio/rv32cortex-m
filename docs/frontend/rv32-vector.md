# RV32: the vector unit

`-DRV32_EXT_ZVE32X=ON` builds it (`src/frontend/rv32/rv_vec.c`); **off by
default, and advertised nowhere** -- no ISA string, no `misa` bit -- until
every instruction Zve32x names is in. An instruction not yet here raises
illegal instruction, as one outside the extension would, so a guest that
probes for vectors finds none and one that uses what is here gets the
right answers.

## The target, and the order

V on RV32 needs ELEN 64 and floating point. The embedded subsets are the
way there, each a whole extension a guest can be told about:

1. **Zve32x** -- integer, ELEN 32, VLEN 128 (Zvl128b). *In progress.*
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
| loads and stores | unit-stride, strided, indexed (ordered and unordered), mask (`vlm`, `vsm`), whole-register, fault-only-first; EEW 8, 16, 32; masked; a fault at an element leaves `vstart` there |
| integer | `vadd`, `vsub`, `vrsub`, `vand`, `vor`, `vxor`, `vsll`, `vsrl`, `vsra`, `vminu`, `vmin`, `vmaxu`, `vmax`; the eight compares into masks; `vmerge`, `vmv.v.*`; `vmul`, `vmulh`, `vmulhu`, `vmulhsu`, `vdiv`, `vdivu`, `vrem`, `vremu` (the scalar rules for zero and overflow); `vmacc`, `vnmsac`, `vmadd`, `vnmsub` |
| masks and moves | the eight mask-logical operations, `vcpop`, `vfirst`, `vmsbf`, `vmsif`, `vmsof`, `viota`, `vid`, `vmv.x.s`, `vmv.s.x` |
| reductions | the eight single-width integer ones |

Elements run from `vstart` to `vl`; tail and masked-off elements are left
undisturbed, which the agnostic policies also permit.

## What is not, yet -- all of it Zve32x

Segment loads and stores (`nf` > 0); `vadc`, `vsbc` and the carry masks;
widening and narrowing arithmetic (`vwadd`... `vnsrl`, `vnclip`); integer
extension (`vzext`, `vsext`); fixed point (`vsadd`, `vaadd`, `vsmul`,
`vssrl`, `vnclip`, with `vxrm` and `vxsat`); slides (`vslideup`,
`vslidedown`, `vslide1up`, `vslide1down`); gathers (`vrgather`,
`vrgatherei16`); `vcompress`; whole-register moves (`vmv<n>r.v`); and
widening reductions. Then the extension is named: `zve32x_zvl128b` in the
ISA string, the configure summary, and -- once the arch-test pin has
vector suites, which `d261a799` does not -- the UDB and Sail
configurations.

## The two backends

`rv_hart_fp`, which both backends reach for FP loads and stores, sends a
vector width (0, 5, 6, 7) to `rv_hart_vec` before it asks anything about
FS -- the vector unit is gated by VS. So the JIT's FP helper runs vector
loads and stores; OP-V the translator declines, and the interpreter runs
it. Nothing a block is specialised on depends on vector state.

## How it is checked

No arch-test suite at the current pin has a vector directory, so probes:
`tests/arch-test/probes/vec-int.S` runs every group above, folds each
result register into a scalar by a whole-register store and scalar
rotate-xor -- so the check does not lean on the reductions it checks --
and compares twelve registers with Spike (`zve32x_zvl128b`), interpreted
and under the JIT. ctest registers it only where the option is on, and
the matrix's `host-zve32x` row is where it is on.

**Not probed yet**: an element that faults. Fault-only-first and the
restart from `vstart` are written, and no probe access faults -- the
next probe needs a trap handler that resumes a load whose fifth element
is outside memory.

The first run agreed on eleven of twelve: `vsetvl` kept vl where Spike
sets vill, for the case the specification reserves. Four mutants -- an
arithmetic shift made logical, a signed minimum reduction made unsigned,
a masked load that ignored its mask, `vmerge` choosing the other way --
are each found, in the register for their group.
