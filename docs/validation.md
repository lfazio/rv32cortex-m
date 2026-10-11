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
| `riscv-arch-test`, interpreter | **490 / 490** | host |
| `riscv-arch-test`, `EMU_EXTRA_ARGS=--jit` | **490 / 490** — the whole suite through translated code | host, x86-64 |
| `riscv-tests`, interpreter | **77 / 77** | host |
| `riscv-tests`, `--jit` through an `EMU_HOST` wrapper | **77 / 77** | host, x86-64 |
| `ctest`, RV32 tree | 19 tests: unit, the guests, and most of them again under `--jit` | host |
| `isatest`, JIT | **491 checks**, 1,116 of 58,255 instructions interpreted — the same two numbers the x86-64 host gives | Nucleo-F746ZG, Thumb-2 |
| CoreMark, 120 iterations | `crcfinal 0xd340` on the interpreter and on the JIT at four cache sizes | Nucleo-F746ZG |

`run-arch-test.sh` **builds its own runner**, with `-DRV32_MISALIGNED=OFF`,
because the suite validates the core against a description of it and
that description says misaligned accesses trap. A stale runner at the
path a script defaults to once hid a real regression for six days.

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
self-test grew from 148 checks to 298 chasing them (it is 491 now, the
rest being the extensions and the suites below), and the checks that
matter are the ones that re-execute *one* instruction at *one* address
after changing the state it was compiled against — a fresh call site is
translated against the current configuration and proves nothing.

The last two rows are from the framework and the platform rather than
from the emitter, and they are why "run it on the board" means a guest
large enough to fill the code cache and a run long enough to wrap the
counter: `isatest` does neither, and passed throughout both.

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

## Official RISC-V Architecture Test Suite — 490/490

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

**What 490 covers is what the script names.** ACT selects suites by
*directory name*, so a suite nobody names is a suite nobody runs,
whatever the core implements. The total stood at 378 for months, and
that was a statement about thirty directory names:

| | tests | how it came to be named |
|---|---|---|
| the thirty | 378 | |
| Zicond, Zihintpause, Zihintntl, `ZihintntlZca`, Zihpm | 13 | the extensions were added |
| `Zcf`, `Zcd`, `ZcbM`, `ZcbZbb`, seven more PMP directories, `SvZicbo`, `SvPMPZicbo`, two `ExceptionsSv*` | 99 | listing the checkout to find `ZihintntlZca` |

The 99 were for things this core had implemented for a long time.
**Thirteen of them failed the first time they ran**, and every one was
a defect in the emulator; they are in the table at the end of this
page. Both groups were confirmed the usual way, by breaking the
emulator and watching exactly the right tests fail.

When the checkout moves, list `tests/rv32i`, `tests/priv` and
`tests/priv/pmp/pmp32` and account for every directory: the comment
above the list in `scripts/run-arch-test.sh` names each one that is
left out and why.

Two extensions the core implements have **no suite at all**: Zalasr and
Zawrs. Zalasr is not in Sail 0.13.1 either, so there is no golden model
for it; it is covered by `tests/guest/isatest.c` alone. Zawrs is in the
model, and `tests/arch-test/probes/zawrs.S` asks it seven questions
directly -- see [frontend/rv32.md](frontend/rv32.md). And three of the
defects the 99 led to are held by no architecture test either, only by
`isatest`: the suite is a floor.

The runner executes every ELF in the work directory, not only the
suites named on the command line: naming one still runs everything
that has ever been built there. The list decides what is *built*.

## riscv-tests — 77/77

The older Berkeley suite: `rv32ui`, `rv32um`, `rv32ua`, `rv32uc` and `rv32mi`.
All of it passes, none skipped — `rv32mi/pmpaddr` once PMP was implemented,
and `rv32mi/breakpoint` once Sdtrig was.

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
