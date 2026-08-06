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
| platform | where it runs | `RV32_PLATFORM=host\|stm32f446` |
| frontend | what it emulates | `EMU_FRONTEND_RV32`, `EMU_FRONTEND_G4MH` |
| backend | how it executes | `RV32_JIT=ON\|OFF`, per frontend |

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

## Build and test

```sh
# host: development and both test suites
cmake -B build/host -DRV32_PLATFORM=host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host && ctest --test-dir build/host -L fast

# both frontends, so the host runner can pick with --frontend
cmake -B build/both -DRV32_PLATFORM=host -DEMU_FRONTEND_G4MH=ON

# firmware
cmake -B build/f746 -DRV32_PLATFORM=stm32f746 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DRV32_GUEST=isatest
cmake --build build/f746 --target flash

# the older board; RV32_PLATFORM picks the CPU, FPU and vendor pack
cmake -B build/stm32f446 -DRV32_PLATFORM=stm32f446 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DRV32_GUEST=isatest
cmake --build build/stm32f446 --target flash
```

`rv32-host` picks a frontend from `--frontend`, else from the image's ELF
`e_machine`, else the first compiled in. A flat binary says nothing about its
architecture, so it gets the default.

Useful options: `-DRV32_JIT=OFF` (interpreter, for isolating JIT bugs),
`-DRV32_JIT_CODE_BYTES=2048` (forces compaction — a good stress test),
`-DRV32_NATIVE_COREMARK=ON` (native ARM baseline instead of the emulator),
`-DRV_GUEST_MARCH=...` (guest ISA; a **cache variable**, so pass it explicitly
when changing it), `-DRV32_GUEST=isatest|hello|bench|stm32drv|coremark`.

## Validation — run before claiming anything works

```sh
./scripts/run-arch-test.sh      # official riscv-arch-test: 274/274 with -DRV32_FPU_SOFTFLOAT=ON, 222/274 without (every failure is F)
./scripts/run-riscv-tests.sh    # Berkeley suite, 77/77
```

Keep the suites' `-march` in step with what `misa` advertises: `rv32mi/csr`
deliberately fails when built without F and run on a core reporting F, and that
failure looks like an emulator bug until you disassemble the test.

**Run both.** They cover different things, and a regression that only the
Berkeley suite catches will sit unnoticed if only arch-test is run -- which is
exactly what happened to `rv32mi/csr` when F was added.


There are two JIT backends, selected by host architecture in `rv_config.h`:
Thumb-2 for ARMv7E-M and **x86-64 for the host**. The x86-64 one exists for
coverage, not speed — with `--jit` the architecture suite and riscv-tests
run against *translated* code, and `ctest -L fast` includes
`guest-isatest-jit`:

```sh
./build/host/rv32-host --jit --quiet --load 0x80000000 build/host/guest/isatest.bin
RV32_HOST=<wrapper adding --jit> ./scripts/run-riscv-tests.sh
```

The **Thumb-2** backend still cannot be exercised by any host suite.
Validate it by flashing `isatest` and reading the UART — **this has caught
real JIT bugs**, including an inlined store that skipped the LR/SC
reservation break.

Hardware: Nucleo-**F746ZG** (Cortex-M7, 216 MHz) on ST-LINK, console
`/dev/ttyACM1` at **921600** 8N1 -- a Nucleo-144 puts the VCP on USART3
(PD8/PD9), not USART2. `probe-rs download --chip STM32F746ZGTx <elf>` then
`probe-rs reset`; add `--connect-under-reset` when running firmware holds
the debug port. The Nucleo-F446RE is still supported and is
`--chip STM32F446RETx` on `/dev/ttyACM0`. With both boards plugged in
`probe-rs` needs `--probe <serial>` to pick one.

Board bring-up -- clock tree, console instance and pins, cycle counter,
caches -- lives in each platform's `board.c` behind `board.h`, not in
`main.c`. Diffing the two `board.c` files is the shortest statement of
what changes between the parts.

## Things that have bitten, and will again

- **Extensions sharing an opcode slot must be decoded in one place.** Zbb's
  `min`/`max` and Zbc's `clmul` share funct7 0x05; a separate `else if` later in
  the chain is unreachable and every `clmul` raised illegal-instruction.
- **Put the common case first in decode.** Zbb tests placed ahead of
  `SLLI`/`SRLI` made every shift pay an extra compare and cost more than Zbb
  saved.
- **In the JIT, what you decline costs more than what you translate badly.**
  Ending a block for an untranslatable instruction fragments hot code. Route it
  through a helper call instead — `jit_helper_alu` exists for exactly this.
- **With the JIT on, FP arithmetic goes to VFP and never reaches SoftFloat.**
  To validate the SoftFloat path on hardware, build `-DRV32_JIT=OFF`. Running
  `isatest` both ways is a differential check between two genuinely different
  FP implementations, and is worth doing after any change to either.
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
- **Every translate-time read of mutable hart state is a staleness bug until
  proven otherwise.** The full list, swept:

  | read at translation | outcome |
  |---|---|
  | `fcsr` frm, for `rm=dyn` | was wrong for RMM -- fixed |
  | `mstatus.FS` (`h_fs_off`) | unchecked for OP-FP, and stale for loads/stores -- fixed |
  | `pmp_active` + `rv_pmp_simple` bounds | **permission bypass** -- fixed |
  | bus regions (RAM and passthrough windows) | safe: written only at init |
  | `g_pt_armed` | safe: flushes on change |
  | guest instruction bytes | safe: `FENCE.I` calls `rv_invalidate` |

  All three defects were invisible to both host suites, because neither runs
  the JIT. All three needed a hardware test with the fix reverted to prove.
- **Watching a flag is not watching the configuration.** The PMP flush
  compared `pmp_active`, but what a block bakes in is the *bounds*
  `rv_pmp_simple` reported. Locking a second entry leaves the flag true while
  the one-entry assumption it encodes stops holding, so a store to the new
  region took the inlined path and wrote memory PMP had been told to deny.
  Snapshot what was baked, not what enabled it.
- **frm, `mstatus.FS` and PMP all change only through a CSR write**, and the
  translator declines `SYSTEM`, so `jit_note_csr` on the interpreter fallback
  is the single place any of them can move. Do not put these in the dispatch
  loop: CoreMark enters blocks 2.9M times a run. Moving the PMP check off the
  dispatch path measured neutral, so the branch was predicting well -- the
  reason to do it is that the fallback is where the check is *correct*.
- **A translate-time legality check is only half a guard; the block outlives
  it.** `mstatus.FS` was consulted when translating FP loads and stores (and
  not at all for OP-FP or the FMAs), on the reasoning that refusing to
  translate while FS is Off was enough. It is not: a block built while FS was
  on stays cached and keeps executing after the guest turns the FPU off, so
  three instructions that must raise illegal-instruction ran silently. Fixed
  the same way as `frm` -- specialise, and flush on the interpreter fallback
  where FS can change. Track FS *off-ness*, not the two-bit field, or
  `emit_fp_dirty` moving Initial/Clean to Dirty flushes the cache on every FP
  operation. The same question applies to anything else decided at
  translation from mutable hart state.
- **JIT blocks are specialised on `frm`; changing it flushes.** `dyn` is
  resolved at translation, not at run time, which is what lets `RMM` be
  declined to the helper -- ARM has no ties-away mode, and the old run-time
  table silently mapped it to `RN`. The flush is safe to hang off the
  interpreter fallback alone: `frm` moves only on a CSR write, and the
  translator declines `SYSTEM`, so every write lands there. Do not move that
  check into the dispatch loop; CoreMark enters blocks 2.9M times a run.
  `g_frm_specialised` skips the flush entirely for guests with no `dyn` FP.
- **ARM and RISC-V float-to-int agree except on NaN.** Both saturate
  out-of-range to the target's limit, both raise invalid doing so, and
  neither adds inexact. Only NaN differs: ARM gives 0, RISC-V gives the
  *maximum*. `VCMP` of the operand against itself is unordered exactly for
  NaN, so `IT VS` / `MOV` patches it in seven instructions. Use `VCVTR`, not
  `VCVT` -- the latter forces round-toward-zero regardless of FPSCR.
- **One weak test is worse than none, because it reads as coverage.**
  `isatest` had a single `FCVT.W.S` check, `10.0` with `rtz`, which passes
  whether or not the NaN fixup exists at all. When adding a translation whose
  whole difficulty is one input, test *that* input. Proving the new path even
  runs is separate again: force it back to the helper and diff the
  `interp ... fell back` counter (112 against 134 here).
- **`RV32_JIT_CODE_BYTES` dominates JIT performance, and the 12 KB default is
  worse than no JIT at all.** CoreMark's translated working set is ~48 KB.
  Measured: 12 KB 10,850,998 ticks (8533 compactions, 94240 evictions), 24 KB
  9,329,706, 32 KB 8,525,192, 48 KB 6,463,217 (904), 64 KB 5,148,168 (231).
  The interpreter is 10,691,637 -- so at the default the JIT *loses*. Guest RAM
  pays one for one: 122 KiB with no JIT, 106 at 12 KB, 70 at 48 KB, 54 at 64 KB.
- **CMake cache variables silently outlive the tree you set them in.** Every
  performance figure in this repo had been measured with a 48 KB code cache
  inherited from an old build directory while the declared default was 12 KB;
  `rm -rf build/` and the numbers changed by 68% with no code change. Before
  quoting a measurement, check `CMakeCache.txt` for what actually built it --
  `RV32_JIT_CODE_BYTES`, `RV_GUEST_MARCH` and `COREMARK_ITERATIONS` are all
  cache variables and all change the result.
- **`RV_JIT_LOOP_CAP` is an interrupt-latency knob, and CoreMark cannot see
  it.** Measured at 64/128/256: CoreMark 31.39/31.16/31.25 (noise -- its loops
  end on unchainable branches, so the cap is not what exits them), `bench`
  18.88/18.39/18.13, `mmiobench` 24.40/23.46/22.99 with block entries halving
  exactly per doubling and its tightest kernels gaining 18% at 256. Each
  doubling returns half the previous one and doubles worst-case latency
  (~11/22/44 us), so 128 is the knee and the default. Do not tune this on
  CoreMark alone.
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
- **Do not conflate "nothing translatable here" with "cache full".** Sharing a
  recovery path made every interpreted `div` flush the code cache.
- **The JIT was only ever kept off the x86 host by nobody selecting it.**
  `RV_ENABLE_JIT` defaults to a `__thumb2__` test, but CMake defined it
  unconditionally from `RV32_JIT`, so the host build compiled a Thumb-2
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
  Measured on the interpreter (`-DRV32_JIT=OFF`), which is where a fetch
  cost lands -- the JIT pays it once per *translation*.
- **An A/B that only half-reverts the fix reads as a passing test.** The
  translator checks fetch permission at two sites, one per halfword.
  Disabling the first alone still gave a clean 243/243, because the
  instruction was 32-bit and the second site caught it -- which looked
  exactly like "the test does not exercise this path". Disable every site,
  and confirm the failure names the mechanism: `pmpx-exec-noeffect` got
  `0xBAD`, i.e. the store really had run inside a no-execute region.
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
  against `rv32-host --trace-count N`; a jump to `Mtrampoline` in the reference
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
- **Measure; do not reason about performance.** Interpreter-in-SRAM was
  *slower*, lazy-IRQ was neutral, and the `clmul` fix was 1.3% when the real
  cost was 4.12-instruction blocks. Layout noise is ±3%, so ignore differences
  below that.
- Guest images link `-nostdlib`; there is no libc. The **core** must not call
  libm either, which is why `fsqrt` is Newton-Raphson rather than `sqrt()`.
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

### G4MH scope

**Implemented.** Formats I and II (the 16-bit reg-reg and imm5 ALU),
III (`Bcond disp9`), IV (`SLD`/`SST` .B/.H/.W through EP), V (`JR`/`JARL
disp22`), VI (the imm16 ALU group), VII (`LD`/`ST` .B/.H/.W `disp16`),
`MOV imm32`, and the Format X system group: `LDSR`, `STSR`, `TRAP`, `RETI`,
`HALT`, `DI`/`EI`, the register-form shifts, `MUL`/`MULU`, `DIV`/`DIVU`,
`SETF`. Plus both exception levels with their own save registers, the
PSW/system-register file, and an interrupt controller with a time base.

Everything else raises `G4MH_EXC_RIE`, which is the correct report for an
unimplemented encoding rather than a silent wrong answer.

**Not implemented, roughly in the order a real guest would miss them:**

| gap | why it matters |
|---|---|
| `PREPARE` / `DISPOSE` | every non-leaf function a compiler emits uses them for its frame |
| `LD.BU` / `LD.HU`, `SLD.BU` / `SLD.HU` | unsigned loads; only the sign-extending forms exist |
| `CALLT` / `CTRET` | `CTBP`/`CTPC`/`CTPSW` are storage with no instructions behind them |
| `CMOV`, `ADF`/`SBF`, `SASF` | the branchless idioms a compiler prefers |
| `BSW`/`BSH`/`HSW`/`HSH`, `SCH*` | byte swaps and bit search |
| Format VIII `SET1`/`CLR1`/`NOT1`/`TST1` | bit manipulation on memory; opcode 0x3E |
| `CAXI`, `LDL.W`/`STC.W` | the atomics — nothing at all today |
| 48-bit `JMP`/`JR`/`JARL disp32`, disp23 loads and stores | long-range code and data |
| `MAC`/`MACU`, the imm9 `MUL`/`DIV` forms, 3-operand `DIVH` | |
| `SYNCE`/`SYNCM`/`SYNCP`/`SYNCI`, `CACHE`, `PREF`, `SNOOZE` | |
| the FPU | `FPSR`/`FPEPC`/`FPST`/`FPCC`/`FPCFG`/`FPEC` exist as storage; no FP instruction is decoded. `G4MH_EXT_FPU` is the switch that would turn it on |

**Architectural features not modelled:**

- **The MPU.** `MPLA`/`MPUA`/`MPAT`/`MPM` and the protection checks behind
  them — the analogue of RISC-V PMP. `MIP` and `MDP` are raised today only
  by a bus fault, never by a protection region.
- **User mode.** `PSW.UM` is defined and nothing enforces it: `LDSR`,
  `STSR`, `DI`, `EI`, `HALT` and `RETI` do not check privilege. Note what
  the RISC-V side of this repo learned the hard way — U-mode turned three
  latent M-mode PMP bugs into failures at once. Expect the same here.
- **Coprocessor gating.** `PSW.CU0-2` and `G4MH_EXC_UCPOP` are defined and
  never consulted.
- **Interrupt priority.** The INTC has a 4-bit priority per channel but does
  not maintain `ISPR` or honour `PMR`, so nesting is not modelled. `INTBP`
  and the table-reference entry method are absent; entry uses the single
  direct vector only.
- **Register banks and hardware context save**, `GMCFG`, the guest modes.
- **Debug level.** No `DBPC`/`DBPSW`, `DBTRAP`/`DBRET` — the analogue of
  Sdtrig.
- **No JIT.** G4MH runs on the interpreter; a Thumb-2 translator for it
  would be a second `emu_backend_t` beside `g4mh_backend_interp`.

**Simplifications to be aware of before trusting a result:**

- The INTC is now the real thing: INTC1 (core-local, channels 0-31) at
  `0xFFFC_0000` SELF and `0xFFFC_4000` PE0, INTC2 (global, channels 32 up)
  at `0xFFF8_0000`, with the EICn bit layout from the U2B manual Section
  6.3. Modelled on the **U2B6**, which has three PEs -- the manual's base
  table runs to PE5 because the larger parts have six. The `OSTM` at
  `0xFFEC_0000` is still a stand-in rather than the real register set.
  What is *not* modelled: `EEIC`, table-reference delivery, and `EIBD`
  is stored but does not route.
- Exception vectors use the compact offsets in `handler_address()`; a real
  part's table is larger and `RBASE`/`EBASE` flag bits are masked off rather
  than honoured.
- Misaligned data accesses raise `MAE`. Most real G4MH parts permit them.
- `PID`, `HTCFG0` and `MCFG0` read as zero rather than identifying a part.

**The encodings are now checked against the manual, and the first pass was
wrong in six places.** `docs/renesas/rh850g4mh-users-manual-software.pdf`
(R01US0209EJ0220) is the authority; it settled that

- `LDSR` and `STSR` use their two register fields in *opposite* senses, so
  implementing one by analogy with the other gets it backwards;
- `JR`/`JARL disp22` carries its *high* displacement bits in the first
  halfword, the reverse of the RISC-V habit;
- G4MH has no `RETI` at all -- V850's single return was split into
  `EIRET`, `FERET` and `CTRET`, which name their level in the opcode
  rather than inferring it from PSW;
- sub-opcode 0x160 is shared by `DI`, `EI`, `PUSHSP`, `POPSP` and `CLL`,
  told apart by the whole reg2 field and not by its top bit;
- `HALT` and `SNOOZE` share 0x120;
- **reg2 == 0 is an opcode extension throughout.** `CALLT` hides in the
  `MOV imm5` slot, `DISPOSE` in `MOVHI`/`SATSUBI`, `MOV imm32` in `MOVEA`,
  `JMP disp32` in `MULHI`, `JR disp32` in `MULH imm5`, `PREPARE` in `JR`.
  Decoding on the opcode alone made six unimplemented instructions retire
  silently as writes into r0 instead of raising RIE -- which is far worse
  than not implementing them, because the guest gets a wrong answer rather
  than a clean exception. Every such slot now tests reg2.

The lesson generalises: **an ISA that reuses a register field as an opcode
extension will not tell you when you ignore it.** Before adding an
encoding, grep the manual for every instruction sharing its opcode, not
just the one being added.

**There is no reference model and no toolchain.** RV32 has riscv-arch-test,
the Berkeley suite and Sail to disagree with; G4MH has none of that here, and
no `rh850-elf-gcc` to build a guest with. Its tests are hand-assembled
halfword arrays in `tests/unit/test_g4mh.c`, deliberately not sharing an
encoder with the interpreter — a shared one would pass while both were wrong
the same way. Treat any G4MH result as verified only as far as those tests
reach.
