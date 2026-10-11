# host / rv32

The RISC-V frontend on the native runner, on either backend: the
interpreter, or with `--jit` the x86-64 JIT. This is where conformance is
established, for both.

Devices added to the guest map: ACLINT MSWI at `0x0200_0000`, ACLINT MTIMER
at `0x0200_4000`, APLIC at `0x0C00_0000`. Same addresses as the firmware,
so a guest image is portable between them.

## Validation

```sh
./scripts/run-arch-test.sh                        # official riscv-arch-test
EMU_EXTRA_ARGS=--jit ./scripts/run-arch-test.sh   # the same, translated
./scripts/run-riscv-tests.sh                      # Berkeley suite
ctest --test-dir build/host -L fast
```

| suite | result |
|---|---|
| riscv-arch-test | **490/490**, interpreter and JIT. SoftFloat is mandatory now; when it was optional, turning it off failed 52 tests and every one of them was F |
| riscv-tests (Berkeley) | **77/77**, interpreter and JIT (the JIT through an `EMU_HOST` wrapper that adds `--jit`) |
| ctest `-L fast` | unit tests, and the guests through the runner on both backends |

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
  same ELF and diff against `emu-host --trace-count N`; a jump to
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

- **Zicbop prefetch.** Decoded as a hint and ignored, which is legal, but
  never exercised.

## Settled since this page was written

- **S-mode and Sv32 landed**, behind `vm_active` folded into
  `fetch_guard`, so a guest that never enables paging pays one branch
  that already existed. Linux boots.
- **Misaligned accesses are emulated by default** (`RV32_MISALIGNED`),
  because picolibc's word-at-a-time string routines read through odd
  pointers. The architecture suite's description of this core says they
  trap, so `run-arch-test.sh` builds its own runner with the option off —
  a build option that changes architectural behaviour has a second home
  in the DUT description.

## Discarded

- **Lazy interrupt-delivery evaluation as a win.** Neutral on the
  interrupt-free benchmark, and that is expected: with `mstatus.MIE` clear,
  `rv_hart_pending_irq` already returns after one load and one test. Left
  on because it pays when a guest actually enables interrupts and costs
  nothing otherwise.
