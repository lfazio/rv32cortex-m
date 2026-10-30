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
[`docs/frontend/`](docs/frontend/), [`docs/backend/`](docs/backend/),
[`docs/jit/`](docs/jit/README.md),
[`docs/host/`](docs/host/README.md),
[`docs/stm32f446/`](docs/stm32f446/README.md),
[`docs/stm32n6/`](docs/stm32n6/README.md),
[`docs/linux.md`](docs/linux.md), [`docs/games.md`](docs/games.md), and
`docs/<platform>/<frontend>/README.md`. [`docs/TODO.md`](docs/TODO.md)
holds open work only: what is done is described where it was done.
[`docs/lessons.md`](docs/lessons.md) is the full account of every rule at
the end of this file.

Three axes, independent of each other:

| axis | what it decides | selected by |
|---|---|---|
| platform | where it runs | `EMU_PLATFORM=host\|stm32f446\|stm32f746\|stm32n6` |
| frontend | what it emulates | `EMU_GUEST_ARCH_{RV32,G4MH,PPC,ARMV7M}` |
| backend | how it executes | `EMU_JIT=ON\|OFF`; `--jit` on the host runner |

```
include/emu/   src/emu/          ISA-agnostic runtime: bus, regions,
                                 passthrough, NS16550 console, ELF loader,
                                 the frontend registry -- and the IR, the
                                 JIT framework and the glue between them
               src/backend/      IR -> host code: thumb2/, x86_64/
include/rv32/  src/frontend/rv32/  RISC-V RV32: hart, CSRs, decoder,
                                 interpreter, IR translator, CLINT, APLIC
include/g4mh/  src/frontend/g4mh/  Renesas RH850 G4MH: core, decoder,
                                 interpreter, IR translator, INTC
include/ppc/   src/frontend/ppc/   NXP e200z7: core, decoder, disassembler,
                                 interpreter, IR translator, EFPU2
include/armv7m/ src/frontend/armv7m/  ARMv7E-M: core, decoder, interpreter,
                                 IR translator, NVIC
               src/platform/     host runner; common/, stm32/ and one
                                 directory per board
```

Every frontend has an interpreter and a translator to the shared IR;
there is no per-host translator anywhere.

**`include/emu/emu_cpu.h` is the contract**, and the note at the top of it is
the thing to read before adding a frontend or a member. The rule it lives by:
`run` executes a whole budget behind one indirect call and every other hook is
setup or fires on a trap. Nothing in that table may end up on a
per-instruction path — a single extra *direct* branch on the fetch path
measured 9.3% on CoreMark.

The frontends are symmetric. `rv32_frontend.c`, `g4mh_frontend.c`,
`ppc_frontend.c` and `armv7m_frontend.c` are the same file with different
contents, which is the intended shape: if a new one needs something none
has, it probably belongs in `emu_cpu_ops_t` rather than in a platform
`#ifdef`.

## Build, run, validate

All of it -- every configuration, every option, the console settings, the
network build -- is in [README.md](README.md) and under
[docs/](docs/Architecture.md). Do not duplicate it here; a recipe in two
places is a recipe that disagrees with itself, which is how this file
came to document an `RV32_PLATFORM` option that has never existed.

What belongs here is only what the recipes do not say:

```sh
./scripts/run-arch-test.sh      # 510/510; EMU_EXTRA_ARGS=--jit for the JIT
./scripts/run-arch-test.sh --misaligned  # 523/523: the *default* build
./scripts/run-riscv-tests.sh    # Berkeley suite, 77/77; EMU_HOST=<a wrapper
                                # adding --jit> for the JIT -- it takes no flag
./scripts/build-matrix.sh --test # every configuration, and its ctest
./scripts/report-figures.sh     # figures, with the cache vars that set them
./scripts/check-doc-flags.sh    # every -D flag in the docs exists
./scripts/run-probe.py PROBE.S  # emulator vs Sail and Spike, for what no
                                # suite covers; scripts/fetch-spike.sh first
./scripts/gdb-check.sh BUILD    # a real gdb against each frontend's stub
./scripts/callgrind-ab.sh OLD NEW [ROW...]  # host instructions, two runners;
                                # exact, so one run a side; both built
                                # -DEMU_LTO_PARTITION=one
BOARD_SRC=<worktree> ./scripts/board-measure.sh NAME SECS -D...
                                # a firmware built from that tree, flashed,
                                # run: ratio, wall clock, translations, CRC
./scripts/board-pc-profile.py ELF  # where a -DEMU_PC_PROFILE=N firmware
                                # spent its time, by function and line
./scripts/fuzz.sh SECS [ISA|ir|rv32-mmu|rv32-vm|g4mh-mpu|armv7m-mpu|ppc-tlb|all]
                                # a frontend's interpreter against the
                                # x86-64 JIT, under ASan/UBSan; `ir`: random
                                # IR blocks, before the passes against the
                                # compiled code; `rv32-mmu`: Sv32, PMP and
                                # the data TLB against a walk from the spec;
                                # `rv32-vm`: the same in S/U code, fetch and
                                # the JIT's probe; `*-mpu`, `ppc-tlb`: an
                                # MPU or TLB against its manual, one
                                # instruction a step; docs/fuzzing.md
./scripts/fuzz-mutant.sh SECS ISA FILE OLD NEW  # break a line on purpose
                                # and see whether that fuzzer notices
./scripts/fuzz-dict.py [--check] # the device dictionaries' tokens, from
                                # the assembly written above each
tests/armv7m-diff/run.py ...    # the ARMv7-M frontend against a real M7
```

**Run both RISC-V suites.** They cover different things: a regression
only the Berkeley suite catches (`rv32mi/csr`, when F was added) sits
unnoticed if only arch-test runs. Keep their `-march` in step with what
`misa` advertises, or `rv32mi/csr` fails by design and looks like a bug.

**The Thumb-2 backend cannot be exercised by any host suite.** Validate
it by flashing a guest and reading the UART; this has caught JIT bugs
both x86 suites passed. Hardware is a Nucleo-F746ZG, console at 921600 on
its ST-LINK's virtual COM port, opened by `/dev/serial/by-id/`; build
`-DEMU_NET=OFF` to keep the UART a console. `--connect-under-reset` is
the default in the `flash` targets because this firmware never idles.

**`isatest` on the board is necessary and nowhere near sufficient.** It
never fills the code cache and finishes in a tenth of a second, and two
defects sat behind those facts while it passed (a compaction that moved
chained jumps; a cycle counter wrapping at 19.9 s). Follow it with
CoreMark x650 (`-DRV32_GUEST=coremark -DCOREMARK_ITERATIONS=650
-DEMU_MAX_INSN=0`) **at `-DEMU_JIT_CODE_BYTES=32768`**, about 100 s, timed
from the host end too. The 64 KB default neither crosses the wrap nor
evicts: right for a performance figure, wrong for this.

## Rules learned the hard way

Each rule is a line or two and the number or symbol that proves it.
Every one has a story -- what happened, the false starts, the instrument
that finally told the truth -- in [docs/lessons.md](docs/lessons.md),
in the order found; search it for the symbol. Reference material (what
is implemented, the figures, how a block is specialised) lives in the
rest of [docs/](docs/Architecture.md). A new rule goes in the theme it
belongs to here and at the end of lessons.md in full.

### The seven

Each has cost more than one session, and every one recurred.

1. **A trap only reports if something catches it.** In a flat guest with
   no vector table an exception lands on ordinary code and carries on
   silently. Give every guest a handler that names its cause and its
   slot, on day one (G4MH, PowerPC and the CC-RH guests each paid for
   not having one).
2. **One weak test is worse than none**, because it reads as coverage.
   Test the awkward input (NaN, `cond=15`, -1, shift 0 and 32), never
   assert a value the implementation computed, and confirm a fix by
   reverting it and seeing the failure name the mechanism.
3. **Identical counters mean the code never ran**, not that nothing
   regressed. A perfect null result is the loudest signal here; the
   first suspect is the instrument.
4. **Every translate-time read of mutable state is a staleness bug until
   proven otherwise** -- and too coarse a flush is as wrong as too
   narrow, and quieter.
5. **Measure; do not reason.** Interpreter-in-SRAM, the register cache,
   the shifted-operand fusion, a decode cache: sound arguments, all
   measured worse.
6. **A flag or invariant quoted in prose is not a tested thing.** Grep
   for the *readers* of every option, field and mode
   (`scripts/check-doc-flags.sh`; 4 of 52 options were read by nothing).
7. **A figure that is too good is the instrument until a second clock
   agrees.** A board run showed the JIT 5x ahead while taking twice as
   long by the wall clock: CYCCNT had wrapped.

### Tests that pass without testing

- **"Nothing failed" also passes when nothing ran.** Assert something only
  a completed run produces (`PPC-ISATEST-END`); a capped run is a verdict
  of its own (riscv-tests reported 77/77 while 64 of them looped in their
  own handler).
- **A harness narrower than the value passes against the bug.**
  `CHECK_EQ` casts to `uint32_t`; doubles compared their low halves.
  `CHECK_EQ64` exists.
- **An A/B that reports zero failures may not have changed anything.**
  Assert the edit's match count; disable *every* site (the second
  halfword check caught what the first missed); a failing build looks
  like a passing run (`grep -c` hid one).
- **A conditional test must be shown to compile somewhere you run it**
  (`#if G4MH_MPU_ENTRIES <= 31` was false at the default); and when
  asserting nothing was corrupted, snapshot all of it.
- **A staleness test shows reuse in its counters, or it has not
  asked**: fuzz-rv32-vm fenced after every run and reported 76,295
  translations and 76,295 block entries -- nothing found twice.
- **Put the event where the behaviours differ.** A fault at a block's
  first instruction cannot see a translator's mistake about the middle;
  a reserved PTE at the leaf cannot see a skipped check; a cache test
  must hit the cache (`-- dtlb -- fills`); a check must run after the
  mechanism arms (mmiobench's second `verify()`).
- **A test of "re-decide when X changes" needs X to be the only change**
  (PowerPC's TLB write followed straight by a branch), and a JIT's keys
  are tested only by running an address twice under different state --
  loop first, then call.
- **Random operands miss relations**: equal operands, `INT_MIN / -1`,
  zero divisors. Mutation testing finds the class; put the relation in.
- **A check named for a bug starts from the value the bug destroys**, and
  resets what it depends on inside the case.
- **A test of a fault checks the address and the cause**, not that it
  faulted or that some handler ran: `dcbz` reported DEAR 0, a fetch fault
  arrived at +8, and an unrelated exception reaching the same handler
  passed three versions of one test.
- **A compiled guest finds what hand-assembled tests by the same author
  cannot**: one 90-line C guest found three G4MH defects in a row, and an
  expectation written from the shape of a number disagreed with a correct
  divide.
- **A timing-dependent multicore test tests the scheduler.** Pass at
  quanta 1, 4 and 64; `tests/guest/same-at-any-quantum.py` requires the
  same output and retired count across slicings.
- **A property no output can show is asserted where it is made** (the
  FreeRTOS round order, which two mutants left output byte-identical).
- **A suite result is about the binary and the configuration that made
  it.** Suites that bring their own start-up never saw `start.S` break;
  a test written in one configuration failed 20 times in `host-all`; a
  test needing root is a test nobody runs (`unshare -rn`).
- **A contract met only by the interpreter is untested under the JIT**:
  `step` ran a whole block, and `gdb-check` never passed `--jit`. When a
  test compares two backends, print which two it used.
- **A filter that keeps a harness safe decides what it can never ask.**
  rawgen kept ARMv7-M operands in r0-r12, so 557 sp/pc encodings the M7
  refuses had never been asked, and the frontend executed them all.
- **The fuzzers find what the suites share a blind spot for**: eighteen
  defects, eight in an interpreter (docs/fuzzing.md). A UBSan report in
  the interpreter is a finding even when both backends agree.
- **A reference that declines what it does not model checks only what
  is left -- and modelling more tests the checker.** `emu_ir_interp`
  had no ADDI or multiply, so `EMU_JIT_DIFF` checked almost no real
  block; given them, it showed a loop it ran once, FP state and mstatus
  it never put back, and a device register it read before the compiled
  code did. And a contract the passes state is a contract a frontend
  may rely on later: `fuzz-ir` built the r15-is-the-pc block no
  frontend emits yet, and a pass forwarded the old pc.
- **Writing a reference is its first test.** Every G4MH MPU defect --
  cause codes, a doubleword checked a word at a time, a fetch checked at
  its first halfword, CAXI's flags -- was found writing fuzz-g4mh-mpu's
  reference from the manual, none by running it; 24 mutants show the
  run would have. A null result (fuzz-armv7m-mpu) is the mutants' to
  vouch for.
- **A test input is checked against the mutant it is for**: a
  dictionary's own sequence took its interrupt and passed under a
  dispatcher that never takes one -- only interpreted code followed the
  enable. And a dictionary gives the words, not the sentence: it
  multiplied device reach and found no mutant the seeds find.
- **A differential fuzzer cannot see what both sides share, and a seed
  is a test: run each alone and read what it did** (`FUZZ_SHOW=1`).
  G4MH's LDSR to PLMR never made the core look for an interrupt; both
  backends run LDSR in the interpreter, so they agreed, and the seed
  written to reach it took no interrupt.

### Measuring

- **On the board, the counter is exact and layout is worth up to 21%.**
  Rerun the same binary before calling a difference noise; move the
  layout (`EMU_LAYOUT_PAD`, `-falign-functions`) and see whether the
  figure follows; for translated code move the buffer
  (`EMU_LAYOUT_PAD_BSS`). On the host, callgrind is exact; under ~10%,
  best-of-five wall clock is not a measurement.
- **Callgrind cannot see what an M7 charges for a loop's shape.** An
  interpreter change is measured on the board (PowerPC's `stretch()`:
  +0.10% callgrind, +10% F746; ARMv7-M's sp/pc rules: +2.3% callgrind,
  +10.3% F746, where GCC moved a predicate out of ITCM into flash). Read
  the firmware's `nm` for what left the loop, and profile with
  `-DEMU_PC_PROFILE` and `scripts/board-pc-profile.py`. LTO
  partitioning alone moved a row by 1%: a host A/B builds both runners
  `-DEMU_LTO_PARTITION=one` (`callgrind-ab.sh` prints each side's). On
  the board one partition reorders `.bss`, a layout change worth 21% on
  RV32 Dhrystone's JIT, so the firmware keeps the default.
- **Pin what every block touches.** Core state goes where the platform
  says (`EMU_HOT_DATA`: the F746's DTCM); left to the linker, RV32's
  hart slid into SRAM1 behind the 4 KB D-cache in a one-partition build
  and Dhrystone moved 21% with where `.bss` started.
- **Time a board run from the host end too.** A 32-bit cycle counter less
  an epoch is a wrap short after 19.9 s; `emu_cycles.h` accumulates
  wrap-correct deltas. A sweep taken with a broken instrument points the
  wrong way, not just at wrong numbers.
- **Profile before choosing what to optimise, and ask which instruction,
  not which class.** `rdtime` was 99.2% of a Linux boot's CSR accesses;
  a SysTick loop was 16% of ARMv7-M's host instructions; reads of the
  image walking the region table were 38.9% of Dhrystone's under the
  JIT. Split by function before estimating how a total divides.
- **Check the instrument can represent the difference.** The
  disassembler had no FP (reported 0 of a 60% share); the pair histogram
  read one funct7 bit; `xlat`/`interp` cannot tell a helper call from a
  lowering; a histogram's x0 bucket was a quarter of its counts.
- **Break `interp` down before acting on it.** Count single
  instructions to ask which one, and pairs only to ask what follows what
  (under a JIT the interpreted ones arrive alone); run the count under
  `--jit`, where it is exactly what the translator declines
  (`-DEMU_PAIR_STATS=ON`). A huge `declined ... (cached N)` is one loop
  head declining forever.
- **Two bottlenecks the same size hide each other**: each fix alone
  measured 202 s, both 109 s. Executing at 4.8 M/s and idle at 0.1 M/s
  are different stalls. Quote a median, not one line.
- **A benchmark that declares its own result invalid is evidence of
  nothing** (CoreMark under 10 s prints "Errors detected").
- **The host's guest clock is the wall clock**; use `retired` for a
  reproducible quantity. A longer quantum is a coarser clock
  (`Zicntr-csrrc` failed past 11,000).
- **Two frontends on one program measure their compilers**: PowerPC
  Quake was 63% compiler-rt float until `-mefpu2`. Profile the guest by
  symbol first.
- **When the ratios of two binaries agree to two decimals, read the raw
  counts.** And a null result tests a hypothesis too: identical counts
  at 1024 table entries said the table was never the limit.

### Stale artefacts, flags and the build

- **CMake cache variables outlive the tree you set them in** (every figure
  once used an inherited 48 KB cache). Check `CMakeCache.txt` before
  quoting; `build-matrix.sh` removes each row's cache.
- **An unpinned checkout is a configuration nobody chose**: a fresh
  worktree cloned riscv-arch-test's head, which on 2026-10-30 wanted
  Sail 0.16 against this host's 0.13.1, and arch-test-I failed in every
  row of the matrix for a reason not the emulator's (`ARCH_TEST_REF`).
- **Name the artefact, not the target.** A stale `emu-host` at a script's
  default path gave a session of false 77/77s; `--target emu-host` does
  not rebuild guests; `md5sum` across two trees localises a difference.
- **A "before" is a `git worktree` at a named commit**, never a second
  build directory of this tree: a board build rebuilds before flashing.
- **`add_custom_command(DEPENDS <target>)` is ordering, not staleness**;
  name the file. `if(TARGET ...)` sees only targets already defined.
  `set(STM32CUBE_FAMILY ...)` must precede the `include()`. lwIP's
  `include_guard(GLOBAL)` empties its source lists (use `SOURCE_SUBDIR`).
- **A capability macro must include what defines it** (`G4MH_HAVE_JIT`
  read before `emu_jit.h`), and a file selected by host goes with the
  other host files, not under a frontend's block.
- **A capability that depends on nobody exercising it is not off.** The
  RISC-V JIT was kept off x86 by `main()` never selecting it; build the
  other value of every flag and count, or accept it does not work
  (`PE_COUNT > 1`, VLE mode, `EMU_JIT=OFF` all broke unbuilt).
- **When a subsystem is replaced, grep for the readers of its options and
  re-check its measured wins.** `EMU_JIT_CODE_BYTES` reached only the
  deleted translator (wired up: 6.55x); the new IR lost Zbb (23,339
  interpreted instructions back to 1 once lowered).
- **A row that builds is not a row that runs** (`board_argv` passed
  `--jit` to a JIT-less build); a board change is not done until a board
  row has linked (libatomic's `__atomic_load_8`).
- **An `#if` around a `default:` removes the default**, and an LTO object
  holds no code to disassemble (compile `-fno-lto`, assert the count).
- **`-ffp-contract=fast` breaks error-free transforms** on hosts with an
  FMA: `rv_fpu.c` is built `-ffp-contract=off`. Guest images link
  `-nostdlib`, the core calls no libm, and the FPU uses `float` only.
- **A comment asserting an invariant is where to look first** -- the
  image split "concatenates to the `.bin`", ROM "cached for reads only",
  Zacas "unfinished" were all false, and G4MH's "cannot happen for a
  halfword-aligned pc" let a 32-bit instruction at `...E` run from a
  refused area. A constant chosen to avoid another (FETRAP at `90H`,
  dodging an MDP that was itself wrong) is a reason to check the other.
- **Prose telling a maintainer to change a constant is a constant that
  will not be changed**: compute it (`EMU_MAX_REGIONS` from the PE count,
  after "raise it to 24" sat in a design note).

### The JIT: what a block depends on

The sweep is [docs/jit/staleness.md](docs/jit/staleness.md).

- **A translate-time legality check is half a guard; the block outlives
  it.** A block built with FS on ran after FS went off.
- **Watch the configuration, not the flag** (PMP bounds, not
  `pmp_active`; mappings via `vm_gen`, not `satp`). Count changes on the
  writes that alter an entry, not on the refresh every privilege change
  calls.
- **Identity and invalidation are different keys.** What a block is *for*
  (privilege, address space, FS off-ness, frm) goes in the exact context,
  mixed into the hash; what makes every block wrong goes in the
  generation. Put identity in the generation and every trap flushes
  (129,293 flushes a Linux boot). When a key is full, widen it.
- **A block bakes in the core that built it, wherever cores share a
  table.** G4MH inlines the stack's region by host address, and the SELF
  window is a different memory on each PE: PE1 ran PE0's loop on PE0's
  stack. The PE is in the context now; the guests kept stacks in shared
  RAM, so only a two-PE fuzz run with a stack in local RAM saw it.
- **Re-derive on the interpreter fallback, never per dispatch**, and
  remember a trap is not always an instruction the interpreter ran:
  `rv_hart_trap` refreshes the context.
- **The fetch is a translate-time read too** (G4MH's MPU), and a cache
  in front of the thing under test can answer for it: flush first.
- **A JIT fast path bypasses the helper's side effects** -- dropping the
  reservation, PMP -- and inlining an access is sound only while nothing
  can deny it.
- **Anything inlined below M answers two questions**, translation and
  permission: `pmp_active` is true below M whatever the entries say, so
  a TLB probe alone changes nothing. The data TLB answers both per page
  (sv32bench paged 21.4 ms against bare 21.5).
- **Derived state goes stale where code clears its inputs by hand**
  (`fetch_guard` after reset sent every later guest to the interpreter);
  compute summaries on read (`mstatus.SD`).
- **A device counting instructions reads a chain's start** under the JIT
  (PowerPC STM read 900 of 1,000); asynchronous interrupts arrive a block
  late, which the architecture permits.
- **A decision needs the information it depends on**: chainability fixed
  in the prologue missed an `FPUT` in the body, and FS stayed Clean.
- **A value read once for a span is stale for every write inside it**:
  a translated IT block put back xPSR as the IT found it around each
  access, losing Q a member had set (`armv7m-jit/q-set-in-it-lost-at-access`).

### The JIT: lowering, declining, the framework

- **What you decline costs more than what you translate badly**, and a
  short block still beats interpreting (minimum length 1 to 6: 0.044 s to
  0.643 s). Route the rare instruction through a helper; a helper call is
  a translation.
- **"Declined", "overflowed" and "cache full" are three outcomes**;
  conflating any two retranslated one block 957 times. Remember
  refusals keyed on (pc, context), with an epoch, not a memset (1.42x on
  Linux).
- **Every `default:` that declines is a list a new operation falls off**,
  and both defaults of `emu_ir_can_lower` have lied (Thumb-2 promising
  what it refused; x86-64 declining what it lowers). Make each effect
  countable (`emu_ir_opt_totals`).
- **A list of exceptions fails open**: the dead-value pass removed a
  load nothing read -- an APLIC claim. It is a list of pure operations
  now.
- **An IR pass must honour every guest invariant the lowerings do**
  (`pass_reg_traffic` forwarded x0). A checker comparing two consumers
  of one bad input validates the consumers.
- **A struct filled by a macro hides a missing member at both ends**:
  `EMU_IR_DEFINE_X86_BACKEND` dropped `.sync` and `.after_interp`.
- **A hook on committed code must not consult the emitter's state**
  (`compact()` with the overflow flag up). The host's JIT sizes hide every
  eviction path: build a host runner with the board's.
- **A table that drops its Nth entry in silence is a wrong program
  waiting for an Nth entry** (64 exit patch sites).
- **A stepped instruction that may write the pc ends the block** --
  including a writeback to the pc (`helper_falls_through`).
- **The framework owns the host's conventions** (the Thumb bit, table
  sizes per target); per-dispatch callbacks cost 129 cycles where four
  loads of bound pointers cost nothing.
- **A hash must keep its chain and use every input bit.** A direct-mapped
  block table hid colliding hot blocks (1,977 translations against
  1,465); a declined-pc cache indexed by low bits evicted every pair
  0x800 apart -- multiply and take the high bits.
- **A capability query is worth nothing if the backend cannot lower the
  fallback**: Thumb-2 declined `EMU_IR_HELPER_TRAP`, so every honest "no"
  cost the whole block. Two hosts disagreeing completely is the tell.
- **The code cache dominates JIT performance**: below the working set the
  JIT loses to the interpreter. A cyclic hot set larger than the buffer
  is no retention problem -- no order holds it -- and cost Whetstone 20x
  until the framework gave up on pcs that keep being evicted
  (`EMU_JIT_THRASH_EVICTIONS`), a few per compaction, interpreting each
  to its first jump. The MCU's sizes on a
  host retranslate constantly and mask translator bugs, so the x86-64
  backend sets its own -- and its stats line is what proves a pass
  exercised it (`interp 7807` of 8527 on the first run).
- **A knob is tuned on a workload that can observe it**:
  `EMU_JIT_LOOP_CAP` (`RV_JIT_LOOP_CAP` in lessons.md, which the IR
  replaced) bounds interrupt latency, which mmiobench sees and CoreMark
  cannot; 128 is the knee.
- **The bus is scanned once per flush, not per translation** (8% on
  CoreMark), and the peripheral window is inlined only once the guest
  has used it (`EMU_JIT_PT_ARM_AT`, once `RV_JIT_PT_ARM_AT`): 2.5-7x for
  RV32's drivers and 5.5-13x for ARMv7-M's, and always-on cost compute
  53% in cache pressure. Flushing from inside a helper is
  safe; a per-dispatch pending flag is not free.
- **An instruction-sync is a flush, a barrier is nothing**: FENCE.I,
  SYNCI and G4MH's CACHE discard translations; FENCE, SYNCM and the rest
  order nothing on one host thread. Self-modifying tests must patch a
  block's *start* and run the backend they name.
- **What the JIT emits is the cost on a fixed cache.** Measure size on
  the host that pays (Thumb-2 operand-sized encodings: 27% smaller,
  1.71x); after code grows, run the workload that fills the buffer and
  read `compact` before the time. A fast path costing bytes goes out of
  line before it is gated; a flush that arms something may be worth more
  than the thing.
- **The register-file round trip is the target, not fusion**:
  `lui`+`addi` is 0.2% of CoreMark pairs, dependent pairs 29.6%. Run the
  pair statistics before writing an encoder; an allocator pays only
  where the value is used in place, and a guest-register cache lost 15.5%
  (88-93% of block/register pairs are touched once or twice).

### Decoders and encodings

- **Encodings sharing a slot are decoded in one place**, common case
  first, and **enumerate the slot against the vendor's assembler** before
  writing a constant (`scripts/g4mh-check-encodings.sh`, binutils for
  PowerPC). Holes sit between implemented neighbours.
- **A suite of instructions cannot see the encodings that are not
  instructions.** Raw encodings on the board found 465 of 6,000 wrong on
  ARMv7-M; both decoders are held equal over all 402,712,576.
- **The board's table is not the manual's.** The M7 raises UNDEFINSTR for
  wrong should-be bits and for every UNPREDICTABLE sp/pc choice asked,
  but executes an LDM of one register and REV/CLZ with Rm copies
  differing -- and every UNPREDICTABLE IT form: a nested IT replaces the
  block, B\<cond\> and CBZ inside one ignore its condition, and any
  pc-writer ends it even when skipped. Ask, keep the
  answers (`test_sp_pc_board_answers`, `test_it_board_answers`), and ask
  dangerous encodings last.
- **A register field reused as an opcode extension will not tell you**
  (G4MH `reg2 == 0`, five instances, the last stopping every C guest);
  check every instruction sharing the opcode, and its width.
- **A staged decoder's first stage is where the exceptions hide**
  (`g4mh_insn_len` and the 48-bit `JR`); one rule in one place, held by
  a property test.
- **A field read too narrow aliases half the instructions** (`e_bc`'s
  BO32), and a translator that reads a field as an index translates the
  reserved values (RV32 LOAD's `f3 & 3`).
- **A mode flag whose only writer is a unit test is not a mode** (`vle`).
- **The disassembler repeats the decoder's old mistakes**: G4MH's
  printed PUSHSP and POPSP as `di` from bit 15 alone, long after the
  interpreter's comment said why that was wrong.
- **The disassembler is not a decoder.** `.short` cannot tell a gap from
  a correct decode; read a trace's pc deltas, and confirm a tool's
  "unsupported" was said, not omitted (CC-RH's listing parser).
- **An oracle is code too, and has a reference table**: the EFPU model's
  square roots, `test_disasm`'s 71 words against binutils, `check.py`'s
  stale exception list.

### Emitters (Thumb-2 and x86-64)

- **An encoder whose wrong answers are other valid instructions needs its
  boundary values tested.** A register that does not fit the 16-bit form
  (`CMP r8` became `CMP r0`), a hand-built ModRM (`r12` became `esp`),
  `imm5 == 0` (LSR/ASR #0 mean #32, ROR #0 is RRX). Register parameters go
  through `encode.c`/`emit_cmp_hi`.
- **`MOVS` writes N and Z**: set the false value before the compare.
- **Branch range is a silent cliff**: pick the encoding by reach
  (`emit_bcond_back`).
- **ARM orders FP flags opposite to RISC-V** (`RBIT`, `LSR #27`); set
  FPSCR.DN, clear FZ; float-to-int differs only on NaN (`VCVTR`, then
  fix with `VCMP` and `IT VS`).
- **A block that sets a mode states it**: MXCSR is a constant, not the
  caller's masked.
- **System V wants rsp 16-aligned at a call**, and `jalr ra, ra` computes
  the target before writing the link.
- **The M7 needs I-cache maintenance after emitting** (clean to PoU,
  invalidate); it is the platform's (`board_sync_icache`), and it has not
  been shown to fail -- say so.

### Architecture details that cost a session

- **RISC-V F/D**: NaN boxing is a change to F, per instruction -- `FLW`
  and `FMV.W.X` write boxed, `FSW` and `FMV.X.W` move raw. There is one FP
  implementation (SoftFloat via `rv_hart_fp`), with the IEEE-exact five
  open-coded only after NaN and flags were written down.
- **PMP**: the lock bit says who an entry binds, not whether; `1u << 32`
  made the widest region the narrowest; execute permission was checked
  nowhere (grep for the enumerator); it applies per halfword. U-mode
  turns latent M-mode bugs into failures. And an access half inside an
  entry fails even an unlocked one, in M: the matching rule comes before
  the lock rule (`pmpstraddle.S`, `rv_pmp_needed`).
- **A privilege rule nobody has run from the refused side is not
  implemented** (MRET ran in U-mode until `umode.S`).
- **A rule stated per instruction is not enforced per value**: the
  ARMv7-M run loop took any 0xFxxxxxxx pc in Handler mode as
  EXC_RETURN; MOV pc, ADD pc and BLX branch, and the F746 faults them
  (`sys.c` `excret-*`, `pc_not_exc_return`).
- **A refused instruction has not happened**: MDP is resumable, so a
  SET1 whose store is refused must not have moved Z (FEPSW held it), a
  doubleword refused on its second word must not have stored its first,
  and a POPSP stopped partway must not have loaded sp.
- **A permitted imprecision is permitted until a program computes with
  it.** FS "may be dirtier" held until a program cleared one FS bit and
  read Clean against Off: mark FS Dirty where an f register is written
  or fflags changes, on both backends (Sail's rule; `probes/fsdirty.S`).
- **A wider access built from narrower ones skips the checks of the
  whole** (`fld` at 4 mod 8; PMP matched by first byte; a misaligned
  store across a page wrote the next physical frame).
- **A failing operation must still ask whether it could have succeeded**
  (`sc.w` to no memory).
- **Sv32**: blocks are keyed on virtual addresses; `satp.PPN` follows the
  physical width, and a PTE's PPN past it is an access fault, not an
  alias (it was masked: `sv32ppn.S`); a 32-bit instruction can straddle
  a page.
- **Semihosting answers M-mode only**: answering U-mode `write()`
  imitated success and hid the kernel.
- **Two registers that are views of one thing are held as one thing**
  (G4MH IMR and EIC), and a threshold that counts what is *allowed* is
  `p < value` (PLMR, EIMASK).
- **A signal's name is not where it goes** (WDTB's NMI): read the
  destination and its reset configuration.
- **Shared state kept per instance is written in one** (G4MH INTC2).
- **A stride right only because nothing uses it is wrong**
  (`rv_freg_offset` kept `n * 4` after D widened `f`).

### Arch tests and configuration

- **"Config" is five places** -- see Conventions. A failing arch test may
  be the Sail config (`amocas` under `AMOArithmetic`): diff
  `sail_riscv_sim --trace-instr` against `emu-host --trace-count`.
- **ACT selects suites by directory name**, and REQUIRED_EXTENSIONS
  admits; naming 15 unnamed suites found 13 failures, all the emulator.
  Declaring an extension releases tests in suites already named.
- **arch-test validates against a description**: an option that changes
  behaviour (`RV32_MISALIGNED`) changes the description too, so the script
  builds its own runner.
- **UDB names every parameter it wants**; take them from an existing
  config. Keep `medeleg`/`mideleg` delegatable masks equal to the core's.

### Boards and probes

- **A failed flash leaves the previous firmware running**, and repeated
  attaches can silence another board. Set `EMU_PROBE` with several
  probes, open consoles by-id, replug (or `usbreset`) when a console goes
  quiet with no firmware change, and **ask the core where it is** before
  believing a tool that says it started it (the N6 programmer).
- **Read the peripheral before suspecting the code**: a UART configured,
  enabled and muxed with nothing arriving is a wire.
- **Diff the code memory against the image** when code misbehaves on
  hardware (picolibc's `__tls` left `errno` at address 8, over ITCM).
- **An M7 port is caches and the DWT lock** (`LAR`); F7's renamed fields
  fail loudly, its HAL config names fail silently.
- **A reserved peripheral address HardFaults the emulator**, not the
  guest: test the window with registers that exist.
- **Board sizes are not host sizes**: 192 KB of `.bss` on a 320 KB part;
  a frontend that allocates its own memory map works on a host and
  cannot be ported (docs/memory.md).
- **A lesson learned in one frontend is not applied in the next**: profile
  a new interpreter on the board (ARMv7-M's MPU fetch was a third of it).

### The network path

The design is in [docs/network.md](docs/network.md).

- **A firmware that gave its console away cannot report its own
  failure**: `fatal_halt` keeps polling the stack, and failures name
  their mechanism (`addr=... halerr=...`).
- **Text and SLIP cannot share a wire**: the handover is one-way and
  announced, and an upload test needs a guest with a non-empty `.rw`
  (`coremark`), or it never exercises the half that was zeroed.
- **`__WFI` stops CYCCNT**, which lwIP tells the time by (29 s parked
  was 1.74 s to lwIP).
- **The host end has silent settings**: `slattach` leaves `CSIZE` at cs5;
  `scripts/slip-up.sh` sets the line and prints it.
- **At 921600 the UART must be interrupt-driven**, and an overrun left
  latched stops reception for good.
- **A recovery wired to one of two symmetric cases recovers from the one
  that does not happen** (the arena filling on the ram half); a callback
  meaning both "done" and "given up" cannot commit (a transfer is done
  when its last block is short).
- **A transport change surfaces latent bugs in code that does not mention
  the transport** (flash programming needed word alignment; SLIP's whole
  frames had hidden it).
- **Things that happen between slices must be seen where the firmware
  is**: the park loop takes uploads too; lwIP timeouts need slots it does
  not reserve.

### Guests and their start-up

- **A guest initialises itself** (`start.S` copies `.data`), checks it
  fits the RAM it is given (exit 224), and keeps `.tdata`/`.tbss` inside
  `.data`/`.bss`. The host cannot prove the copy; the board can.
- **Memory a test reaches through a translation is `volatile`**: the MMU
  makes the alias, which no compiler sees.
- **`&&label` is not a resume address** -- emit it inside the asm -- and
  entering S-mode by `mret` and leaving by `ecall` loses the frame.
- **A program with no C runtime has no `gp`**; print a line before each
  step of a test that can die, and find the faulting pc first.
- **`char` is signed on x86 and not here** (`-fsigned-char` for DOOM);
  when half of a symmetric thing works, suspect arithmetic.
- **A second target is the only audit a data format gets** (DOOM's baked
  tables: four defects), and a list derived from observing a program is
  not what it requires.
- **A device that drops what it does not recognise is a hang in the
  guest**; complete with an error (virtio's UNSUPP). Waiting for a
  completion, not reading registers, proves a driver and device agree.
- **"Runs to the instruction cap" is not "works"**: ask the device with
  a ten-line guest (the LTSC behind a hook nothing called).

### The contract and its callers

- **A hook the contract calls optional is optional at every caller**
  (`set_time` called unconditionally broke ARMv7-M on the board).
- **A struct the callee fills in part is uninitialised in the rest**:
  `emu_core_status` clears it once for all frontends.
- **A halt can arrive from outside the execute switch**: test the state
  at the top of the loop.
- **Moving a value from link time to run time moves its readers into an
  order nothing checks** (`g_img_*`), and code right when something
  happened once breaks when it happens twice (the bus rebuilt without
  its devices; `emu_session_start` owns bring-up).
- **A block backend may retire more than its budget**: compare
  `total >= max_insn`, never subtract.
- **`emu_fault_t` and `rv_exc_t` are both `uint32_t`**: grep the calls
  when a success value changes -- and `PPC_EXC_NONE`, like `RV_EXC_NONE`,
  is 0xFFFFFFFF: a harness checking for zero aborted on every success.
- **A platform's own copy of shared plumbing keeps the bugs** the shared
  one fixed (the F446's guest staging).
- **`set_image` runs on a reload, never on the first start**: per-image
  state comes through `emu_boot_info_t`.
- **gdb's protocol fails by hanging, and its register layout is not
  ours to choose** ([docs/gdb.md](docs/gdb.md)): a bad checksum is retried
  silently, `c`/`s` are answered only when the guest stops, ^C is a bare
  0x03 outside any packet. Test the stub against what it did not produce
  -- the ELF's symbol at a breakpoint, gdb's own register numbering --
  since a read-back passes against a byte-swapped encoding.

### More than one core

The SMP design is in [docs/os.md](docs/os.md#smp-on-the-host).

- **Grep a frontend for file-scope mutable state before running two cores
  at once**, and run a parallel test forty times: PowerPC's shared decode
  cache, a lazily built index and a stale WFI each failed one run in
  five or twenty.
- **Thread-local means per-thread initialisation**, and TLS comes out of
  the thread's stack.
- **Two reads of one value are two values** (LDL.W read twice lost
  increments), and every PE once booted onto one stack.
- **Count the system calls**: `pthread_sigmask` around critical sections
  was most of what made the parallel run slower (0.88x to 2.06x).
- **Duplicate interrupt paths drift**: G4MH's JIT has its own, so grep
  for the other copy before an interrupt change is done.

### Performance costs and where they land

- **Anything on the fetch path is paid by every guest**: give the
  features one branch (`fetch_guard`, 9.3% to 2.7%), and measure a
  feature's cost by compiling it out (Sdtrig 14.8 host cycles an
  instruction, PMP 3.2; hoisting the load made it worse).
- **What a build option costs is paid by every frontend compiled beside
  it** (PowerPC's byte-order test cost RV32 6.1%), and an `ALWAYS_INLINE`
  in the bus is part of every interpreter loop.
- **Code reachable from an interpreter's rare cases goes out of line**
  (`EMU_NOINLINE`): LTO inlines it and costs the loop (TLB flushes 2%,
  PowerPC's reservation code).
- **A new call site of something the loop inlines is a decision handed to
  LTO**; check `nm` and measure on the board (21% on the F746).
- **An out-parameter on a hot function is not free** (+1.3%), and cold
  state goes at the end of a hot struct.
- **A counter costs everyone who never reads it**: keep it where its
  question is asked; remove one nothing reads (G4MH's `cycles`: 4.5%
  interpreted on the F446).
- **At this size the compiler's choices move a row as much as the code**:
  measure every variant, on the machine whose cost it is (of six forms
  of ARMv7-M's sp/pc rules, the cheapest on x86-64 was not the cheapest
  on the board).
- **Compressed guest code is slower to interpret** (Zcb in the guest's
  march, 9%); `-Os` is 33% smaller and 8.8% slower; a last-block cache in
  front of the hash was 1.2% slower.
- **Where an interpreter runs from matters before what it runs**: ITCM
  fixed ARMv7-M's layout sensitivity on the F746 and was 4.8% slower on
  the N6.

### Oracles

- **No GPL emulator, even as a reference.** The references are the real
  boards, Sail, and Spike (BSD-3, built by `scripts/fetch-spike.sh`).
- **The oracle is a reading too**: the interpreter was the wrong side
  eight times of eighteen; ask Sail and Spike or the board rather than reason
  which to fix. Where no suite exists, write a probe
  (`tests/arch-test/probes/`, run by `scripts/run-probe.py` and
  `ctest -L probe`), with any permitted difference listed in it
  (`probe-permitted:`).
- **A second model is a second reading, and its configuration is part of
  its answer**: read its CSR masks before its semantics (Spike's
  `mcounteren` without Zicntr in `--isa`).
- **For G4MH and PowerPC the manual is the only authority**: a shared
  misreading is invisible to every check, so say so wherever a result is
  quoted.

### Editing hazards

- **Replace the smallest unique string**, never a span between markers
  (one edit removed 99 lines of `docs/TODO.md`); assert the line count.
- **Never `git checkout -- <file>` with uncommitted work in it**; copy to
  the scratchpad first.
- **A mutation driver restores whatever its backup was**: take backups in
  the run, give a restored file a new mtime, rebuild before the
  baseline, and assert the binary was relinked.
- **Adding a table entry must not replace its neighbour** (IVPR over
  ESR).
- **A formatter can break a source whose binary is committed** (CC-RH's
  `;` comments), and a fence is checked by running the formatter through
  it: `clang-format off` must be the bare comment.

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

"Config" is five places, and each has been the one that was missed:
the UDB yaml, `sail.json` (a separate job, and Sail may not have the
extension at all), `RV32_ISA_STRING`, the configure summary in
`CMakeLists.txt` (held to the string by the `rv32-isa-string` test), and
the default suite list in `scripts/run-arch-test.sh` -- **by directory
name**, so check `ls build/arch-test/tests/rv32i` for a second directory
with a longer name before believing a suite is covered.
The `--misaligned` description is generated from the first two by
`tests/arch-test/derive-misaligned.py`, so edit the base and run both
modes; the script fails if a line it rewrites has moved.

Enabling `F` forces `Zcf` on RV32 (UDB rejects the config without it),
and CoreMark's `core_main.c` defines `main()`, so `-Dmain=...` is scoped
to that file.

### Adding a frontend

1. `include/<isa>/` — public headers, `<isa>_` prefixed
2. `src/frontend/<isa>/` — state, decoder, interpreter, its own devices
3. one `emu_cpu_ops_t`, declared and listed in `src/emu/emu_cpu.c`
4. `option(EMU_GUEST_ARCH_<ISA> ...)` and a `target_sources` block in
   `CMakeLists.txt`
5. tests in `tests/unit/`, guarded by `EMU_GUEST_ARCH_<ISA>`
6. a `tests/fuzz/fuzz_<isa>_jit.c` target on `fuzz_harness.h`, and a
   guest with a vector table that names its cause

Nothing in `src/emu/` or `src/platform/` should need editing beyond step 3.
That is the property to check when the contract changes: build the firmware
with `-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON` and see that it
links -- and that the frontend does not allocate its own memory map
([docs/memory.md](docs/memory.md)). G4MH's scope is in
[docs/frontend/g4mh.md](docs/frontend/g4mh.md).
