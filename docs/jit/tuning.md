# JIT tuning

Each knob below, what it is worth, and — as important — **which workload
can see it**. Several of these were tuned on CoreMark, which is blind to
two of them.

Regenerate the host figures with `scripts/report-figures.sh`, which
prints the CMake cache variables that produced them. Board figures need a
flash cycle.

---

## `EMU_JIT_CODE_BYTES` — the dominant term

CoreMark's translated working set is ~48 KB, and at the 12 KB default the
JIT **loses to the interpreter**:

| code cache | CoreMark ticks | compactions | evictions |
|---|---|---|---|
| 12 KB | 10,850,998 | 8533 | 94,240 |
| 24 KB | 9,329,706 | | |
| 32 KB | 8,525,192 | | |
| 48 KB | 6,463,217 | 904 | |
| 64 KB | 5,148,168 | 231 | |
| *interpreter* | *10,691,637* | | |

Guest RAM pays one for one: 122 KiB with no JIT, 106 at 12 KB, 70 at
48 KB, 54 at 64 KB. On a microcontroller those bytes are the guest's,
which is the whole tension — there is no right answer, only a stated one.

**Check `CMakeCache.txt` before quoting any of these.** Every performance
figure in this repo was once measured with a 48 KB cache inherited from
an old build directory while the declared default was 12 KB; `rm -rf
build/` moved them by 68% with no code change.

## `RV_JIT_LOOP_CAP` — an interrupt-latency knob

And CoreMark cannot see it. Measured at 64/128/256:

| workload | 64 | 128 | 256 |
|---|---|---|---|
| CoreMark | 31.39 | 31.16 | 31.25 |
| `bench` | 18.88 | 18.39 | 18.13 |
| `mmiobench` | 24.40 | 23.46 | 22.99 |

CoreMark is noise: its loops end on unchainable branches, so the cap is
not what exits them. `mmiobench`'s block entries halve exactly per
doubling and its tightest kernels gain 18% at 256.

Each doubling returns half the previous one and doubles worst-case
latency (~11/22/44 µs), so **128 is the knee** and the default. Do not
tune this on CoreMark alone.

## `EMU_JIT_INLINE_PERIPH` — worth 2.2–3.1× to drivers, −53% to compute

So the guest arms it rather than it being always on. The emitted test is
~18 bytes per load and ~48 per store; always-on grew CoreMark's image
past the 48 KB code cache and doubled evictions. `pt_note` counts
passthrough accesses through the helper and flushes once at
`RV_JIT_PT_ARM_AT`.

## `EMU_JIT_LOOP_CHAIN`

Chaining loop back edges. What is left in block entry needs this, not
micro-optimisation: the prologue is `PUSH {r4-r7,lr}` plus a `MOV`, and
the hash lookup is a shift, mask, load and compare — a last-block cache
in front of it measured **1.2% slower**.

Watch the branch range. Chaining was once emitted only when the back edge
fitted the 16-bit conditional branch (±254 bytes), so a larger block
stopped chaining rather than widening the encoding, costing 2.4× on the
loops that crossed the line.

## Framework table sizes

`EMU_JIT_MAX_BLOCKS` and `EMU_JIT_HASH_SIZE` follow the target, keyed on
`EMU_JIT_THUMB2`: 256/256/12 KB there, host figures otherwise. 8192
blocks and an 8192-entry hash cost a host nothing and are 192 KB of
`.bss` on a part with 320 KB.

## Things that did not work

- **A guest-register cache in r8–r10**: 15.5% slower. See
  [../backend/thumb2.md](../backend/thumb2.md).
- **The ARM shifted-operand fusion**: built, correct on hardware
  (296/296), byte-identical translations *because it never fired*, and a
  10% CoreMark regression that remains unexplained. Reverted. The mistake
  was extrapolating from "ARM has a shifted operand" to "there will be
  shifts to fold". `-DRV32_PAIR_STATS=ON` answers that in one run —
  **run the pair stats before writing the encoder, not after.**
- **Interpreter-in-SRAM**: slower.
- **Lazy IRQ**: neutral.
- **Instruction fusion generally.** The textbook RISC-V fusions are not
  present in this guest: `lui`+`addi` is 0.2% of CoreMark pairs and
  `auipc`+`addi` is 0.00%, because the guest is built `-O2` for a small
  target where constants fit the 12-bit immediate and globals go through
  `gp`. Address-generation feeding a memory access is 0.0–2.9%.

  What *is* large is that **29.6% of adjacent pairs are data dependent**,
  of which 5.7% have a dead intermediate. That is the number worth
  attacking, and it is what the reload elision and the x86 memory operand
  take — both without any pattern matching, for 6.1% of emitted code size
  on x86-64 and 771,100 from 963,899 ticks on ARM.

## Guest-side, not JIT-side

**Compressed guest code is slower to interpret, not faster.** Enabling
Zcb in guest codegen cost ~9% on CoreMark at an identical instruction
count: the compiler swapped 32-bit encodings for Zcb ones, each of which
now pays an RVC expansion. Supporting Zcb in the *emulator* is a small
win (38.0 vs 39.2 cyc/insn); it is the *guest* `-march` that costs.
Toggle `RV32_EXT_ZCB` against a fixed guest binary to separate the two.

**`-Os` is 33% smaller and 8.8% slower** on the F446. The ART accelerator
is not the binding constraint, so the code-density argument does not pay.
Use `MinSizeRel` only when flash is actually scarce.
