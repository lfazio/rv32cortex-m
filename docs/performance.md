# Performance

Every figure here is a measurement, not an estimate, and every one of
them depends on build settings that are **CMake cache variables** --
`EMU_JIT_CODE_BYTES`, `RV_GUEST_MARCH` and `COREMARK_ITERATIONS` all
silently outlive the tree they were set in. A whole generation of the
numbers below was once measured with a 48 KB code cache inherited from an
old build directory while the declared default was 12 KB; `rm -rf build/`
moved them by 68% with no code change.

So: **`scripts/report-figures.sh` regenerates the host figures and prints
the cache variables that produced them.** Board figures need a flash
cycle and carry the commit they were measured at instead.

Two standing cautions:

- The board's tick counter is exactly deterministic. The same CoreMark
  binary reflashed and rerun three times gave 847,616 ticks every time,
  to the digit. Never call a difference noise without rerunning the same
  binary -- it costs one reflash. What *does* move by up to 10% on that
  part is where the emulator's hot loop lands in flash, so two different
  emulator binaries can differ by 10% with byte-identical translations.
- Read the JIT stats line before believing a result. A backend that
  declines everything and falls back passes every suite while proving
  nothing.

## Performance

Measured on the Nucleo-F446RE (Cortex-M4F @ 180 MHz, code in flash with 5 wait
states and the ART accelerator enabled), using
[`tests/guest/bench.c`](../tests/guest/bench.c) — a compute-bound workload with no
I/O between the start and end markers.

## CoreMark: native vs interpreted vs JIT

The same CoreMark sources, 150 iterations, on the same 180 MHz Cortex-M4 —
compiled natively for ARM, and compiled for RV32 and run under each backend.
This is the number that says what emulation actually costs.

These use the full B extension in the guest, which is the best configuration
for each backend.

| | Ticks (µs) | Iterations/s | CoreMark/MHz | vs native |
|---|---|---|---|---|
| **Native ARM** | 336,130 | 446.3 | 2.479 | 1× |
| **JIT**, 64 KB code cache | 5,148,168 | 29.1 | 0.162 | **15.3× slower** |
| JIT, 48 KB | 6,463,217 | 23.2 | 0.129 | 19.2× slower |
| JIT, 32 KB | 8,525,192 | 17.6 | 0.098 | 25.4× slower |
| JIT, 24 KB | 9,329,706 | 16.1 | 0.089 | 27.8× slower |
| JIT, **12 KB — the default** | 10,850,998 | 13.8 | 0.077 | 32.3× slower |
| Interpreter | 10,691,637 | 14.0 | 0.078 | 31.8× slower |

All rows at 150 iterations, `crcfinal 0xca90` throughout.

Re-measured after the correctness work on `frm`, `mstatus.FS`, PMP staleness
and the FP translations: 10,851,525 at 12 KB and 6,375,722 at 48 KB, against
the 10,850,998 and 6,463,217 above. Both inside the ±3% layout noise, with
identical compaction and eviction counts — the translated code is byte for
byte what it was. None of those fixes costs a guest that does not use the
feature anything, which the flush count confirms: CoreMark reports one flush
for a whole run.

**The JIT's speed is set by `EMU_JIT_CODE_BYTES` more than by anything in the
translator**, and the default is not the configuration to quote. CoreMark's
translated working set is about 48 KB; below that the cache thrashes, and the
compaction counts show it directly — 231 compactions at 64 KB, 904 at 48 KB,
8,533 at 12 KB, with evictions going from 8,585 to 94,240.

At the 12 KB default the JIT is **slower than the interpreter**. That is the
number a fresh checkout reproduces, and it means the default currently buys
12 KB of SRAM worth of nothing: a build that small should use
`-DEMU_JIT=OFF` and hand the guest all 122 KiB instead. The JIT starts
earning its RAM at about 24 KB and is worth 2.1× at 64 KB.

The cost is guest RAM, one for one: 122 KiB with no JIT, 106 at 12 KB, 70 at
48 KB, 54 at 64 KB. Which end of that to sit at depends on whether the guest
needs memory or speed, so it is left as a build option rather than decided
here — but every performance figure below is quoted at **48 KB**, and figures
from any other size are not comparable.

## Driver performance: the passthrough window

CoreMark and `bench` are deliberately I/O free, so neither says anything about
the path a guest *driver* takes. `mmiobench` measures that one. Each kernel
runs twice with identical machine code, once against the peripheral window and
once against guest RAM; the RAM form is the control, so dividing removes the
loop overhead and leaves the access path.

Nanoseconds per access on the F446, `EMU_JIT_INLINE_PERIPH` off and on:

| kernel | helper | inlined | speedup | vs its RAM control |
|---|---|---|---|---|
| `read` — load a status register | 1493 | **654** | 2.28x | 1.34x |
| `write` — store a command register | 1236 | **558** | 2.22x | 1.44x |
| `rmw` — read-modify-write | 2286 | **735** | 3.11x | 1.38x |
| `poll` — read, test a bit, branch | 1337 | **479** | 2.79x | 1.15x |

Overall 46.66 to **24.40** cycles per guest instruction. The RAM controls move
by at most 4%, which is inside the noise, and CoreMark by 0.66% — the point of
arming the path from guest behaviour rather than always emitting it.

The residual 1.15-1.44x over RAM is not emulator overhead: a GPIO register on
AHB1 costs more to reach than SRAM on this part, and native ARM code pays that
too.

## Tuning the loop cap

`RV_JIT_LOOP_CAP` is how many guest instructions a chained loop runs before
returning to the dispatcher. Interrupts are delivered between blocks, so it is
directly the guest's worst-case interrupt latency — a throughput-against-
latency knob, not a free parameter. Measured on the F446, cycles per guest
instruction:

| | 64 | 128 | 256 |
|---|---|---|---|
| `bench` | 18.88 | 18.39 | 18.13 |
| `mmiobench` | 24.40 | 23.46 | 22.99 |
| CoreMark | 31.39 | 31.16 | 31.25 |
| worst-case latency | ~11 µs | ~22 µs | ~44 µs |

**CoreMark does not care at all.** Its loops end on branches the translator
cannot chain, so the cap is not what exits them — its 0.7% spread is inside
the noise. The tightest loops care most: `mmiobench`'s block entries halve
exactly with each doubling (25,125 → 12,810 → 6,600), and its RAM-only
kernels gain 5% at 128 and 18% at 256.

Each doubling returns about half of the previous one, which puts **128** on
the knee, and that is the default. The cost is linear and certain where the
gain is small and diminishing: 256 buys a further 2% on aggregate for double
the latency again, which is a poor trade for an emulator whose guest drives
real peripherals. Drop to 64 if a guest ISR has a deadline tighter than
~22 µs; `-DEMU_JIT_LOOP_CAP=` sets it.

**Two bugs surfaced here that had nothing to do with peripherals**, both found
because inlining changed block sizes and made them observable:

*Chaining was silently dropped for large blocks.* The loop back edge was only
emitted when it fit the 16-bit conditional branch's ±254 bytes; past that the
block quietly stopped chaining instead of getting a wider encoding. Inlining
pushed two-access loop bodies over the line and cost them 2.4x. Now the
32-bit form (±1 MB) is used exactly when the short one will not reach, so the
common case pays nothing.

*A silently malformed compare.* `RV_JIT_LOOP_CAP` is enforced by comparing the
retired-instruction accumulator in r8 against the limit — through the 16-bit
`CMP`, which encodes only r0-r7. Passing it r8 set a bit belonging to the
other operand and it assembled as `CMP r0, r1`. The cap therefore never
applied: chained loops ran to completion in one block entry, 3700 guest
instructions where 64 was intended. It read as a throughput win and was
really the interrupt-latency bound being discarded. Fixing it restored 64
(`blk entr` 434 to 25125 on `mmiobench`) at about 10% on tight loops.

The lesson both share is the one this file keeps relearning: **on ARM, a
register number that does not fit the encoding does not fail — it assembles
as a different instruction.** Neither bug produced a wrong result, so no test
caught either; they only showed up as performance that made no sense.

**The interpreter costs 46% for two features almost no guest uses.** Measured
by compiling each out:

| Configuration | cycles/guest insn |
|---|---|
| neither | **35.18** |
| PMP only | 38.36 |
| Sdtrig only | 49.96 |
| both (default) | 51.38 |

So Sdtrig is 14.8 cycles per instruction and PMP 3.2. The JIT pays far less
because it tests `trig_active` once per block dispatch rather than per
instruction.

The obvious fix is not the fix. Hoisting `trig_active` into a local, so the
fetch path tests a register instead of loading hart state, measured *slower*
— 51.38 to 53.66 — because maintaining it across the loop costs more register
pressure than the load did. The cost is not the load; the likely culprit is
the `TRAP` call site the check introduces into the fetch sequence, which
constrains register allocation for the whole dispatch loop. Confirming that
means reading the generated code, not guessing again.

`-DRV32_EXT_SDTRIG=OFF` recovers 29% today, at the price of `rv32mi/breakpoint`
and a 76/77 on `riscv-tests`. Both remain on by default because conformance is
the more defensible default for an emulator, but a deployment that will never
attach a debugger should turn Sdtrig off.

Full extension set — F, B, Zacas — compiled into the emulator. `Zcb` is
supported but deliberately **not** used by the guest; see below.

**CoreMark cannot measure the VFP work.** It is integer-only: our port sets
`HAS_FLOAT 0` to keep soft-float out of the guest, so not one translated FP
instruction executes. Showing the FP translation as a speedup needs an
FP-bearing benchmark, which this is not.

All three produce **`crcfinal 0xca90`** — native ARM and emulated RISC-V agree
bit for bit, which is independent confirmation that the emulation is correct.

The 2.49 CoreMark/MHz native figure is in the expected band for a Cortex-M4,
which is a useful sanity check on the measurement itself.

*(These are not reportable CoreMark scores: EEMBC requires a ≥10 s run and a
specific disclosure format, and the native run takes 0.34 s. They are valid
as a relative comparison, which is what they are used for here.)*

## Per-instruction cost

Host cycles per guest instruction, measured on hardware. The B extension
column is the same guest source rebuilt with `-march=..._zba_zbb_zbc_zbs`.

| Workload | | Interpreter | JIT |
|---|---|---|---|
| CoreMark | RV32 | 35.7 | 35.9 |
| CoreMark | + B | **28.7** | 33.4 |
| `bench`  | + B | 127.2 | **28.7** |

*(The interpreter column is from before PMP and Sdtrig were added; both cost it
a little. The JIT column is current.)*

B is a clear win for the guest: it removes 12% of CoreMark's instructions
(42.94 M → 37.67 M) and takes the interpreter from 35.7 to 28.7 cycles each.
Against native ARM, CoreMark interpreted improves from 26.3× to **17.7×**.

The JIT does not benefit as much, and on CoreMark it is now the slower of the
two. That is an honest open result rather than a tuned one — see below.

## What the JIT needed along the way

Each of these was found by measurement, not by inspection:

1. **Inlining the guest-RAM fast path.** Loads and stores had each been a
   helper call; describing guest RAM in callee-saved registers turned a RAM
   access into a subtract, a compare and a register-offset load.
   CoreMark 47.9 → 35.7.
2. **Translating Zbb.** Untranslated Zbb ended a block every time and fell back:
   307,128 fallbacks. Adding `clz`/`ctz` (`RBIT`+`CLZ`), `min`/`max`,
   `andn`/`orn`/`xnor`, `rol`/`ror`, `sext.*`/`zext.h`, `rev8` → 89 fallbacks,
   44.2 → 35.9.
3. **A helper call for what cannot be translated.** `MULH`/`DIV`/`REM`, `clmul`,
   `cpop` and `orc.b` have no short Thumb-2 form. Ending the block for them cost
   far more than the instruction: CoreMark took 175,305 fallbacks and `bench`
   40,010. Calling a helper instead keeps the block intact — **fallbacks fell to
   1 and 3**, `bench` went 64.5 → 34.3 and CoreMark 46.3 → 40.8.

The recurring lesson is that what a translator *declines* costs more than what
it translates badly.

## Block chaining, and how it was found

The JIT trailed the interpreter on CoreMark, and the first explanation was
wrong. Per-operation counters showed **175,216 `clmul` helper calls** against 88
for multiply/divide, so `clmul` was translated inline — a shift-and-XOR loop
that exits at the highest set bit instead of the helper's fixed 32 iterations.
It worked and it barely mattered: **1.3%**.

The arithmetic said why: an ~11.8 cycle-per-instruction gap over 37.67 M
instructions is ~446 M cycles, which 87 k operations cannot account for. It was
spread across everything.

A block-entry counter found it — **9,141,951 entries for 37,670,524
instructions, an average block of 4.12 instructions.** Every block paid a hash
lookup, `PUSH {r4-r7,lr}`, three constants for the guest-RAM registers, an
epilogue and a return: 30–40 cycles amortised over four instructions.

So blocks were extended rather than given more instruction coverage:

- **Forward `JAL` is simply followed.** Nothing is emitted for it at all and
  translation continues at the target. Backward jumps still end the block —
  they are loop back edges, and returning to the dispatcher there is what
  bounds interrupt latency and stops translation looping over a body.
- **A conditional branch only exits on the taken path.** The fall-through keeps
  being translated behind a short skip-branch over the exit stub. CoreMark is
  full of forward if/else branches that previously ended a block every few
  instructions.

| | Blocks entered | Insns/block | cyc/insn |
|---|---|---|---|
| CoreMark before | 9,141,951 | 4.12 | 40.5 |
| CoreMark after | 5,454,702 | **6.91** | 32.6 |
| `bench` after | 158,025 | **8.07** | 27.4 |

**CoreMark 19.8% faster, `bench` 20% faster**, and against native ARM CoreMark
goes from 25.4× to **20.2×** slower. The JIT still trails the interpreter on
CoreMark (32.6 vs 28.7) but the gap is now 14% rather than 41%; the remaining
per-block cost is the prologue, and the next step is to emit the guest-RAM
registers only for blocks that actually access memory.

Zbc's `clmul` is not translated: ARMv7-M has no carry-less multiply (`PMULL` is
a NEON/crypto instruction, absent on Cortex-M).

## Block retention

The code cache keeps the most-used blocks rather than discarding everything
when it fills. Blocks are relocatable — every guest pc and helper address is an
absolute `MOVW`/`MOVT` constant and the only pc-relative branch is internal —
so compaction slides the hot ones down and drops the cold, with an ageing pass
so past popularity decays.

It matters at realistic cache sizes. CoreMark's working set does not fit in
12 KB:

| Code cache | Translations | Compactions | Ticks |
|---|---|---|---|
| 12 KB | 106,799 | 3,676 | 12,881,419 |
| 48 KB | 3,005 | 168 | 11,462,247 |

## `-Os` versus `-O3`

The STM32F446's ART accelerator holds about 1 KB of instructions, so a smaller
emulator might plausibly fit its hot loop better. Measured, on the same guest
and the same run length:

| Build | Flash | cycles/guest insn |
|---|---|---|
| `Release` (`-O3`) | 49,652 B | **57.2** |
| `MinSizeRel` (`-Os`) | 33,240 B | 62.2 |

**A third smaller, and 8.8% slower.** The accelerator is evidently not the
binding constraint — what `-Os` gives up in inlining and loop structure costs
more than the extra code density recovers.

`-O3` is therefore the default. `-Os` is the right choice on a part where flash
is genuinely scarce: 33 KB against 50 KB is a real difference on a 64 KB device,
and 9% of emulator speed is a reasonable price for fitting at all.

*(These figures come from a short run and include startup, so they are higher
than the 150-iteration numbers above. Only the ratio between them is meant.)*

## Two optimisations that did not work

Recorded because the negative results are as informative as the wins:

**Interpreter in SRAM** (`RV32_INTERP_IN_RAM`, default off) measured *slower* —
162 vs 122 cycles — while also taking 8 KiB from the guest. Executing from flash
lets the Cortex-M4 fetch over the I-bus while data goes to SRAM over the D-bus,
and the ART accelerator keeps a hot loop effectively wait-state free; moving code
into SRAM puts fetch and data on the same interface and serialises them. Kept as
an option because the trade-off is part-specific.

**Lazy interrupt evaluation** (`RV32_LAZY_IRQ`, default on) showed no measurable
gain, which is the expected result: with `mstatus.MIE` clear the eager check
already returns after one load and one test. It pays when a guest enables
interrupts. Measurements across these interpreter builds span 155–160 M cycles
for identical instruction counts, so differences of a few percent are code
layout, not algorithm.

The two optimisations that mattered:

1. **Bus fast paths.** Every emulated instruction did one or two instruction
   fetches plus up to one data access, each an out-of-line call into a region
   walk with permission and width checks. Caching the last plain-memory region
   per access kind turns the common case into a compare and a load.
2. **LTO.** Without cross-translation-unit inlining, the interrupt check and the
   memory helpers stayed real calls on every instruction.

**Measure compute, not the console.** For contrast, on the same build:

| Guest | Reported | What it actually measures |
|---|---|---|
| `bench` | 122 cyc/insn, 1.48 MIPS | the interpreter |
| `stm32drv` | 330 cyc/insn, 545 KIPS | mostly USART2 TX waits and MMIO polling |
| `isatest` | 427 cyc/insn, 421 KIPS | mostly console output and the timer spin |

Only `bench` is a throughput figure. The other two are dominated by waiting on
real hardware, which is a property of the workload, not of the emulator.

Remaining headroom, in rough order of expected value: allocating guest registers
to ARM registers across a block (the JIT still loads and stores every operand,
which is the bulk of the remaining 25× gap), chaining blocks so a hot loop stops
returning to the dispatcher, and teaching the translator `DIV`/`REM`/`MULH`.

---
