# rv32cortex-m — working notes

RV32 emulator for ARM Cortex-M hosts. The guest drives the host's real
peripherals through an identity-mapped passthrough window, so **peripheral
drivers live in the guest, not in the emulator**. Keep it that way: adding a
GPIO or UART driver to `src/platform/` is almost always the wrong fix.

## Build and test

```sh
# host: development and both test suites
cmake -B build/host -DRV32_PLATFORM=host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host && ctest --test-dir build/host -L fast

# firmware
cmake -B build/stm32f446 -DRV32_PLATFORM=stm32f446 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DRV32_GUEST=isatest
cmake --build build/stm32f446 --target flash
```

Useful options: `-DRV32_JIT=OFF` (interpreter, for isolating JIT bugs),
`-DRV32_JIT_CODE_BYTES=2048` (forces compaction — a good stress test),
`-DRV32_NATIVE_COREMARK=ON` (native ARM baseline instead of the emulator),
`-DRV_GUEST_MARCH=...` (guest ISA; a **cache variable**, so pass it explicitly
when changing it), `-DRV32_GUEST=isatest|hello|bench|stm32drv|coremark`.

## Validation — run before claiming anything works

```sh
./scripts/run-arch-test.sh      # official riscv-arch-test: 230/230 with -DRV32_FPU_SOFTFLOAT=ON, 178/230 without (every failure is F)
./scripts/run-riscv-tests.sh    # Berkeley suite, 77/77
```

Keep the suites' `-march` in step with what `misa` advertises: `rv32mi/csr`
deliberately fails when built without F and run on a core reporting F, and that
failure looks like an emulator bug until you disassemble the test.

**Run both.** They cover different things, and a regression that only the
Berkeley suite catches will sit unnoticed if only arch-test is run -- which is
exactly what happened to `rv32mi/csr` when F was added.

```sh
```

The JIT cannot be exercised by the x86 host suites. Validate it by flashing
`isatest` (104 checks) and reading the UART — **this has caught real JIT bugs**,
including an inlined store that skipped the LR/SC reservation break.

Hardware: Nucleo-F446RE on ST-LINK, console `/dev/ttyACM0` at 115200 8N1.
`probe-rs download --chip STM32F446RETx <elf>` then `probe-rs reset`.

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

`src/core/` is portable C11 with no platform dependencies and must stay that
way — it builds for ARMv6-M through ARMv8.1-M and for the host. Shared
semantics (`rv_hart_amo`, `rv_hart_cbo`) live in the core so the interpreter and
JIT cannot drift apart. New ISA work goes in **both** backends plus
`tests/arch-test/` config, or is declared unsupported.
