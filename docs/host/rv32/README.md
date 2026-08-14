# host / rv32

The RISC-V frontend on the native runner, always on the interpreter — the
JIT is forced off on a non-Thumb-2 host. This is where conformance is
established.

Devices added to the guest map: ACLINT MSWI at `0x0200_0000`, ACLINT MTIMER
at `0x0200_4000`, APLIC at `0x0C00_0000`. Same addresses as the firmware,
so a guest image is portable between them.

## Validation

```sh
./scripts/run-arch-test.sh      # official riscv-arch-test
./scripts/run-riscv-tests.sh    # Berkeley suite
ctest --test-dir build/host -L fast
```

| suite | result |
|---|---|
| riscv-arch-test | **274/274**. SoftFloat is mandatory now; when it was optional, turning it off failed 52 tests and every one of them was F |
| riscv-tests (Berkeley) | **77/77** |
| ctest `-L fast` | unit tests + the guest self-test through the runner |

**Run both suites.** They cover different things, and a regression only the
Berkeley suite catches will sit unnoticed — which is exactly what happened
to `rv32mi/csr` when F was added.

Keep the suites' `-march` in step with what `misa` advertises. `rv32mi/csr`
deliberately fails when built without F and run on a core reporting F, and
that failure looks like an emulator bug until you disassemble the test.

## Things that have bitten

- **A failing arch test may be the Sail config, not the emulator.** ACT
  runs the golden model to bake expected values into each test, so a wrong
  `sail.json` produces wrong expectations. `amocas` failed for three
  sessions because guest RAM declared `atomic_support: AMOArithmetic`,
  which excludes CAS, so Sail *trapped* and the signatures recorded the
  trap. When targeted checks say an instruction is right and the suite
  disagrees, run `sail_riscv_sim --config <sail.json> --trace-instr` on the
  same ELF and diff against `rv32-host --trace-count N`; a jump to
  `Mtrampoline` in the reference is the tell.
- **ACT's `--extensions` selects test suites by *directory name*, not by
  required extension.** `U` matches nothing and silently builds nothing.
  The U-mode PMP tests live in `tests/priv/pmp/pmp32/**PMPU**`. What a test
  *requires* is declared in its own `REQUIRED_EXTENSIONS` header and
  checked against the UDB config by `select_tests`; naming a suite only
  offers it. Four rounds of guessing the flag would have been one round of
  reading `framework/src/act/parse_test_constraints.py`.
- **U-mode turns latent M-mode bugs into failures at once.** Three PMP
  defects sat in shipped, suite-passing code because every M-mode path
  through them ends in "matching nothing permits": the lock bit read
  without the privilege level, `1u << 32` decoding the widest NAPOT region
  as the narrowest, and execute permission never being checked at all.
  Assume the PMP code is wrong until the privileged tests say otherwise —
  and they will not run until the suite is named correctly.
- **Enabling `F` forces `Zcf` on RV32.** `C@2.0` is defined to include the
  compressed FP load/stores, and UDB rejects the config without it.

## Interpreter cost, measured

- **Sdtrig costs 14.8 cycles per instruction and PMP 3.2** — 46% together,
  measured by compiling each out. Hoisting the `trig_active` load into a
  local made it *worse*; the cost is the `TRAP` call site in the fetch
  sequence, not the load.
- **Folding both behind one `h->fetch_guard` took the PMP execute check
  from 9.3% to 2.7%.** Anything on the fetch path is paid per instruction
  by every guest, so give the features one branch, not one each. Measured
  on `-DEMU_JIT=OFF`, which is where a fetch cost lands.
- **Compressed guest code is slower to interpret, not faster.** Enabling
  Zcb in guest codegen cost ~9% on CoreMark at an identical instruction
  count: the compiler swapped 32-bit encodings for Zcb ones, each of which
  now pays an RVC expansion. Supporting Zcb in the *emulator* is a small
  win (38.0 vs 39.2 cyc/insn); it is the *guest* march that costs.

## To do

- **S-mode.** Started and not landed — see the uncommitted work on
  `main`: the CSR bank, delegation, `SRET` and the TVM/TW/TSR traps,
  with `satp` Bare only. Sv32 would put a page-table walk on the fetch and
  access paths, which is the most expensive place in this emulator to add
  anything.
- **`amocas.d`.** Implemented and wrong; `RV_EXT_ZACAS` gates it.
- **Zicbop prefetch.** Decoded as a hint and ignored, which is legal, but
  never exercised.

## Investigate

- **Whether `RV_MISALIGNED_OK` should default on.** We raise, which matches
  most embedded RISC-V cores and keeps the memory path branch-free. A guest
  that relies on hardware misalignment support gets an exception instead.

## Discarded

- **Lazy interrupt-delivery evaluation as a win.** Neutral on the
  interrupt-free benchmark, and that is expected: with `mstatus.MIE` clear,
  `rv_hart_pending_irq` already returns after one load and one test. Left
  on because it pays when a guest actually enables interrupts and costs
  nothing otherwise.
