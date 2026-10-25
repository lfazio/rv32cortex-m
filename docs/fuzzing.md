# Fuzzing

One target so far: **`fuzz-rv32-jit`**, the RV32 interpreter against the
x86-64 JIT on whatever bytes libFuzzer produces. It found four defects
in its first hour, every one in code that both suites and `isatest`
passed, and one of them in the interpreter rather than the JIT.

```sh
scripts/fuzz.sh 600          # build build/fuzz, seed it, fuzz ten minutes
FUZZ_STATS=1 build/fuzz/tests/fuzz/fuzz-rv32-jit crash-...   # replay one
cd build/fuzz && ctest -L fuzz   # the smoke run and every saved regression
```

`-DEMU_FUZZ=ON` needs clang and the host platform. It builds the *whole*
tree under ASan and UBSan with libFuzzer's coverage instrumentation --
the emulator's own decoder compares are what steer the fuzzer towards
valid encodings -- so it is a build directory of its own, never one a
figure is measured in. `-fsanitize=function` is off: it checks an
indirect call by reading a signature word ahead of the callee, and a
translated block has none, so the first call into the code buffer read
below its start and the harness faulted on its first input.

## What it compares

The input is the program. Both sides get their own hart and bus, built
identically, and run from `0x2000_0000`:

| | address | what for |
|---|---|---|
| code, read-only | `0x2000_0000`, 4 KiB | the input, then `ecall`s to the end |
| trap vector, read-only | `0x2001_0000` | `ecall` |
| data RAM, read-write, **not executable** | `0x8000_0000`, 16 KiB | a fixed pseudo-random fill |

x1-x31 start at `0x8000_2000 + 32n`, so any of them is a base register
into RAM; the f registers hold small singles (boxed) and doubles; FS is
Initial; `mtime` is a constant. The ECALL hook halts the hart, so a trap
of any kind, an `ecall` in the program and falling off its end all stop
the run at the same instruction on both sides.

Each choice exists to remove a difference the architecture permits:

- **Code is ROM**, so a store into it faults on both. Self-modifying code
  without FENCE.I may legally differ between an interpreter and a
  translator.
- **Data is not executable**, so a jump into it faults rather than
  running bytes the program wrote.
- **A run that does not finish is not compared.** A JIT stops only
  between blocks, so at the budget the two stop at different
  instructions. If one side finished and the other ran *past* that
  point, that is reported; if it stopped short, it is not.

Then every x and f register, the pc, fcsr, privilege, state, the trap
count, every M- and S-mode CSR the hart keeps, the PMP and trigger
registers, the reservation, minstret, mcycle, the hpm counters and all
of data RAM must agree. Two things may differ, and the harness says why
beside each:

- **mstatus.FS may be dirtier under the JIT, never cleaner.** The
  privileged spec lets an implementation track it imprecisely ("may
  choose to ... report the state to be dirty even when it has not been
  modified"; SmF's `mstatus_fs_imprecise`, which the arch tests call
  untestable). The JIT marks a block's FP writes at its exit, so a block
  whose `flw` faults reports Dirty. Cleaner would be a defect -- an OS
  reading Clean does not save the registers -- and was one.
- **The run's own instruction count.** That is budget bookkeeping: the
  interpreter charges a trapped instruction to the budget and the JIT
  framework does not when a block traps on its first instruction, which
  is why `isatest` reports different counts on the two backends.
  minstret is the architectural count, and it is compared.

The smoke test (`fuzz-rv32-jit-smoke`) requires the stats line to show
runs compared *and* blocks translated: a harness whose runs never finish,
or whose JIT declines everything, passes every input and tests nothing.
Ten minutes is about 158,000 inputs, 95% of them compared, 7.8M
translations and 9.8M block entries.

## What it found

Each fix was confirmed by reverting it with its regression in place.

| defect | where | held by |
|---|---|---|
| a trapping instruction -- ECALL and EBREAK too -- counted in minstret and mcycle, and so did interrupt entry | the **interpreter**, `rv_interp.c` | `probes/instret.S` (Sail and Spike: it does not retire), `regressions/rv32-jit/trapped-load-minstret` |
| LOAD funct3 3, 6 and 7, STORE funct3 3-7 and JALR funct3 != 0 translated as real instructions -- LD as an 8-byte load, LWU as LW, funct3 4-7 stores as SB/SH/SW, any JALR as a jump | `rv_ir.c`, both hosts | `isatest` `rv32-ld` ... `rv32-jalr-f3-7`, `regressions/rv32-jit/lwu-translated` |
| a misaligned RAM or ROM access is a cast to `uint16_t *` at an odd address: UB, harmless on x86-64, M4 and M7, a fault on ARMv6-M -- under a comment saying callers guaranteed alignment | `emu_bus.c`, `emu_bus.h` | `regressions/rv32-jit/flh-misaligned-host-load` (UBSan) |
| a block that only moved a float and left by a constant exit chained, skipped the call that marks FS dirty, and left FS Clean with f-state changed | both JIT backends' `ir_lower.c` | `isatest` `fs-dirty-flw-chained`, `regressions/rv32-jit/flw-chained-fs-clean` |

And two inputs held for the permitted differences above:
`flw-fault-fs-dirtier` and `trap-first-in-block-run-count`.

Things worth keeping from finding them:

- **The interpreter is the reference, and was the one that was wrong
  first.** The probe asked Sail and Spike rather than reasoning about
  which backend to fix. On mcycle they disagree with each other -- Sail
  charges a trap a cycle, Spike does not -- and cycles are the
  implementation's to define, so here mcycle follows minstret on both
  backends and Sail's answer is the probe's permitted difference.
- **Fixing the interpreter's count without moving its loop took five
  tries.** A label past the increments, suppressing them through
  `ctr_written`, a helper returning the new pc: each read 0.5-1.1% more
  host instructions on CoreMark, for paths no benchmark takes. What
  stayed at 0.00% changes only which function the trap path calls
  (`rv_hart_trap_unretire`), and it needed `noipa`: without it GCC
  cloned the function per constant cause and split the interpreter, for
  1.2%.
- **LTO partitioning can move a row by 1% on its own.** With all of the
  above at 0.00% in a one-partition build (`-flto-partition=one`), the
  default build still read ARMv7-M's interpreter 1.07% slower -- a
  change in RV32 files moved where the partitions fell. The one-partition
  build is also 4.4% faster for that interpreter outright; see
  [TODO.md](TODO.md).
- **The bus fix keeps the type.** `memcpy` makes an access a byte
  access, which may alias anything; the alignment-1 typedef keeps
  `uint16_t` and `uint32_t`, so the compiler's aliasing rules are what
  they were, and the F746 firmware differs only in four compare-and-
  branches that became `cbnz` in `rv_hart_load` and `rv_hart_store`.
  The 1.07% on ARMv7-M was first blamed on `memcpy`, and stayed with
  the old bus put back -- it was the partitioning above.
- **The FS defect was a decision taken before the information it
  needed.** Chainability is fixed in the prologue; "this block writes an
  f register" was learnt while lowering its body. The fix scans for
  `FPUT` beforehand, which costs Whetstone under the x86-64 JIT 2.09% of
  host instructions, because those blocks no longer chain at all. Marking
  FS dirty inside the block would let them -- see [TODO.md](TODO.md).

## Adding a finding

Fix it, confirm the fix by reverting it, and copy the input to
`tests/fuzz/regressions/rv32-jit/` under a name that says what it found.
`fuzz-rv32-jit-regressions` replays the directory. Where the defect can
reach a board -- anything in `rv_ir.c` or a backend -- add an `isatest`
check as well: the fuzzer runs only on x86-64, and the Thumb-2 backend
shares the translator and had the same chaining defect.

What it cannot reach yet: interrupts (none are enabled), devices and
MMIO, more than one hart, runs longer than 20,000 instructions, and the
other frontends. The other targets are in [TODO.md](TODO.md).
