# Validation

Each frontend is held against something that is not this code, and what
that something is differs for each — which is most of what this page is
about.

| frontend | reference | where |
|---|---|---|
| RV32 | the official architecture suite (Sail computes the expected values), the Berkeley suite | below |
| G4MH | **none.** Renesas' assembler as a second encoder, and compiled guests | [frontend/g4mh.md](frontend/g4mh.md) |
| ARMv7-M | **a Cortex-M7**: the same programs on the Nucleo-F746ZG and on the emulator | [frontend/armv7m.md](frontend/armv7m.md) |
| PowerPC | binutils for every encoding; a Python model on unbounded integers; exact rational arithmetic for the FP unit. **No e200** | [frontend/ppc.md](frontend/ppc.md) |

```sh
./scripts/run-arch-test.sh       # official riscv-arch-test
./scripts/run-riscv-tests.sh     # Berkeley suite
./scripts/build-matrix.sh --test # every configuration, and its ctest
./scripts/report-figures.sh      # the host figures, regenerated
```

## RV32: two suites, and run both

They cover different things. A regression that only the Berkeley suite
catches will sit unnoticed if only arch-test is run — which is exactly
what happened to `rv32mi/csr` when F was added — and it runs the other
way too: four `ExceptionsSv` tests failed for six days while riscv-tests
stayed at 77/77, because its guests never make a misaligned access.

Keep the suites' `-march` in step with what `misa` advertises:
`rv32mi/csr` deliberately fails when built without F and run on a core
reporting F, and that failure looks like an emulator bug until you
disassemble the test.

Both are wired to CMake targets as well:

```sh
cmake --build build/host --target arch-test        # official RISC-V suite
cmake --build build/host --target arch-test-quick  # base integer only
cmake --build build/host --target riscv-tests      # Berkeley suite
cmake --build build/host --target validate         # everything
```

Current state:

| | result | runs on |
|---|---|---|
| `riscv-arch-test`, interpreter | **510 / 510** | host |
| `riscv-arch-test`, `EMU_EXTRA_ARGS=--jit` | **510 / 510** — the whole suite through translated code | host, x86-64 |
| `riscv-arch-test --misaligned`, both backends | **523 / 523** — the default build, `RV32_MISALIGNED=ON`, and the four `Misalign*` suites it qualifies for | host |
| `riscv-tests`, interpreter | **77 / 77** | host |
| `riscv-tests`, `--jit` through an `EMU_HOST` wrapper | **77 / 77** | host, x86-64 |
| `ctest`, the default host tree -- all four frontends (`host-all`) | **56 tests**: unit, the guests of every frontend, most of them again under `--jit`, and `gdb-check`. 20 of them failed in this configuration until their flat images named a frontend: a `.bin` carries no architecture and runs as RV32 | host |
| `ctest`, RV32 alone | 20 tests | host |
| `scripts/gdb-check.sh`, the `gdb-check` ctest | a real gdb attached to all four frontends, interpreted and under `--jit`: pc at a breakpoint against the ELF symbol, the stop reply's register number against gdb's, a register round trip, `stepi` landing on the next instruction, a breakpoint ahead hit, memory -- **8 of 8 sessions** | host, x86-64, gdb-multiarch 17.2 |
| the ARMv7-M guests, both backends | all eight pass, each with the host's retired count to the instruction | Nucleo-F746ZG |
| `isatest`, JIT | **937 checks**, 96,995 retired -- the count the x86-64 host gives, which interprets 2,710 of them (2026-10-25) | Nucleo-F746ZG and Nucleo-F446RE, Thumb-2 |
| `isatest`, interpreter | **937 checks**, 96,883 retired, as on the host | Nucleo-F446RE |
| `fuzz-rv32-jit`, ten minutes ([fuzzing.md](fuzzing.md)) | 157,768 inputs, 150,680 compared between the interpreter and the JIT, no divergence once four defects were fixed; 7.8M translations and 9.8M block entries, so the JIT ran | host, x86-64, clang 21 under ASan and UBSan |
| `fuzz-armv7m-jit`, twenty minutes | 666,479 inputs, 622,915 compared, no divergence once three defects were fixed; 827,853 translations, 12.1M block entries (2026-10-26) | host, x86-64, ASan and UBSan |
| `fuzz-g4mh-jit`, twenty minutes | 134,211 inputs, 119,843 compared, no divergence once one defect was fixed; 12.5M translations, 32.8M block entries | host, x86-64, ASan and UBSan |
| `fuzz-ppc-jit`, twenty minutes | 1,845,065 inputs, 1,771,551 compared, no divergence once one defect was fixed; 113 ended in a timer interrupt and are not compared; 2.1M translations, 112M block entries | host, x86-64, ASan and UBSan |
| `fuzz-ir`, ten minutes on four workers ([fuzzing.md](fuzzing.md#the-ir-with-no-frontend)) | 5,658,718 random IR blocks, 5,595,340 compared between the reference on the block as built, the reference after the passes, and the x86-64 lowering; no divergence once three defects were fixed (two in the reference, one in `pass_reg_traffic`); eight mutants found (2026-10-26) | host, x86-64, ASan and UBSan |
| `fuzz-rv32-mmu`, ten minutes on four workers ([fuzzing.md](fuzzing.md#translation-and-protection-rv32)) | 6,196,844 inputs, 65.9M loads and stores through Sv32, PMP and the data TLB against a walk written from the spec, 17.6M data TLB entries checked; no divergence once two defects were fixed (a PPN past the bus, an M-mode straddle of an unlocked PMP entry); thirteen mutants found (2026-10-27) | host, x86-64, ASan and UBSan |
| `fuzz-g4mh-mpu`, ten minutes on four workers ([fuzzing.md](fuzzing.md#protection-g4mh)) | 1,107,316 inputs, 8.39M instructions against the U2B manual's MPU on both backends -- 3.43M completed, 1.34M MIP, 1.55M MDP (670,524 by the MPU), 2.08M MAE; 38,501 fetches across a word into another area's decision, 560,779 setting checks; no divergence once seven defects were fixed, all found writing the reference; 24 mutants found (2026-10-30) | host, x86-64, ASan and UBSan |
| `fuzz-armv7m-mpu`, ten minutes on four workers ([fuzzing.md](fuzzing.md#protection-armv7-m)) | 1,256,489 inputs, 12.6M instructions against the ARM ARM's MPU pseudocode on both backends -- 4.67M completed, 1.61M MemManage (1.28M IACCVIOL, 687,199 DACCVIOL), 766,738 HardFaults escalated, 702,575 unaligned accesses across a region's end; no divergence and no defect; 15 mutants found (2026-10-30) | host, x86-64, ASan and UBSan |
| unaligned accesses across an MPU region's end (`sys.c` `mpu-ua-*`) | MemManage at the first refused byte, a store's bytes before it written -- the emulator agrees, interpreted and under the JIT; 70 of 70 system tests (2026-10-30) | Nucleo-F746ZG |
| ARMv7-M, sp and pc as operands ([frontend/armv7m.md](frontend/armv7m.md#what-the-board-corrected)) | 1,935 register variants and 69 named encodings asked: every one the board refuses, both decoders now refuse; what still differs is a value derived from sp or a code address, which the two images place differently. 36 answers held by `test_sp_pc_board_answers` | Nucleo-F746ZG |
| ARMv7-M's two decoders, `ARMV7M_UNDEF_FULL=1` | all 402,712,576 encodings agree, 215,972,741 of them UNDEFINED | host |
| after the sp/pc rules and the fuzzers' fixes (2026-10-26) | ARMv7-M `alu`, `t2exec`, `crypto` and CoreMark (`crcfinal 0x65c5`, 11,145,477 retired, the host's count) under the JIT and `alu` interpreted; PowerPC `sys` 184 checks and `intc` 49, no failures, and CoreMark valid at 12,170,464 retired, the host's count | Nucleo-F746ZG, Thumb-2 |
| `probes/instret.S` | a trapping instruction does not retire: Sail and Spike agree on minstret; mcycle follows Spike's answer, Sail's is permitted | host, against Sail and Spike |
| CoreMark x650 at a 32 KB cache, after the chaining fix | "Correct operation validated", `crcfinal 0x25e3`, 8,485 compactions; 78.56 s by the host, 79 by the firmware | Nucleo-F746ZG |
| `isatest` with the Thumb-2 chaining fix reverted | fails `fs-dirty-flw-chained` (got 2, want 3) and nothing else -- the board had the defect | Nucleo-F746ZG |
| Linux 6.12 under OpenSBI, `scripts/run-linux.sh` | boots to userspace; the test init passes **17 checks** from U-mode on the extensions the device tree declares | host, x86-64 JIT |
| CoreMark, 120 iterations | `crcfinal 0xd340` on the interpreter and on the JIT at four cache sizes | Nucleo-F746ZG |

`run-arch-test.sh` **builds its own runner**, with `-DRV32_MISALIGNED=OFF`,
because the suite validates the core against a description of it and
that description says misaligned accesses trap. A stale runner at the
path a script defaults to once hid a real regression for six days.
`--misaligned` builds the *default* configuration instead, and runs it
against a description derived for it; see below.

### What the host suites do not cover

**The Thumb-2 lowering**, which compiles only for ARM. The x86-64 backend
exists so that everything *above* it — each translator, the IR, the
passes, the framework — runs under the suites on a host; what turns IR
into Thumb-2 is validated by flashing a guest and reading the UART.

That gap is not theoretical. Every defect below was found on hardware,
and none could have been caught by a signature-checking suite on a host:

| defect | how it presented |
|---|---|
| inlined store ignored PMP | a protected region was writable under the JIT only |
| inlined store skipped the LR/SC reservation break | a later `SC` wrongly succeeded |
| loop cap compared the wrong register | *no* wrong answer — chained loops simply ran unbounded |
| loop chaining dropped past a branch's reach | *no* wrong answer — one loop shape ran 2.4× slower |
| `rm=dyn` resolved `RMM` as round-to-nearest | ties rounded to even where the guest asked for away |
| `mstatus.FS` decided at translation | FP ran after the guest turned the FPU off |
| PMP flush watched a flag, not the configuration | a store landed in memory PMP had been told to deny |
| a shift by zero encoded as a shift by 32 | two architecture tests, on the board only |
| a compaction caused by an overflow unlinked nothing | a hard fault, or a hang: RV32 CoreMark at 120 iterations never finished |
| the cycle counter wrapped | *no* wrong answer — guest time restarted every 19.9 s, and every long run's performance figure was a wrap short |

Three of those produced no wrong answer at all, only numbers that made
no sense. Three were staleness: a decision taken when a block was
translated, still in force after the state behind it changed. The
self-test grew from 148 checks to 298 chasing them (it is 748 now, the
rest being the extensions, the suites below and the B extension), and the checks that
matter are the ones that re-execute *one* instruction at *one* address
after changing the state it was compiled against — a fresh call site is
translated against the current configuration and proves nothing.

The last two rows are from the framework and the platform rather than
from the emitter, and they are why "run it on the board" means a guest
large enough to fill the code cache and a run long enough to wrap the
counter: `isatest` does neither, and passed throughout both.

**The B extension's 230 checks are the board's only view of its
Thumb-2 lowering.** The architecture suite covers Zba/Zbb/Zbc/Zbs
properly -- and, since they are translated, now covers the translator
too, under `--jit` -- but only on x86-64. So `isatest` holds every one
of them to `tests/guest/bitmanip-expect.py`, a model written from the
specification with unbounded integers, at the inputs where the two
hosts disagree with the guest (a count of zero, an amount of 32, the
sign boundary), with every register aliasing. Thirty-four mutants of
the lowering, its helper, the shared arithmetic and the interpreter
were run against it: every one that changes an answer fails on the
backend it changes. Five pass everywhere, and none of them can change
an answer -- `bexti` and `bext` through an arithmetic shift are still
one bit after the `& 1`, a rotation by `32 - 0` is masked to 0 by both
emitters, `ror` by zero written as `v | v` is `v`, and one was a
mutant of the helper that, as written, could not alter its result.

## Floating point (F and D)

Both are implemented — the 64-bit register file with NaN-boxing,
`fcsr`/`frm`/`fflags`, `mstatus.FS`, all of OP-FP in both widths, the
fused multiply-adds, the loads and stores, and `Zcf` and `Zcd`, which
`C` on RV32 is defined to include alongside F and D.

There is one implementation. **Berkeley SoftFloat is the FP unit** — not
an option, and a missing checkout is a configure error rather than a
fallback. It is the library the RISC-V FP spec was written against, and
the fit is exact rather than convenient: its rounding modes are
numerically identical to `frm` and its exception flags to `fflags`, so
neither needs translating, and its `mulAdd` is a genuine single-rounding
fused multiply-add.

It was once a build option beside a host-FPU path through `<fenv.h>`,
and that path was the default. It passed 172 of 224 F tests: the flags
it could report were the ones the hardware happened to raise, which
differ from RISC-V's rules on the fused multiply-adds and around
subnormals. Worse, the documented way to select SoftFloat named a
variable that did not exist, so the check that would have shown the
difference changed nothing and reported nothing. That history is why
`scripts/check-doc-flags.sh` exists.

The JIT lowers the five IEEE-exact single-precision operations to the
host FPU and sends the rest to the same SoftFloat routines the
interpreter uses; see [jit/floating-point.md](jit/floating-point.md).

**Zcb is implemented and passes 7/7.** It reuses the `funct6=100111` slot that
RV64 spends on `c.subw`/`c.addw`: bits [6:5] select `c.mul` or a group of unary
operations, and the byte/halfword accesses sit in quadrant 0 under `funct3=100`.
Three of the unary ops (`c.sext.b`, `c.zext.h`, `c.sext.h`) expand to Zbb
instructions, which is why the spec makes Zcb depend on Zbb — without it there
would be nothing to expand them into.

## Official RISC-V Architecture Test Suite — 510/510

[`riscv/riscv-arch-test`](https://github.com/riscv/riscv-arch-test), the RVCP
suite governed by RISC-V International. Modern versions are self-checking: the
build runs the **Sail golden model** to compute expected results and bakes them
into each test, which then reports `RVCP-SUMMARY: TEST PASSED/FAILED` and sets
its exit status.

Our device description lives in
[`tests/arch-test/`](../tests/arch-test/rv32cortex-m-rv32) — a UDB
configuration, a Sail model configuration, the `RVMODEL_*` macros and a linker
script — and is version controlled with the emulator rather than inside a
third-party clone. `scripts/run-arch-test.sh` fetches the suite, the Sail model
and the UDB gems, then builds and runs.

Prerequisites beyond the normal toolchain: `uv`, Ruby, and Bundler
(`gem install --user-install bundler`).

**What 510 covers is what the script names.** ACT selects suites by
*directory name*, so a suite nobody names is a suite nobody runs,
whatever the core implements. The total stood at 378 for months, and
that was a statement about thirty directory names:

| | tests | how it came to be named |
|---|---|---|
| the thirty | 378 | |
| Zicond, Zihintpause, Zihintntl, `ZihintntlZca`, Zihpm | 13 | the extensions were added |
| `Zcf`, `Zcd`, `ZcbM`, `ZcbZbb`, seven more PMP directories, `SvZicbo`, `SvPMPZicbo`, two `ExceptionsSv*` | 99 | listing the checkout to find `ZihintntlZca` |
| Zicbop | 6 | claimed -- three in its own directory, and three that had been sitting in `SvZicbo` and `PMPZicbo`, already named, waiting for the description to say the extension was there |
| Zfhmin, ZfhminD, Svinval | 14 | the extensions were added. Sscofpmf, after them, has no directory: [frontend/rv32.md](frontend/rv32.md#the-small-extensions) has the probe that stands in for one |

That last row is both halves of the mechanism in one line. Naming a
directory *offers* its tests; each test's own requirements, checked
against the UDB description, decide whether it is built. A suite can be
named and still be silently short.

The 99 were for things this core had implemented for a long time.
**Thirteen of them failed the first time they ran**, and every one was
a defect in the emulator; they are in the table at the end of this
page. Both groups were confirmed the usual way, by breaking the
emulator and watching exactly the right tests fail.

**A build without PMP is a configuration the matrix checks**
(`host-nopmp`, `-DRV32_EXT_PMP=OFF`). `isatest` and `sv32bench` hung
there the first time it was built: a test relied on a PMP fetch fault to
leave S-mode, and both wrote PMP CSRs that do not exist. The guests take
the option now, as they take `RV32_MISALIGNED`, and without PMP
`isatest` checks instead that reading and writing a PMP CSR is an
illegal instruction.

When the checkout moves, list `tests/rv32i`, `tests/priv` and
`tests/priv/pmp/pmp32` and account for every directory: the comment
above the list in `scripts/run-arch-test.sh` names each one that is
left out and why.

### The default build, and the four suites only it can take

The description above is of an `RV32_MISALIGNED=OFF` core, and the
emulator's default is ON -- picolibc's word-at-a-time `strcmp` reads
`lw` off an odd pointer. So for as long as that default has stood, the
build everybody runs was described by nothing, and `Misalign`,
`MisalignD`, `MisalignF` and `MisalignZca` -- thirteen tests, each
requiring `MISALIGNED_LDST: true` -- could not be offered at all. That
mattered more than the count suggests: the misaligned path had just been
found wrong under paging, by a hand-written test, with nothing in the
suite able to see it.

`scripts/run-arch-test.sh --misaligned` builds the default configuration
into `build/arch-test-host-misaligned` and runs it against a second
description, `rv32cortex-m-rv32-misaligned`, which
`tests/arch-test/derive-misaligned.py` *generates* from the first. The
two differ in one property and agree in every other, and two hand-kept
copies of an 18 KB Sail config would stop agreeing the first time one of
them was edited; the script asserts the match count of every line it
changes, so a base edit that moves one fails there instead of yielding a
description that still says "raises". What it changes:

| | value | because |
|---|---|---|
| `MISALIGNED_LDST` | true; Sail `load_store: None` globally and in every region | loads and stores complete, and the bus hands a device a misaligned access as it is |
| `MISALIGNED_MAX_ATOMICITY_GRANULE_SIZE` | 0 | `mem_wide` splits an access into aligned bus transactions, and per page under Sv32 |
| `MISALIGNED_SPLIT_STRATEGY` | `sequential_bytes` | every part is checked before any is performed, and a fault names the lowest part refused |
| `MISALIGNED_LDST_EXCEPTION_PRIORITY` | high, unchanged | AMOs and LR/SC still raise, and check alignment first; UDB allows high only with a zero granule |

**523/523 on both backends**: the 510 and the thirteen. The thirteen were
then run against the OFF build and all thirteen failed, which is what
says they test the property and not merely run.

Four extensions the core implements have **no suite at all**: Zalasr,
Zawrs, Sscofpmf and Zkt. The first three are held by probes against
the reference models -- [below](#probes-and-the-reference-models) --
and Zkt, a promise about timing, by an examination of every path its
instructions take ([frontend/rv32.md](frontend/rv32.md#profiles)). And
three of the defects the 99 led to are held by no architecture test
either, only by `isatest`: the suite is a floor.

The run is of the suites named and nothing else. It used not to be:
`run_tests.py` executes every ELF under the directory it is given, and
the work directory keeps whatever any earlier run built, so
`--extensions I` -- the `arch-test-I` ctest -- ran 39 tests from a fresh
checkout and 496 after one full run, and its name was a statement about
the history of a directory. The script now hands it a directory of
links to the named suites' ELFs, prints how many the work directory
holds besides, warns about a named suite that built nothing, and fails
if none of them built anything.

## Probes and the reference models

A probe is a short bare-metal program in `tests/arch-test/probes/` that
asks a question no suite asks, and leaves the answers in registers.
`scripts/run-probe.py` builds it, runs it on the emulator and on every
reference model it names that is installed, and compares the registers
it lists:

```sh
./scripts/fetch-spike.sh                              # once: build/spike
./scripts/run-probe.py tests/arch-test/probes/zalasr.S
ctest -L probe                                        # all of them, both backends
```

| probe | asks | Sail | Spike |
|---|---|---|---|
| `zawrs.S` | WRS.NTO and WRS.STO in M and below M under TW, with and without a reservation | agrees | differs on one case, permitted |
| `zalasr.S` | every load-acquire and store-release, sign extension, misaligned, unmapped, the eight reserved encodings, an SC after a store-release | has no Zalasr | differs on one register, permitted |
| `sscofpmf.S` | which bits of `mhpmevent3h` exist, `mideleg`, `sie`, `scountovf` and `sip` from S, STI against LCOFI | agrees | agrees |
| `sstvala.S` | `stval` for 23 traps from S to S under Sv32: 16- and 32-bit illegal instructions, ones illegal by FS, TVM, TW, TSR or `mcounteren`, page and access faults on fetch, load and store, an instruction straddling pages, misaligned LR and AMO, an S-mode trigger | agrees but for two, permitted: no Sdtrig, and WFI completes | agrees |
| `umode.S` | every privileged instruction from U-mode with TW, TSR and TVM clear | agrees | agrees |
| `pmpstraddle.S` | an M-mode doubleword half inside an unlocked PMP entry, and the same entry locked | agrees | agrees |
| `sv32ppn.S` | a leaf whose PPN reaches past 4 GiB -- PTE bit 31 and bit 30, a megapage, a pointer to a table there -- and a table inside the CLINT | agrees, but for the CLINT, permitted: a PMA, and Sail's walk refuses MMIO | agrees |
| `sdtrig.S` | when a breakpoint trigger may match: M with MIE clear and set, S with breakpoints to M or to S and SIE clear or set, U | has no Sdtrig | agrees |

**Two models, because one reading is not a check.** Sail is the suite's
golden model and does not have everything this core implements: Zalasr
is in neither 0.13.1, 0.15 nor the 2026-10-12 nightly. Spike
(riscv-isa-sim, BSD-3-Clause) has it, and where both have an extension
they can disagree with each other -- on Zawrs they do. Neither is in the
tree: Sail comes from `run-arch-test.sh`, Spike from
`scripts/fetch-spike.sh` at a pinned commit, both into `build/`. This
project takes nothing from GPL code, even as an oracle; Spike is not
GPL.

**A difference the specification allows is declared, not hidden.** Each
probe lists them (`probe-permitted:`) with the reason, and the runner
prints them and passes. Three so far are cases the specification
leaves to the implementation: whether an SC fails after the hart's own
store to the reserved word, and how long a `wrs.nto` or a WFI below M
may take before TW makes it illegal. The fourth is a model lacking the
extension: Sail has no Sdtrig. Anything else that differs fails.

**The two newest found four defects on their first run** -- MRET
executing in S-mode and U-mode, WFI parking the hart from U-mode, a
16-bit instruction's illegal trap naming its 32-bit expansion, and
triggers matching only on their `m` bit
([frontend/rv32.md](frontend/rv32.md#profiles)). Both suites passed
with all four in place.

**A model's configuration is part of its answer**, as this page already
says of `sail.json`. Spike makes `mcounteren`'s counter bits writable
only when Zicntr and Zihpm are in its `--isa`; without them S-mode's
`scountovf` read 0 there and 0x80 here, which looked like a defect in
either until Spike's CSR mask was read. Each probe names Spike's ISA.

The ctest entries skip -- not pass -- when no model the probe names is
installed: a probe with nothing to compare against has said nothing.

## riscv-tests — 77/77

The older Berkeley suite: `rv32ui`, `rv32um`, `rv32ua`, `rv32uc` and `rv32mi`.
All of it passes, none skipped — `rv32mi/pmpaddr` once PMP was implemented,
and `rv32mi/breakpoint` once Sdtrig was.

**From 2026-09-16 to 2026-10-16, 64 of the 77 could not fail.** The
tests end with an `ecall` carrying the verdict, and the `rv32u*` ones make
it from U-mode. 1babe4e made the runner's exit call M-mode only, so a
Linux process's `write(2)` reaches its kernel -- and from then on those
`ecall`s trapped to the tests' own handler, which writes the verdict to
`tohost` and loops: the suite's own protocol, which this emulator does not
watch. The run went to the cap, a capped run exits 0, and a failing test
loops in the same place with a different value. With `ADD` broken, the
script reported `rv32ui` 42 of 42.

`run-riscv-tests.sh` reads the verdict where the test leaves it now: a run
at the cap must be parked in `write_tohost`, and `gp` is what it wrote --
1 for a pass, `(sub-test << 1) | 1` for a failure, the code the `ecall`
would have carried. Anywhere else at the cap is a hang. Against the same
broken `ADD`: 15 failures, `add` at sub-test 8 among them. 77 of 77
interpreted and under the JIT, and through `scripts/emu-board` over the
host's link.

It has no `rv32uf`, so it contributes nothing to FP coverage; that comes
entirely from arch-test's `F` family. Two of its tests are load-bearing in a
way the count hides: `rv32mi/csr` fails on purpose when the runner's `-march`
disagrees with what `misa` advertises, and `rv32mi/breakpoint` is the single
test standing between the default build and the 29% interpreter gain that
compiling Sdtrig out would give.

## Bugs these suites caught

Worth recording, because each was a genuine defect:

| Found by | Defect |
|---|---|
| unit test vs. assembler ground truth | `C.ADDI4SPN` took its destination register from bits `[9:7]` instead of `[4:2]`, corrupting every guest stack-frame address computation |
| `riscv-tests` `instret_overflow` | a CSR write to `minstret` must *replace* that instruction's increment, not be followed by it |
| `riscv-arch-test` `Zicntr` | the Sail config declared a clock tick every 100 instructions while the emulator ticks every instruction |
| `riscv-arch-test` `Zacas` | the Sail config declared `atomic_support: AMOArithmetic` on guest RAM, so the golden model **trapped** on `amocas` and baked trap-derived values into the signatures — three sessions were spent looking for an emulator bug that was never there |
| `riscv-tests` `rv32mi/csr` | the suite was built without F while `misa` advertised it. The test detects exactly that mismatch and fails on purpose; the emulator was correct and the runner's `-march` was not |
| hardware `isatest` | the JIT's inlined store wrote guest RAM without consulting PMP, so a protected region was writable under the JIT and not under the interpreter |
| `riscv-arch-test` `PMPSm`, under `--jit` | **the optimiser deleted a load nothing read.** A load whose destination was overwritten later in the block was never executed: no fault, and on a device no read -- `(void)UART->DR;` did nothing. Passed interpreted, failed translated; reproduced on the F746, where an APLIC claim discarded that way left the interrupt pending |
| `riscv-arch-test` `PMPSm` | `pmpaddr` stored 32 bits where a 32-bit bus allows 30, so it reported a boundary nothing could reach |
| `riscv-arch-test` `PMPF`, `PMPZca` | `fld`/`fsd` are two word accesses, each aligned, so a double at an address 4 mod 8 never raised address-misaligned on a core built to report it |
| `riscv-arch-test` `ExceptionsSvZalrsc` | a failing `sc.w` returned "reservation lost" for an address with no memory behind it, where any store reports an access fault |
| `riscv-arch-test` `SvZicbo`, `PMPZicbo` | cbo.clean, cbo.flush and cbo.inval took the guest's *virtual* address straight to the bus, and were not checked against PMP |
| guest `isatest`, written to hold the above | `menvcfg` and `senvcfg` were stored, read back, and consulted by nothing: every cache-block operation ran at every privilege |

### What the second backend found in the first runner

`rv32mi/scall` and `rv32ui/ma_data` hung under `--jit`, and the bug was
not in the translator: it was in the host runner's instruction cap, which
had been correct for as long as there was only one backend.

The runner sized each slice as `max_insn - total`. A backend may retire
*more* than the budget it was given — the JIT executes whole translated
blocks and can only stop between them, so the last one overshoots. `total`
then passes `max_insn`, the unsigned subtraction goes below zero into a
very large number, the budget never reaches zero, and the loop never ends.
The guest kept running perfectly; the cap simply stopped existing.

The interpreter retires one instruction at a time and lands exactly on the
cap, which is why this was invisible for the entire life of the project.
It is only reachable by the two tests that *rely* on the cap to terminate
— they spin in `write_tohost` by design — and only with a backend that
retires in blocks. The ARM firmware's loop is unaffected: it runs a fixed
slice with no cap, so there is no subtraction to underflow.

### What the Sv32 tests found

Declaring Sv32 built 37 more tests than S-mode alone, and getting them to
pass took two config fixes and three code fixes. The config half is worth
recording because both entries were *correct* before paging existed:

| `sail.json` field | was | why it had to move |
|---|---|---|
| `supports_pte_read` on guest RAM | `false` | a page table lives in ordinary RAM. With it false the golden model fails the PTE *read*, reports an access fault where the architecture calls for a page fault, and bakes that into the signature |
| `xtval_nonzero.*_page_fault` | `false` | the three page-fault causes report the faulting virtual address, and the UDB config already said so — the two models have to agree |

Both are the same shape as the `Zacas` row above: a value that described
"this cannot happen" outlived the thing that made it true.

The code half was three genuine gaps, none of them reachable before there
was a page table: non-leaf PTEs did not reject the reserved `D`/`A`/`U`
bits, a PMP-denied PTE read reported a *load* access fault rather than one
matching the access that caused the walk, and `menvcfg`/`menvcfgh` were
not implemented at all — so the framework's prolog took an
illegal-instruction trap the reference did not, and every later signature
disagreed about the privilege it was in.

The RVC expansion table in [`tests/unit/test_decode.c`](../tests/unit/test_decode.c)
is assembler-derived, not hand-computed: each entry was produced by assembling
the compressed form and its 32-bit equivalent and reading both back with
`objdump`.

---
