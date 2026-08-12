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

## Build and test

```sh
# host: development and both test suites
cmake -B build/host -DEMU_PLATFORM=host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host && ctest --test-dir build/host -L fast

# both frontends, so the host runner can pick with --frontend
cmake -B build/both -DEMU_PLATFORM=host -DEMU_FRONTEND_G4MH=ON

# firmware
cmake -B build/f746 -DEMU_PLATFORM=stm32f746 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DRV32_GUEST=isatest
cmake --build build/f746 --target flash

# the older board; EMU_PLATFORM picks the CPU, FPU and vendor pack
cmake -B build/stm32f446 -DEMU_PLATFORM=stm32f446 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DRV32_GUEST=isatest
cmake --build build/stm32f446 --target flash
```

`rv32-host` picks a frontend from `--frontend`, else from the image's ELF
`e_machine`, else the first compiled in. A flat binary says nothing about its
architecture, so it gets the default.

Useful options: `-DEMU_JIT=OFF` (interpreter, for isolating JIT bugs),
`-DEMU_JIT_CODE_BYTES=2048` (forces compaction — a good stress test),
`-DEMU_NATIVE_COREMARK=ON` (native ARM baseline instead of the emulator),
`-DRV_GUEST_MARCH=...` (guest ISA; a **cache variable**, so pass it explicitly
when changing it), `-DRV32_GUEST=isatest|hello|bench|stm32drv|coremark`.

## Validation — run before claiming anything works

```sh
./scripts/run-arch-test.sh      # official riscv-arch-test, interpreter and --jit alike
./scripts/run-riscv-tests.sh    # Berkeley suite, 77/77 both ways
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
(PD8/PD9), not USART2. The `flash` targets pass `--connect-under-reset`
always, because this firmware never idles and a plain attach races it --
see the entry below on what a *failed* flash costs. The
Nucleo-F446RE is still supported and is
`--chip STM32F446RETx` on `/dev/ttyACM0`. With both boards plugged in
`probe-rs` needs `--probe <serial>` to pick one.

Board bring-up -- clock tree, console instance and pins, cycle counter,
caches -- lives in each platform's `board.c` behind `board.h`, not in
`main.c`. Diffing the two `board.c` files is the shortest statement of
what changes between the parts.

### Network transport (`-DEMU_NET=ON`, F746 only so far)

lwIP over **SLIP on the console UART**, so the board takes a guest image
and reports results without reflashing -- which is what makes running
274 architecture tests on hardware practical. There is no second wire:
SLIP reuses the ST-LINK's virtual COM port rather than bringing up the
on-board LAN8742A, which would mean an ETH driver, DMA descriptors
maintained by hand on a part with caches, and PHY bring-up.

```sh
cmake -B build/f746net -DEMU_PLATFORM=stm32f746 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DEMU_NET=ON
cmake --build build/f746net --target flash
sudo ./scripts/slip-up.sh          # *after* the board prints "net SLIP on this port"
ping 192.168.7.2 && telnet 192.168.7.2
```

**The UART stops being a console.** `emu_net_init()` is a one-way
handover; after it the serial line carries nothing but IP and the
console is telnet. Everything printed before a client connects is held
in a 4 KiB ring and delivered on connect, which is why the banner
survives. Build with `EMU_NET=OFF` to get the serial console back.

Costs, measured on hardware: guest RAM **294 KiB to 276**, against the
worst architecture test's 222. `rx drops 0` across a full `isatest` run.

Still to do: TFTP with `rom`/`ram` pseudo-files, and the RISCOF shim.

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
  These notes said `-DEMU_FPU_SOFTFLOAT=ON` against a real
  `RV32_FPU_SOFTFLOAT`, defaulting to OFF. So every default build had an
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
- **`EMU_JIT_CODE_BYTES` dominates JIT performance, and the 12 KB default is
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
  `EMU_JIT_CODE_BYTES`, `RV_GUEST_MARCH` and `COREMARK_ITERATIONS` are all
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
    framework returned NULL for a block that overran the buffer, exactly
    as it does for one the translator declined -- so the interpreter ran
    one instruction and the same oversized block was translated and thrown
    away again, 957 times. The existing lesson below is the same defect
    mirrored: **"declined", "overflowed" and "cache full" are three
    outcomes and collapsing any two of them is pathological.** They are
    now counted, not reasoned about.
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
  against `rv32-host --trace-count N`; a jump to `Mtrampoline` in the reference
  is the tell.
- **Compressed guest code is slower to interpret, not faster.** Enabling Zcb in
  guest codegen cost ~9% on CoreMark at an identical instruction count: the
  compiler swapped 32-bit encodings for Zcb ones, each of which now pays an RVC
  expansion. Supporting Zcb in the *emulator* is a small win (38.0 vs 39.2
  cyc/insn); it is the *guest* march that costs. Toggle `RV32_EXT_ZCB` against
  a fixed guest binary to separate the two.
- **Instruction fusion is the wrong target; the register-file round trip
  is the right one.** Measured with `-DEMU_PAIR_STATS=ON`, which
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
  with `-DEMU_PAIR_STATS=ON` on CoreMark *after* building it:

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

### G4MH scope

**Implemented.** Formats I and II (the 16-bit reg-reg and imm5 ALU),
III (`Bcond disp9`), IV (`SLD`/`SST` .B/.H/.W through EP), V (`JR`/`JARL
disp22` and `disp32`), VI (the imm16 ALU group), VII (`LD`/`ST` .B/.H/.W
`disp16`), VIII (the memory bit ops), `MOV imm32`, and the Format X
system group: `LDSR`, `STSR`, `TRAP`, `EIRET`/`FERET`/`CTRET`, `HALT`,
`DI`/`EI`, `CLL`, the register-form shifts, `MUL`/`MULU`, `DIV`/`DIVU`,
`SETF`, `CAXI`, `LDL.W`/`STC.W`, the swap and bit-search group.

Plus the set a compiler actually emits, added later: `PREPARE`/`DISPOSE`
with the full list12, `CALLT`, the unsigned loads `LD.BU`/`LD.HU` and
`SLD.BU`/`SLD.HU`, the branchless `CMOV`/`ADF`/`SBF`/`SASF`, `MAC`/`MACU`,
`BINS`, `ROTL`, `LOOP`, `PUSHSP`/`POPSP`, `JARL [reg1], reg3`, and
`JMP disp32`.

Everything else raises `G4MH_EXC_RIE`, which is the correct report for an
unimplemented encoding rather than a silent wrong answer.

**Not implemented, roughly in the order a real guest would miss them:**

| gap | why it matters |
|---|---|
| the FPU | `FPSR`/`FPEPC`/`FPST`/`FPCC`/`FPCFG`/`FPEC` exist as storage; no FP instruction is decoded. `G4MH_EXT_FPU` is the switch that would turn it on |
| the disp23 loads and stores | the 48-bit long-displacement forms; they share the `0x3C`/`0x3D` slot with `LD.BU` and PREPARE and are declined there |
| `CLIP.B`/`.BU`/`.H`/`.HU` | saturating narrowing |
| `LD.DW` / `ST.DW`, `LDL.BU`/`LDL.HU`, `STC.B`/`STC.H` | the doubleword and narrow atomic accesses |
| `LDM.MP` / `STM.MP`, `RESBANK` | bank and context-block transfers |
| `DIVHU`, `DIVQ`/`DIVQU`, 3-operand `DIVH`, the imm9 `MUL`/`DIV` forms | |
| `FETRAP`, `SYSCALL` | further trap flavours; `TRAP` is there |
| `PREPARE list12, imm5, imm32` | the only 64-bit encoding in the ISA, and past what the length decoder reports — see below |
| `CACHE`, `PREF` | `SYNCE`/`SYNCM`/`SYNCP`/`SYNCI` and `SNOOZE` are decoded and are no-ops here |

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

**And it happened again, twice, in the same shape.** Adding the compiler
set found both:

- `SATADD imm5` never tested reg2, so half of every `CALLT` -- the half
  with bit 5 of the vector set, because the opcode's low bit *is*
  imm6[5] -- retired as a saturating add into r0. Discarded, call never
  made, nothing to show for it. Its neighbour `MOV imm5` had the check.
  One slot of a straddling pair being guarded is not the pair being
  guarded.
- **A shared opcode can hold two instructions of different lengths, and
  that is worse than two of the same length.** `g4mh_insn_is_48` answered
  "48-bit" for the whole of `0x37` with reg2 == 0 and for the whole of
  `0x3C`/`0x3D` with bit 0 of the second halfword set. `LOOP` is 32-bit
  and lives in the first; `PREPARE`'s short form is 32-bit and lives in
  the second, alongside 48-bit disp23 loads *and* a 64-bit `PREPARE`. A
  wrong length is not a wrong answer, it is a desynchronised instruction
  stream -- every instruction after it is garbage. The length decoder and
  the execute switch have to make the *same* test, and the only way to
  know is to enumerate everything in the slot including its width.

Both were invisible while the instructions raised RIE, which is the
argument for implementing a slot completely rather than one encoding of
it at a time.

**There is no reference model and no toolchain.** RV32 has riscv-arch-test,
the Berkeley suite and Sail to disagree with; G4MH has none of that here, and
no `rh850-elf-gcc` to build a guest with. Its tests are hand-assembled
halfword arrays in `tests/unit/test_g4mh.c`, deliberately not sharing an
encoder with the interpreter — a shared one would pass while both were wrong
the same way. Treat any G4MH result as verified only as far as those tests
reach.
