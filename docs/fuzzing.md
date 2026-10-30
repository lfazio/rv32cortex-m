# Fuzzing

Ten targets: one per frontend, each the interpreter against the
x86-64 JIT on whatever bytes libFuzzer produces; one for the IR itself,
with no frontend ([below](#the-ir-with-no-frontend)); RV32's
translation and protection against a walk written from the spec, from
M-mode ([below](#translation-and-protection-rv32)) and in running code
([below](#fetch-and-the-jits-data-tlb-rv32)); and one each for G4MH's
and ARMv7-M's MPUs and PowerPC's TLB, against references written from
their manuals ([G4MH](#protection-g4mh),
[ARMv7-M](#protection-armv7-m), [PowerPC](#translation-powerpc)). Between them they found
eighteen defects in code that every suite passed --
eight of them in an interpreter, which is the reference -- and one of
those turned out to be a whole class, which a board then answered
encoding by encoding. Writing fuzz-g4mh-mpu's reference found seven
more before it ran ([below](#protection-g4mh)). One more, in G4MH's interrupt ceiling, was found
by writing a seed for the interrupt controllers each target now maps:
both backends shared it.

| target | the reference, when the two disagree |
|---|---|
| `fuzz-rv32-jit` | the interpreter; Sail and Spike for what it might have wrong (`scripts/run-probe.py`) |
| `fuzz-armv7m-jit` | the interpreter; the Nucleo-F746ZG for what it might have wrong (`tests/armv7m-diff/`) |
| `fuzz-g4mh-jit` | the interpreter and the manual, and nothing else: no RH850 is attached |
| `fuzz-ppc-jit` | the interpreter and the manual, and nothing else: no e200 is attached |
| `fuzz-ir` | the IR interpreter on the block as built, before any pass; the rules both lowerings state in `emu_ir.h` for what it might have wrong |
| `fuzz-rv32-mmu` | a Sv32 walk and PMP written from the privileged spec, sharing nothing with `rv_mmu.c`; Sail and Spike, through a probe, for what it might have wrong |
| `fuzz-g4mh-mpu` | the MPU's check, the cause codes and the instructions' pages, written from the U2B and G4MH manuals; the manual and nothing else |
| `fuzz-armv7m-mpu` | the architecture's MPU pseudocode, with the F746's answers where it leaves room (`tests/armv7m-diff/sys.c`) |
| `fuzz-ppc-tlb` | chapter 10 and the interrupt tables of the e200z7 manual; the manual and nothing else |
| `fuzz-rv32-vm` | `rv32_ref.h`, the same walk as `fuzz-rv32-mmu`'s, with fetch; Sail and Spike, through a probe, for what it might have wrong |

No GPL emulator is consulted, by any of them.

```sh
scripts/fuzz.sh 600 armv7m   # build build/fuzz, seed it, fuzz ten minutes
scripts/fuzz.sh 600 all      # the ten in turn, and G4MH on three PEs
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

### Dictionaries

The seeds show the fuzzer whole programs; a dictionary gives it the
words. `tests/fuzz/dict/<isa>-jit.dict` is a libFuzzer dictionary of
the instructions that reach a target's devices -- an address built in
a register, a store through it, a value, the enable the translator
declines -- and `scripts/fuzz.sh` passes it to every run
(`FUZZ_NO_DICT=1` leaves it out). The input is the program, so an
address is no use as bytes: it has to arrive as the instructions that
build it.

Every token is written as assembly on the `#:` line above it, and
`scripts/fuzz-dict.py` assembles them with the seeds' toolchains --
CC-RH for G4MH -- and rewrites the bytes (`--check` compares, and
fails a token that would need the linker). The `.dict` files are
committed, as the G4MH seeds are. The tokens share the seeds'
registers, so that two dropped into one input compose, and each
dictionary's `#!` line names a sequence of them that takes an
interrupt: `fuzz-<isa>-jit-dict`, in the fuzz tree's ctest, composes it
(`fuzz-dict.py --compose`) and requires the run to be compared, to
touch a device and to take an interrupt. A harness change that moved an
initial register would leave every token valid and the dictionary
meaning nothing; that test is what notices. Dropping the enable from
RV32's sequence fails it. What they buy is measured under
[Mutants](#mutants): reach, not finds.

### A small code cache

At the host's own sizes a fuzz program never fills the code buffer, so
compaction and everything behind it -- eviction, chain unlinking, the
thrash fallback -- is never fuzzed. A fuzz tree configured with the
board's tables first reaches it, since `fuzz.sh` keeps a cache it finds:

```sh
CC=clang cmake -S . -B build/fuzz-small -DEMU_FUZZ=ON \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo "-DCMAKE_C_FLAGS=-DEMU_JIT_MAX_BLOCKS=256u \
  -DEMU_JIT_HASH_SIZE=256u -DEMU_JIT_BLOCK_RESERVE=512u -DEMU_HOST_JIT_CODE_BYTES=8192u"
FUZZ_BUILD=$PWD/build/fuzz-small scripts/fuzz.sh 300 rv32
```

The stats line counts compactions and pcs given up, which is what says
it did: five minutes of RV32 made 188,272 compactions and gave up on
799 pcs, and found nothing.

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

## The IR, with no frontend

`fuzz-ir` (`tests/fuzz/fuzz_ir.c`) builds IR blocks itself, from the
input, rather than through a translator, and runs each three ways from
one initial state:

| run | what | a difference from `want` means |
|---|---|---|
| `want` | `emu_ir_interp` on the block as built, before any pass | -- |
| `opt` | `emu_ir_interp` on the block after `emu_ir_optimise` | a pass is wrong |
| `got` | the optimised block lowered to x86-64 and called | the lowering is wrong, if `opt` agrees |

`EMU_JIT_DIFF` runs the reference on the *optimised* IR, which cannot
see a defect in a pass -- the zero-register forwarding passed it
([lessons.md](lessons.md)). Here the reference runs the block before any
pass touched it.

The machine is made up, and small on purpose: sixteen registers, a flag
word in one of three layouts (low bits, ARM's N Z C V at the top, none),
eight FP registers held 64 bits wide so the NaN box means something, a
page of RAM, a page of read-only image, a four-byte device whose reads
count and whose writes are logged -- so an access deleted or repeated
shows -- and four helpers which between them read the registers, the pc
and the flags and write a register and the flags. The input chooses
whether r0 reads as zero and whether r15 *is* the pc, as on ARMv7-M, and
how memory is reached: through the target's load and store, the RAM
window whole or in part, the image window, or a four-entry data TLB
with RAM writable or not, and the store guard up or down. Compared:
every register, the flags, the pc, the FP registers, all of RAM, the
device's count and log, every helper call in order, the trap and the
pc it was taken at, and the retired count.

It builds only what the contract in [emu_ir.h](../include/emu/emu_ir.h)
allows, in the orders the frontends use, since anything else would
report a difference no translator can cause. Each instruction writes the
pc first and retires before any exit (a back edge that retires nothing
loops for ever, in compiled code too); a divisor is never zero or -1; a
predicated load's result is selected away with a mask, as ARMv7-M's IT
blocks do. It asks `emu_ir_can_lower` exactly where a frontend would,
and not otherwise -- its default is false, so asked about ADD it says
no. SELECT, POPCNT and ROTL with a register amount are left out: no
backend lowers all of them and no frontend emits them.

```sh
scripts/fuzz.sh 600 ir     # its regressions are tests/fuzz/regressions/ir/
FUZZ_SHOW=1 build/fuzz/tests/fuzz/fuzz-ir crash-...   # print the block, both ways
```

What it found, in its first hour -- all three by fuzzing from nothing:

| defect | where | held by |
|---|---|---|
| a NaN result was the host's -- 0/0 is x86's 0xFFC00000, and a NaN operand's payload came through -- where both lowerings produce the canonical 0x7FC00000 | the IR **interpreter**, `interp.c`; the rule is in `emu_ir.h` now | `ir/fdiv-nan-not-canonical-in-reference` |
| NEG computed `-(int32_t)a`, undefined for INT_MIN; the Thumb-2 lowering's ADDI had the same expression (by reading, not run) | the IR **interpreter**; `thumb2/ir_lower.c` | `ir/neg-int-min-undefined-in-reference` (UBSan) |
| **a read of the register that is the pc was forwarded across a SETPC**, giving the old pc; and `pass_dead_puts`, seeing no read of it left, deleted the SETPC as overwritten. `emu_ir.h` says a guest register may be the pc; no frontend reads it twice across a SETPC today -- ARMv7-M reads r15 only straight after a helper -- so it waited for the next one that did | `pass_reg_traffic` | `test_pc_register_not_forwarded_across_setpc`, `ir/pc-register-forwarded-across-setpc` |

After the fixes, ten minutes on four workers (`-jobs=4`), from the
regressions: 5,658,718 blocks built, 5,595,340 compared, 63,378 not
lowered; 1,279,298 ran their body more than once and 1,523,363
trapped; 10.1M helper calls and 13.2M accesses through the
target's functions, 907,097 of them to the device. The passes elided
10.6M reloads and 10.5M stores, folded 3.0M constants and 1.9M
displacements and merged 28.1M retirements -- and formed no
multiply-accumulate, below. Nothing found.

Mutants (`scripts/fuzz-mutant.sh 120 ir ...`), each a defect this tree
has had or a guard a lowering keeps, all found:

| mutant | where | found |
|---|---|---|
| the zero register forwarded, as before 2b35810 (`reg_is_zero` answering no) | `pass_reg_traffic` | by fuzzing, 3 s |
| a load not an observer, so a PUT before a faulting load is dropped (riscv-tests' `ma_addr`) | `op_observes_state` | by fuzzing, 4 s |
| a load counted pure, so an unread one is deleted (the device read `pmpsm_cfg_A_tor_zero` caught) | `op_is_pure` | by fuzzing, 3 s |
| RETIREs merged across an observer, so an early exit is charged for what it skipped | `pass_merge_retire` | by fuzzing, 4 s |
| a helper not an observer of the pc, so the SETPC before it is deleted | `pass_dead_puts` | by fuzzing, 3 s |
| an address fold that replaces the displacement instead of adding to it | `pass_fuse` | by fuzzing, 4 s |
| a store probing the data TLB with the load tag, so a read-only page takes it | x86-64 `emit_inline_guard` | by fuzzing, 6 s |
| the window guard bounding the last aligned slot instead of the last byte | x86-64 `emit_window_guard` | by `ram-window-last-byte-held`, 3 s; not by 120 s of fuzzing without it |

The last is the same test as the right one whenever a window's size is
a multiple of four, and the bytes past a window are the same either way
when it is part of a larger buffer -- so the machine has a mode with
4,093 bytes of RAM ending where its buffer does, and the input that
loads across that end is kept, since fuzzing did not build it in two
minutes.

What it cannot see: the FP exception flags, which the reference does not
compute; FSQRT and FMA, which it declines; the Thumb-2 lowering, which
needs a board; and `pass_mac`, which forms a multiply-accumulate only
where the backend lowers one, and x86-64 does not.

## Translation and protection: RV32

`fuzz-rv32-mmu` (`tests/fuzz/fuzz_rv32_mmu.c`) asks a question no other
target can: both of RV32's backends walk page tables and check PMP with
the same `rv_mmu.c` and `rv_pmp.c`, so a misreading there is agreed
with. Its reference is a Sv32 walk and a PMP check written from the
privileged spec, in 34-bit physical addresses, sharing nothing with them.

The input builds page tables in a page of RAM -- two roots, two
second-level tables, megapages and pointers, leaves naming RAM, a
read-only image, a device, nothing, or a page past 4 GiB -- and then runs
a sequence of loads and stores through `rv_hart_load` and
`rv_hart_store`, which is the whole chain: the data TLB probe, the TLB,
the walk, PMP, the bus, the data TLB fill. Between them it writes satp,
SUM, MXR, MPRV and MPP, PMP entries and PTEs, and fences. Each access is
compared on its cause, or on the value at the physical address the
reference computed, and after a store on all of RAM and the device's log.
And after every step **every data TLB entry is checked against what it
claims**: that each aligned access to its page at its table's privilege
is a plain access at the host address it holds -- a load, and a store if
it says so. The data privilege comes from MPRV, as a kernel's copy
routines get it, so the hart stays in M.

Where the spec lets an implementation be stale, the harness does what
software must: a global SFENCE.VMA after a PTE changes -- a store to a
page a walk has read is one -- and after a PMP write. satp, SUM, MXR and
MPRV take effect at once. The reference reads the CSRs back after each
write, so their WARL rules are the CSR file's business, not this one's.
Three choices the spec leaves to the implementation are modelled as this
core makes them, each found by a disagreement first:

- **Svade**: A and D are checked, not written.
- **A table is read wherever a load would read**, a device included:
  which regions may hold page tables is a PMA. Spike reads a table in
  the CLINT as this core does; Sail refuses it (`sv32ppn.S`, case 7).
- **A misaligned access splits at a page only when it is translated**,
  and then each part is its own physical access to PMP; untranslated it
  is one access, held to one entry. And a page past the bus is refused
  by the walk, before a split access's second page is translated.

```sh
scripts/fuzz.sh 600 rv32-mmu   # its regressions are tests/fuzz/regressions/rv32-mmu/
```

What it found:

| defect | where | held by |
|---|---|---|
| **a PTE's PPN reaching past 4 GiB was masked to the bus**, so a leaf naming 0x2_8000_1000 read 0x8000_1000 -- another page, by an alias -- and a pointer past the bus read a table below it. Sv32 addresses 34 bits; nothing is there: an access fault. Found in its first minute; Sail and Spike agree | `rv_mmu.c` | `probes/sv32ppn.S`, `isatest` `sv32-past-bus-*` (both backends and the F746), `rv32-mmu/leaf-past-the-bus-aliased` |
| **an M-mode access half inside an unlocked PMP entry succeeded.** The entry that matches must hold every byte "irrespective of the L, R, W, and X bits", and only then does an unlocked one exempt M; the hart skipped PMP in M whenever nothing was locked. Only an access crossing a four-byte granule can be half in an entry -- misaligned, a doubleword, a cache block -- so those ask now (`rv_pmp_needed`, `pmp_any`) and aligned words still do not; the data path tests one flag where it tested two. Found once the generator reached RAM untranslated; Sail and Spike agree | `rv_pmp.c`, `rv_hart.c` | `probes/pmpstraddle.S`, `isatest` `pmp-straddle-unlocked-*` (both backends and the F746), `rv32-mmu/m-mode-straddle-into-unlocked-entry` |

After both fixes, ten minutes on four workers, from the regressions:
6,196,844 inputs, 65.9M accesses -- 11.1M loads and 6.4M stores done,
48.4M refused -- and 17.6M data TLB entries checked against their claims,
27.4M fences; 584,528 device accesses compared by cause only. Nothing.

Mutants (`scripts/fuzz-mutant.sh 120 rv32-mmu ...`), each a rule these
files keep, all found by fuzzing from an empty corpus:

| mutant | where | found |
|---|---|---|
| SUM ignored: S reads U pages | `leaf_permits` | 5 s |
| MXR ignored | `leaf_permits` | 6 s |
| D not checked for a store (Svade) | `leaf_permits` | 7 s |
| a megapage's misalignment not checked | the walk | 3 s |
| a pointer's reserved D, A, U not checked | the walk | 5 s |
| PMP's straddle rule dropped | `rv_pmp_check` | 35 s |
| unlocked entries binding M | `rv_pmp_check` | 7 s |
| the walk's own reads not PMP-checked | `read_pte` | 4 s |
| a U page filled into S's data TLB | `rv_mmu_dtlb_rights` | 9 s |
| PMP's write permission ignored by a fill | `rv_dtlb_fill` | 10 s |
| the TLB's tag not compared | `rv_mmu_translate` | 6 s |
| the PPN past the bus masked again | the walk | 2 s |
| the M-mode straddle fix taken out | `rv_pmp_refresh` | 125 s, in a 300 s run; not in 120 |

Four of them first went unfound, and each time it was the generator: a
straddle needs an access across a PMP edge, which only half-page
regions and offsets near them made likely; MXR needs execute-only
leaves, which the bias towards readable ones had made rare; and the M
lock rule and the straddle in M need an M-mode access to reach RAM,
which the addresses the tables were built for never were. The last
of these was the defect, found the same way once the generator could
reach it.

What it does not reach: fetch -- the hart never leaves M, so the fetch
table is never filled; the JIT's inlined probe, which reads the same
data TLB (its entries are checked, the probe is not). fuzz-rv32-vm
reaches both ([below](#fetch-and-the-jits-data-tlb-rv32)).

## Protection: G4MH

`fuzz-g4mh-mpu` (`tests/fuzz/fuzz_g4mh_mpu.c`) holds G4MH's MPU to a
reference written from R01UH0923EJ0130 section 3.2.5.1 -- the check,
tables 3.111 to 3.116 -- table 3.103's cause codes, and the instruction
pages of the G4MH software manual, sharing nothing with `g4mh_mpu.c`.
For this frontend the manual is the only authority: no RH850 is
attached, and a reading this file and the core share is invisible here.

Unlike fuzz-rv32-mmu it runs instructions, because what it is after is
in them: one a step, placed in RAM at a pc chosen near an area's end and
followed by HALT, on registers pointing near an area's ends -- loads and
stores of every width, LD.DW and ST.DW, the bit operations, CAXI,
PUSHSP, POPSP, PREPARE, DISPOSE, SLD/SST, a 48-bit MOV, NOP, and the
setting check (`LDSR` to `MCC`, `STSR` from `MCR`). Between them the
input writes entries through the `MPIDX` window, `MPM`, `MPIDn`, the
core's SPID and mode, or builds the areas a program would have:
executable code, readable-writable data, two areas end to end, one over
all of memory. Each step runs on two cores, one per backend, with the
JIT's blocks discarded first, and is compared with the reference on
where the core stopped, PSW, FEPC, FEPSW, FEIC with its detail half,
MEA, the registers, MCR and all of memory. Entry 31 is the harness's,
supervisor-executable over the vector table, so every exception reaches
its handler's HALT.

Readings the reference makes where the manual leaves room, each written
in it: an instruction that both reads and writes, refused, names each
permission it was refused (table 3.115's notes speak of a read violation
and a write violation, not of both); `MCR.OV` is a span passing
`FFFF FFFFH` to 0 or `7FFF FFFFH` to `8000 0000H`, not one that merely
includes either; and DISPOSE restores r31 first, from the lowest
address, PREPARE's inverse, which its page does not spell out.

```sh
scripts/fuzz.sh 600 g4mh-mpu   # its regressions are tests/fuzz/regressions/g4mh-mpu/
```

What it found -- and a note on *how*: every one of these was found
**while writing the reference**, by reading the manual's rule next to
the code that should keep it. The running fuzzer then found none of the
core's -- it found its own harness starting both cores from the
reference's finished registers, and the disassembler printing POPSP as
`di` -- and each fix reverted was found by it in seconds to a minute,
which is what the mutants below are.

| defect | where | held by |
|---|---|---|
| **the cause codes were not table 3.103's**: MIP `30H` for `90H`, MDP `31H` for `91H`, MAE `62H` for `C0H`, FETRAP `90H`+n for `31H`+n -- the last chosen to avoid MDP, a collision of this file's making. A handler reading FEIC was told the wrong exception; the vectors were right | `g4mh_types.h` | `g4mh-mpu/mip-cause-without-detail` |
| **MIP and MDP carried no detail**: table 3.115's `UR`/`UW`/`SR`/`SW`, `UX`/`SX`, `RMW` and `BL` in FEIC's top half | `g4mh_cpu.c`, `g4mh_interp.c` | `test_mpu_manual_rules`, `g4mh-mpu/prepare-mdp-without-bl`, `dispose-mdp-without-bl` |
| **an instruction ran with its second halfword in a refused area**: the check was on the first halfword, argued safe in a comment because bounds are 4-byte aligned -- and a 32-bit instruction at `...E` crosses one anyway; both backends | `g4mh_interp.c`, `g4mh_ir.c` | `test_mpu_manual_rules`, `g4mh-mpu/fetch-32-bit-insn-half-past-its-area`, `jit-fetch-...` |
| **LD.DW and ST.DW were two word accesses to the MPU**, so one across two permitted areas went through, one wrapping past `FFFF FFFFH` under an area over everything went through, and ST.DW refused on its second word had written its first (tables 3.112, 3.114); both backends | `g4mh_cpu.c`, `g4mh_ir.c`, `g4mh_mpu.c` | `test_mpu_manual_rules`, `g4mh-mpu/ld-dw-across-two-areas`, `doubleword-wrapping-past-ffffffff`, `jit-doubleword-as-two-word-accesses` |
| **SET1, NOT1 and CLR1 moved Z before their store was refused**, so FEPSW held the instruction's Z for a resumable exception; and neither they nor CAXI were refused as a read-modify-write | `g4mh_cpu.c`, `g4mh_interp.c`, `g4mh_ir.c` | `test_mpu_manual_rules`, `g4mh-mpu/set1-z-moved-before-refused-store`, `jit-bit-op-inlined-under-mpu`, `caxi-refused-without-rmw` |
| **POPSP loaded sp** partway and put the final value over it, so an MDP between left the handler a loaded sp; the manual never loads it, which is what makes the instruction restartable | `g4mh_interp.c` | `test_mpu_manual_rules`, `g4mh-mpu/popsp-loads-sp-partway` |
| **CAXI set no flags**; the manual sets CY, OV, S and Z from reg2 - token. The translator declines CAXI, so both backends shared it | `g4mh_interp.c` | `test_mpu_manual_rules`, `g4mh-mpu/caxi-sets-no-flags` |

And one thing documented as missing, now there: the setting check
(`MCC` computes `MCR`), with its writes, and `MPIDX`'s, no longer
discarding every translation (`g4mh-mpu/setting-check-not-computed`,
`setting-check-ignores-mci`).

Mutants (`scripts/fuzz-mutant.sh 120 g4mh-mpu ...`), all found by
fuzzing from an empty corpus:

| mutant | where | found |
|---|---|---|
| the fetch asks the first halfword only | `g4mh_fetch_permitted` | 24 s |
| the translator's fetch, likewise | `g4mh_ir_translate` | 6 s |
| a doubleword checked a word at a time | `dw_refused` | 63 s |
| an access wrapping past `FFFF FFFFH` allowed | `g4mh_mpu_permits` | 47 s |
| the JIT's doubleword as two IR accesses under the MPU | `lower_one48` | not in 120 s; 13 s once the generator built adjacent areas |
| a bit operation's Z moved before its refusal | `g4mh_bitop` | 7 s |
| the JIT's EMU_IR_BITOP under the MPU | `lower_bitop` | 12 s |
| CAXI's flags | the interpreter | 7 s |
| CAXI refused a permission at a time | the interpreter | 22 s |
| POPSP loads sp | the interpreter | 24 s |
| PUSHSP's, PREPARE's, DISPOSE's BL dropped | the interpreter, `g4mh_prepare`, `g4mh_dispose` | 6 s, 3 s, 6 s |
| the setting check not computed | `g4mh_mpu_sr_write` | 2 s |
| the setting check ignores MCI's SPID gate | `check_span` | not in 120 s; 152 s once spans were put inside areas |
| the setting check asks only the span's start | `check_span` | 37 s |
| MDP's mode bits swapped, MIP's detail dropped | `g4mh_mdp`, the interpreter | 4 s, 5 s |
| the two gates as alternatives | `g4mh_mpu_permits` | 5 s |
| MPUA exclusive | `g4mh_mpu_permits` | 5 s |
| SVP opening user mode too | `g4mh_mpu_permits` | 3 s |
| a fetch asking the write group | `g4mh_mpu_permits` | 3 s |
| the first matching area decides | `g4mh_mpu_permits` | 7 s |
| a store's check dropped | `g4mh_store` | 7 s |


## Protection: ARMv7-M

`fuzz-armv7m-mpu` (`tests/fuzz/fuzz_armv7m_mpu.c`) holds ARMv7-M's MPU
to `ValidateAddress()`, `CheckPermission()`, `DefaultPermissions()` and
`MemU_with_priv[]` from the architecture manual (DDI 0403E B3.5.8,
B2.3), sharing nothing with `mpu_permits`, and to what the F746 answered
where those leave the implementation room. The shape is fuzz-g4mh-mpu's:
one instruction a step -- every load and store width, LDRSH, the
immediate-offset forms, LDRD, STRD, LDM, STM, LDRT, STRT, LDREX, LDR
from a literal, two NOPs -- placed in ROM at a pc chosen near a region's
end and followed by BKPT, with r0 near a region's end; regions and
MPU_CTRL written through the System Control Space; Thread mode's
privilege and which faults are enabled from the input. Each step runs
on both backends and is compared with the reference on the exception
taken, CFSR, HFSR, MMFAR and BFAR where valid, the stacked return
address, the registers and the RAM. Regions 6 and 7 are the harness's
-- the handlers' BKPT and the stack -- and, being the highest numbered,
nothing the input writes overrides them.

The board was asked first (`tests/armv7m-diff/sys.c`, `mpu-ua-*`): an
unaligned word or halfword across the end of a region into one that
refuses it is a MemManage with MMFAR at the first refused byte, and a
store's bytes before it are written -- MemU's byte-at-a-time sequence,
which is also what the interpreter does. The emulator agreed on every
case, interpreted and under the JIT. The pseudocode's UNPREDICTABLE
cases are not asked: AP `100`, regions under 32 bytes, subregions below
256, HFNMIENA without ENABLE, an unaligned access to anything but
Normal memory. Two things are left unasserted until the board says:
BFAR for an unaligned access that faults partway into unmapped memory
(the manual says "the address requested by the instruction"; this
emulator reports the byte), and the registers of an LDM or LDRD
abandoned partway, which are UNKNOWN.

```sh
scripts/fuzz.sh 600 armv7m-mpu
```

It found nothing: the MPU agrees with the architecture. That is a
result only because every rule below, broken on purpose, was found by
fuzzing from an empty corpus:

| mutant | where | found |
|---|---|---|
| the lowest-numbered region wins | `mpu_permits` | 16 s |
| subregions from 512 bytes, not 256 | `mpu_permits` | 14 s |
| a quarter of a region per subregion bit | `mpu_permits` | 5 s |
| every region half its size | `mpu_permits` | 3 s |
| PRIVDEFENA's background for unprivileged too | `mpu_permits` | 4 s |
| AP `010` refusing unprivileged reads | `mpu_permits` | 20 s |
| AP `101` letting unprivileged code read | `mpu_permits` | 9 s |
| XN ignored | `mpu_permits` | 3 s |
| LDRT checked as privileged | `armv7m_mem_read_checked` | 4 s |
| MMFAR aligned down | `armv7m_mem_read_checked` | 5 s |
| a disabled fault never escalating | `armv7m_raise` | 3 s |
| an unaligned read checked at its start only | `rd_u` | 29 s |
| a 32-bit instruction's second halfword unchecked | the interpreter | 13 s |
| the translator's, likewise | `fetch` (`armv7m_ir.c`) | 11 s |
| a literal read from ROM folded to a constant under the MPU | `literal_const` | not in 120 s, until the generator had LDR literal; then 17 s |


## Translation: PowerPC

`fuzz-ppc-tlb` (`tests/fuzz/fuzz_ppc_tlb.c`) holds the e200z7's TLB to
chapter 10 of its manual -- the match of figure 10-1 and table 10-1,
the permissions of figure 10-3, the MAS updates of table 10-15 -- and to
the register settings of tables 7-13, 7-15, 7-23 and 7-24, sharing
nothing with `ppc_mmu.c`. As for G4MH, the manual is the only
authority; no e200 is attached.

It asks what fuzz-ppc-jit cannot: whether a translated block outlives
what it was built under. The input writes up to eight one-instruction
programs (loads and stores of each width, `lmw`, `stmw`) into ROM once;
each step then changes the TLB -- an entry through MAS0-3 and `tlbwe`,
`tlbivax`, MAS4's defaults, `tlbsx` -- or MSR's PR, IS and DS, or PID0,
or runs a slot with r3 near a page's end and r1, whose page a block
takes as its RAM window, chosen afresh. The JIT's blocks are kept across
steps, and its counters show they are found again. Entries 0, 30 and 31
are the harness's: the vectors, and the code in both address spaces,
below anything the input writes, which is made the identity over them.
Every system instruction is made as software makes it -- `ppc_spr_write`
and the TLB instructions' own functions, in supervisor mode, then the
context recomputed as the JIT does after any interpreted instruction.

It found nothing in the core: `ppc_mmu.c` agrees with the manual, and
the JIT's windows and retirement agree with it under every change the
input made. What it found first, five times, was its own harness (see
the lesson on harnesses in [lessons.md](lessons.md)); every rule below,
broken on purpose, it finds.

Mutants (`scripts/fuzz-mutant.sh 120 ppc-tlb ...`):

| mutant | where | found |
|---|---|---|
| TS ignored in the match | `lookup` | 16 s |
| TID 0 not shared | `lookup` | 3 s |
| the highest entry wins | `lookup` | 7 s |
| every page twice its size | `page_mask` | 3 s |
| user stores checked against SW | `ppc_mmu_xlate` | 73 s |
| a TLB error's MAS1 TID always 0 | `tlb_error_update` | 3 s |
| a TLB error's MAS6 without SAS | `tlb_error_update` | 4 s |
| a tlbsx miss's MAS1 without TS | `ppc_tlbsx` | 10 s |
| tlbivax ignoring IPROT | `ppc_tlbivax` | 5 s |
| DEAR written on an ITLB error | `ppc_mmu_xlate` | 3 s |
| tlbwe retiring the old span only | `ppc_tlbwe` | 20 s |
| tlbwe retiring the new span only | `ppc_tlbwe` | 13 s |
| a window over a page a lower entry overlaps | `ppc_mmu_data_page` | 36 s, once the generator put small pages inside others |
| a window over a read-only page | `ppc_mmu_data_page` | 30 s |
| a window not recorded, so a TLB change keeps its blocks | `ppc_fast_mem` | 31 s |
| the block key without PID | `ppc_cpu_ctx` | 93 s, once data pages could be private |
| the block key without DS | `ppc_cpu_ctx` | 21 s, once a step flipped DS alone |
| the block key without PR | `ppc_cpu_ctx` | 61 s |
| a misaligned load across pages of two byte orders | `load_xlate` | 26 s, once a step built such a pair and aimed at its boundary |

## Fetch, and the JIT's data TLB: RV32

`fuzz-rv32-vm` (`tests/fuzz/fuzz_rv32_vm.c`) reaches what fuzz-rv32-mmu
cannot: instruction fetch below M, and the JIT, whose translated loads
and stores probe the data TLB inline. Its reference is fuzz-rv32-mmu's,
moved into `tests/fuzz/rv32_ref.h` so the two cannot drift, with
execute added -- X, U against the mode, SUM never for a fetch -- and a
fetch by 16-bit parcel, each translated and PMP-checked, mtval the
parcel that faulted.

The input's page tables map virtual pages onto a two-page code ROM
whose slots each hold a load or store and an ECALL, one straddling the
ROM's page boundary. A run enters a slot in S or U by MRET from an
M-mode trampoline, and every trap, the slot's ECALL included, goes to
an M-mode ECALL that stops the hart; mcause, mepc, mtval, the registers
and RAM are compared. Between runs the steps change PTEs (then fence),
satp, SUM and MXR, and PMP, and the JIT's blocks are kept: a block, and
the data TLB its probes read, built under one is found again under the
next. The reference decodes what the fetch returned, so a page mapped
onto the wrong part of the ROM is still predicted; what it does not
model -- two instructions' halves a straddle can join, a compressed
parcel -- is compared between the backends only. After a store that
rewrote a live PTE, with no fence, the next fetch may use the old
translation or the new, and the interpreter (its fetch table) and the
JIT (a fresh walk) were seen to choose differently: the store's effect
is still checked, how the run ends is not. And a run the reference does
not model can loop: a side still running at the budget gets 16,384
instructions more, so that a JIT looping where the interpreter stops
is a finding; only a run neither finishes is set aside, as
fuzz_harness.h sets one aside.

It found nothing in the core.

Mutants (`scripts/fuzz-mutant.sh 120 rv32-vm ...`), the late ones found
only after the generator could reach them:

| mutant | where | found |
|---|---|---|
| S's data TLB holding user pages (SUM's) | `rv_mmu_dtlb_rights` | 88 s |
| the data TLB holding execute-only pages (MXR's) | `rv_mmu_dtlb_rights` | 45 s |
| one data table for S and U | `rv_dtlb_table` | 6 s |
| a straddle's second half not translated | the interpreter | 12 s |
| its mtval the instruction's address | the interpreter | 5 s |
| its PMP check skipped | the interpreter | 98 s, once PMP could cover the code's second page and a step mapped the two pages in order |
| SFENCE.VMA keeping the JIT's blocks | `rv_mmu_flush` | 10 s |
| the translator's fetch without PMP | `ir_fetch16` | 14 s |

Two more, on the fetch table's rights -- S running user pages, X ignored
-- are equivalent: `rv_itlb_fill` is called only after the same fetch's
translation succeeded at the same privilege, so the rights it asks of
the TLB entry were established by that walk. That is read from the
code, not measured; a caller that fills the table without a walk would
make them matter.


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
| **Q set inside an IT block was lost at the next load or store in it**: the translator put xPSR back, as the IT instruction had found it, around every access in the block, so a multiply-accumulate's saturation vanished -- from the live xPSR and from the frame a fault stacked. Found during the dictionary sessions (below), in an input that touches no device | `armv7m_ir.c`, `it_access` | the `itblock` guest's `IT-Q-across-access` (on the F746 it fails with the old translator), `armv7m-jit/q-set-in-it-lost-at-access` |

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

And with the interrupt controllers mapped and two cores where there are
two, at 3ad607b, five minutes each, nothing found:

| target | inputs | compared | touched a device | took an interrupt | set aside |
|---|---|---|---|---|---|
| RV32 | 34,944 | 31,884 | 127 | 46 | 9 |
| ARMv7-M | 117,931 | 106,074 | 121 | 11 | 22 (7 unsynchronised) |
| G4MH | 26,966 | 23,695 | 107 | 23 | 0 |
| G4MH, three-PE build | 152,704 | 143,576 | 15,631 | 731 | 194 |
| PowerPC | 260,183 | 243,055 | 827 | 60 | 19 |

## Mutants

`scripts/fuzz-mutant.sh SECS TARGET FILE OLD NEW` breaks one line,
fuzzes from a fresh corpus, puts the line back -- and replays whatever
it found on the restored build, because a find the unmutated build
makes too is not the mutant's. Each mutant below ran twice, 300 s,
with the device seeds and without (`FUZZ_NO_DEVICE_SEEDS=1`), in a
worktree so the tree could go on being worked in:

| mutant | with the seeds | without |
|---|---|---|
| A: the dispatcher never takes an interrupt (`emu_jit.c`) | all four targets, by a seed, in 3-7 s | PowerPC, by a saved regression (`decrementer-taken-a-block-late`), 3 s; the other three not in 300 s |
| B: ARMv7-M's dispatcher stacks xPSR without packing the flags translated code keeps apart (`a7_take_irq`) | not found | not found |
| C: G4MH's JIT copy of the interrupt path does not raise the ISPR ceiling (`g4mh_jit_take_irq`) | by a seed, 4 s | not found |

**What the seeds are worth is all of it**: no device mutant was found
by fuzzing that started without them. The interrupt has to be raised
while masked and then enabled by an instruction the translator
declines, with translated code after it; a mutation of guest code does
not stumble on that in five minutes.

**B cannot be found by this design, and that is the result.** An
interrupt the dispatcher takes right after an enable finds the flags
just packed by the interpreter fallback that ran the enable, so not
packing them again changes nothing. Stale flags reach the stack only
when the interrupt became deliverable *inside* translated code -- a
store to the NVIC -- and those runs are set aside as asynchronous. The
set-aside rule's blind spot is exactly where B lives.

**A dictionary raises reach; it did not find a mutant.** The same three
mutants, without the seeds, 300 s, libFuzzer seeds 1 and 2: without a
dictionary and with the first one at 20a104a, and with the second at
c264744. Compared runs that touched a device, and that took an
interrupt:

| mutant | device: none / first / second | interrupt: none / first / second | found |
|---|---|---|---|
| A, RV32 | 10, 95 / 855, 148 / 83, 657 | 0 / 28 / 0 | no |
| A, ARMv7-M | 972, 748 / 2,956, 2,468 / -, 2,898 | 0 / 0 / 0 | no |
| A, G4MH | 667, 239 / 703, 582 / 1,070, 696 | 0 / 0 / 28 | no |
| C, G4MH | 16, 2,629 / 1,215, 3,998 / 617, 502 | 0 / 20 / 47 | no |

The first dictionary's own `#!` sequence passed under mutant A: its
enable was followed by the ECALL fill, which runs in the interpreter,
so both backends took the interrupt there and a dispatcher that never
takes one went unseen. One translated instruction after the enable and
they disagree. The second form carries that pair as one token, and each
dictionary's sequence was shown by hand to disagree under A on all four
targets and under C on G4MH (the ctest shows only that it takes its
interrupt on the unmutated build). Still, 300 s did not put four or
five tokens in order with nothing between them that traps. **A
dictionary gives the words; the seeds give the sentence**, and they
remain what finds a device mutant. The dictionaries stay: they are
cheap and multiply the runs that reach a device. ARMv7-M's first
session with the second form stopped at a JIT defect instead (the Q
flag, above), in an input with no device in it, so not to the
dictionary's credit that anyone can show.

Three things made the first session's verdicts worthless, each fixed
in the scripts:

- **one input hung for 1,711 s** of a 300 s run -- libFuzzer's default
  timeout is twenty minutes -- and it was a real hang in the ARMv7-M
  interpreter, mutant or not (above). Every run has `-timeout=10` now.
- **a "found by fuzzing" was the FS difference**, which the unmutated
  build made too -- hence the replay.
- **a guest that would not build in a fresh worktree failed the whole
  build**, after the fuzz binary had linked, and the empty log read as
  "not found". Guests only seed the corpus now, and a failed build is
  reported as one.

## Things worth keeping from finding them

- **The interpreter is the reference, and was the one that was wrong
  -- eight times out of eighteen**, two of them the IR's own. The RV32
  probe asked Sail and Spike rather than reasoning about which backend
  to fix. On mcycle they
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
  quoted both ways for that reason. A host A/B now builds both sides
  `-DEMU_LTO_PARTITION=one`
  ([performance.md](performance.md#one-lto-partition-for-abs)).
- **The bus fix keeps the type.** `memcpy` makes an access a byte
  access, which may alias anything; the alignment-1 typedef keeps
  `uint16_t` and `uint32_t`, and the F746 firmware differs only in four
  compare-and-branches that became `cbnz`.
- **The FS defect was a decision taken before the information it
  needed.** Chainability is fixed in the prologue; "this block writes an
  f register" was learnt while lowering its body. The fix cost
  Whetstone under the x86-64 JIT 2.09% of host instructions, because
  those blocks no longer chained -- until RV32 marked FS in the block
  itself, after which they chain again
  ([jit/floating-point.md](jit/floating-point.md#fs-dirty-and-which-blocks-chain)).

## Adding a finding

Fix it, confirm the fix by reverting it, and copy the input to
`tests/fuzz/regressions/<isa>-jit/` -- `regressions/ir/` for `fuzz-ir`,
`regressions/rv32-mmu/` for `fuzz-rv32-mmu` -- under a name that says
what it found; each target's `-regressions` test replays its directory. Where the defect can
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
