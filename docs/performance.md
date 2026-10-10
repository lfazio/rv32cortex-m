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
- **A board run longer than the cycle counter takes to wrap was reported
  a wrap short**, until `emu_cycles.h`: 19.9 seconds at 216 MHz. Every
  board figure below this line that predates it is suspect if its run
  was long, and none of them says how long its run was. The board
  figures in the next section were taken after the fix, with a wall
  clock on the host end of the UART agreeing.

## On the board, measured again

Nucleo-F746ZG, Cortex-M7 at 216 MHz, `RelWithDebInfo`, serial console
(`-DEMU_NET=OFF`). Host cycles per guest instruction; the JIT rows carry
the number of translations the run needed.

| | RV32 CoreMark | RV32 `bench` | PowerPC CoreMark | PowerPC crypto |
|---|---|---|---|---|
| interpreter | **108.2** | **98.2** | **222.3** | **243.4** |
| JIT, 32 KB (default) | 169.6 (82,075) | 199.5 (1,873) | 335.1 (46,199) | 716.5 (28,545) |
| JIT, 64 KB | 97.7 (29,682) | | 212.1 (27,780) | 373.7 (12,532) |
| JIT, 96 KB | **53.1** (588) | | 125.8 (15,309) | **35.4** (468) |

RV32 CoreMark is 120 iterations and 30,027,635 instructions — 14.8 s on
the interpreter and 7.3 s on the JIT at 96 KB, `crcfinal 0xd340` in every
row. PowerPC's is 40 iterations and 12,170,464 instructions.

**The JIT wins only where the translated working set fits the cache, and
at the default it does not.** RV32 CoreMark's set is about 83 KB; at
32 KB the run needs 82,075 translations and is 1.57× *slower* than
interpreting, and at 96 KB it needs 588 and is 2.04× faster. PowerPC
crypto goes from 2.9× slower to 6.9× faster across the same range.
The knob, the block-length cap that goes with it, and what was tried are
in [jit/tuning.md](jit/tuning.md).

There was an older table here — native ARM against the JIT at five cache
sizes on a 180 MHz M4, quoting 15.3× to 32.3× slower than native. It
described the hand-written Thumb-2 translator, which no longer exists,
and was removed rather than left to read as current. One conclusion from
it survives in a new place: then the JIT lost to the interpreter at
12 KB and the default was moved to 32 KB; now it loses at 32.

## The same three ways, on the x86-64 host

The section above is the board. This is the host runner, which is a
different question: there the JIT competes with a 216 MHz M7 and a code
cache measured in tens of kilobytes, here it has 32 MB and an
out-of-order superscalar to emit for.

RV32 CoreMark, each row run long enough for CoreMark to call it valid:

| | iterations | wall | per second | guest MIPS | vs native |
|---|---|---|---|---|---|
| **Native x86-64** | | 11.06 s | 18,181 | — | 1× |
| **JIT** | 12,000 | 13.69 s | 877 | 219 | 21× slower |
| Interpreter | 6,000 | 25.50 s | 235 | 59 | 77× slower |

At 6,000 iterations both backends give `crcfinal 0xa14c`; the JIT row is
at 12,000 because at 6,000 it finishes in under the ten seconds CoreMark
requires. 250,073 instructions an iteration on both. Native retires no
guest instructions, so MIPS is not defined for it; the comparison there
is the CoreMark score, and the native row was not re-run.

**The JIT is 3.7× the interpreter.** It was 1.98× when this table was
first written, with the interpreter where it is now. That is many
changes to the translator and the backends since, not one, and this page
does not apportion it.

4.6% of instructions still fall back: 136.6M of 3,000.9M. That is the
floor set by what the RV32 translator declines — SYSTEM and MISC-MEM,
deliberately, so the interpreter fallback stays the one place `frm`,
`mstatus.FS`, PMP and `satp` can change.

### All four frontends

Every frontend has a benchmark-sized guest and a JIT now. Host wall
time, medians, same checksums on both backends:

| frontend | guest | instructions | interpreter | JIT | |
|---|---|---|---|---|---|
| RV32 | CoreMark, 6,000 iterations | 1,500,449,974 | 25.50 s | 6.96 s | 3.7× |
| ARMv7-M | CoreMark, 600 iterations | 166,887,432 | 6.12 s | 0.56 s | 11.0× |
| PowerPC | CoreMark, 40 iterations | 12,170,464 | 258 ms | 43 ms | 6.0× |
| PowerPC | crypto | 2,422,217 | 58 ms | 12 ms | 5.0× |

They are not comparable across rows: different compilers, different
optimisation of the *guest*, and interpreters of different ages. G4MH
runs DOOM and Quake under its JIT and has no CoreMark figure here; CC-RH
is what builds its guests.

This subsection used to be titled "the other two frontends cannot be
measured", and said PowerPC had no translator and a 212-instruction
guest. Both were true.

## What a guest instruction costs, measured live

The runner draws a performance line on stderr -- automatically on a
terminal, and with `--rate` anywhere -- plus a whole-run average at
exit, which is what a guest finishing inside one sample interval gets.

```
guest  143.5  host 6681.9  ratio 46.58  cyc 2766.2  c/g 19.28  br 1243.0  miss 17.19 M/s
```

`ratio` is host instructions per guest instruction and is the headline:
it is what a translation change moves. `c/g` is the same in cycles,
which is that plus whatever the host is stalling on. Host figures need
`perf_event_open`; `kernel.perf_event_paranoid` must be 2 or lower, and
everything derived from an absent counter shows a dash rather than a
number.

| guest | floating point | translated | ratio |
|---|---|---|---|
| dhrystone | none | 98.5% | 52.6 |
| whetstone | heavy | | 34.2 |
| quake | heavy | 98.1% | 46.6 |

**The first thing these said was that a plausible theory was wrong.**
The ratio on Quake looked like evidence that SoftFloat dominates --
this tree routes every FP operation through it unless a backend lowers
it natively, and Quake is full of them. Dhrystone contains no floating
point at all and costs *more* per instruction; Whetstone is the most
FP-heavy of the three and costs least. So the ~50x is the JIT's
baseline, and it is not about FP.

It also confirms the native FP lowering is doing its job: were the
arithmetic going to helpers, Whetstone would be the worst of the three
rather than the best. See [floating-point.md](jit/floating-point.md)
for what each backend lowers and what it declines.

What the ~50x *is* about is not yet established. 98% of instructions
run translated in both guests, so it is not interpreter fallback; block
entries are 7.1 guest instructions apart on Quake, so dispatch is
amortised over very short blocks. Longer blocks are the thing to
attack, which is what this file already says about the 4.12-instruction
average.

### Two ways these numbers went wrong first

Both are worth keeping, because both produced a plausible figure rather
than an error.

**A run under ten seconds is not a CoreMark result.** The first
calibration reported 2000 iterations in 0.113 s, and CoreMark printed
*"Must execute for at least 10 secs for a valid result"* along with
`Errors detected` — which reads as a miscompile and is not. Iteration
counts here are chosen so every row exceeds ten seconds; a row that does
not is not comparable to one that does.

**Two guests share a basename in one build tree.** The PowerPC frontend
was first measured against `build/*/guest/isatest.bin`, which is 14,704
bytes and is the *RV32* isatest; the PowerPC one is
`build/*/tests/guest/ppc/isatest.bin` at 2,704 bytes. The wrong one runs
without complaint under the PowerPC frontend and retires instructions at
a plausible rate. It also needs `--load 0x80000000`; without it the
earlier attempt interpreted 50M instructions of whatever sat at the
default address and produced a figure that looked like a result. `ctest`
has both right — read its `COMMAND` before running a guest by hand.

## Dhrystone and Whetstone, and which clock they read

Both run as guest images, and both divide work by *guest* time — so what
their figures mean is decided by what drives the guest's clock.

| | what drives `mtime` | what a self-reported rate then means |
|---|---|---|
| host | the host's monotonic clock, since September | this emulator's throughput on this machine, and different on every run |
| board | the cycle counter, scaled to 1 MHz | the same, on that part |

**This section used to say the opposite about the host**, and the figures
it quoted are why it is worth keeping the correction visible. The host's
guest clock was once one tick per retired instruction: a benchmark then
reported the same rate on the interpreter and on the JIT — 470.0 µs a
run, to the digit — and the page said, correctly at the time, that
comparing backends with it was meaningless by construction. That clock
was replaced because it is wrong for anything that measures time (a
Linux kernel's watchdogs fired on a machine running perfectly), and the
statement outlived it.

Today, x86-64 host, medians of five:

| | self-reported | host wall |
|---|---|---|
| Dhrystone 2.1, `DHRY_RUNS=20000`, interpreter | 9.3 µs/run, 107,271 Dhrystones/s | 193 ms |
| the same, JIT | 1.7 µs/run, 576,053 Dhrystones/s | 41 ms |
| Whetstone 1.2, `WHET_LOOPS=100`, interpreter | 30.4 MIPS | 264 ms |
| the same, JIT | 157.1 MIPS | 66 ms |

9,431,251 and 10,665,593 guest instructions. The JIT translates
essentially all of both: 120,566 and 51,238 instructions fell back,
1.3% and 0.48% — the ratio to read before believing any figure here.

Neither rate is comparable with a published number: Dhrystone does not
print DMIPS, and these are an emulated core on a desktop. For a
reproducible quantity use `retired`, which is the same on every run and
nearly the same across backends — they differ by a few instructions
because guest time is sampled once per round, and the JIT's rounds end
on a block boundary.

### What lowering the arithmetic to the host FPU bought

The JIT figure was 120 ms while every FP instruction went to SoftFloat.
Best of twelve, three interleaved rounds, same tree, one table entry
apart:

| | host wall | step | share of executed |
|---|---|---|---|
| everything on the helper | 120 ms | | |
| FLW/FSW/FMV lowered | 110 ms | −8% | 5.3% |
| `fmul.s` as well | 94 ms | −15% | 4.33% |
| `fadd.s` as well | 85 ms | −9% | 1.49% |
| `fsub.s` as well | 74 ms | −12% | 2.32% |
| `fdiv.s` as well | 74 ms | **0%** | 0.15% |
| `fsqrt.s` as well | 74 ms | **0%** | 0.087% |

**The last column is the whole explanation, and it is the one that has
to be measured rather than assumed.** It is the instruction's share of a
500,003-instruction sample taken from the middle of a run
(`--trace-skip 3000000 --trace-count 500000` on a `-DEMU_ENABLE_TRACE=ON`
build, histogrammed by mnemonic). Floating point is 14.2% of the sample.

The last two rows are below the floor and were *predicted* to be, which
is the more useful result. `fdiv.s` is 29× rarer than `fmul.s`;
`fsqrt.s` is executed 9,300 times in 10,664,954, and at a plausible
couple of hundred host cycles saved per call that is under a millisecond
against a run-to-run spread of 73-78 ms. The prediction was made from
the histogram before the timing was taken, and the timing agreed:
73 ms against 74 ms, with the spread *within* each binary wider than the
difference between them.

Both stay lowered: each is one case, each is covered by architecture
tests that fail without the canonicalisation, and division and square
root are the two most expensive SoftFloat operations, so a guest that
leans on either would see it. But nothing in this project's guest set
can measure them, and a figure quoted here would be noise with a number
on it.

**`fsqrt.s` also has the thinnest suite coverage of the five** — one
`F-fsqrt.s` test where the others have five each — so
`test_lower_fp_nan_canonical` is carrying it, and that is where its
awkward input lives: `sqrt(-1)` is the one case whose wrong answer
(x86's `0xFFC00000`) differs from the right one in the sign bit alone.

CoreMark is unchanged at 8 ms and Dhrystone reported 2128.3 to the digit
throughout — under the instruction-count clock the host had at the time,
when that figure could not move for any reason but a change in what the
guest executed — which is the check that the MXCSR framing is not being
paid by blocks with no float in them. `fptest` is too short to resolve.

**Emitted code grows while the clock falls**: 158,208 bytes to 164,676
across the last three steps, because a native FP sequence is longer than
a call. The call was the expensive part, not the bytes — which is the
opposite of the rule for the 12 KB Thumb-2 cache, and is why that rule
is stated as being about *that* cache rather than about JITs.

**Best of five was not enough.** A five-sample run had Dhrystone moving
77 ms to 71 ms across two binaries that differ only in floating point;
fifteen samples in three interleaved rounds gives 72-74 for both. Layout
noise on this host is real (CLAUDE.md records ±3%, and 10% on the
board), so an A/B whose effect is under ~10% needs interleaved rounds,
not one pass.

The reason this is safe when the earlier attempt at host FP was not is
in [`docs/jit/floating-point.md`](jit/floating-point.md): add, subtract,
multiply and divide are the operations IEEE 754 specifies *exactly*, so
the arithmetic never differed. What differed was the NaN convention and
the exception flags, and both are now the backend's rather than being
hoped for.

### Against the same source compiled natively

`whetstone.c` built for x86-64 with `gcc -O2` and glibc's libm, same
`WHET_LOOPS=100`, timing its own measured region:

| | measured region |
|---|---|
| native x86-64 | 0.653 ms |
| RV32 on the JIT | ~72 ms (74 less 2 ms of emulator startup) |
| RV32 on the interpreter | ~252 ms |

so **110× native on the JIT and 386× on the interpreter**, and 148 MIPS
against 42 MIPS in guest instructions retired.

**That 130× is not an emulation-overhead figure, and reading it as one
would be wrong.** Modules 7 and 11 are 99.4% of the guest instruction
count and are `sinf`/`logf`/`expf`/`sqrtf`, so the two runs do the same
*benchmark* work through two different libm implementations on two
different instruction sets: x86-64 has `sqrtss` as one instruction and a
vectorising compiler above it, where the guest runs newlib's software
argument reduction compiled for RV32. The emulator is being asked to
execute far more instructions for the same result, and 130× is the
product of that and the per-instruction cost.

The figure that isolates the emulator is the second one: **148 million
guest instructions a second** on this host, with 99.5% of them
translated. Comparing that against the interpreter's 42 MIPS is the
like-for-like measurement; comparing either against native measures the
ISA and the libm as much as the emulator.

**Those figures are eight times better than the ones first recorded
here, and the change was one flag.** The guest ABI is `ilp32`, which
also selects the multilib, and `rv32imac/ilp32`'s libm is compiled
*soft-float*: every `sinf` and every float multiply in the benchmark
was `__mulsf3`. The FP benchmark was measuring libgcc's integer soft
float. This image now overrides the shared ABI with `-mabi=ilp32f`,
which selects `rv32imafc/ilp32f` and its hard-float libm: 86.0 million
guest instructions became 10.7 million, and 116.3 KIPS became 937.9.

Two things about it that Dhrystone does not share.

**The rate depends on the LOOP count, so a figure must name it.** 113.6
KIPS at `WHET_LOOPS=10`, 116.3 at 100, 125.1 at 1000 — a 10% spread. It
is not warm-up: modules 7 (trigonometric) and 11 (`sqrt`/`exp`/`log`)
are **99.4%** of the whole benchmark's instruction count here, measured
by rebuilding with each module's iteration count zeroed and
differencing, and both get cheaper per iteration as their inputs
converge — module 11 drives X toward 1.0, where `logf`'s argument
reduction has least to do. A longer run is a different measurement, not
a more precise one.

**So it is mostly a libm benchmark.** That is inherent to Whetstone on a
machine with no hardware transcendentals, and it does exercise the F
extension hard, since newlib's `sinf` and `logf` are float arithmetic
throughout. But it means a cross-frontend comparison — the point of this
image — compares three libm implementations along with the three
compilers. Worth stating before RV32, G4MH and PowerPC numbers are put
in one table.

The correctness check is `-DWHET_PRINTOUT=ON`, which restores upstream's
per-module digest. At `WHET_LOOPS=10` all ten lines agree to four digits
across the interpreter, the JIT, and the same source compiled natively
for x86-64 against glibc. There is nothing to compare the digest with
*a priori*: module 11's result converges on 1.0 only as LOOP grows
(0.8347 at 10, 0.9972 at 100, 0.9999 at 1000), so the value that looks
like the obvious expectation is wrong at every practical setting.

---

# From the F446, under the hand-written translator

**Everything from here to the end of this file is history.** It was
measured on the Nucleo-F446RE (Cortex-M4F at 180 MHz, code in flash with
5 wait states and the ART accelerator enabled), mostly with
[`tests/guest/bench.c`](../tests/guest/bench.c), under the hand-written
Thumb-2 translator that the shared IR backends replaced. Several of the
knobs it names were options on that translator and are gone
([jit/tuning.md](jit/tuning.md) says what became of each), and the
inlined peripheral window it measures does not exist in the IR backends
at all. Read these sections for how each thing was found — which is why
they are kept — and not for a figure.

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
