# Performance

Every figure here is a measurement, not an estimate, and every one of
them depends on build settings that are **CMake cache variables** --
`EMU_JIT_CODE_BYTES`, `RV_GUEST_MARCH` and `COREMARK_ITERATIONS` all
silently outlive the tree they were set in. A whole generation of the
numbers below was once measured with a 48 KB code cache inherited from an
old build directory while the declared default was 12 KB; `rm -rf build/`
moved them by 68% with no code change.

So: **`scripts/report-figures.sh` regenerates a quick set of host
figures and prints the cache variables that produced them** -- coverage,
`bench`, a two-iteration CoreMark, Dhrystone and Whetstone. The tables
below are longer runs than it makes, and each states its settings. Board
figures need a flash cycle and carry the commit they were measured at
instead.

**The current tables were measured at ce02bd0** (18 October 2026), on
the x86-64 host and on the F746. Anything else on this page names its
own commit or says it is history.

Three standing cautions:

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

## On the board, at ce02bd0

Nucleo-F746ZG, Cortex-M7 at 216 MHz, `RelWithDebInfo`, serial console
(`-DEMU_NET=OFF -DEMU_MAX_INSN=0`), each firmware built from a git
worktree at ce02bd0. Host cycles per guest instruction from the
firmware's own counter; the JIT rows carry the number of translations
the run needed:

| | RV32 CoreMark | RV32 `bench` | PowerPC CoreMark | PowerPC crypto | guest RAM |
|---|---|---|---|---|---|
| interpreter | **102.27** | **91.87** | **212.29** | **282.49** | 304 / 294 KiB |
| JIT, 32 KB | 107.00 (41,989) | 15.01 (62) | 263.95 (35,487) | 336.76 (10,559) | 225 / 214 KiB |
| JIT, 48 KB | 49.49 (17,103) | | | | 209 KiB |
| JIT, 64 KB (RV32's default) | **14.55** (333) | | 148.35 (18,688) | 29.57 (362) | 193 / 182 KiB |
| JIT, 96 KB (PowerPC's default) | 14.59 (297) | **14.50** (61) | **53.63** (5,077) | **29.14** (347) | 161 / 150 KiB |

Guest RAM is RV32's firmware, then PowerPC's. RV32 CoreMark is 120
iterations, 30,027,635 instructions, `crcfinal 0xd340` in every row;
PowerPC's is 40 iterations, 12,170,464 instructions, `crcfinal 0x65c5`
and validated in every row. Crypto prints the same four `ok` lines and
checksum in every row, and `bench` the same output. The host's clock
agrees with the firmware's wherever a run is long enough to time: RV32
CoreMark interpreted is 3.07G cycles, 14.2 s at 216 MHz, against 14.0 s
from the banner to `-- done --` at the host end of the UART; at 96 KB,
2.03 s against 2.00.

**At 120 iterations RV32 CoreMark is no longer a run CoreMark accepts
under the JIT.** From 48 KB up it finishes inside ten seconds -- 2.0 s
at 96 KB -- and CoreMark says so. The figure is still the firmware's
count and the CRC is still right. For a run CoreMark validates, x650 at
96 KB measured **14.03** at 5e5a3c3, whose emulator is what ce02bd0
builds for this board (ce02bd0 touched scripts, documents and the N6's
configure only). x650 at 32 KB, at the same commit, took 80 s by the
firmware and 79.0 s by the host, `crcfinal 0x25e3`.

**The JIT wins wherever the translated working set fits, and RV32
CoreMark's now fits in 64 KB**: 333 translations there, against 29,682
when this table was last measured. At 96 KB the JIT is **7.0×** the
interpreter on RV32 CoreMark, 6.3× on `bench`, 4.0× on PowerPC CoreMark
and 9.7× on crypto. **The default follows the frontend.** An RV32
firmware gets 64 KB: RV32 CoreMark gives up nothing there and the guest
gets 32 KiB back. A PowerPC or ARMv7-M firmware gets 96 KB, because
neither CoreMark fits 64: PowerPC's is 1.4× the interpreter there and
4.0× at 96, and ARMv7-M's 106.41 against 34.7
([frontend/armv7m.md](frontend/armv7m.md)). G4MH's is 64 KB, which is
all its RAM leaves. At 32 KB, where none of the
CoreMarks fits, it is 1.05× *slower* than interpreting on RV32, 1.24×
on PowerPC CoreMark and 1.19× on crypto -- where this table said 1.57×,
1.5× and 2.9×. The cache did not grow; what goes into it shrank. PowerPC
CoreMark is still being evicted at 96 KB, with 97,524 of 98,304 bytes
in use. The knob, the block-length cap that goes with it, and what was
tried are in [jit/tuning.md](jit/tuning.md).

**The interpreters moved less, and PowerPC crypto's moved the wrong
way**: 282.49 against the 243.4 this table last showed, while PowerPC
CoreMark went from 222.3 to 212.29. This page has not apportioned that.
[frontend/ppc.md](frontend/ppc.md#on-the-f746) records crypto moving
12% with nothing but where `.bss` lands, the same order as this 16%. RV32's
108.2 to 102.27 is the run loop in ITCM (below).

### How the RV32 figure got here

RV32 CoreMark at 96 KB. From the second step on, each was measured
against a worktree at the commit before it:

- **53.1** at 96 KB when 32 KB was the default, which ran it at 169.6.
  Later, at 47.6, lowering the B extension made it **31.1**: 1,366,260
  instructions handed to the interpreter became 1.
- **29.85** with 96 KB the default and the dispatch path in ITCM,
  against 152.85 at the old default the day before -- 4.1 s against
  20.9 by the host's clock. At 650 iterations, 22 s and past the
  counter's wrap, it was 29.30 and CoreMark validated itself.
- **15.71** from short encodings alone. The Thumb-2 emitters always
  chose the 32-bit form of MOV, CMP, LDR and STR, and choosing by
  operand took emitted code from 60.8 to 44.6 bytes a guest instruction.
  That was enough for CoreMark's working set to fit 96 KB: 29,862
  translations became 294, and x650 went from 19.92 s to 11.65 s.
- **14.29** from folding the window guard's bounds and alignment tests
  into one compare: 36.0 bytes a guest instruction, x650 in 10.61 s
  ([backend/thumb2.md](backend/thumb2.md#where-the-bytes-go)).
- **13.94** from keeping one `RETIRE` per stretch between observation
  points: 3,291 four-byte adds became 1,429, 33.9 bytes a guest
  instruction ([jit/README.md](jit/README.md)).
- **14.03** at x650 after the data TLB and the fetch table, which an
  M-mode guest does not use: 13.90 against 14.03 across those commits,
  inside the 13.88-14.17 spread that `.bss` layout alone produces.
- **64 KB the default** once 64 KB served it as well: 14.55 against
  14.59 at x120.

PowerPC's columns moved through the same changes: 125.8 and 35.4 at
96 KB when this table was last measured, 53.63 and 29.14 now.

**Guests that read their image in their loops moved since, and CoreMark
did not.** The bus's read-only cache and then the JIT's image window
([memory.md](memory.md#the-three-kinds-of-region)) took RV32 Dhrystone
at 64 KB from 31.37 to 28.92 to **14.54**, and Whetstone from 43.99 to
40.93 to **32.94**. CoreMark x120 stayed at 14.5-14.7 throughout: it
reads its image 1,143 times a run, so the window never arms for it.

### The interpreters' loops in ITCM

`EMU_INTERP_TEXT_SECTION`, `.itcm` on the F746 only, puts an
interpreter's run loop in the 16 KB of zero-wait ITCM instead of flash
behind the 4 KB I-cache. It started with ARMv7-M
([frontend/armv7m.md](frontend/armv7m.md#the-run-loop-in-itcm-and-the-decode-cache-that-lost))
and was then measured for each of the other three, interpreted,
CoreMark, three function alignments against a worktree at c7b4c5a,
results and `crcfinal` identical:

| interpreted, F746 | flash | ITCM | |
|---|---|---|---|
| ARMv7-M (x40) | 161.9 / 172.9 / 164.3 | 143.6 / 143.8 / 144.3 | **kept**, -11 to -17% |
| RV32 (x40) | 110.25 / 110.29 / 109.88 | 103.06 / 102.96 / 102.92 | **kept**, -6.6% |
| G4MH | 95.13 / 97.57 / 98.89 | 95.02 / 93.86 / 93.84 | **kept**, -0.1 to -5.1% |
| PowerPC (x40) | 212.52 / 208.59 / 215.27 | 224.04 / 219.70 / 219.86 | **not**, +2 to +5% |
| ...with `ppc_load` and `ppc_store` there too | | 214.83 / 214.83 / 216.36 | not |

**What decides it is how much of the loop is inlined.** ITCM is at 0x0
and flash at 0x0800_0000, out of a branch's reach, so every call from
code in ITCM to code in flash goes through a veneer. ARMv7-M's and
RV32's loops have their decode and execute inlined and call out only on
slow paths; PowerPC's calls `ppc_exec`, `ppc_decode`, `ppc_load` and the
address translation for every instruction, and moving the loop needed 28
veneers to reach them. Moving the accessors as well needed 31, because
their own callees then need them, and was no better than flash. G4MH's
smaller gain fits the same reading: its loads and stores are calls too.

Under the JIT the interpreter is the fallback and moves with it: RV32
CoreMark x120 at 96 KB 14.41 to 14.40, G4MH 53.22 to 52.74. ITCM left
after the move: 5.6 KB in the RV32 firmware with the network stack's
flash driver, 2.7 KB in a G4MH one, which is the tightest.

There was an older table here — native ARM against the JIT at five cache
sizes on a 180 MHz M4, quoting 15.3× to 32.3× slower than native. It
described the hand-written Thumb-2 translator, which no longer exists,
and was removed rather than left to read as current. One conclusion from
it survives in a new place: then the JIT lost to the interpreter at
12 KB and the default was moved to 32 KB; now it loses at 32.

## The same three ways, on the x86-64 host

The section above is the board. This is the host runner, which is a
different question: there the JIT competes with a 216 MHz M7 and a code
cache measured in tens of kilobytes, here it has 64 MB and an
out-of-order superscalar to emit for.

Measured at ce02bd0 on a Xeon E5-2470 v2 (2.4 GHz, 3.2 GHz turbo),
`-DCMAKE_BUILD_TYPE=Release` as the README builds it, guests from the
tree's own `RV_GUEST_MARCH` (`rv32imafc_..._zba_zbb_zbc_zbs_zacas`) with
riscv64-unknown-elf-gcc 15.3, the runner with gcc 16.2. Medians of
three. RV32 CoreMark, each row run long enough for CoreMark to call it
valid:

| | iterations | wall | per second | guest MIPS | vs native |
|---|---|---|---|---|---|
| **Native x86-64** | 200,000 | 10.94 s | 18,280 | — | 1× |
| **JIT** | 24,000 | 15.56 s | 1,542 | 386 | 11.9× slower |
| Interpreter | 6,000 | 25.57 s | 235 | 59 | 77.9× slower |

Native is `-DEMU_NATIVE_COREMARK=ON`, the runner compiled *as* CoreMark
at `-O3`. The JIT row is at 24,000 iterations because at 6,000 it now
finishes in four seconds, and CoreMark rejects anything under ten.
250,073 instructions an iteration on both backends; at 6,000 iterations
both give `crcfinal 0xa14c`. Native retires no guest instructions, so
MIPS is not defined for it. `RelWithDebInfo`, the tree's default when
no build type is given, measured 25.28 s and 3.77 s for 6,000
iterations against these 25.57 s and 4.00 s. So `Release` is no faster
here, and on the JIT the two sets overlap (3.63-4.06 s across six
runs); the build type is not what this table is about.

**The JIT is 6.6× the interpreter.** It was 3.7× the last time this
table was written and 1.98× the first, with the interpreter about where
it is now -- 6.96 s for the 6,000 iterations that now take 4.00. That is
many changes since, not one, and this page does not apportion them.

**One instruction falls back, of 6,001,741,696.** This table used to
say 4.6% did -- 136.6M of 3,000.9M -- and called that the floor set by
the SYSTEM and MISC-MEM instructions the translator declines on
purpose. It was not: it was the B extension, which the translator
declined for want of a lowering (see CLAUDE.md, "A capability the old
translator had"), and CoreMark in M-mode reaches SYSTEM only at start-up.
The run makes 281 translations into 177,432 bytes and enters a block
202,366,421 times, about thirty guest instructions an entry.

### All four frontends

Every frontend has a benchmark-sized guest and a JIT. Same runner,
host wall time, medians of three, same checksums on both backends:

| frontend | guest | instructions | interpreter | JIT | |
|---|---|---|---|---|---|
| RV32 | CoreMark, 6,000 iterations | 1,500,449,974 | 25.57 s | 4.00 s | 6.4× |
| ARMv7-M | CoreMark, 600 iterations | 166,870,755 | 3.60 s | 0.53 s | 6.8× |
| PowerPC | CoreMark, 600 iterations | 182,243,221 | 3.76 s | 0.58 s | 6.5× |
| PowerPC | crypto | 2,422,217 | 62 ms | 18 ms | 3.4× |

**Start-up is 6.0 ms interpreted and 6.6 ms under the JIT** (`hello`,
median of nine), which is most of crypto's JIT row: less start-up the
row is about 56 ms against 11, 5×. The same ELF measured 16.5-16.9 ms
under the JIT on runners built at ae5d5bd and 2c6e0c0 as well, so the
12 ms this table once gave is older than any of them, and this page
does not know what moved it.

ARMv7-M's two backends retire different counts -- 166,887,441 under the
JIT, 16,686 more -- and that is a convention rather than a miscount:
the run total counts one more per interrupt taken under the JIT than the
interpreter does ([frontend/armv7m.md](frontend/armv7m.md#not-done)),
and this guest's SysTick fires every ten thousand instructions, which is
16,687 interrupts. Guests that take none -- crypto, `t2exec`, `itblock`,
`alu` -- retire identical counts on both.
PowerPC CoreMark shares ARMv7-M's `ARMV7M_COREMARK_ITERATIONS`, and
both are at 600 here: the 40 this table once used is 12M instructions,
a run start-up is a visible part of.

The interpreters have moved as well as the JITs: ARMv7-M's took 6.12 s
for the same 600 iterations when this table was first written, before
its fetch path stopped asking the MPU out of line (CLAUDE.md, "A lesson
learned in one frontend is not applied in the next").

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
  guest  310.4  host 5408.2  ratio 17.42  cyc 2483.5  c/g  8.00  br  878.0  miss 10.29 M/s | jit 3572 blk 3572 built  2917 KiB 14,749,991 exec
```

`ratio` is host instructions per guest instruction and is the headline:
it is what a translation change moves. `c/g` is the same in cycles,
which is that plus whatever the host is stalling on. After the bar are
the JIT's resident blocks, its translations so far, the code they
occupy and the block entries so far. Host figures need
`perf_event_open`; `kernel.perf_event_paranoid` must be 2 or lower, and
everything derived from an absent counter shows a dash rather than a
number.

At ce02bd0, the Release runner above, the whole-run figure, medians of
five:

| guest | floating point | interpreted under the JIT | ratio | was |
|---|---|---|---|---|
| dhrystone | none | 1 of 9,431,235 | **21.40** | 52.6 |
| whetstone | heavy | 51,216 of 10,665,594 (0.48%) | **26.06** | 34.2 |
| quake, its first 2e9 instructions | heavy | 1 | **16.57** | 46.6 |

The interpreter's figures on the same runs are 127.67 and 141.39.
Quake runs its demo loop until stopped, so it is capped with
`--max-insn 2000000000`, which takes in start-up, the PAK load and
`demo1`.

**The first thing these said was that a plausible theory was wrong.**
The ratio on Quake looked like evidence that SoftFloat dominates --
this tree routes every FP operation through it unless a backend lowers
it natively, and Quake is full of them. In the first version of this
table Dhrystone, which contains no floating point at all, cost *more*
per instruction than Quake. So the ~50x was the JIT's baseline, and it
was not about FP. Quake still says so: it is the cheapest of the three
per instruction.

**Whetstone is now the most expensive, and not because of its
arithmetic.** This paragraph used to say Whetstone being the cheapest
confirmed the native FP lowering. The order changed, so it was
measured rather than re-argued. Callgrind over the JIT run
(RelWithDebInfo, 295M host instructions) gives:

- **21.9%** is `rv_ir_load`, the checked load, with the bus walk under
  it. 3,953 of 3,954 addresses sampled at its entry are in the image at
  0x2000_0000, nearly all in `.rodata`: the float constants newlib's
  `sinf` and `logf` load with `auipc`+`flw`. The JIT inlined guest RAM
  and nothing else, so every one of them was a call. Both halves are
  gone since. The bus walk under the call went into a read-only cache
  ([memory.md](memory.md#the-three-kinds-of-region)), for 4.5% of this
  run's host instructions and 8.9% of Dhrystone's. The call went into
  an inline window over the image
  ([backend/x86_64.md](backend/x86_64.md#the-image-window)), for 11.8%
  more here and 25.9% more on Dhrystone. Measured by callgrind, that is
  about 22.1 and 14.5 host instructions a guest instruction, where this
  table says 26.06 and 21.40.
- **19.5%** is the dispatch loop's own instructions.
- **11.1%** is `rv_hart_fp`, the SoftFloat helper, which takes what no
  backend lowers -- the fused multiply-adds among them.
- 2.3% is the interpreter, for the 51,215 CSR accesses it is handed.

The arithmetic the backend does lower is in the translated code, the
part callgrind cannot name. See [floating-point.md](jit/floating-point.md)
for what each backend lowers and what it declines.

What the ~50x was took two measurements to answer, and the first
guess -- dispatch over very short blocks, 7.1 guest instructions apart
on Quake -- was only part of it.

### What the ~50x was

`--rate` reports host instructions per guest instruction directly,
and on Dhrystone the JIT was **44.21** against the interpreter's
131. A translated ALU operation should be 4-8. (It is 21.40 against
127.67 at ce02bd0; what follows is how the first half of that came
about.)

**40% of guest instructions were a C call.** Every load and store
went to rv_hart_load/rv_hart_store, which walk the region table:
3,728,397 calls in a 9.4M-instruction run. Inlining the ones that
are plain RAM took the ratio to 33.02.

**The rest was the dispatcher**, at about 80 cycles a round trip
for blocks of a handful of instructions -- measured by differencing
two runs that differed only in block entries, not by reasoning.
Linking exits took it to **29.65** and Dhrystone from 0.104 s to
0.046 s, 2.0x to **5.0x** the interpreter.

What is left at 29.65 is the *register file*: every GET is a load
from the cpu struct and every PUT a store, because the allocator
covers temps rather than guest registers.

**Measured, and the answer is no.** `EMU_JIT_HOT_REG_STATS` was
an option nothing read -- declared in CMakeLists, defaulted in
`rv_config.h`, consulted by no line of code -- so the histogram
it names had never existed. It does now, in `emu_regstats.c`,
counting what a block leaves behind *after* the optimiser:

| | Dhrystone | CoreMark | bench |
|---|---|---|---|
| refs to one register in one block: 1 | 67.5% | 67.6% | 54.0% |
| 2 | 21.1% | 25.0% | 26.9% |
| **3 or more** | **11.5%** | **7.4%** | **19.1%** |
| surviving reads | 594 | 1122 | 451 |
| surviving writes | 1300 | 1678 | 910 |
| distinct registers per block | 5.79 | 5.61 | 12.27 |
| busiest register's share | 12.3% | 14.8% | 9.2% |

Pinning costs a load at block entry and a store at exit, so it
breaks even at two references and only pays above them: **7-19%
of the traffic can pay for itself and the rest is a loss.**

**And what survives is writes, by 1.5 to 2.2 to one** -- which is
exactly the traffic a register cache cannot help. This is the
cost model the Thumb-2 attempt lacked, and it explains the 15.5%
rather than contradicting it: a cached read is a MOV where an
uncached one is an LDR, one instruction either way, while a
write must still be written through. `pass_reg_traffic` has
already taken the redundant reads; the residue is stores that a
short block has to make before it exits.

There is also no small hot set to choose: the busiest register is
9-15% and the top eight reach 46-69%, so a three-register pin
covers about a third of a third.

**The histogram is weighted per translation, not per block
entry**, and that limit is stated in the header. It does not
weaken this conclusion -- benefit and cost both scale with
entries, so a register that cannot pay in a block cannot pay in a
hot one -- but it would have to be fixed before any *positive*
result was believed.

Two things about building it are worth keeping.

**The first version reported RV32's `x0` as the busiest register
in every guest**, at 19-24% of all references. A GET of x0
survives the passes deliberately and every backend answers it
with a constant at lowering, so it is not traffic at all -- the
instrument was counting a quarter of its own histogram into a
bucket that cannot be optimised, and would have made a pinned x0
the obvious first choice. `emu_reg_note_block` takes the target
and skips hardwired-zero registers now.

**Its `#include` landed inside `#if EMU_PAIR_STATS`**, an
unrelated option, so the declaration was absent in exactly the
configuration the header was written for and the build failed on
an implicit declaration. A conditional include belongs to its own
condition.

So: **do not build a guest-register allocator.** The lever is longer
blocks.

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

At ce02bd0, the Release runner and machine of the CoreMark table above,
medians of five:

| | self-reported | host wall |
|---|---|---|
| Dhrystone 2.1, `DHRY_RUNS=20000`, interpreter | 9.8 µs/run, 101,594 Dhrystones/s | 206 ms |
| the same, JIT | 1.5 µs/run, 657,009 Dhrystones/s | 39 ms |
| Whetstone 1.2, `WHET_LOOPS=100`, interpreter | 38.4 MIPS | 266 ms |
| the same, JIT | 158.0 MIPS | 70 ms |

Each wall time includes the runner's start-up, 6.0 ms interpreted and
6.6 ms under the JIT. 9,431,2xx and 10,665,593 guest instructions. The
JIT translates essentially all of both: 1 and 51,216 instructions fell
back -- the ratio to read before believing any figure here. Dhrystone's
used to be 120,566, all `sh1add`/`sh2add`/`sh3add`, which the
translator declined until the B extension was lowered; that alone took
the JIT run from 42.5 ms to 35.8 (medians of fifteen, interleaved with
the build before). Whetstone's 51,216 are CSR accesses, and are not
what makes it the costlier of the two per instruction -- see the
profile in [the section above](#what-a-guest-instruction-costs-measured-live).

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
| native x86-64 | 0.653 ms (not re-run) |
| RV32 on the JIT | ~63 ms (70 less 6.6 ms of start-up, at ce02bd0) |
| RV32 on the interpreter | ~260 ms (266 less 6.0) |

so **97× native on the JIT and 398× on the interpreter**, and 168 MIPS
against 41 MIPS in guest instructions retired. The JIT row was 72 ms,
110× native, when this table was first written.

**That 97× is not an emulation-overhead figure, and reading it as one
would be wrong.** Modules 7 and 11 are 99.4% of the guest instruction
count and are `sinf`/`logf`/`expf`/`sqrtf`, so the two runs do the same
*benchmark* work through two different libm implementations on two
different instruction sets: x86-64 has `sqrtss` as one instruction and a
vectorising compiler above it, where the guest runs newlib's software
argument reduction compiled for RV32. The emulator is being asked to
execute far more instructions for the same result, and 97× is the
product of that and the per-instruction cost.

The figure that isolates the emulator is the second one: **168 million
guest instructions a second** on this host, with 99.5% of them
translated. Comparing that against the interpreter's 41 MIPS is the
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

**The IR backends inline it again**, on Thumb-2, for RV32. Measured on
the F746, ticks per kernel, the JIT before and after, against each
kernel's RAM control:

| kernel | helper | inlined | speedup | vs its RAM control |
|---|---|---|---|---|
| `read` | 70,009 | **28,295** | 2.47x | 2.2x |
| `write` | 42,092 | **8,938** | 4.71x | 1.41x |
| `rmw` | 94,044 | **13,509** | 6.96x | 1.46x |
| `poll` | 51,188 | **9,362** | 5.47x | 1.27x |

38.73 to **13.15** cycles per guest instruction overall, and 724 of the
run's roughly 800,000 peripheral accesses still reach the bus -- the ones
made before arming and the refused ones the checks provoke. CoreMark,
which drives no peripheral and never arms, measures 13.88-14.14 across
three layouts on both sides of the change.

Before, at 91df012, against the interpreter on the same board -- the
JIT was 2.2x the interpreter on peripheral access, and every access left
the block through the checked accessor and a bus walk (MMIO / RAM
control, ticks per kernel):

| kernel | interpreter | JIT |
|---|---|---|
| read | 152,233 / 101,144 | 70,009 / 11,532 -- **6.1x** |
| write | 94,330 / 70,624 | 42,092 / 6,801 -- **6.2x** |
| rmw | 205,154 / 124,091 | 94,044 / 9,206 -- **10.2x** |
| poll | 128,149 / 96,248 | 51,188 / 7,307 -- **7.0x** |

The window is an identity map, so it is the guest-RAM fast path with
the host address equal to the base: the same rotate-and-compare guard,
then the access. A store also tests each read-only hole (PWR, the RCC
clock tree, the flash controller) and leaves for the helper on a match,
which faults. It is armed only once a guest has made `EMU_JIT_PT_ARM_AT`
(64) passthrough accesses through the bus, and arming flushes once
between slices -- always-on cost the hand-written translator a CoreMark
image past its cache. The window itself is built from the bus by
`emu_ir_pt_window`: the longest run of adjacent, identity-mapped,
readable passthrough regions, at most four holes.

`mmiobench` runs its correctness checks twice now, before the kernels
and after: the first finishes before arming, on the helper alone, and
only the second puts the holes to the inlined path. A board mutant that
drops the hole tests fails exactly the two hole checks, eight times
each, in the second pass only.

What follows is the hand-written translator's measurement, on the F446.

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

**PMP mapped onto the ARM MPU** is **not possible**: the MPU cannot tell
a guest access from an emulator access, because the JIT's inlined load
*is* both.

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

Remaining headroom, as this was written in the F446 days, in rough order of
expected value: allocating guest registers to ARM registers across a block,
chaining blocks so a hot loop stops returning to the dispatcher, and teaching
the translator `DIV`/`REM`/`MULH`. Chaining and the divides were done; the
register allocator was measured and is a loss ([above](#what-the-50x-was)).

---
