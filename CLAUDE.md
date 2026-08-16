# rv32cortex-m — working notes

A retargetable 32-bit ISA emulator for ARM Cortex-M hosts. The guest drives
the host's real peripherals through an identity-mapped passthrough window, so
**peripheral drivers live in the guest, not in the emulator**. Keep it that
way: adding a GPIO or UART driver to `src/platform/` is almost always the
wrong fix.

## Layout

Detailed notes live under `docs/`, split by platform and by
platform/frontend pair — memory maps, peripheral policy, what has been
measured, and **to do / investigate / discarded** for each:
[`docs/Architecture.md`](docs/Architecture.md),
[`docs/host/`](docs/host/README.md),
[`docs/stm32f446/`](docs/stm32f446/README.md), and
`docs/<platform>/<frontend>/README.md`.

Three axes, independent of each other:

| axis | what it decides | selected by |
|---|---|---|
| platform | where it runs | `EMU_PLATFORM=host\|stm32f446` |
| frontend | what it emulates | `EMU_FRONTEND_RV32`, `EMU_FRONTEND_G4MH` |
| backend | how it executes | `EMU_JIT=ON\|OFF`, per frontend |

```
include/emu/   src/emu/          ISA-agnostic runtime: bus, regions,
                                 passthrough, NS16550 console, ELF loader,
                                 cache ops, the frontend registry
include/rv32/  src/frontend/rv32/  RISC-V RV32: hart, CSRs, decoder,
                                 interpreter, Thumb-2 JIT, CLINT, APLIC
include/g4mh/  src/frontend/g4mh/  Renesas RH850 G4MH: core, decoder,
                                 interpreter, INTC
               src/platform/     host runner and STM32F446 firmware
```

**`include/emu/emu_cpu.h` is the contract**, and the note at the top of it is
the thing to read before adding a frontend or a member. The rule it lives by:
`run` executes a whole budget behind one indirect call and every other hook is
setup or fires on a trap. Nothing in that table may end up on a
per-instruction path — a single extra *direct* branch on the fetch path
measured 9.3% on CoreMark.

The two frontends are symmetric. `rv32_frontend.c` and `g4mh_frontend.c` are
the same file with different contents, which is the intended shape: if a third
one needs something neither has, it probably belongs in `emu_cpu_ops_t` rather
than in a platform `#ifdef`.
## Build, run, validate

All of it -- every configuration, every option, the console settings, the
network build -- is in [README.md](README.md) and under
[docs/](docs/Architecture.md). Do not duplicate it here; a recipe in two
places is a recipe that disagrees with itself, which is how this file
came to document an `RV32_PLATFORM` option that has never existed.

What belongs here is only what the recipes do not say:

```sh
./scripts/run-arch-test.sh      # 274/274, interpreter and --jit alike
./scripts/run-riscv-tests.sh    # Berkeley suite, 77/77 both ways
./scripts/report-figures.sh     # figures, with the cache vars that set them
./scripts/check-doc-flags.sh    # every -D flag in the docs exists
```

**Run both suites.** They cover different things, and a regression that
only the Berkeley suite catches will sit unnoticed if only arch-test is
run -- which is exactly what happened to `rv32mi/csr` when F was added.
Keep their `-march` in step with what `misa` advertises: `rv32mi/csr`
deliberately fails when built without F and run on a core reporting F,
and that failure looks like an emulator bug until you disassemble the
test.

**The Thumb-2 backend cannot be exercised by any host suite.** Validate
it by flashing `isatest` and reading the UART. This has caught real JIT
bugs that both x86 suites passed. Hardware is a Nucleo-F746ZG on
`/dev/ttyACM1` at 921600; `--connect-under-reset` is the default in both
`flash` targets because this firmware never idles and a plain attach
races it.

**`isatest` cannot measure JIT coverage** -- it arms PMP early, so
everything downstream interprets. Use `bench` or `coremark` for that, and
read `isatest` only as a correctness check.

## Things that have bitten, and will again

One entry per defect, in the order they were found. Reference material --
what is implemented, what the figures are, how a block is specialised --
lives in [docs/](docs/Architecture.md); what is here is only what was
*learned the hard way*, with the number or symbol that proves it.

If you read nothing else, read these six. Each has cost more than one
session, and every one of them recurred:

1. **A trap only reports if something catches it.** Unimplemented
   encodings raise an exception, which in a flat guest with no vector
   table lands on ordinary code twenty bytes further on and carries on
   silently. "Raises an exception rather than a silent wrong answer" is a
   claim about the *guest*, not the emulator.
2. **One weak test is worse than none.** Five of them, listed below,
   passed against the bug they covered.
3. **Identical counters mean the code never ran**, not that nothing
   regressed. A perfect null result is the loudest signal here.
4. **Every translate-time read of mutable hart state is a staleness bug
   until proven otherwise** -- and too coarse a flush is as wrong as too
   narrow, and quieter.
5. **Measure; do not reason.** Interpreter-in-SRAM, the register cache
   and the shifted-operand fusion were all built on sound arguments and
   all measured worse.
6. **A flag or invariant quoted in prose is not a tested thing.** Two
   documented build options named variables that did not exist;
   `scripts/check-doc-flags.sh` exists because of it.

- **Extensions sharing an opcode slot must be decoded in one place.** Zbb's
  `min`/`max` and Zbc's `clmul` share funct7 0x05; a separate `else if` later in
  the chain is unreachable and every `clmul` raised illegal-instruction.
- **Put the common case first in decode.** Zbb tests placed ahead of
  `SLLI`/`SRLI` made every shift pay an extra compare and cost more than Zbb
  saved.
- **In the JIT, what you decline costs more than what you translate badly.**
  Ending a block for an untranslatable instruction fragments hot code. Route it
  through a helper call instead — `jit_helper_alu` exists for exactly this.
- **There is one FP implementation, and both backends reach it.**
  SoftFloat is the FP unit -- not an option, and a missing checkout is a
  configure error rather than a fallback. Everything that rounds,
  classifies or reports a flag goes to `rv_hart_fp`, from the
  interpreter and from the JIT alike; only `FMV.X.W`/`FMV.W.X` are
  lowered, because they move bits and cannot round.

  This replaced an arrangement where the JIT emitted host FP
  instructions, which was a *second* implementation of semantics the
  core already owns, and the two disagreed exactly where the
  architecture is fussiest -- NaN propagation, subnormals, and which of
  fflags an operation may raise. rv32i/F: interpreter 78/78, JIT 55/78,
  same binary. Routing the arithmetic to the helper made it 78/78 both
  ways, and the JIT is still ahead of the interpreter on FP work
  (fptest x5: 38ms against 54ms) because the block stays whole and only
  the arithmetic becomes a call. To make it faster, bring operations
  back one at a time, each measured against the F suite -- not the whole
  table on the argument that the host has an FPU.

  **The option that hid this named a variable that does not exist.**
  These notes named the option `EMU_FPU_SOFTFLOAT` against a real
  `RV32_FPU_SOFTFLOAT`, defaulting to OFF. (Both names are history now:
  SoftFloat is mandatory and neither option exists.) So every default build had an
  FP unit failing two thirds of the F suite, and the documented way to
  check that theory changed nothing and reported nothing -- which reads
  as "SoftFloat makes no difference" rather than as a typo. A build flag
  quoted in prose is not a tested thing; paste it from `CMakeLists.txt`.
- **`MOVS` on a low register writes N and Z.** Zeroing a result register
  between `VMRS APSR_nzcv` and the `IT` that tests it destroys the comparison:
  Z ends up set and N clear, so `EQ` is always true, `MI` always false and `LS`
  always true. Set the false value up *before* the compare.
- **ARM and RISC-V order the FP exception flags in reverse.** ARM is
  `IOC,DZC,OFC,UFC,IXC` from bit 0, RISC-V is `NX,UF,OF,DZ,NV`, so `RBIT` plus
  `LSR #27` converts between them. The JIT also needs `FPSCR.DN` set (ARM's
  default NaN is RISC-V's canonical NaN) and `FZ` clear (RISC-V wants real
  subnormals). `RMM` has no ARM rounding mode and stays on the helper.
- **JIT fast paths bypass the C helpers and their side effects.** The inlined
  store had to drop the LR/SC reservation by hand, and had to be abandoned
  entirely once PMP is active, because it writes guest RAM without checking.
  Both were found on hardware, not by the x86 suites. When adding anything that
  `rv_hart_load`/`rv_hart_store` does beyond the access itself, ask what the
  inlined path does about it.
- **A helper call is a translation; declining is not.** `FMIN`/`FMAX` and
  `FCLASS` have no ARMv7-M equivalent, but routing them to `rv_hart_fp` still
  keeps the block whole -- worth 24 interpreted instructions and 23 dispatches
  on the self-test. Open-coding costs 25-35 emitted instructions each in the
  code cache, which sets performance more than the translator does, and makes
  a second copy of semantics the core owns. Reach for the helper first.
- **Inlining a memory access is only sound while nothing can deny it.** That
  holds in M-mode with no locked PMP entry, and stops holding below M, where
  matching no entry *denies* rather than permits. `pmp_active` therefore
  depends on `h->priv`, not only on the entry configuration, and anything
  changing the privilege level must call `rv_pmp_refresh`. The same flag
  gates the interpreter's skip of `rv_pmp_check`, so getting it wrong permits
  accesses that should fault -- in both backends. Snapshotting it in the JIT
  also snapshots privilege, which is what stops a block built for M-mode
  running below M: privilege only *drops* through `MRET`, a SYSTEM
  instruction the translator declines, so it lands on the interpreter
  fallback where the snapshot is checked.
- **Every translate-time read of mutable hart state is a staleness bug
  until proven otherwise.** One rule, five separate defects, and the
  reference sweep now lives in
  [`docs/jit/staleness.md`](docs/jit/staleness.md). What to carry in your
  head:

  | read at translation | outcome |
  |---|---|
  | `fcsr` frm, for `rm=dyn` | wrong for RMM -- fixed |
  | `mstatus.FS` (`h_fs_off`) | unchecked for OP-FP, stale for loads/stores -- fixed |
  | `pmp_active` + `rv_pmp_simple` bounds | **permission bypass** -- fixed |
  | `satp` / page mappings | needs `vm_gen`; satp alone would miss an edited PTE |
  | bus regions, `g_pt_armed`, guest bytes | safe: fixed at init, or they flush |

  Four things generalise from the five:

  - **A translate-time legality check is only half a guard; the block
    outlives it.** `mstatus.FS` was consulted when translating FP loads
    and stores on the reasoning that refusing to translate while FS is
    Off was enough. It is not: a block built while FS was on keeps
    executing after the guest turns the FPU off, so three instructions
    that must raise illegal-instruction ran silently.
  - **Watching a flag is not watching the configuration.** The PMP flush
    compared `pmp_active`, but what a block bakes in is the *bounds*
    `rv_pmp_simple` reported. Locking a second entry leaves the flag true
    while the one-entry assumption it encodes stops holding. Snapshot
    what was baked, not what enabled it. Same shape as watching `satp`
    instead of the mappings.
  - **Too coarse is as wrong as too narrow, and quieter.** Keeping the
    whole two-bit FS field rather than its off-ness means every FP
    operation -- which moves FS Clean to Dirty as a side effect --
    flushes the code cache. That is a correctness-preserving way to have
    no JIT, and no test of correctness would notice. `test_jit_generation_key`
    checks both directions; both were confirmed by breaking them (10
    failures narrow, 2 coarse).
  - **Re-derive on the interpreter fallback, never per dispatch.** frm,
    `mstatus.FS` and PMP all move only through a CSR write, and the
    translator declines `SYSTEM`, so the fallback is the single place any
    of them can change -- and it is where the check is *correct*.
    CoreMark enters blocks 2.9M times a run. Moving the PMP check off the
    dispatch path measured neutral, so the branch was predicting well;
    correctness was the reason, not speed.

  All of the first three were invisible to both host suites, because
  neither ran the JIT, and each needed a hardware test with the fix
  reverted to prove.

  `frm` deserves its own note: `dyn` is resolved at translation, not at
  run time, which is what lets `RMM` be declined to the helper -- ARM has
  no ties-away mode, and the old run-time table silently mapped it to
  `RN`. `g_frm_specialised` skips the flush entirely for guests with no
  `dyn` FP.
- **ARM and RISC-V float-to-int agree except on NaN.** Both saturate
  out-of-range to the target's limit, both raise invalid doing so, and
  neither adds inexact. Only NaN differs: ARM gives 0, RISC-V gives the
  *maximum*. `VCMP` of the operand against itself is unordered exactly for
  NaN, so `IT VS` / `MOV` patches it in seven instructions. Use `VCVTR`, not
  `VCVT` -- the latter forces round-toward-zero regardless of FPSCR.
- **One weak test is worse than none, because it reads as coverage.**
  This is the most-repeated mistake in this file. Every one of these
  existed, named the right thing, and passed against the bug it nominally
  covered:

  | test | why it passed anyway |
  |---|---|
  | every double-precision test, against `SUBF.D` with its operands reversed | `CHECK_EQ` casts to `uint32_t`, and 2.0 against -2.0 differs in the *high* word |
  | `isatest`'s single `FCVT.W.S`, `10.0` with `rtz` | passes whether or not the NaN fixup exists, and NaN is the *whole* difficulty |
  | `test_lower_zero_register` | does `GET r0` then `PUT r0`; the bug needs the other order. Two lines apart |
  | `test_lower_flags` | only ever expected S/V/C **clear**, so a flag emitter that clobbered them looked right |
  | the `CMOVF.S` test | mirrored the compare and the select, so an inverted implementation cancelled out |
  | the G4MH FP encodings | all used fcbit 0, the one value where the right and wrong field splits coincide |

  The last one is the newest and the least obvious, because the test
  was not weak -- the **harness** was. `CHECK_EQ(got, want)` casts both
  sides to `uint32_t`, which is written down in the macro and is
  therefore not a compiler warning. Twelve assertions about doubles and
  register pairs were comparing low halves, and the A/B that found it
  was a break I expected to fail and that did not. `CHECK_EQ64` exists
  now. **When an assertion is about something wider than the harness's
  native width, check the harness before trusting the green.**

  Three rules fall out.

  **When the whole difficulty of a change is one input, test that
  input.** Not a representative one -- the awkward one. NaN for a
  conversion, `cond=15` for a four-bit field, `-1` for a split immediate,
  `0` and `32` for a shift amount.

  **Never assert a value the implementation computed.** That is how the
  `CMOVF` test came to encode the bug as the expectation.

  **Proving the new path even *runs* is a separate question.** Force it
  back to the old behaviour and diff a counter: `interp ... fell back`
  (112 against 134), `diff_checked` (53 checked, 27 declined),
  translations, ticks. Identical counters mean the code never ran, not
  that nothing regressed -- and a *perfect* null result across a change
  that should have moved something is the loudest signal in this file.

  The corresponding discipline for a fix: **confirm it by reverting it**
  with the new test in place, and check the failure names the mechanism.
  An A/B that only half-reverts reads as a passing test -- disabling one
  of the translator's two fetch-permission sites still gave a clean
  243/243, because the instruction was 32-bit and the second site caught
  it.
- **`EMU_JIT_CODE_BYTES` dominates JIT performance, and at the 12 KB
  default the JIT *loses to the interpreter*.** CoreMark's translated
  working set is ~48 KB; the interpreter is 10,691,637 ticks and the JIT
  at 12 KB is 10,850,998. Guest RAM pays one for one. Full sweep in
  [`docs/jit/tuning.md`](docs/jit/tuning.md).
- **CMake cache variables silently outlive the tree you set them in.** Every
  performance figure in this repo had been measured with a 48 KB code cache
  inherited from an old build directory while the declared default was 12 KB;
  `rm -rf build/` and the numbers changed by 68% with no code change. Before
  quoting a measurement, check `CMakeCache.txt` for what actually built it --
  `EMU_JIT_CODE_BYTES`, `RV_GUEST_MARCH` and `COREMARK_ITERATIONS` are all
  cache variables and all change the result.
- **`RV_JIT_LOOP_CAP` is an interrupt-latency knob, and CoreMark cannot
  see it** -- its loops end on unchainable branches, so the cap is not
  what exits them. `mmiobench` sees it clearly. Each doubling returns
  half the previous one and doubles worst-case latency, so 128 is the
  knee. **Do not tune a knob on a workload that cannot observe it**;
  figures in [`docs/jit/tuning.md`](docs/jit/tuning.md).
- **A register that does not fit a Thumb-2 encoding assembles as a different
  instruction, not an error.** The 16-bit `CMP`/data-processing form encodes
  r0-r7; `emit_dp_reg(DP_CMP, R8, R1)` set a bit belonging to `rm` and became
  `CMP r0, r1`, so `RV_JIT_LOOP_CAP` never applied and chained loops ran
  unbounded -- 3700 guest instructions per block entry where 64 was intended.
  It looked like extra throughput and was the interrupt-latency bound being
  thrown away. Use `emit_cmp_hi` for r8 and above. Nothing computed a wrong
  answer, so no test caught it.
- **Branch range is a silent cliff.** Loop chaining was emitted only when the
  back edge fitted the 16-bit conditional branch (+/-254 bytes); a larger
  block stopped chaining rather than widening the encoding, costing 2.4x on
  the loops that crossed the line. `emit_bcond_back` picks the encoding by
  reach, so the common case still gets the short form.
- **Inlining the peripheral window is worth 2.2-3.1x to drivers and -53% to
  compute, so the guest arms it.** The emitted test is ~18 bytes per load and
  ~48 per store; always-on grew CoreMark's image past the 48 KB code cache and
  doubled evictions. `pt_note` counts passthrough accesses through the helper
  and flushes once at `RV_JIT_PT_ARM_AT`. Flushing from inside a helper is
  safe -- the running block stays intact and only the next translation reuses
  its memory -- and a pending-flush flag tested per dispatch would put the
  cost back on the hot path.
- **Scan the bus once per flush, not once per block.** The region scan sat in
  `translate()`, so a workload that re-translates hard (CoreMark: 26575
  evictions) paid a bus walk per block and 8% overall. The region table is
  fixed before execution and anything changing it flushes.
- **Reserved peripheral addresses are not a harmless near miss.** An
  unimplemented address in the passthrough window makes the AHB signal an
  error, which is a HardFault in the *emulator*, not a fault delivered to the
  guest: the firmware dies rather than the test failing. Every hole in the
  STM32 policy table begins just above reserved space, so there is no register
  immediately below one to probe with. Test the window with registers that
  exist.
- **`add_custom_command(DEPENDS <target>)` is ordering, not staleness.** The
  guest image was staged with a target-only dependency, so the copy ran once
  and never again: editing a guest source rebuilt the `.bin`, left the staged
  copy alone, and produced firmware carrying the *previous* guest. Nothing
  failed. Name the file and the target.
- **"Declined", "overflowed" and "cache full" are three outcomes, and
  collapsing any two of them is pathological.** Both halves of this were
  found separately, years apart, in the same shape:
  - sharing a recovery path between "nothing translatable here" and
    "cache full" made every interpreted `div` flush the code cache;
  - returning NULL for a block that *overran the buffer*, exactly as for
    one the translator declined, made the interpreter run one instruction
    and the same oversized block be translated and thrown away again --
    957 times in one CoreMark run, with translation reaching 65% of all
    host cycles.

  They are counted now, not reasoned about. `overflow` and `declined` are
  in the stats line.
- **The JIT was only ever kept off the x86 host by nobody selecting it.**
  `RV_ENABLE_JIT` defaults to a `__thumb2__` test, but CMake defined it
  unconditionally from `EMU_JIT`, so the host build compiled a Thumb-2
  emitter and the *only* thing preventing a jump into it was that
  `src/platform/host/main.c` happened never to set `rv_backend`. Moving
  backend selection into the frontend — where it belongs, because it is a
  property of the frontend and not of a platform's `main()` — turned that
  latent trap into an immediate segfault on the first guest instruction.
  `rv_config.h` now reconciles "asked for" with "possible" and forces 0 off
  ARM. A capability that depends on nobody exercising it is not a
  capability that is off.
- **A halt can arrive from outside the execute switch.** The syscall hook
  calls `ops->halt` to implement `exit()`, so the run loop has to test
  `EMU_STATE_HALTED` at the top and not only where the halt instruction is
  decoded. The G4MH interpreter did the latter, so an `exit()` was followed
  by whatever instruction came next — which in the test happened to be a
  `HALT`, so the core stopped anyway and returned the *wrong reason*.
  Nothing computed a wrong answer; the run reason was simply a lie.
- **ACT's `--extensions` selects test suites by *directory name*, not by
  required extension.** `generate_test_dict` globs `tests/*/<name>/*.S`, so
  `U` matches nothing and silently builds nothing -- which is why declaring
  U-mode to UDB and Sail changed the results not at all. The U-mode PMP
  tests live in `tests/priv/pmp/pmp32/**PMPU**`. What a test *requires* is
  declared in its own `REQUIRED_EXTENSIONS` header and checked against the
  UDB config by `select_tests`; naming a suite on the command line only
  offers it. Four rounds of guessing the flag would have been one round of
  reading `framework/src/act/parse_test_constraints.py`.
- **The PMP lock bit says who an entry applies to, not whether it is in
  force.** `L` clear means the entry is invisible to M-mode; below M it
  binds exactly as a locked one does. `rv_pmp_check` read it alone --
  "unlocked, therefore permitted" -- which is right in M-mode and denies
  every U-mode access matching an unlocked entry, i.e. the usual background
  region. Consult `L` only together with the privilege level.
- **Anything on the fetch path is paid per instruction by every guest, so
  give the features one branch, not one each.** The PMP execute check
  measured **9.3%** on CoreMark -- which never arms PMP -- as its own
  `RV_UNLIKELY(h->pmp_active)` test next to Sdtrig's `trig_active` one.
  Folding both behind a single `h->fetch_guard` (maintained by the two
  refresh functions, the only writers of the flags it combines) brought
  that to **2.7%**, at the noise floor. The second halfword needs its own
  check only when `pc & 2`, because every PMP bound is 4-byte aligned.
  Measured on the interpreter (`-DEMU_JIT=OFF`), which is where a fetch
  cost lands -- the JIT pays it once per *translation*.
- **Disable every site, and confirm the failure names the mechanism.**
  The half-revert case from the weak-test entry above, concretely: the
  translator checks fetch permission at two sites, one per halfword, and
  disabling the first alone gave a clean 243/243 because the instruction
  was 32-bit and the second site caught it. With both disabled,
  `pmpx-exec-noeffect` returned `0xBAD` -- the store really had run
  inside a no-execute region, which is the mechanism, not just a failure.
- **Execute permission was never checked at all.** `RV_ACC_FETCH` existed
  and was passed only to Sdtrig; no caller ever handed it to
  `rv_pmp_check`. Grep for the enumerator, not for the feature. The check
  belongs per *halfword*, which gets the straddle rule for a 32-bit
  instruction crossing a region boundary for free.
- **`1u << 32` decoded the widest PMP region as the narrowest.** NAPOT with
  `pmpaddr` all ones is a shift by 32 -- undefined, and in practice 1, so
  the permit-everything entry that `riscv-tests` installs became four bytes
  at the top of memory. Invisible in M-mode, where matching nothing
  permits; fatal in U-mode, where it does not. The whole encoding reaches
  sizes that overflow 32 bits, so `pmp_range` does the arithmetic in 64 and
  saturates.
- **U-mode turns three latent M-mode bugs into failures at once.** All
  three above sat in shipped, suite-passing code because every M-mode path
  through them ends in "matching nothing permits". Any privilege work
  should assume the PMP code is wrong until the privileged tests say
  otherwise -- and they will not run until the suite is *named* correctly.
- **`&&label` is not a resume address.** Handing the address of a C label
  to a trap handler looks equivalent to a label inside an asm block and is
  not: the compiler owns basic-block layout, and here it placed the
  label's block *ahead* of the faulting call, so resuming there re-ran the
  fault -- forever. Emit the label between two instructions in the asm, as
  `call_stub` and `call_may_fault` do.
- **Entering S-mode by `mret` and leaving by `ecall` destroys the caller's
  frame.** The callee's prologue runs and its epilogue never does, so `sp`
  comes back low and every callee-saved register still holds the callee's
  value. It does not present as a stack bug -- it presents as the test
  suite restarting from the top. `enter_smode` saves sp and s0-s11 to a
  global and reloads them at the resume label, reloading the base address
  with `la` because the operand register is caller-saved too.
- **A reserved-encoding test can pass against an implementation that never
  heard of the rule.** `W` without `R` faults at the *leaf* level either
  way, because a PTE with neither R nor X is by definition a pointer and
  there is no level below the last. Only at the root does the check
  discriminate: skip it and the walk follows a perfectly good table and
  the access succeeds. Put the encoding where the two behaviours differ.
- **A block backend may retire more than its budget, and one caller's
  arithmetic could not survive that.** The host runner sized each slice as
  `max_insn - total`; the JIT overshoots because it can only stop between
  blocks, so `total` passed the cap, the unsigned subtraction went below
  zero, and the loop never ended -- the guest ran on correctly while the
  cap silently stopped existing. Reachable only by the two riscv-tests
  that *depend* on the cap to terminate, and invisible for the life of the
  project because the interpreter lands on it exactly. Compare
  `total >= max_insn`, never a budget that has to reach zero.
- **The x86-64 JIT is for coverage, and its stats line is what proves it.**
  A backend that declines everything and falls back passes every test while
  proving nothing, so the host runner prints `xlat/entries/interp`. First
  run said `interp 7807` of 8527 -- 92% interpreted, because compressed
  encodings were skipped and `isatest` locks a PMP entry early, after which
  `fetch_guard` sends *everything* to the interpreter. On CoreMark, which
  arms nothing, it is 42026 of 520078. Read the ratio before believing a
  pass.
- **The MCU's JIT tuning is wrong for a host, in the direction that hides
  bugs.** `RV_JIT_CODE_SIZE` 12 KB and `RV_JIT_MAX_BLOCKS` 256 exist
  because on a microcontroller those bytes are the guest's; on a host they
  made CoreMark flush 19 times per run, and constant retranslation is
  exactly what would mask a translator bug behind a fresh translation. The
  x86-64 backend sets its own sizes.
- **Porting the Thumb-2 backend onto the shared framework cost 4x, in four
  separate ways, and every suite passed throughout.** CoreMark went 79,502
  ticks to 321,035 while `isatest` stayed 296/296 and riscv-tests 77/77.
  What the *stats line* said, in order of size:
  - **`interp 14660` against 341.** `may_run` was copied from the x86-64
    backend, which declines under PMP and paging because it implements
    neither. Thumb-2 implements both -- it checks fetch permission per
    halfword while translating, walks the page tables, and snapshots the
    PMP configuration, `satp` and `vm_gen` -- so it must gate on
    `trig_active` alone. Gating on `fetch_guard` costs correctness nothing
    and coverage everything: `isatest` arms PMP early, so the whole rest
    of the run interpreted.
  - **`overflow 957`, and translation was 65% of all host cycles.** The
    three-outcomes entry above, found again from the other end.
  - **A direct-mapped hash hides blocks rather than costing a probe.** The
    table is far larger than the live set, which makes one entry per
    bucket look adequate; a collision instead makes the loser unreachable
    while it still holds its code and its slot, so two hot blocks
    retranslate each other every time round the loop. 1977 translations
    against 1465. Keep the chain.
  - **+129 cycles per dispatch for six indirect calls.** `pc`, `state`,
    `generation` and `may_run` as `emu_jit_ops_t` callbacks read better
    and cost 4.99M of the 7.03M host cycles the framework added: the loop
    runs once per block entry, and an M7 cannot predict an indirect call.
    They are pointers in `emu_jit_hot_t` now, bound once per
    `emu_jit_run`, which is four loads. This is not the layout guess the
    `pc` callback was written to avoid -- the frontend states where each
    value lives, once, in `bind`.

  Result 95,107 ticks, ~16-20% above the pre-framework backend, which is
  the price of the shared dispatch loop and is real. Inlining the emitters
  was **neutral** (LTO already did it); do not re-try it.
- **A host calling convention belongs in the framework, not the
  frontend.** Cortex-M selects instruction set with the low bit of a
  branch target, so a block address needs bit 0 set before it is called.
  The framework knows the host; the frontend knows the guest. Getting it
  wrong faults on the first block entry rather than computing anything
  wrong -- the banner printed and nothing else.
- **Framework table sizes have to follow the target.** 8192 blocks and an
  8192-entry hash cost a host nothing and are 192 KB of `.bss` on a part
  with 320 KB, where those bytes are the guest's. Keyed on
  `EMU_JIT_THUMB2` now: 256/256/12 KB there, the host figures otherwise.
- **System V wants rsp 16-byte aligned at a `call`.** Entry leaves it 8
  past, two pushes bring it back to 8, so the block prologue needs one more
  8. Getting it wrong does not fault in the emitted code -- it faults
  inside whatever libc routine a helper eventually reaches that uses an
  aligned SSE store.
- **`jalr rd, rs1` may name the same register twice.** Compute the target
  before writing the link, or `jalr ra, ra` -- an indirect call through a
  saved pointer -- jumps to the return address it just wrote.
- **Sv32 goes behind `vm_active`, folded into `fetch_guard`.** Translation
  is on the fetch and access paths, the two places this repo has measured
  as the most expensive to touch, so it is gated exactly as PMP is: false
  whenever satp is Bare or every relevant privilege is M. Interpreter
  CoreMark was unchanged by adding it, because CoreMark never enables
  paging and pays one already-existing branch.
- **`satp.PPN` is 20 bits here, not the 22 Sv32 defines.** The field is
  WARL and its width follows the *physical* address space; this bus is
  32-bit, so a page number is PA[31:12]. Masking to 22 does not widen
  anything -- the walk shifts the PPN back up and silently drops the top
  two bits -- it just lets satp report a root table the walk cannot reach.
  `sv32_satp_access_test` writes all-ones and checks the readback.
- **`mstatus.SD` has to be computed on read, not stored.** It summarises
  the extension-state fields, so storing it means every write to FS has to
  remember to maintain it, and the one place that cannot forget is the
  read. Two Sv tests check it and nothing before them did.
- **A 32-bit instruction can straddle a page.** Both halves are fetched,
  and with paging the second may be in a different page that is not
  mapped, so it needs its own translation -- but only when
  `pc & 0xFFF == 0xFFE`, which keeps it off the common path. The same
  applies in the JIT's translator, which walks exactly as the interpreter
  does or it compiles whatever physical memory sits at the virtual address.
- **JIT blocks are keyed on virtual addresses, so a mapping change
  invalidates them even when satp does not move.** Editing a PTE and
  issuing `SFENCE.VMA` is exactly that case, which is why `rv_mmu_flush`
  bumps a generation counter the JIT snapshots. Watching satp alone would
  miss it -- the same shape as watching `pmp_active` instead of the PMP
  entries.
- **Declaring an extension to UDB and to Sail are two separate jobs, and
  the Sail half can make the golden model disagree about correct
  behaviour.** `sail.json` had `medeleg.delegatable_bits = 0x0`, exactly
  right while there was no S-mode. Left at zero once S exists, the
  reference takes in M-mode every trap the emulator correctly delegates to
  S, and eight PMPU tests fail on the one field that is right -- the
  signature's handler mode. Keep both masks equal to `MEDELEG_WMASK` and
  `MIDELEG_WMASK`. A config value meaning "this cannot happen" stops being
  a safe default the moment it can.
- **UDB names every parameter it wants, so let it.** Adding S asked for 25;
  fill them from an existing config that already has S
  (`udb-*/.data/cfgs/rv32-riscv-tests.yaml`) rather than guessing schemas.
  It also caught `MCOUNTENABLE_EN` still declared all-false from when
  `mcounteren` was hardwired zero for want of a lower privilege level --
  true when written, and not since U-mode.
- **Porting to a Cortex-M7 is mostly about the two things the M4 does not
  have: caches and a DWT lock.** The JIT writes instructions as data and
  branches to them, which needs a real clean-to-PoU and I-cache invalidate
  by address, not the DSB/ISB that sufficed with no caches -- getting it
  wrong executes arbitrary bytes rather than producing a wrong answer.
  `RV_ARM_HAS_CACHES` is set by the platform because nothing in the
  compiler flags distinguishes the parts: `-mcpu=cortex-m4` and
  `-mcpu=cortex-m7` both define `__ARM_ARCH_7EM__`. And the M7 implements
  the optional DWT software lock, so `DWT->LAR = 0xC5ACCE55` has to come
  before enabling CYCCNT; without it the writes are discarded silently, the
  cycle counter never runs, and every guest timer interrupt stops -- which
  is exactly how it presented (`timer-fired` and `timer-cause`, nothing
  else).
- **The other F7 differences are the ones that fail loudly, and so cost
  nothing.** `USART_TypeDef` splits F4's `DR` into `RDR`/`TDR`, which does
  not compile. The ones that would have been silent are in the HAL config:
  F4 spells the flash accelerator `INSTRUCTION_CACHE_ENABLE` /
  `DATA_CACHE_ENABLE` and F7 spells it `ART_ACCELERATOR_ENABLE`, so a
  renamed F4 `hal_conf.h` leaves the accelerator off with nothing to show
  for it. Do not rename the vendor config; start from the family's own
  template.
- **The peripheral policy table survived the move, and that is a checked
  result rather than luck.** PWR, RCC and the flash interface sit at
  identical addresses in RM0390 and RM0385, and everything the F7 adds
  falls inside spans that were already read-write. Check the boundary
  table before assuming this again for another part.
- **`set(STM32CUBE_FAMILY ...)` has to precede the `include()`.** After it,
  the pack has already been chosen and fetched, so the line is a no-op that
  reads exactly like a fix.
- **A failing arch test may be the Sail config, not the emulator.** ACT runs the
  golden model to bake expected values into each test, so a wrong `sail.json`
  produces wrong expectations. `amocas` failed for three sessions because guest
  RAM declared `atomic_support: AMOArithmetic`, which excludes CAS, so Sail
  *trapped* and the signatures recorded the trap. When targeted checks say an
  instruction is right and the suite disagrees, run
  `sail_riscv_sim --config <sail.json> --trace-instr` on the same ELF and diff
  against `emu-host --trace-count N`; a jump to `Mtrampoline` in the reference
  is the tell.
- **Compressed guest code is slower to interpret, not faster.** Enabling Zcb in
  guest codegen cost ~9% on CoreMark at an identical instruction count: the
  compiler swapped 32-bit encodings for Zcb ones, each of which now pays an RVC
  expansion. Supporting Zcb in the *emulator* is a small win (38.0 vs 39.2
  cyc/insn); it is the *guest* march that costs. Toggle `RV32_EXT_ZCB` against
  a fixed guest binary to separate the two.
- **Instruction fusion is the wrong target; the register-file round trip
  is the right one.** Measured with `-DRV32_PAIR_STATS=ON`, which
  histograms adjacent *executed* instruction pairs on the interpreter:

  | guest | pairs | dependent | of which dead | addr-gen -> mem |
  |---|---|---|---|---|
  | CoreMark | 443k | 28.3% | 5.5% | 2.9% |
  | bench | 1.12M | 24.5% | 10.4% | 0.3% |
  | mmiobench | 1.21M | 33.3% | 0.1% | 0.0% |
  | isatest | 7.0k | 50.2% | 17.5% | 1.1% |

  The textbook RISC-V fusions are **not there**: `lui`+`addi` is 0.2% of
  CoreMark pairs and `auipc`+`addi` is 0.00%, because the guest is built
  `-O2` for a small target where constants fit the 12-bit immediate and
  globals go through gp. Address-generation feeding a load or store --
  the other classic, and the one Thumb-2's `LDR Rt,[Rn,Rm,LSL #n]` would
  serve -- is 0.0-2.9%. Neither justifies lookahead machinery in the
  translator.

  What *is* large is that a quarter to a third of adjacent pairs are data
  dependent, and with the register file in memory each one emits
  `STR Rx,[r4,#n]` followed immediately by `LDR Rx,[r4,#n]` of the same
  slot. Dropping the reload saves one instruction; when the intermediate
  is dead in the block, dropping the store too saves two. At ~4.5 host
  instructions per guest instruction that is **5-7% of executed host
  instructions and a similar share of code size** -- and code size is the
  dominant term, so it counts twice.

  This is a peephole with a scratch-register tracking window, not fusion.
  It is also *not* the register cache below: that pre-loaded at block
  entry and wrote through, paying a setup load and three callee-saved
  registers per PUSH/POP. This pays nothing -- it declines to emit a load
  whose value is already in the register. The window has to reset at every
  helper call (r0-r3 clobbered) and at every block boundary, and blocks
  average 4.12 guest instructions, so the figures above are a ceiling.
- **The ARM shifted-operand fusion was built, measured and reverted.**
  Every 32-bit data-processing instruction on ARM carries a shift on its
  second operand, so folding a shift into the ALU op that consumes it is
  one instruction instead of two -- and it fires on nothing. Measured
  with `-DRV32_PAIR_STATS=ON` on CoreMark *after* building it:

  | pair | share |
  |---|---|
  | `srai`+`andi` | 1.32% -- and `andi` is an immediate form, so the shift feeds operand *a*, which ARM cannot fold |
  | `srai`+`srai` | 1.32% |
  | `add`+`srai` | 1.32% -- the shift is the consumer, not the producer |

  A single-use immediate shift feeding the *second* operand of a
  register-register ALU instruction is essentially absent. The fusion
  was correct on hardware (296/296) and changed the translation and
  compaction counts not at all -- byte-identical, because it never
  fired -- while CoreMark went 771,100 to 849,822 ticks, a 10%
  regression still unexplained. Reverted.

  The mistake was extrapolating from "ARM has a shifted operand" to
  "there will be shifts to fold". The histogram was already in the tree
  and answers it in one run. **Run the pair stats before writing the
  encoder, not after.**
- **What the pair histogram actually says about CoreMark** (518,206
  instructions, 440,934 adjacent pairs): **29.6% are data dependent**, of
  which 5.7% have a dead intermediate, and address-generation feeding a
  memory access is 2.9%. That 29.6% is the number worth attacking, and
  it is what the reload elision and the x86 memory operand take -- both
  without any pattern matching, for 6.1% of emitted code size on x86-64
  and 771,100 from 963,899 ticks on ARM. The textbook fusions are not
  there: `lui`+`addi` 0.2%, `auipc`+`addi` 0.00%.
- **The board's tick counter is exactly deterministic, and the ±10% I
  blamed on it was build-to-build code layout.** The same CoreMark binary
  reflashed and rerun three times gave 847,616 ticks every time, to the
  digit, with identical translation and compaction counts. So the earlier
  reading of 771,100 against 849,822 was not noise: those were different
  emulator binaries, and one of them -- the reverted shifted-operand
  fusion -- emitted *byte-identical* guest translations. The only thing
  left that can move a byte-identical translation by 10% is where the
  emulator's own hot loop lands in flash. Read that as **layout is worth
  up to 10% on this part, not the ±3% recorded above**, and as: never
  attribute a difference to noise without rerunning the same binary,
  which costs one reflash.
- **A register allocator buys nothing on a host where a move and a memory
  access are the same size, unless the value is used *in place*.** The
  first Thumb-2 version routed allocated temps through `ld_operand` and
  `st_slot` exactly as x86-64 does, and measured **worse**: 6040
  translations against 5943, 894 compactions against 828, and 410 buffer
  overflows where there had been 309. On x86-64 that substitution turns
  an eight-byte `[rsp + disp32]` into a three-byte `mov` and is most of
  the win; on Thumb-2 `LDR.W` and `MOV.W` are four bytes each and it is
  pure cost -- a wider PUSH, the load detour a helper's out-pointer
  needs, and nothing on the other side. Computing into the allocated
  register instead is what pays, and pays far more than on x86-64,
  because Thumb-2 is three-address: an ADD with both operands and its
  destination allocated is **one** instruction where the frame needs
  four. Same allocator, same block, 4596 translations and 522
  compactions, 788,181 ticks. The mistake was assuming a win transfers
  between hosts because the analysis does.
- **Four registers is most of what there is to get, and the fourth is
  nearly free to give up.** Sweeping the x86-64 allocator over CoreMark:
  0 registers 173,828 bytes of emitted code, 1 → 167,492, 2 → 164,120,
  3 → 162,836, 4 → 161,932. The first register is 3.6% and the fourth is
  0.6%. Live sets in these blocks are small, which is what 4.12 guest
  instructions a block should predict -- so do not go looking for more
  registers before looking for longer blocks.
- **A hand-encoded ModRM will not tell you the register did not fit.**
  `ld_slot`/`st_slot` built the byte as `0x84 | (reg << 3)` for the
  three scratch registers they were written for. Handed r12 the fourth
  bit fell off the top of the three-bit field and `mov r12d, [rsp+n]`
  assembled as `mov esp, [rsp+n]` -- the block loaded a guest value into
  its own stack pointer and did not fault until the return. The same
  shape as the 16-bit Thumb-2 `CMP` that became a different instruction.
  Encoders that take a register parameter belong in `encode.c`, where
  the REX/high-register logic is written once.
- **An IR pass that rewrites the IR must honour every guest invariant
  the lowerings do, and being right in all three backends does not help
  when the thing that is wrong runs before them.**
  `pass_reg_traffic` forwards a guest register's value from the temp
  that last wrote it, so a `GET` becomes a `MOV` instead of a reload.
  It tracked **x0**. Every lowering -- the IR interpreter, x86-64 and
  Thumb-2 -- consults `reg_is_zero` and correctly discards a write to
  x0 and answers a read with a constant; the pass never saw the target
  at all, so it recorded the discarded write and rewrote the next read
  into a `MOV` of it. All three then faithfully compiled a guest
  reading its own thrown-away result where the architecture guarantees
  zero. Writing x0 is not a corner case: every discarded result and
  every canonical NOP is one. **36 of 39 `I` tests and 3 of 8 `M`.**

  Three things about finding it are worth keeping.

  **The differential checker cannot see it, by construction.**
  `ir_diff_ref` runs `emu_ir_interp` on the *optimised* IR, so
  reference and compiled code were wrong identically and agreed
  perfectly. A checker that compares two consumers of a bad input
  validates the consumers, not the input -- and its silence read
  exactly like a clean bill of health. It also declines any block
  holding a store, which is most of an architecture test, so "declined
  everything" and "agreed with everything" were the same silence.
  `diff_checked`/`diff_declined` are in the stats line now: the first
  run said 53 checked, 27 declined, and that number is what turned the
  silence into evidence.

  **`test_lower_zero_register` existed, covered x0, and passed
  throughout.** It does `GET r0` and *then* `PUT r0`; the bug needs the
  other order. One weak test is worse than none, again -- and here the
  distance between the passing test and the failing case was the order
  of two lines. `test_zero_register_write_then_read` is the same test
  with the writes first, and it was confirmed by reverting the fix.

  **Bisecting the passes found it in four runs; reading them did not.**
  `EMU_IR_PASSES` as a temporary bitmask over the four passes said
  `pass_reg_traffic` alone reproduced it and the other three were
  clean. Before that, two plausible readings of the code -- a missing
  invalidation on LOAD/STORE, and the allocator claiming x0 -- were
  both wrong. The optimiser is four passes and an A/B is one rebuild.
- **The IR path has no `generation`, and floating point is the first
  thing that would need one.** `rv_jit_bind` fills in `pc`, `state` and
  `blocked` and leaves `generation` unset, which is correct today for a
  reason worth stating: the only mutable hart state the IR translator
  reads is what `fetch_guard` already covers, and it *declines to run at
  all* when PMP, paging or Sdtrig are armed rather than tracking them.
  Nothing it emits is specialised on anything.

  Emitting the FP class breaks that. A block is specialised on `frm` --
  the IR resolves "dynamic" at translation, deliberately, so a backend
  can decline a mode it lacks -- and a block containing an FP load or an
  OP-FP is only legal while `mstatus.FS` is on. Both are exactly the
  shape already recorded above for the hand-written backend: **a
  translate-time legality check is only half a guard; the block outlives
  it**, and a block built while FS was on keeps executing after the
  guest turns the FPU off, so instructions that must raise
  illegal-instruction run silently.

  It now has one: `rv_ir_gen_key` folds frm, FS off-ness and `vm_gen`
  into a word that `after_interp` refreshes and `bind` points
  `generation` at. Re-derived on the interpreter fallback rather than in
  `generation` itself, because that is read on every block entry and
  CoreMark enters blocks 2.9M times a run.

  **The key can be wrong in two opposite directions and only one of them
  is a wrong answer.** Too narrow -- missing frm, say -- leaves blocks
  running that were specialised on state the guest has changed. Too
  coarse is the subtler one: keeping the whole two-bit FS field rather
  than its off-ness means every FP operation, which moves FS from Clean
  to Dirty as a side effect, flushes the code cache. That is a
  correctness-preserving way to have no JIT, and no test of correctness
  would notice. `test_jit_generation_key` checks both, and both were
  confirmed by breaking them: 10 failures narrow, 2 failures coarse.
  Accrued fflags share fcsr with frm and must *not* flush; that is
  checked too.
- **`isatest` cannot measure JIT coverage, because it arms PMP early.**
  Confirmed on the board, same firmware, one guest apart: `isatest`
  interprets 14,700 of 45,399 instructions (32%), `bench` interprets
  40,521 of 1,274,518 (**3.2%**). `rv_jit_bind` points the framework's
  `blocked` at `h->fetch_guard`, so once a PMP entry is locked
  everything downstream goes to the interpreter whatever the backend
  could have lowered.

  That is why three consecutive changes here -- floating-point emission,
  then Thumb-2 helper calls -- each produced a board run whose counters
  were *identical to the digit* to the run before. The changes were
  fine; the guest could not see them. Use `bench` or `coremark` to
  measure translation on this host, and read `isatest` only as a
  correctness check.

  Which leaves a real question, not yet answered: whether the IR path
  should gate on `fetch_guard` at all. It was inherited from the x86-64
  backend, which declines PMP and paging because it implements neither
  -- but the IR path's memory operations go through `rv_ir_load` and
  `rv_ir_store`, the same checked accessors the interpreter uses, so PMP
  is enforced either way. Fetch permission is the part that would still
  need doing. Do not just relax it.
- **A capability query is worth nothing to a backend that cannot emit the
  fallback.** `emu_ir_can_lower` exists so a frontend can turn an
  operation a host lacks into a helper call rather than a declined
  block. The Thumb-2 IR backend answers it honestly -- it declines
  FMIN/FMAX, the comparisons, the conversions -- and then declines
  `EMU_IR_HELPER_TRAP` as well, so every one of those answers costs the
  whole block anyway. The mechanism is a no-op there.

  It presented as a *perfect* null result: flashing the board after
  wiring the frontend to emit floating point gave `xlat 170` and
  `interp 14700`, identical to the previous run to the digit, with the
  test still passing 296/296. Identical numbers are not "no regression",
  they are "the code never ran" -- and on the host the same commit moved
  an F architecture test from 452 interpreted to 275. Two hosts
  disagreeing that completely is the tell.

  Lowering `EMU_IR_HELPER` and `EMU_IR_HELPER_TRAP` on Thumb-2 is the
  fix and is not hard: r0 the cpu, r1 and r2 the arguments, `t2_call`,
  then compare and take the existing conditional exit -- the same shape
  as the LOAD and STORE calls already beside it.
- **A guest-register cache in r8-r10 was tried and is 15.5% slower.** Reads per
  block said it should win; it did not, because a cached read is `MOV` where an
  uncached one is `LDR` -- one instruction either way -- while write-through
  adds an instruction per write and three more registers hit every PUSH/POP.
  Do not retry without a cost model, not just a frequency count.
- **`-Os` is 33% smaller and 8.8% slower** on the F446. The ART accelerator is
  not the binding constraint, so the code-density argument does not pay. Use
  `MinSizeRel` only when flash is actually scarce.
- **The JIT's block dispatch is already cheap.** The prologue is `PUSH {r4-r7,lr}`
  plus a `MOV`, and the hash lookup is a shift, mask, load and compare -- a
  last-block cache in front of it measured 1.2% slower. What is left in block
  entry needs chaining across loop back edges, not micro-optimisation.
- **Sdtrig costs the interpreter 14.8 cyc/insn and PMP 3.2** -- 46% together,
  measured by compiling each out. Hoisting the `trig_active` load into a local
  made it *worse*; the cost is the `TRAP` call site in the fetch sequence, not
  the load. Adding anything to the fetch or access path is paid by every
  instruction whether the feature is used or not, so measure the interpreter
  after doing so, not only the JIT.
- **`emu_fault_t` and `rv_exc_t` are both `uint32_t`, and the compiler will
  not tell you when you mix them.** Splitting the bus out of the RISC-V
  frontend changed its success value from `RV_EXC_NONE` (0xFFFFFFFF) to
  `EMU_FAULT_NONE` (0), and every unconverted `!= RV_EXC_NONE` then read
  *every successful load* as a fault returning cause 0, i.e. instruction
  address misaligned. The guest ran 200k instructions and printed nothing.
  Grep for the call, not for the constant: `emu_bus_read|emu_bus_write|
  emu_bus_fetch16` has fifteen call sites and enumerating them is the check.
- **A failed flash leaves the previous firmware running, which is worse
  than a failed build.** `probe-rs` could not attach -- "Connecting to
  the chip was unsuccessful / Timeout occurred during operation" -- and
  the next test then exercised the *old* image while looking like it
  exercised the new one. The board reports a plausible result, so the
  conclusion is "the change does not work" about a change that was never
  loaded. This firmware never idles: it runs a guest flat out and parks
  in `__WFI` with interrupts arriving, so the debug port is contended
  from reset onwards and a plain attach races it.
  `--connect-under-reset` costs nothing on an idle board and is the
  default in both `flash` targets now. Two full debugging rounds went
  into the network stack before anyone read the flash log.
- **`if(TARGET ...)` only sees targets already defined.** The firmware
  asked `if(TARGET guest-${RV32_GUEST})` from a directory added *before*
  `tests/guest`, so the answer was always no and every configure warned
  that the link would fail. It never did -- `guest_image.S` carries an
  `OBJECT_DEPENDS` on the staged files, which is the dependency that
  matters. A warning that is always wrong is worse than none: it is the
  one that teaches you to skip reading them, and it was still being
  printed while a real problem was being hunted.
- **lwIP ships a `CMakeLists.txt`, unlike the Cube packs, and its
  `Filelists.cmake` opens with `include_guard(GLOBAL)`.** So
  `FetchContent_MakeAvailable` configures it as a subdirectory, that
  include runs first, and the project's own `include()` of the same file
  silently does nothing -- every `lwip*_SRCS` comes out empty and the
  library is built from whatever was named explicitly. Here that was one
  file, and it presented as a hundred undefined references to functions
  whose sources were plainly sitting in `_deps`. `SOURCE_SUBDIR` naming
  a directory that does not exist is the documented "populate, do not
  configure".
- **Text and SLIP cannot share a wire, so the handover has to be
  one-way and announced.** `emu_net_init()` gives the UART to the IP
  stack and never takes it back; the last two lines it prints are
  whether the stack started and what address to telnet to. Framing
  around the text instead would make a corrupted stream
  indistinguishable from a working one, and the failure that produces --
  a board that never answers a ping -- says nothing about why. The cost
  is real and worth stating: after the handover the firmware has **no
  channel to report its own failure on**, so anything that breaks the
  stack breaks the only way of hearing about it.
- **At 921600 the UART must be interrupt-driven or the protocol cannot
  work at all.** A byte lands every 10.8 us, this family's USART holds
  exactly one, and the run loop reaches the port once per guest slice --
  thousands of instructions apart. Polling loses most of every frame.
  The two families differ in how an overrun is dismissed (F7 writes
  `ICR`, F4 reads `SR` then `DR`) and leaving `ORE` latched stops
  reception *permanently* rather than dropping one byte. Measured
  working: `rx drops 0` across a full `isatest` run.
- **The host end of the wire has two settings that fail silently, and
  both cost a day.** `slattach -s 921600` does not work: net-tools maps
  `-s` through a fixed table of `Bxxx` constants that stops short of it,
  and rather than falling back it fails the open outright -- `tty_open:
  cannot set 921600 bps!` -- so no interface is created. That one at
  least says so. The other does not: **slattach clears `CSIZE` without
  setting it**, so the line comes up at **cs5**, and five data bits
  against the board's eight means every frame misassembles and SLIP
  discards all of it. `stty raw` does not touch `CSIZE` either, so the
  obvious thing to try changes nothing.

  What makes it expensive is that the board looks broken and every check
  on the board passes. Pins, AF7, `RCC` enables, `BRR`, the PLL, even a
  breakpoint on `board_console_putc` showing it called with a real
  character -- all correct, while the host reads a dozen bytes of
  garbage. Two things nearly sent it further wrong: `TC` and `TXE` are
  **set out of reset**, so they never prove a transmission; and a byte
  count that does not change with the sampling rate is *not* a baud
  mismatch, which is what a mismatch would look like. `scripts/slip-up.sh`
  sets the line with `stty` before and after slattach and prints what it
  ended up with, because a mis-framed line is otherwise silent.
- **A firmware that has given its console away cannot report its own
  failure, so `fatal_halt` must not mask interrupts.** `emu_net_init()`
  hands the UART to SLIP, after which everything printed goes to a ring
  drained by telnet. Halting with `__disable_irq()` therefore writes the
  reason into memory nobody can reach: the board answers no ping, no
  telnet and no TFTP, and presents as a dead link. It halted with "could
  not build the guest address space" sitting in the ring, and the hour
  that followed went on the network. With the stack up, spin on
  `emu_net_poll()` instead -- nothing else runs, which is the point, and
  a client can still connect and collect the reason. This is the cost
  `emu_net.h` warns about, made concrete.
- **`build_address_space()` reads `g_img_*`, and main() was setting them
  afterwards.** That is what a run-time-replaceable image costs: the
  function used to read the `.incbin` symbols and so was correct wherever
  it was called from, and now it depends on two variables assigned
  thirty lines later. With them zero the ROM region is zero-length,
  `emu_bus_add` rejects anything under 4 bytes, and the firmware halts
  before its first guest instruction. Moving a value from link time to
  run time moves every reader of it into an ordering that nothing checks.
- **Rebuilding the bus drops the frontend's devices.** `start_guest()`
  begins with `emu_bus_init()`, which clears the region table, so a
  reload that did not re-add the CLINT and the APLIC would take them
  away from a guest that had them a moment earlier. Registering them in
  `main()` was correct only while the bus was built exactly once -- the
  same shape as the two above: code that was right when something
  happened once, left alone when it started happening twice.
- **The guest image split was lossy for half the guests, and the comment
  claiming otherwise was load-bearing.** `guest_image.S` states that the
  `.ro` and `.rw` halves concatenate to the unsplit `.bin` "which the
  build checks". The build checked nothing, and it was false for two of
  four guests: `hello` came out 393 + 4 against a 400-byte binary. The
  `. = ALIGN(4)` that sets `__guest_ro_end` sits *between* output
  sections, so it moves the location counter without padding `.rodata`
  itself, and `objcopy -j .text -j .rodata` emits the section's own
  extent. The three lost bytes put `.data` below where the guest reads
  it -- and the guest runs anyway, on zeros, which is why nothing
  noticed. It bites only when `.rodata` happens to end unaligned:
  `coremark` and `isatest` are aligned by luck and were byte-exact. The
  `ALIGN` is now *inside* the section, and the build really does compare
  `cat ro rw` against the binary. **A comment asserting an invariant is
  where to look first when something downstream is three bytes wrong.**
- **`imm5 == 0` does not mean "no shift", and only `LSL` reads it that
  way.** ARM spends the otherwise-useless encoding on the amount `imm5`
  cannot hold: `LSR #0` **is** `LSR #32`, `ASR #0` is `ASR #32`, and
  `ROR #0` is `RRX`. So a shift by zero -- which RISC-V spells
  `srli`/`srai rd, rs, 0` and means as a move -- assembled as a shift
  by 32 and gave zero or a sign extension. Rewriting the type to `LSL`
  is the fix; *skipping* the emit is not, because `rd` and `rm` differ
  and the move still has to happen.

  Invisible to x86-64, whose `shr r32, 0` is a genuine no-op, and so to
  both host suites. The board found it: `I-srli-00` and `I-srai-00`
  shift by zero 17 and 16 times, and were the only two failures in
  39. This is the **second** defect in the same twelve-line function --
  the comment beside it records an imm3:imm2 split that came out as a
  shift by zero. An encoder whose wrong answers are other valid
  instructions needs its *boundary* values tested, not its typical
  ones; both bugs are at an end of the range.
- **`__WFI` stops the clock lwIP tells the time by.** `sys_now()` comes
  from `board_cycles()`, which is DWT CYCCNT -- a counter of *processor*
  cycles. The park loop waited in `__WFI`, which gates the processor
  clock, so the stack's notion of time nearly stopped: measured across
  29 seconds of wall time parked, lwIP's clock advanced **1.74 seconds,
  about 6% of real time**.

  Every timeout in the stack is slowed by that factor. It became fatal
  in TFTP, whose 10-second session timeout then needs ~3 minutes of wall
  time -- so a client killed mid-transfer wedged every later upload
  behind "Only one connection at a time" until the board was reset,
  which is how it presented and cost a whole debugging pass. A TCP
  retransmission or an ARP entry ageing out is equally late and would
  present as a mysteriously sluggish link rather than as a stopped
  clock. The park loop no longer sleeps while the stack is up; a
  free-running TIM would be the better answer if it ever needs to again,
  because a peripheral keeps its clock through Sleep where CYCCNT does
  not.

  Two things about finding it are worth keeping. **`sys_timeout()`
  asserts and then returns**, so a timer that fails to register is
  simply absent -- which is indistinguishable from one that is armed and
  never due, and sent the first diagnosis at `MEMP_NUM_SYS_TIMEOUT`
  (raised to +4 anyway, since running that pool to the edge is what
  makes a leak fatal). And **a watchdog gated on the wrong observable is
  a watchdog that never fires**: the first version keyed on this file's
  own `g_busy`, but lwIP refuses a request inside `tftp_recv()` before
  `ctx->open()` is ever reached, so whether the server is stuck is not
  something those callbacks can see. It sat silent through a board
  refusing every upload. Trigger on idleness, which is observable, and
  rebuild unconditionally -- an idle server costs a `udp_remove` and a
  `udp_new`, so it is cheaper to rebuild than to know whether you had to.
- **A recovery path wired to one of two symmetric cases recovers from
  the one that does not happen.** The flash arena filling up is the
  *expected* failure -- there is no length in a TFTP request, so running
  out is how the end is discovered -- and the recovery is to erase and
  let the client retry. It was wired for the `rom` half only. The client
  sends rom then ram, so the transfer that runs off the end is almost
  always the **second**: the rom half still fits at the address
  `begin()` handed out. Every retry then re-sent a rom half that fitted
  and a ram half that did not, failing identically for ever, and the
  board needed a power cycle to take another image. It presents as a
  dead server rather than a full one -- three retries, three identical
  `error writing file`. Surfaced only by running the suite far enough to
  fill the arena, about twenty tests.

  Its neighbour is still open and has the same shape: lwIP's TFTP
  server keeps **one** session and never reclaims it, so a client killed
  mid-transfer wedges every later upload behind "Only one connection at
  a time is supported" until reset. The shim's 8-second backoff assumes
  the session ages out. It does not.
- **A reload that only the run loop can see never happens.** `g_reload`
  was checked between guest slices, which is exactly when a harness is
  *not* uploading: it runs a test, waits for it to halt and report, then
  pushes the next one. By then the run loop has exited and the firmware
  is parked in `__WFI`. Both transfers completed, the server said so,
  and nothing happened -- the most convincing kind of failure, because
  every visible signal is success. The park loop takes an image too now,
  and both paths go through one `take_uploaded_image()`.
- **An application timeout needs a slot lwIP does not reserve.**
  `MEMP_NUM_SYS_TIMEOUT` defaults to `LWIP_NUM_SYS_TIMEOUT_INTERNAL`,
  which counts the stack's own timers and nothing an application
  registers, so starting the TFTP server overflows it. The failure
  arrives long after the cause: the server comes up, announces itself,
  and the *first transfer* dies with `sys_timeout: timeout != NULL, pool
  MEMP_SYS_TIMEOUT is empty` -- a message about memory pools, while
  debugging a file transfer. Write it as
  `(LWIP_NUM_SYS_TIMEOUT_INTERNAL + 1)`; the macro is undefined at that
  point in `lwipopts.h` and expands later, which is the documented idiom
  rather than an accident.
- **The uploaded `.data` was being zeroed by the reload that installed
  it.** The writable half went straight into guest RAM, and
  `start_guest()` clears RAM beyond the image -- deliberately, so one
  test cannot pass on state another wrote. Both halves go to the flash
  arena now, back to back, which also makes an uploaded image identical
  in shape to the baked-in one. Two of the guests in the tree have an
  empty `.rw`, so the obvious thing to test an upload with cannot see
  this: use `hello`, whose four bytes of `.data` are the whole test.
- **The gdb stub debugs the guest, and its split is the same one the
  tree already uses.** `src/emu/emu_gdb.c` is the RSP protocol with no
  ISA in it; a frontend supplies one `emu_gdb_target_t` saying how many
  registers gdb expects, in what order, and which is the pc. That order
  is not a choice -- gdb's `g` packet is a fixed per-architecture
  concatenation (rv32: x0..x31 then pc, 32-bit, **little-endian by
  byte**) and it does not ask. Getting it wrong yields an `info
  registers` that is plausible and entirely wrong, so check it against
  the board's own state dump rather than against itself.

  Three things in RSP that fail by hanging rather than erroring: the
  checksum is a modulo-256 sum of the body and gdb silently retries a
  bad one; `c` and `s` must **not** be answered until the guest actually
  stops, or gdb believes it halted where it already was; and Ctrl-C
  arrives as a bare 0x03 *outside* any packet, so a stub that only looks
  for `$` makes ^C do nothing at all.

  Breakpoints are a pc list, not a patched trap instruction. Patching is
  what real silicon needs and is wrong here twice over: the read-only
  half of a guest image is served from the board's flash, and a patched
  instruction is invisible to the JIT until the block is retranslated.
- **"Unimplemented raises RIE, which is a clean report rather than a
  silent wrong answer" was false for the whole life of the G4MH
  frontend.** A trap only reports if something catches it. A flat test
  guest has no vector table, so `G4MH_EXC_RIE` sends the pc to
  `RBASE + 0x60` -- which in a flat image is an ordinary instruction
  *inside the same function*, about twenty bytes further on. Execution
  carries on with the unimplemented instruction skipped and nothing
  written anywhere. That is precisely the silent wrong answer the design
  was meant to rule out, and it is worse than a hang because the guest
  keeps producing plausible output.

  It presented as arithmetic: `puthex` printed `1c 05 02 ff fc f9 f6 f3`
  where `31 32 33 34 61 62 63 64` was wanted -- an arithmetic sequence
  stepping by -3, which reads as a broken expression rather than a
  missing instruction. Three sessions went on bisecting the *expression*
  with single-purpose guests, ruling out the variable shift, the
  conditional select and the subtract direction, all of which were
  correct. What found it in one run was `-DEMU_ENABLE_TRACE=ON`: the pc
  jumped 0x52 to 0x60 without `retired` advancing, and an instruction
  that changes the pc without retiring is a trap, not an instruction.

  **Read the pc deltas in a trace, not the disassembly.** The
  disassembler prints `.short` for anything it does not know, which looks
  like a disassembler gap and not a decoder gap -- and it is
  independently behind the interpreter, so it also prints confident
  nonsense (`movhi` for a perfectly well executed `DISPOSE`). The trace
  is trustworthy about *addresses* and unreliable about names.
- **The forms a compiler emits are not the forms a manual chapter is
  organised around, and the hand-written tests all used the other one.**
  `SHR reg1, reg2` was implemented and `SHR reg1, reg2, reg3` was not;
  CC-RH emits the three-operand form for every `v >> n`. Likewise `DIV`
  was there and `DIVQ` -- which is *the same instruction* with a shorter
  cycle count, and is what CC-RH emits for `a / b` -- was not. Both slots
  differ from the implemented one by a single sub-opcode bit, which is
  exactly how they were missed: 0x080 against 0x082, 0x2C0 against 0x2FC.

  The general rule this repo already knows, in a new place: **an ISA that
  varies one bit of a sub-opcode to mean "and also write reg3" will not
  tell you when you implement one half of the pair.** Enumerate the slot.
  `scripts/g4mh-check-encodings.sh` answers it in one run and is how all
  six constants here were obtained -- including the confirmation that
  `CMOV`'s condition really is `(sub >> 1) & 0xF`, which only `cond=15`
  (sub 0x33E) can settle, because every smaller condition is consistent
  with the three-bit reading too.
- **A guest that runs is worth more than a suite that passes, when the
  suite shares an author with the thing it tests.** G4MH's unit tests are
  hand-assembled halfword arrays, deliberately not sharing an encoder
  with the interpreter -- and they still could not find any of this,
  because they exercise the encodings someone thought to write. One
  compiled 90-line C guest found three defects in a row: the three-operand
  shifts, then `DIVQ`, then `DIVH`. Run `docs/renesas`' CC-RH over
  something real before believing a coverage claim.
- **The G4MH interpreter was unreachable from the host, so "the JIT
  agrees with the interpreter" had never been checked.** `--jit` was
  parsed inside `#if EMU_FRONTEND_RV32`, and `g4mh_ops_init` assigned the
  JIT unconditionally, so a G4MH-only build *rejected the option* and ran
  translated whatever was asked. Both backends now come from the host's
  choice, as RV32's already did. This matters more for G4MH than for
  RV32, which has three reference models to disagree with: here the
  interpreter is the only statement of what an answer should be, and a
  run that silently translated cannot be diffed against one that did not.
  The first thing the fix bought was evidence, and it was the useful
  kind -- both backends produced the *same* wrong bytes, which is what
  said the defect was in shared semantics and not in the translator.
- **`emu_ir_jit.c` defines both frontends' backends from one macro, and
  it lived in the RV32-only source block.** So a G4MH-only tree left
  `g4mh_backend_jit` undefined against a frontend that referenced it
  unconditionally -- a link error, which is the good outcome. It is in
  the shared block now with the two host emitters, all of which guard
  themselves on the host they emit for. The rule: **a file whose contents
  are selected by host belongs beside the other files selected by host,
  not under whichever frontend happened to need it first.**
- **The G4MH JIT has its own interrupt path, so anything added to the
  interpreter's is absent from it.** `g4mh_jit_take_irq` is a separate
  copy of the run loop's interrupt check. Adding FE-level delivery for
  the TPTM to the interpreter left the JIT -- which is the *default*
  backend -- taking none at all. Exactly the performance-counter defect
  one entry up, found within the same week, and the reason the test
  caught it is worth more than the fix: it asserts the **cause
  register**, not that a handler ran. The zeros between a flat guest's
  vectors run through to whatever comes next, so an unrelated exception
  lands on the same handler and sets the same register. Three earlier
  versions of that test passed against three different bugs -- a
  misaligned word store to a halfword register, `RESBANK` written where
  `DI` was meant, and PSW.ID masking the channel -- each of which
  vectored somewhere and fell through to the handler being watched.
- **A capability macro that depends on include order is worse than no
  macro.** `G4MH_HAVE_JIT` was defined in `g4mh_cpu.h` from
  `EMU_JIT_X86_64`, which `emu/emu_jit.h` defines -- and
  `g4mh_frontend.c` included `emu_jit.h` on the line *after* `g4mh_cpu.h`.
  So the header saw the macro undefined, declared no backend, and the
  frontend quietly ran interpreted while every build succeeded. Two unit
  tests caught it only because they assert the translation counter moves.
  A header that tests a capability must include whatever defines it.
- **Measure; do not reason about performance.** Interpreter-in-SRAM was
  *slower*, lazy-IRQ was neutral, and the `clmul` fix was 1.3% when the real
  cost was 4.12-instruction blocks. Layout noise is ±3% on the host; on the
  board it has been measured at 10%, and the counter itself is exact -- see
  the entry above before calling a difference noise.
- Guest images link `-nostdlib`; there is no libc. The **core** must not call
  libm either, which is why `fsqrt` is Newton-Raphson rather than `sqrt()`.
- **`-ffp-contract=fast` silently breaks the fused multiply-add's own
  error term, and only on hosts that have an FMA.** `rv_hart_fp`
  recovers a product's rounding error with 2Product/2Sum so an FMA
  rounds once, and that is exact only if `a * b - p` is evaluated as
  written. GCC's default fuses precisely that expression into a single
  instruction wherever one exists, so the error computes as zero and the
  result rounds twice. The linked F746 firmware had **eleven**
  `VFMA`/`VFNMS` inside `rv_hart_fp`; x86-64 has no FMA in this build and
  was correct, which is why no host suite ever saw it. Fixed with
  `set_source_files_properties(... -ffp-contract=off)` on `rv_fpu.c`.

  The lesson generalises past this file: **any algorithm that depends on
  an intermediate being exactly the rounded value needs contraction
  off**, and nothing warns you. It is not a wrong answer you can grep
  for — it is a last-bit difference that only appears on one of the two
  machines you build for.

  Two things about finding it are worth keeping. It was invisible to
  every unit test and both suites, and was caught only by
  `tests/guest/fptest.c` running the *same guest binary* on the host and
  the board and disagreeing — a differential check across
  implementations, not against an expectation. And `fptest` reports a
  checksum **per kernel** rather than one total, which is what turned
  "the two disagree" into "the disagreement is in the one kernel that
  compiles to an `fmadd.s`" in a single board run instead of five
  bisecting reflashes.

  The unit test added alongside (`test_fma_rounds_once`) pins the
  semantics but has **not** been shown to catch this: rebuilding
  `rv_fpu.c` with `-ffp-contract=fast -mfma` emitted no FMA on x86-64
  anyway, so the A/B was inconclusive rather than passing. `fptest` on
  hardware is the thing that would notice a regression.
- **The FPU uses `float` only.** `double` on an M4F is libgcc soft-float: 17 KiB
  of firmware to emulate a single-precision FPU on a part that has one. Flags
  and rounding come from `<fenv.h>` (in libm on both glibc and newlib); the
  fused multiply-adds need 2Product/2Sum to round once.
- Enabling `F` forces `Zcf` on RV32: `C@2.0` is defined to include the
  compressed FP load/stores, and UDB rejects the config without it.
- CoreMark's `core_main.c` defines `main()`; `-Dmain=...` must be scoped to that
  file or it renames the firmware's entry point.

## Conventions

`src/emu/` is portable C11 with no platform *and no ISA* dependencies, and must
stay that way — it builds for ARMv6-M through ARMv8.1-M and for the host. It
knows about regions, permissions and access widths; it does not know what an
architecture calls the fault that results. Accesses report an `emu_fault_t` and
the frontend maps it (`rv_exc_from_fault`, `g4mh_exc_from_fault`).

`src/frontend/<isa>/` owns one instruction set. Shared semantics within a
frontend (`rv_hart_amo`, `rv_hart_cbo`) live beside the state so that
frontend's interpreter and JIT cannot drift apart. New RV32 ISA work goes in
**both** RV32 backends plus `tests/arch-test/` config, or is declared
unsupported.

### Adding a frontend

1. `include/<isa>/` — public headers, `<isa>_` prefixed
2. `src/frontend/<isa>/` — state, decoder, interpreter, its own devices
3. one `emu_cpu_ops_t`, declared and listed in `src/emu/emu_cpu.c`
4. `option(EMU_FRONTEND_<ISA> ...)` and a `target_sources` block in
   `CMakeLists.txt`
5. tests in `tests/unit/`, guarded by `EMU_FRONTEND_<ISA>`

Nothing in `src/emu/` or `src/platform/` should need editing beyond step 3.
That is the property to check when the contract changes: build the firmware
with `-DEMU_FRONTEND_RV32=OFF -DEMU_FRONTEND_G4MH=ON` and see that it links.

That check passes now, and did not until recently: `g4mh_frontend.c`
modelled the U2B6's whole memory map as `.bss` -- 3 MiB of code flash,
384 KiB of cluster RAM, 64 KiB of local RAM per PE -- which is 3.44 MiB
on a part with 320 KiB, and the link failed with `cannot move location
counter backwards`. **A frontend that allocates its own memory map works
on a host and cannot be ported**, and nothing says so until someone
builds the firmware. Flash especially: it is where code is *executed
from*, so the platform serves the guest image out of its own flash
read-only and pays nothing, exactly as the RV32 side already did. See
[`docs/memory.md`](docs/memory.md).

### G4MH scope

Moved to [`docs/frontend/g4mh.md`](docs/frontend/g4mh.md) -- what is
implemented, what is not, and what is *simplified* is project status
rather than a lesson, and it was 170 lines of this file.

The two things from it that belong here, because they are how the
mistakes get made rather than what the mistakes were:

- **A length decoder that answers from a rule of thumb will not tell
  you about the slot the rule is wrong for.** `g4mh_insn_len` said "op6
  below 0x30 is 16 bits", which is true of every G4MH encoding except
  `JR`/`JARL disp32` -- 48 bits at op6 0x17, sharing MULH imm5's slot.
  So the second stage never ran, `g4mh_insn_is_48` was never asked
  about the case it had always handled, the two halfwords past the
  first read as zero, and the jump went to `pc + 0`. **An infinite
  loop, not a wrong answer**, in a fully-written implementation that
  had never once executed. The comment naming the slot was accurate;
  the code it described was unreachable.

  Two things generalise. **When a decoder is staged, the first stage's
  shortcut is where the exceptions hide** -- it is the one that answers
  before it has the bits that would tell it otherwise. And the same
  rule was spelled a second time in `g4mh_is_16bit`, which is how they
  came apart; it defers now, and a property test asserts the two agree
  across all 65536 first halfwords. That test exists because nothing
  downstream can see the divergence today -- the JIT declines that
  opcode for other reasons -- and a capability that depends on nobody
  exercising it is not a capability.
- **An ISA that reuses a register field as an opcode extension will not
  tell you when you ignore it.** `reg2 == 0` is an opcode extension
  throughout G4MH: `CALLT` hides in the `MOV imm5` slot, `DISPOSE` in
  `MOVHI`/`SATSUBI`, `MOV imm32` in `MOVEA`, `JMP disp32` in `MULHI`,
  `JR disp32` in `MULH imm5`, `PREPARE` in `JR`. Decoding on the opcode
  alone made six unimplemented instructions retire silently as writes
  into r0. Before adding an encoding, grep the manual for every
  instruction sharing its opcode -- **and its width**: a shared opcode
  can hold instructions of different lengths, and a wrong length is not a
  wrong answer, it is a desynchronised instruction stream.
- **There is a second encoder, and it is the only thing that can say a
  hand-written opcode constant is wrong.** `scripts/g4mh-check-encodings.sh`
  assembles with Renesas CC-RH and prints the fields this frontend
  decodes them into. Run it *before* writing a constant. It found
  `CMPF.S`'s field split on its first run, and every constant added
  since came from it.
- **A tool that silently drops a class of input reads as the input
  being unsupported -- and that reading gets written down.** The same
  script has now dropped three classes by parsing its listing too
  narrowly: 16-bit encodings (it matched only 8 hex digits), then
  48-bit ones (4 or 8, where those are 12), then anything on a
  continuation line (it demanded a numeric line-number column, and
  CC-RH lists the 48-bit forms under `--`). A dropped line and a
  rejected instruction look identical -- both are absent -- so the
  second of those became a *documented blocker*: "CC-RH's assembler
  does not accept `LD.DW`/`ST.DW`, so their encodings are not guessed
  at". It accepts them, and the encodings were a five-minute check
  behind a regex. When a tool reports that something is unsupported,
  confirm it *said so* rather than merely failing to mention it.
