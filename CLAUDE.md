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
./scripts/run-arch-test.sh      # official riscv-arch-test: 135/135 integer, 224/224 with -DRV32_FPU_SOFTFLOAT=ON, 172/224 without
./scripts/run-riscv-tests.sh    # Berkeley suite, 76/77 (breakpoint needs Sdtrig)
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
- **`mstatus.FS` is checked for FP loads and stores but not for OP-FP.**
  `h_fs_off` gates `OP_LOAD_FP`/`OP_STORE_FP` only, so the open-coded
  arithmetic paths run whatever FS says, and nothing flushes when FS changes.
  Helper-routed OP-FP is correct because `rv_hart_fp` checks it. Whether a
  guest can observe the gap is unverified -- test before assuming either way.
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
