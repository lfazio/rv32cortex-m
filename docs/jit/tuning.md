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

**Below the working set the JIT is slower than the interpreter**, for
every guest large enough to matter. Host cycles per guest instruction on
the F746 at ce02bd0, and how many translations the run needed:

| | RV32 CoreMark | RV32 `bench` | PowerPC CoreMark | PowerPC crypto | guest RAM |
|---|---|---|---|---|---|
| interpreter | **102.27** | **91.87** | **212.29** | **282.49** | 304 KiB |
| 32 KB (the F746 default until 96 KB) | 107.00 (41,989) | 15.01 (62) | 263.95 (35,487) | 336.76 (10,559) | 225 KiB |
| 48 KB | 49.49 (17,103) | | | | 209 KiB |
| 64 KB (RV32's default) | **14.55** (333) | | 148.35 (18,688) | 29.57 (362) | 193 KiB |
| 96 KB (PowerPC's and ARMv7-M's default) | 14.59 (297) | **14.50** (61) | **53.63** (5,077) | **29.14** (347) | 161 KiB |

RV32 CoreMark is 120 iterations, 30,027,635 instructions; PowerPC's is
40 iterations, 12,170,464. Guest RAM is RV32's; PowerPC's is eleven KiB
less. The same table with how each run was checked is in
[../performance.md](../performance.md#on-the-board-at-ce02bd0). It is
the second version of this table: the first, before the short
encodings and the folded window guard, had RV32 CoreMark at 140.0
(63,733 translations) at 32 KB and 31.1 (7,152) at 96 KB, and PowerPC
crypto at 716.5 at 32 KB.

**What the B extension was worth here**, the same configuration before
and after, `crcfinal 0xd340` both times: 32 KB 172.3 → 140.0, 96 KB
47.6 → 31.1, and instructions handed back to the interpreter 1,366,260
→ 1. The 96 KB row needs *more* translations than it did (588 → 7,152)
and is still a third faster: a block that used to end at an `sh1add`
now runs on through it, so there is more code to hold and far fewer
dispatches to pay for it -- block entries 2,212,003 → 1,025,577.

Read the translation counts before the cycles. A guest with a few
hundred blocks that needs 41,989 translations is not being translated,
it is being *re*-translated: the cache is a queue. RV32 CoreMark fits
from 64 KB and is then 7.0× the interpreter; PowerPC crypto fits from
64 KB and is 9.7× ahead at 96 KB; PowerPC CoreMark does not fit here,
and is 4.0× ahead at 96 KB with 97,524 of 98,304 bytes in use.

So the knob is not a dial, it is a threshold per guest, and below the
threshold the JIT costs more than it saves. This is the statement this
file once made about a 12 KB cache, when the default was moved to 32 KB.
The threshold moved up when the IR backends replaced the hand-written
translator, because they emitted more per guest instruction, and the
F746's default followed it to 96 KB. Since then it has come back down
from the other side. The Thumb-2 emitters' short encodings, the folded
window guard and the merged `RETIRE` took a guest instruction from 60.8
bytes to 33.9 ([../backend/thumb2.md](../backend/thumb2.md#where-the-bytes-go)),
and that is what brought RV32 CoreMark inside 64 KB.

Guest RAM pays one for one. An RV32 JIT build at 32 KB is 79 KiB down
on an interpreter one: the buffer, plus about 47 KiB of block table and
IR storage that it pays at any size. At the 96 KB default it is 143 KiB
down, 161 KiB against 304. On a microcontroller those bytes are the
guest's, which is the whole tension -- there is no right answer, only a
stated one. **The stated one, on the F746, depends on the frontend,
with the dispatch path in ITCM.** RV32 takes 64 KB. It took 96 KB from
when that took RV32 CoreMark x120 from 152.85 host cycles an instruction
to 29.85, and came down once the emitted code had shrunk enough for
64 KB to serve RV32 CoreMark as well (14.55 against 14.59), giving the
guest 32 KiB back. PowerPC and ARMv7-M take 96 KB, because neither
CoreMark fits 64: they run at 148.35 and 106.41 there, against 53.63
and 34.7 at 96. G4MH takes 64, which is all its RAM leaves, and decides
when it is compiled in beside the others. The N6 takes 128 KB and the
F446 32 KB; `CMakeLists.txt` has the measurements behind each.

**Check `CMakeCache.txt` before quoting any of these.** Every performance
figure in this repo was once measured with a 48 KB cache inherited from
an old build directory while the declared default was 12 KB; `rm -rf
build/` moved them by 68% with no code change.

## Layout on the F746 — what the 140-to-153 swing was

RV32 CoreMark at 32 KB measured 140.0 host cycles an instruction before
FreeRTOS and 152.8 after, with identical translations, and the
difference was put down to the linker moving the hot path. Three
instruments say what it was, all CoreMark x120 under the JIT, one
flash each -- the counter repeats to 0.05% on one binary:

| what moves | how | result |
|---|---|---|
| all of `.text`, by 0-1,536 bytes | `-DEMU_LAYOUT_PAD=N` | 152.67-152.85: **nothing** |
| guest RAM, by 104 or 512 bytes | `-DEMU_LAYOUT_PAD_RAM=N` | 152.84 both: nothing |
| everything in `.bss` after `.dtcm` -- the code buffer, its tables, guest RAM | `-DEMU_LAYOUT_PAD_BSS=N` | added later, for PowerPC crypto at 96 KB: 25.67 to **28.74** over 0-8,192 bytes, byte-identical translations ([frontend/ppc.md](../frontend/ppc.md#on-the-f746)) |
| the order of functions | `-falign-functions` 32 / 64 | pre-FreeRTOS 140.01 → **147.90 / 149.30**; HEAD 152.85 → 148.53 / 150.54 |

So the old 140.0 was a lucky *relative* order of functions, which LTO
redraws whenever code is added -- not a baseline FreeRTOS cost 9%
against. The kernel's own cost is the 0.5% measured by bypassing it.

**And why order matters at all: at 32 KB the hot path is the
translator.** A tick-driven PC profile (`-DEMU_PC_PROFILE=8192`, F746
only) over the first 8 s of that run:

| | share of samples |
|---|---|
| `emu_ir_optimise` | 23.5% |
| `memset` | 15.9% |
| `lower_one` (Thumb-2) | 11.8% |
| `rv_translate` | 10.5% |
| translated blocks | **7.6%** |
| `rv_run`, the interpreter | 3.6% |

63,733 translations for a few hundred blocks: CoreMark's working set
does not fit, the cache is a queue, and the code being run is tens of
kilobytes of translator spread across flash behind a 4 KiB I-cache --
exactly the code whose speed depends on what sits next to it. At
96 KB, where translation is rare, the same three `-falign-functions`
layouts give 32.56 / 32.12 / 31.89: a 2% spread instead of 9%.

Placement in the TCMs is worth something and is stable, but it is small
beside the buffer size:

| at 32 KB | flash, SRAM buffer | hot path in ITCM | buffer in DTCM | both |
|---|---|---|---|---|
| cycles / instruction | 152.85 | 150.39 (150.33-150.42 over five pads) | 150.11 | 148.15 |

## `EMU_JIT_HOT_THRESHOLD` — translating only hot code

`-DEMU_JIT_HOT_THRESHOLD=N`, or `--jit-hot N` on the host runner: a pc
that misses the block cache is interpreted, one instruction at a time,
until it has missed N times in the current epoch, and only then
translated. The counts share the declined-pc table, reset on every
flush, and are reported as `cold` beside `interp`. **The default is 1,
which is translate-on-first-miss, because nothing measured gains from
more:**

| | 1 | 2 | 4 | 8 | 16 | 32 |
|---|---|---|---|---|---|---|
| F746 CoreMark, 32 KB, cycles / instruction | **152.70** | 156.19 | 154.83 | 156.37 | 159.64 | 160.08 |
| ...translations | 63,733 | 64,818 | 64,448 | 64,696 | 65,724 | 64,860 |
| F746 CoreMark, 96 KB | **32.52** | | 39.44 | | 32.38 | |
| ...translations | 7,152 | | 10,241 | | 7,051 | |
| host, Linux kernel init (s, two runs) | 4.38 / 4.66 | **4.35 / 4.36** | 5.21 / 5.05 | | 7.80 / 8.26 | |

Three reasons, each visible in the counters:

- **The F746's problem is a hot set larger than the buffer**, not cold
  code in it. Translations do not fall at any threshold, because the
  blocks being retranslated are the ones that were evicted while hot.
- **Single-stepping cold code lets the middle of a block cross the
  threshold first**, and its block then overlaps the one translated
  later from the real start: 96 KB at 4 is 10,241 translations against
  7,152.
- **On x86-64 a translation is cheap and an interpreted step is not**,
  and a Linux boot flushes thousands of times, so every flush restarts
  every warm-up.

Host CoreMark at two iterations does translate less -- 278 blocks at 1,
175 at 8, 127 at 32 -- which is the cold code the option exists for;
it is simply not where the time goes in anything measured here. Try it
on a guest whose translated set is mostly run-once code before
believing it pays.

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

## The N6

The same knob on the Nucleo-N657X0-Q, where it costs the guest nothing:
guest RAM is AXISRAM1, and the buffer goes in AXISRAM2. RV32 CoreMark
x120 at 600 MHz, the dispatch path in ITCM throughout:

| | 32 KB in ITCM | 64 KB | 96 KB | **128 KB** (the default) |
|---|---|---|---|---|
| cycles / instruction | 175.46 | 110.05 | 36.60 | **20.21** |
| translations | 63,462 | 35,336 | 6,885 | 301 |
| compactions | 3,616 | 1,175 | 183 | 2 |

At 128 KB the working set fits. A 650-iteration run takes 5.33 s by the
host's clock and 5.30 s by the firmware's.

**G4MH builds** kept 32 KB in ITCM, because G4MH's cluster and local
RAM, 128 and 64 KiB of `.bss`, left no room in AXISRAM2 -- and on the
F746 left the guest 32 KiB beside a 32 KB cache. A board's defaults
are 32 and 16 KiB now. G4MH CoreMark x40, built by
`scripts/g4mh-build-coremark.sh` (CRC 0x65c5, as on every frontend):

| | cluster / local RAM | cache | guest RAM | cycles / instruction | translations |
|---|---|---|---|---|---|
| F746, before | 128 / 64 KiB | 32 KB | 32 KiB | 92.38 | 22,369 |
| F746 | 32 / 16 KiB | 32 KB | 176 KiB | 93.11 | 23,089 |
| F746, **default** | 32 / 16 KiB | **64 KB** | **144 KiB** | **53.66** | 525 |
| F746 | 32 / 16 KiB | 96 KB | 112 KiB | 54.75 | 455 |
| F746, interpreter | 128 / 64 KiB | | 110 KiB | 97.01 | |
| N6, before | 128 / 64 KiB | 32 KB, ITCM | 1535 KiB | 93.96 | 23,023 |
| N6, **default** | 32 / 16 KiB | **128 KB** | 1535 KiB | **51.33** | 458 |
| F446, **default** | 32 / 16 KiB | interpreter | 62 KiB | 143.57 | |
| F446 | 16 / 8 KiB | 12 KB | 28 KiB | 204.36 | 65,179 |

Shrinking the two RAMs alone changes nothing (the second row); 64 KB is
where G4MH CoreMark's working set fits, and more buys nothing on the
F746 while costing its guest 32 KiB a step. The F446 cannot hold a
cache that fits, so its G4MH default is the interpreter.

**Then the smaller Thumb-2 code** -- short encodings, the one-compare
window guard and one RETIRE per stretch, 60.8 to 33.9 bytes a guest
instruction on the F746 ([../backend/thumb2.md](../backend/thumb2.md#where-the-bytes-go)).
CoreMark already fitted here, so this is the executed code getting
shorter rather than the cache stopping churning: x1500, a run CoreMark
accepts as valid at 600 MHz, **19.76 → 17.09** cycles an instruction,
12.41 s → 10.73 s by the host's clock, 125 → 150 iterations a second,
against a worktree at 7450c6b. (x650 is 4.6 s on this part, under the
10 s CoreMark requires; it measured 19.60 → 17.16 the same way.)

## Retention and chained blocks

`compact()` ranks blocks by dispatcher lookups, and a block entered
through a chain is never looked up. So the body of a hot loop -- reached
from the block before it by a patched jump -- read as cold and was
evicted, while the block jumping into it survived and the body had to be
translated again. `EMU_JIT_CHAIN_HEAT` (on by default, and
`emu_jit_set_chain_heat`) gives every block a linked exit leads to at
least the count of the block exiting to it, at compaction only, along
every linked edge until nothing changes. RV32 CoreMark x120 on the F746:

| | chain heat off | on |
|---|---|---|
| 96 KB, cycles / instruction | 29.85 | **27.80** |
| ...a second function order | 29.10 | **27.25** |
| ...translations | 6,413 | **5,731** |
| 32 KB, cycles / instruction | 150.20 | 154.32 |
| ...translations / compactions | 63,246 / 2,671 | 64,147 / 3,596 |

At 96 KB it is the effect the TODO suspected: 11% fewer translations and
6-7% faster under two layouts. At 32 KB, where CoreMark's hot set does
not fit at all, keeping more of it leaves less room, and compaction runs
35% more often for 3% -- in a regime where the JIT already loses to the
interpreter. Inheriting half the count instead was worse at both sizes
(29.63, 156.71). `test_chain_heat_keeps_chained_blocks` asserts both
settings, so a switch that does nothing fails it.

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
- **Translating only hot code** (`EMU_JIT_HOT_THRESHOLD`): kept as an
  option, off by default; no measured workload gains. See above.
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
