# Validation

Two suites, and they cover different things. **Run both.** A regression
that only the Berkeley suite catches will sit unnoticed if only
arch-test is run -- which is exactly what happened to `rv32mi/csr` when F
was added.

```sh
./scripts/run-arch-test.sh      # official riscv-arch-test, interpreter and --jit
./scripts/run-riscv-tests.sh    # Berkeley suite
./scripts/report-figures.sh     # every number below, regenerated
```

Keep the suites' `-march` in step with what `misa` advertises:
`rv32mi/csr` deliberately fails when built without F and run on a core
reporting F, and that failure looks like an emulator bug until you
disassemble the test.

G4MH has no reference model and no architecture suite. Its evidence is
`tests/unit/test_g4mh.c` -- hand-assembled halfword arrays that
deliberately do not share an encoder with the interpreter -- plus a
CC-RH-built guest, which is the thing that has actually found bugs. See
[frontend/g4mh.md](frontend/g4mh.md).

## Validation

Two suites, both wired to CMake targets:

```sh
cmake --build build/host --target arch-test        # official RISC-V suite
cmake --build build/host --target arch-test-quick  # base integer only
cmake --build build/host --target riscv-tests      # Berkeley suite
cmake --build build/host --target validate         # everything
```

Current state, all re-run on the tree as it stands:

| | result | runs on |
|---|---|---|
| `riscv-arch-test` | **274 / 274** | host |
| `riscv-arch-test`, default (host FPU) | 222 / 274 — every failure in `F` | host |
| `riscv-tests` | **77 / 77** | host |
| host unit + guest self-tests (`ctest -L fast`) | **3 / 3**, one of them through the JIT | host |
| `riscv-arch-test`, `--jit` + SoftFloat | **274 / 274** — the whole suite through translated code | host |
| `riscv-arch-test`, `--jit`, host FPU | 222 / 274 — identical to the interpreter on the same build | host |
| `riscv-tests`, `--jit` | **77 / 77** | host |
| `isatest`, JIT | **296 / 296** | hardware |
| `isatest`, `-DEMU_JIT=OFF` | **296 / 296** | hardware |
| `isatest`, host, both FP backends | **296 / 296** | host |
| `mmiobench` | **72 / 72** | hardware |
| CoreMark | `crcfinal 0xca90` on all three backends | hardware |

**What these suites do not cover: the JIT.** Both run the host interpreter, so
nothing in `src/frontend/rv32/rv_jit_thumb2.c` is exercised by either — it only
compiles for ARM. Everything the translator emits is validated by `isatest`
and `mmiobench` on the board, and by CoreMark's CRC agreeing across native
ARM, interpreter and JIT.

That gap is not theoretical. Every JIT defect found so far was found on
hardware, and none of them could have been caught by a signature-checking
suite:

| defect | how it presented |
|---|---|
| inlined store ignored PMP | a protected region was writable under the JIT only |
| inlined store skipped the LR/SC reservation break | a later `SC` wrongly succeeded |
| loop cap compared the wrong register | *no* wrong answer — chained loops simply ran unbounded |
| loop chaining dropped past a branch's reach | *no* wrong answer — one loop shape ran 2.4× slower |
| `rm=dyn` resolved `RMM` as round-to-nearest | ties rounded to even where the guest asked for away |
| `mstatus.FS` decided at translation | FP ran after the guest turned the FPU off |
| PMP flush watched a flag, not the configuration | a store landed in memory PMP had been told to deny |

Two of those produced no wrong answer at all, only performance that made no
sense. Three were staleness: a decision taken when a block was translated,
still in force after the state behind it changed. The self-test grew from 148
checks to 296 chasing them, and the checks that matter are the ones that
re-execute *one* instruction at *one* address after changing the state it was
compiled against — a fresh call site is translated against the current
configuration and proves nothing.

## Floating point (F)

F is implemented — the register file, `fcsr`/`frm`/`fflags`, `mstatus.FS`,
all of OP-FP, the four fused multiply-adds, `FLW`/`FSW`, and `Zcf`'s
compressed FP load/stores (which `C` on RV32F is defined to include).

There is one implementation. SoftFloat is the FP unit -- not an option,
and a missing checkout is a configure error rather than a fallback. The
history below is why.

**Berkeley SoftFloat (`ON`) passes all 224 tests.** It is the library the
RISC-V FP spec was written against, and the fit is exact rather than
convenient: its rounding modes are numerically identical to `frm`
(`near_even`/`minMag`/`min`/`max`/`near_maxMag` = 0..4) and its exception
flags identical to `fflags` (1/2/4/8/16), so neither needs translating. It
also ships a `RISCV` specialization carrying the canonical-NaN and
NaN-propagation rules. `f32_mulAdd` is a genuine single-rounding fused
multiply-add, which is where most of the previous failures were.

**The host FPU via `<fenv.h>` (`OFF`, the default) passes 172 of 224.** It is
smaller and faster — one hardware instruction per operation on a Cortex-M4F —
but the flags it can report are the ones the hardware happens to raise, and
those differ from RISC-V's rules on the fused multiply-adds and around
subnormals. It remains the default because most guests never look at `fflags`,
and conformance is a build option away.

That split is the useful outcome: correctness when it is wanted, size and speed
when it is not. On the Nucleo, SoftFloat costs about 8.7 KB of flash
(51.3 KB against 42.6 KB with the JIT enabled), which on a 512 KB part is
affordable but is real.

Both are validated on hardware. Note that with the JIT enabled the arithmetic
is translated to VFP and never reaches SoftFloat, so the run that actually
exercises it is `-DEMU_JIT=OFF`; both configurations pass 296/296. That the
two backends agree is worth having deliberately — they use genuinely different
FP implementations, VFP against SoftFloat, so the self-test doubles as a
differential check between them.

**D is not implemented and is not planned**: the Cortex-M4F and M7 FPUs are
single-precision, so D would be entirely soft-float on the intended targets.
**Zcd follows from that** — it is the compressed *double* load/stores
(`c.fld`/`c.fldsp`/`c.fsd`/`c.fsdsp`), which target 64-bit FP registers that do
not exist without D. It is ruled out by the D decision, not separately skipped.

**Zcb is implemented and passes 7/7.** It reuses the `funct6=100111` slot that
RV64 spends on `c.subw`/`c.addw`: bits [6:5] select `c.mul` or a group of unary
operations, and the byte/halfword accesses sit in quadrant 0 under `funct3=100`.
Three of the unary ops (`c.sext.b`, `c.zext.h`, `c.sext.h`) expand to Zbb
instructions, which is why the spec makes Zcb depend on Zbb — without it there
would be nothing to expand them into.

## Official RISC-V Architecture Test Suite — 274/274 with SoftFloat

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
