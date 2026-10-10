# JIT tuning

Each knob below, what it is worth, and — as important — **which workload
can see it**. Several of these were tuned on CoreMark, which is blind to
two of them.

Regenerate the host figures with `scripts/report-figures.sh`, which
prints the CMake cache variables that produced them. Board figures need a
flash cycle, and the ones in this file were taken on a Nucleo-F746ZG
(216 MHz, `RelWithDebInfo`) with **two clocks that agree**: the
firmware's cycle count and a wall clock on the host end of the UART.

That second clock is not decoration. Until `emu_cycles.h` the firmware's
figure was a 32-bit counter less an epoch, which is a whole wrap short —
19.9 seconds — for any run longer than that, and still a plausible
number. A board figure in this repository that predates it and came from
a long run is wrong by an amount nobody recorded.

**And a figure belongs to one binary.** Where the emulator's own hot
loop lands in flash is worth up to 10% on this part, so two builds of
the same configuration differ: the PowerPC CoreMark cell quoted below as
335.1 measured 310.0 on the tree as finally committed, with identical
translation counts. Compare rows taken from one build, or differences
well clear of that.

---

## `EMU_JIT_CODE_BYTES` — the dominant term

**At the 32 KB default the JIT is slower than the interpreter**, for
every guest large enough to matter. Host cycles per guest instruction,
and how many translations the run needed:

| | RV32 CoreMark | RV32 `bench` | PowerPC CoreMark | PowerPC crypto | guest RAM |
|---|---|---|---|---|---|
| interpreter | **108.2** | **98.2** | **222.3** | **243.4** | 309 KiB |
| 32 KB (default) | 169.6 (82,075) | 199.5 (1,873) | 335.1 (46,199) | 716.5 (28,545) | 232 KiB |
| 48 KB | 143.5 (60,171) | | | | 216 KiB |
| 64 KB | 97.7 (29,682) | | 212.1 (27,780) | 373.7 (12,532) | 200 KiB |
| 96 KB | **53.1** (588) | | 125.8 (15,309) | **35.4** (468) | 168 KiB |

RV32 CoreMark is 120 iterations, 30,027,635 instructions, which CoreMark
accepts as a valid run; PowerPC's is 40 iterations, 12,170,464. Guest RAM
is RV32's; PowerPC's is three or four KiB less.

Read the translation counts before the cycles. A guest with a few
hundred blocks that needs 82,075 translations is not being translated,
it is being *re*-translated: the cache is a queue. RV32 CoreMark's
working set is about 83 KB, and the row where it fits is the only one
where the JIT is clearly ahead — 2.04× the interpreter. PowerPC crypto
fits at 96 KB and is 6.9× ahead; PowerPC CoreMark never fits here.

So the knob is not a dial, it is a threshold per guest, and below the
threshold the JIT costs more than it saves. This is the statement this
file once made about a 12 KB cache, when the default was moved to 32 KB.
It has moved up again: the IR backends emit more per guest instruction
than the hand-written translator did — a hundred bytes for a memory
access on x86-64, see [../backend/x86_64.md](../backend/x86_64.md) — and
the default did not follow.

Guest RAM pays one for one, and a JIT build is 77 KiB down at the
default: the 32 KB buffer, and 45 KiB of block table and IR storage that
it pays at any size. On a microcontroller those bytes are the guest's,
which is the whole tension — there is no right answer, only a stated
one. What is *not* defensible is the present default, which pays the
77 KiB and runs slower; that is an open decision in
[../TODO.md](../TODO.md).

**Check `CMakeCache.txt` before quoting any of these.** Every performance
figure in this repo was once measured with a 48 KB cache inherited from
an old build directory while the declared default was 12 KB; `rm -rf
build/` moved them by 68% with no code change.

## Block length — per frontend, and shorter where the cache is small

Interrupts are delivered between blocks, so a cap on guest instructions
per block is a latency bound as well as a size one. Every frontend caps
at 64. PowerPC caps at **16 on a microcontroller**, because while the
cache is thrashing a long block is the expensive one to lose: it is more
work to translate, and it is the one that overruns the buffer's reserve
and is translated twice.

| PowerPC, F746 | cap 64 | cap 32 | cap 16 | cap 8 |
|---|---|---|---|---|
| 32 KB, crypto | 1,567.1 | 721.7 | 716.5 | |
| 32 KB, CoreMark | 393.4 | 388.3 | 335.1 | 341.2 |
| 96 KB, crypto | 33.2 | | 35.4 | |
| 96 KB, CoreMark | 167.9 | | 125.8 | |

Sixteen wins everywhere the cache is thrashing and costs 6.6% in the one
cell where the working set fits, because a shorter block is one more
trip through the dispatcher. It has not been measured for the other
frontends; RV32's blocks average four instructions, so a cap of 16
would seldom apply.

## `EMU_JIT_LOOP_CAP` — an interrupt-latency knob

Guest instructions a chain of blocks, or a block looping on itself, may
retire before it must return to the dispatcher, which is where
interrupts are delivered. CoreMark cannot see it. Measured at 64/128/256
on the F446 with the hand-written translator, cycles per guest
instruction:

| workload | 64 | 128 | 256 |
|---|---|---|---|
| CoreMark | 31.39 | 31.16 | 31.25 |
| `bench` | 18.88 | 18.39 | 18.13 |
| `mmiobench` | 24.40 | 23.46 | 22.99 |

CoreMark is noise: its loops end on unchainable branches, so the cap is
not what exits them. `mmiobench`'s block entries halve exactly per
doubling and its tightest kernels gain 18% at 256.

Each doubling returns half the previous one and doubles worst-case
latency, so **128 is the knee** and the default. Do not tune this on
CoreMark alone — and do not read the absolute figures as current: they
are from a different translator on a different part.

## The pc store

Not a knob: a `SETPC` that nothing observes is deleted by the optimiser.
It is here because it was found the way a knob should be — a histogram
of emitted bytes per IR operation said one operation was 19.3% of
CoreMark's code — and because of what it was worth on this target: 15%
on RV32 CoreMark and 2.07× on `bench`, which sits on the threshold above.
See [README.md](README.md).

## Framework table sizes

`EMU_JIT_MAX_BLOCKS`, `EMU_JIT_HASH_SIZE` and `EMU_JIT_BLOCK_RESERVE`
follow the target, keyed on `EMU_HOST_JIT_THUMB2`: 256, 256 and 512
bytes there; 65,536, 65,536 and 8 KB on a host, where 8192 blocks once
filled while the code buffer was a fifth used.

**The block table is not the limit on the board**, and that was checked
rather than assumed. PowerPC CoreMark at 96 KB ends with 68,668 of
98,304 bytes in use and 15,309 translations behind it, which reads as a
full table; with 1024 blocks and buckets the translation and compaction
counts were *identical to the digit*, for that run and two others. It
is the working set that does not fit, and the retention rule that keeps
the buffer three quarters full.

The reserve is the one with a consequence: a block larger than it can
overrun the buffer, which is translated twice and — until it was fixed —
compacted without unlinking
([../backend/thumb2.md](../backend/thumb2.md)).

## What is no longer a knob

Four options configured the hand-written Thumb-2 translator and outlived
it: declared in CMake, forwarded as compile definitions, defaulted in
`rv_config.h`, and read by nothing. They have been removed. What each
was, and what became of the behaviour:

| option | then | now |
|---|---|---|
| loop chaining | on by default; chained a back edge to the block's own start | unconditional, and exits chain to *other* blocks too |
| inlining the peripheral window | 2.2–3.1× to drivers, −53% to compute if always on, so armed after 64 passthrough accesses | **does not exist.** The IR backends inline guest RAM only; a peripheral access is a helper call |
| eliding the reload, and the dead store | off by default; neutral at 48 KB and a regression at 12 KB | unconditional in both backends, as a one-instruction window |

The driver-shaped workload is therefore slower than these documents used
to say, by an amount nobody has measured since.

## Things that did not work

- **A guest-register cache in r8–r10**: 15.5% slower. See
  [../backend/thumb2.md](../backend/thumb2.md).
- **The ARM shifted-operand fusion**: built, correct on hardware,
  byte-identical translations *because it never fired*, and a 10%
  CoreMark regression that remains unexplained. Reverted. The mistake
  was extrapolating from "ARM has a shifted operand" to "there will be
  shifts to fold". `-DEMU_PAIR_STATS=ON` answers that in one run —
  **run the pair stats before writing the encoder, not after.**
- **A larger block table on the board**: no effect at all; see above.
- **A minimum block length**: refusing short blocks so the batched
  fallback takes them. Monotonically worse, by 15× at six instructions —
  a one-instruction block still beats the interpreter by enough to pay
  for its own dispatch.
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
  take — both without any pattern matching.

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
