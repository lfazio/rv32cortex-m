# Fuzzing

Four targets, one per frontend, each the interpreter against the x86-64
JIT on whatever bytes libFuzzer produces. Between them they found nine
defects in code that every suite passed -- four of them in an
interpreter, which is the reference -- and one of those turned out to
be a whole class, which a board then answered encoding by encoding. A
tenth, in G4MH's interrupt ceiling, was found by writing a seed for the
interrupt controllers each target now maps: both backends shared it.

| target | the reference, when the two disagree |
|---|---|
| `fuzz-rv32-jit` | the interpreter; Sail and Spike for what it might have wrong (`scripts/run-probe.py`) |
| `fuzz-armv7m-jit` | the interpreter; the Nucleo-F746ZG for what it might have wrong (`tests/armv7m-diff/`) |
| `fuzz-g4mh-jit` | the interpreter and the manual, and nothing else: no RH850 is attached |
| `fuzz-ppc-jit` | the interpreter and the manual, and nothing else: no e200 is attached |

No GPL emulator is consulted, by any of them.

```sh
scripts/fuzz.sh 600 armv7m   # build build/fuzz, seed it, fuzz ten minutes
scripts/fuzz.sh 600 all      # the four in turn
FUZZ_STATS=1 build/fuzz/tests/fuzz/fuzz-armv7m-jit crash-...   # replay one
cd build/fuzz && ctest -L fuzz   # each target's smoke run and saved regressions
```

`-DEMU_FUZZ=ON` needs clang and the host platform. It builds the *whole*
tree under ASan and UBSan with libFuzzer's coverage instrumentation --
the emulator's own decoder compares are what steer the fuzzer towards
valid encodings -- so it is a build directory of its own, never one a
figure is measured in. `-fsanitize=function` is off: it checks an
indirect call by reading a signature word ahead of the callee, and a
translated block has none, so the first call into the code buffer read
below its start and the harness faulted on its first input.

## What every target shares

`tests/fuzz/fuzz_harness.h` is the part that belongs to no architecture:
the run loop, the rule for a run that did not finish, the report and the
stats line. Each target builds two cores on two buses from identical
state and runs one under each backend:

| | address | what for |
|---|---|---|
| code, read-only | `0x2000_0000`, 4 KiB | the program, then the frontend's stop instruction to the end |
| traps, read-only | `0x2001_0000` | every vector, filled with the stop instruction |
| data RAM, read-write, **not executable** | `0x8000_0000`, 16 KiB | a fixed pseudo-random fill |
| the interrupt controller | where the frontend puts it | each side its own; see [Devices](#devices-and-interrupts) |

| | stops the core | traps go to | lr starts at | first four bytes |
|---|---|---|---|---|
| RV32 | `ecall`, through the ECALL hook | `mtvec` | RAM | the program |
| ARMv7-M | `bkpt` | `VTOR`, every vector a `bkpt` | a `bkpt` | the configuration |
| G4MH | `halt` (the run reports WFI) | `RBASE` and `EBASE` | a `halt` | the configuration |
| PowerPC | `wait` (WFI), the same word in VLE and Book E | `IVPR`, `IVORn` = 16n, so the pc says which | a `wait` | the configuration |

The general registers start pointing into RAM, 32 bytes apart, so any
of them is a base. Each choice removes a difference the architecture
permits:

- **Code is ROM**, so a store into it faults on both. Self-modifying
  code without a synchronising instruction may legally differ between
  an interpreter and a translator.
- **Data is not executable**, so a jump into it faults rather than
  running bytes the program wrote.
- **A run that does not finish is not compared.** A JIT stops only
  between blocks, so at the budget the two stop at different
  instructions. If one side finished and the other ran *past* that
  point, that is reported; if it stopped short, it is not.

**The configuration word** chooses state no program in this machine can
change, because a block built under one value of it is exactly what to
check: ARMv7-M's privilege, stack, CCR traps, FPU access and modes,
Handler mode, masks and fault enables; G4MH's user mode, FPU and FPSR;
PowerPC's encoding (VLE or Book E), user state, SPE and SPEFSCR. Each
target's source lists the bits.

## What each compares, and may differ in

Every register, the state, the status and system registers each core
keeps, and all of data RAM. Then, per target:

- **RV32.** mstatus is compared whole, FS included. FS was allowed to
  be dirtier under the JIT -- the spec permits tracking it imprecisely,
  and the JIT marked every block holding an FP operation Dirty on every
  exit -- until a program cleared one FS bit and saw Clean against Off,
  with FP traps differing after. Both backends now mark it where the
  state changes, so it is exact (below). Not the run's own instruction
  count, which is budget bookkeeping; minstret is compared.
- **ARMv7-M.** The flags translated code keeps in four words are packed
  into xPSR before comparing. The core's own retired count is compared,
  because SysTick counts it -- though SysTick itself cannot be enabled
  (below).
- **G4MH.** The reservation tracker is the frontend's, by PE id, so the
  side about to run is registered as PE0 -- a core's own store must
  break its own reservation.
- **PowerPC.** A run that ends in the decrementer's, the fixed-interval
  timer's or the watchdog's slot is not compared. A guest's own start-up
  arms the decrementer, and the interpreter takes it on the instruction
  it expires on while the JIT takes it at a block boundary -- permitted
  for an asynchronous interrupt, and everything after differs by that
  much. Every handler stops the core one instruction in, so where it
  stopped says which interrupt it was (`fz_asynchronous`; the stats
  line counts them).

The smoke tests (`fuzz-<isa>-jit-smoke`) require the stats line to show
runs compared *and* blocks translated: a harness whose runs never
finish, or whose JIT declines everything, passes every input and tests
nothing.

## Devices and interrupts

Each target maps its frontend's own interrupt controller, a separate
instance per side, through `fz_dev_t` in the harness, which sits between
the bus and the device and only watches:

| target | mapped | how a program raises an interrupt |
|---|---|---|
| RV32 | the CLINT at `0x0200_0000`; `mtime` does not advance | `msip`, or `mtimecmp` at or below `mtime`; `mie` starts with both enabled |
| ARMv7-M | the System Control Space: NVIC, SCB, SysTick, MPU | NVIC_ISPR, STIR, ICSR's PendSV and NMI bits |
| G4MH | INTC1 (SELF and PE0's window), INTC2, the IPIR | a channel's EIRF, or the core interrupting itself through the IPIR |
| PowerPC | the core's INTC, both vector modes | the eight software-settable sources (SSCIR) |

Two things a device write can do are allowed to come out differently on
the two backends, and the wrapper notices both as they happen. Either
sets the run aside, finished or not, before anything is compared --
the stats line counts each:

- **make an interrupt deliverable at once.** The interpreter takes it
  before the next instruction; a JIT when the block returns to its
  dispatcher, or later across a chain. The architecture allows either
  for an interrupt raised by a store. ARMv7-M's SYSRESETREQ is the same
  case.
- **change what a translated block assumed, where the architecture
  defers the change to a synchronising instruction**: ARMv7-M's CCR trap
  bits and MPU, which move the JIT's generation and are in force from
  the next ISB.

What stays comparable is an interrupt made deliverable by an
*instruction* -- an enable, a lowered mask, an exception return --
because every translator declines all of those (RV32's CSR writes and
MRET, ARMv7-M's CPS and MSR, G4MH's EI, LDSR and EIRET, PowerPC's
wrteei, mtmsr and the rfis). The interpreter runs them on both sides and
takes the interrupt at the same boundary, through the interpreter's
check or, after a fallback that ends there, the JIT's own -- which for
G4MH is a separate copy of the code (`g4mh_jit_take_irq`) that has
drifted from the interpreter's three times.

**SysTick cannot be enabled on ARMv7-M**, and no timer is mapped on G4MH:
the JIT counts time by the block, by design, and the G4MH timers count
time, which does not pass here. PowerPC's own core timers stay as they
were: a run that stops in their slots is set aside after the fact.

### Seeds

A compiled guest addresses a CLINT or an NVIC through constants the
fuzzer would take a long time to happen on, so each target starts from
hand-written programs that reach its devices:
`tests/fuzz/seeds/<isa>-jit/`. `scripts/fuzz.sh` assembles RV32's,
ARMv7-M's and PowerPC's (Book E, behind configuration word 1) with the
same cross toolchains the guests use; G4MH's are CC-RH assembly with
their binaries committed beside them, as the G4MH guests are, built by
`scripts/g4mh-build-seeds.sh` (`--check` compares).

**Run each seed alone before trusting it.** `FUZZ_SHOW=1` prints every
register the comparison read and how each side ended. Of the first 24,
one computed an address wrong and never reached its device; the first
check of ARMv7-M's seven read their code as the configuration word,
because `objcopy` writing to `/dev/stdout` truncated the four bytes
already there; and one did exactly what it was written for and found a
defect both backends shared (next section). And `fuzz.sh` first linked
them through the gcc drivers, whose default scripts have a `flash`
region that `0x2000_0000` is not in, so the first five-minute run was
seeded with nothing and its stats line said 0 of 37,617 compared runs
had touched a device. That count is in the line for exactly this.

## More than one core

G4MH and PowerPC can give each side a second core, chosen per input by
bit 11 of the configuration word: both cores run the same program, each
on its own bus over shared memory, and a core gives way only where it
parks -- WFI or halt -- never at a budget or a block boundary, so both
backends interleave the cores identically. A parked core runs again
when something wakes it; the run ends when a whole round retires
nothing (`fz_run` in the harness).

What it is for is the JIT's one table of blocks, which every core on a
thread shares -- as `--serial` and every board with more than one core
share it. A block one core built is found and run by the next, under
whatever that block took from the core that built it.

| | the second core | reached by |
|---|---|---|
| G4MH | PE1, with its own MPU, INTC1 and local RAM, and its stack in that local RAM through the SELF window -- one address, a different memory on each PE | `scripts/fuzz.sh SECS g4mh-smp`: `fuzz-g4mh-jit` built with three PEs, in `build/fuzz-smp`, because G4MH's translated stores are built differently for one PE and for several |
| PowerPC | core 1, with its own INTC; both marked as running together, so stores take the path that breaks the other's reservation | the default build, which has two cores |

PowerPC's cores cannot reach each other's INTC, so a parked core stays
parked; what its second core exercises is the blocks, the reservation
path and the shared memory. A G4MH PE can wake the other through the
IPIR (`pe1-wakes-pe0`).

## What it found

Each fix was confirmed by reverting it with its regression in place.

| defect | where | held by |
|---|---|---|
| a trapping instruction -- ECALL and EBREAK too -- counted in minstret and mcycle, and so did interrupt entry | the RV32 **interpreter**, `rv_interp.c` | `probes/instret.S` (Sail and Spike: it does not retire), `rv32-jit/trapped-load-minstret` |
| LOAD funct3 3, 6 and 7, STORE funct3 3-7 and JALR funct3 != 0 translated as real instructions | `rv_ir.c`, both hosts | `isatest` `rv32-ld` ... `rv32-jalr-f3-7`, `rv32-jit/lwu-translated` |
| a misaligned RAM or ROM access a cast to `uint16_t *` at an odd address, under a comment saying callers guaranteed alignment | `emu_bus.c`, `emu_bus.h` | `rv32-jit/flh-misaligned-host-load` (UBSan) |
| a block that only moved a float and left by a chained exit skipped the call that marks FS dirty | both JIT backends' `ir_lower.c` | `isatest` `fs-dirty-flw-chained`, `rv32-jit/flw-chained-fs-clean` |
| SMMLA and SMMLS summed `(Ra << 32) +/- Rn * Rm` in `int64_t`, which overflows | the ARMv7-M **interpreter** | `armv7m-jit/smmls-signed-overflow` (UBSan) |
| an instruction the translator steps through the interpreter, and which writes back to the pc, was followed by the rest of its block | `armv7m_ir.c`, `helper_falls_through` | `armv7m-jit/ldrd-pc-writeback-stepped` |
| **sp or pc where the manual says UNPREDICTABLE executed**: `bfi pc, lr, #0, #13` wrote an odd pc, and the next fetch read the host buffer at an odd address. A Cortex-M7 raises UNDEFINSTR for all of them -- see below | the ARMv7-M **interpreter** and decoder | `test_sp_pc_board_answers`, `armv7m-jit/bfi-pc-odd-fetch` |
| MAC summed a 64-bit product and accumulator in `int64_t` | the G4MH **interpreter** | `g4mh-jit/mac-signed-overflow` (UBSan) |
| an interrupt a block raised or took cost the interpreter a cycle and the JIT none, so the time base -- which counts cycles once HID0[TBEN] is set -- drifted between them | `ppc_ir.c`, every raise path | `ppc-jit/interrupt-cycle-uncharged` |
| a vector holding a value shaped like EXC_RETURN was read as an exception return; the return failed, the UsageFault went back into the same vector, nothing retired, and the slice **never ended** -- a hang, under both backends | the ARMv7-M **interpreter**, `armv7m_exc_return` | `test_vector_shaped_like_exc_return`, `armv7m-jit/vector-exc-return-shape` |
| **a block one PE built ran on another and read the first PE's memory**: the translator inlines accesses to the region the stack is in by host address, the stack was in local RAM through the SELF window, and the block's context was the user-mode bit alone, so PE1 found PE0's loop and read PE0's stack. Wherever PEs share a table -- `--serial`, a board with more than one -- as soon as a guest keeps its stack where a U2B program does. Found on the first run of the two-PE target, by the seed written for it | `g4mh_jit_ctx_refresh`: the PE is in the context now | `test_mc_self_window_blocks`, `g4mh-jit/stack-through-self` |
| a block holding an FP operation made FS Dirty on every exit, even a side exit taken before it -- permitted by the spec, until the program cleared one FS bit (`csrrc mstatus`) and the JIT showed Clean where the interpreter showed Off. And the interpreter left FS Clean when a compare or a conversion to integer raised a flag, where Sail and Spike both go Dirty (`probes/fsdirty.S`) | `rv_ir.c` (`rv_ir_fp_flags`, FS marked at a block's first f write, `fput`) and the RV32 **interpreter** (`f_accrue`) | `probes/fsdirty.S`, `rv32-jit/side-exit-fs-dirty`; the target now compares mstatus exactly |
| an LDSR lowering the interrupt ceiling (PLMR, ISPR, INTCFG) never made the core look again, so the channel it admitted stayed pending; the manual says the next instruction sees it. **Found by writing a seed, not by a divergence**: both backends run LDSR in the interpreter | the G4MH core, `g4mh_sr_write` | `test_ldsr_lowers_ceiling` |

And inputs held for the permitted differences above:
`rv32-jit/flw-fault-fs-dirtier` (no longer a difference: FS is exact), `rv32-jit/trap-first-in-block-run-count`
and `ppc-jit/decrementer-taken-a-block-late`.

Each run below is after its target's fixes, and found nothing more:

| target | time | inputs | compared | translations | block entries |
|---|---|---|---|---|---|
| RV32 | 10 min | 157,768 | 150,680 | 7.8M | 9.8M |
| ARMv7-M | 20 min | 666,479 | 622,915 | 827,853 | 12.1M |
| G4MH | 20 min | 134,211 | 119,843 | 12.5M | 32.8M |
| PowerPC | 20 min | 1,845,065 | 1,771,551 (113 asynchronous) | 2.1M | 112M |

## Things worth keeping from finding them

- **The interpreter is the reference, and was the one that was wrong
  -- four times out of nine.** The RV32 probe asked Sail and
  Spike rather than reasoning about which backend to fix. On mcycle they
  disagree with each other -- Sail charges a trap a cycle, Spike does
  not -- and cycles are the implementation's to define, so mcycle
  follows minstret on both backends.
- **A UBSan report in the interpreter is a finding whatever the JIT
  says.** SMMLS and MAC agreed between backends -- ARMv7-M's JIT steps
  SMMLS through the interpreter, so both sides computed the same
  wrapped value -- and only the sanitizer could see that the value came
  from undefined behaviour.
- **What the fuzzer found on ARMv7-M was a class, and the board answered
  it.** `bfi pc` was the first of 557: rawgen's filter keeps every
  operand in r0-r12, because an arbitrary write to sp or pc wanders off,
  so no encoding naming either had ever been asked. Asked
  (`rawgen.py --regs`, then by name), the M7 raised UNDEFINSTR for
  every UNPREDICTABLE register choice but three, which it executed: an
  LDM of one register, an LDM writing back over a base in its list, and
  REV or CLZ with their two Rm copies differing. **A rule read from the
  manual would have been wrong in both directions**; the frontend
  follows the board's table. The cost, and what did not make it cheaper, is in
  [frontend/armv7m.md](frontend/armv7m.md#what-the-board-corrected).
- **A permitted difference has to be recognisable from the end state**,
  or it cannot be excluded without excluding everything near it. Every
  PowerPC vector has its own slot and stops there, so a timer interrupt
  says it was one.
- **A counter nothing reads is not a comparison -- and is a cost.**
  G4MH's `cycles` differed between backends and could not matter;
  PowerPC's differed in the same way and could, because the time base
  counts them. The question is the readers, which a grep answers. G4MH's
  had none, and its interpreter incremented it on every instruction:
  removed, CoreMark runs 4.5% faster interpreted on the F446 and 3.6% on
  the F746.
- **Fixing the RV32 interpreter's count without moving its loop took
  five tries**; each read 0.5-1.1% more host instructions on CoreMark,
  for paths no benchmark takes, until only the function the trap path
  calls changed (`rv_hart_trap_unretire`), with `noipa`.
- **LTO partitioning can move a row by 1% on its own.** With the RV32
  change at 0.00% in a one-partition build (`-flto-partition=one`), the
  default build read ARMv7-M's interpreter 1.07% slower: a change in
  RV32 files moved where the partitions fell. The ARMv7-M cost above is
  quoted both ways for that reason. See [TODO.md](TODO.md).
- **The bus fix keeps the type.** `memcpy` makes an access a byte
  access, which may alias anything; the alignment-1 typedef keeps
  `uint16_t` and `uint32_t`, and the F746 firmware differs only in four
  compare-and-branches that became `cbnz`.
- **The FS defect was a decision taken before the information it
  needed.** Chainability is fixed in the prologue; "this block writes an
  f register" was learnt while lowering its body. The fix costs
  Whetstone under the x86-64 JIT 2.09% of host instructions, because
  those blocks no longer chain -- see [TODO.md](TODO.md).

## Adding a finding

Fix it, confirm the fix by reverting it, and copy the input to
`tests/fuzz/regressions/<isa>-jit/` under a name that says what it found;
`fuzz-<isa>-jit-regressions` replays the directory. Where the defect can
reach a board -- a translator or a backend -- add a guest check as well:
the fuzzers run only on x86-64. Where the two backends agree and the
reference might be wrong, ask whatever can answer: Sail and Spike for
RV32, the F746 for ARMv7-M, and for G4MH and PowerPC say in the fix that
the manual is the only authority it had.

What none of them reaches yet: timers (time does not pass, SysTick
cannot be enabled, and PowerPC's core timers are set aside); whether a
JIT takes an interrupt by the point the architecture requires -- an
interrupt made deliverable by a store, and ARMv7-M's CCR and MPU before
the ISB, are set aside rather than compared, so ISB's synchronisation
is untested here; a third core, and PowerPC's cores waking each
other, which its INTCs cannot do; runs longer than 20,000
instructions; and self-modifying code. RV32 does not yet have Spike as a
third side. The other targets are in [TODO.md](TODO.md).
