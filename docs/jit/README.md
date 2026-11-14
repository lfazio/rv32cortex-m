# The JIT

Four documents:

- this one — the shared framework, the block model, and what the two
  emitters have in common
- [staleness.md](staleness.md) — what a translated block bakes in, and
  why every translate-time read of mutable guest state is a bug until
  proven otherwise
- [tuning.md](tuning.md) — the knobs, what each one is worth, and which
  workload can see it
- [floating-point.md](floating-point.md) — what goes to the host FPU,
  what goes to SoftFloat, and the NaN and flag rules that decide which

Emitter-specific notes live beside the code they describe:
[backend/thumb2.md](../backend/thumb2.md) and
[backend/x86_64.md](../backend/x86_64.md).

---

## The shape

A frontend never calls its interpreter directly; it goes through
[`emu_backend_t`](../../include/emu/emu_backend.h). `run` is budgeted
rather than free-running so a backend can execute a whole translated
block and report what it retired, and `invalidate` lets a guest's
"I rewrote code" and an image load discard translations.

Translation is a pipeline, not a switch statement per host:

```
frontend->translate()      guest instructions  ->  emu_ir
emu_ir_optimise()          the passes, host-independent
emu_ir_lower()             IR  ->  host code, per backend
```

| layer | owns | files |
|---|---|---|
| frontend | which guest instructions translate, and to what | `src/frontend/<isa>/<isa>_ir.c` |
| IR | the operations and the passes over them | `src/emu/emu_ir.c` |
| backend | how to spell an instruction, and the host ABI | `src/backend/thumb2/`, `src/backend/x86_64/` |
| framework | code buffer, block table, hash, chaining, compaction, dispatch, stats | `src/emu/emu_jit.c` |
| glue | binds a frontend to the host's emitter | `src/emu/emu_ir_jit.c` |

`src/backend/common/interp.c` is an IR *interpreter*, used as the
reference by `-DEMU_JIT_DIFF=ON`.

All four frontends — RV32, G4MH, ARMv7-M and PowerPC — have a translator,
and one macro in `emu_ir_jit.c` defines each one's backend for whichever
host is being built. **That macro is where a hook gets dropped**: it has
lost three at one time or another, `sync` and `after_interp` among them,
each invisible at both ends because the frontend saw its hook assigned
and the framework saw a NULL it is designed to tolerate. When a struct is
filled by a macro, grep the macro for every member of the struct.

The frontend states where its state lives, once, in `bind` —
`emu_jit_hot_t` then holds `pc`, `state`, `generation`, `context`,
`blocked` and `irq_pending` as *pointers*, not callbacks. That is not a
stylistic choice: as callbacks they cost 4.99M of the 7.03M host cycles
the framework added on a Cortex-M7, because the dispatch loop runs once
per block entry and an M7 cannot predict an indirect call.

A block is called with two arguments, the cpu and `emu_jit_block_arg`
-- a pointer the host backend chooses, for state every block needs and
none should pay bytes to materialise. Thumb-2's is the slow path all
its loads and stores share, kept in r11
([../backend/thumb2.md](../backend/thumb2.md#where-the-bytes-go));
x86-64 sets none and ignores it.

## What a frontend decides, per instruction

Three outcomes, and every frontend counts them:

- **lowered** to IR;
- **helper** — the interpreter's own single-instruction core, called
  from inside the block. A helper call is a translation; declining is
  not. Ending a block for an awkward instruction fragments hot code, and
  what is declined costs more than what is translated badly. A helper
  gets the cpu, two operands and, when the instruction's `aux` is
  non-zero, `aux` as a third argument (`emu_ir.h`) -- which is how
  PowerPC's FP helpers learn rd without unpacking anything;
- **declined** — the block ends and the interpreter runs the instruction
  from the dispatcher. Each frontend declines the instructions that
  change what a block may assume (a CSR write, an MSR write, a mode
  change), deliberately: it makes the interpreter fallback the one
  place that state can move, and so the one place the keys below have
  to be re-derived.

`emu_ir_can_lower` lets a frontend ask the host before emitting an
operation, and turn one the host lacks into a helper call. Two things
about it have bitten. It is worth nothing if the backend cannot emit the
fallback — Thumb-2 once declined `HELPER_TRAP` itself, so every "no"
cost the whole block. And **its default is an answer**: x86-64's says
no, and for a while did not name operations it lowers, so a frontend
sent them to the helper with every test passing; Thumb-2's says yes, and
has promised operations it refuses three times, which costs the block --
the last time CLMUL, and RV32 CoreMark under the JIT was 32% slower on
the F746 with every host suite green. So a backend reports each
refusal (`emu_ir_note_refused`), and one the query had promised is
counted: the run's `-- ir --` report prints it as `unkept`, with the
first operation's number in `emu_ir_op_t`, and `board-ab.sh` marks
that side of a row `UNKEPT`.

## Identity, validity and permission

Three different questions about an existing block, and conflating any
two of them has been measured:

| | question | what moves it | on a change |
|---|---|---|---|
| `context` | what is this block *for* | privilege, address space, FP unit on or off, rounding mode, encoding | blocks for every context coexist; the lookup simply finds another |
| `generation` | is anything still valid | PMP configuration, page tables, MPU registers | flush |
| `blocked` | may translated code run at all | Sdtrig | interpret until it clears |

Putting identity in the generation flushes the cache on every trap and
return: 129,293 flushes and 952,308 translations across one Linux boot,
which made the JIT slower than no JIT. The context is 64 bits, compared
exactly, and mixed into the hash. Details and the defects behind each
row are in [staleness.md](staleness.md).

## A budget is a floor, not a ceiling

A block backend may retire **more** than the budget it was given: it can
only stop between blocks. Any caller sizing the next slice as
`max_insn - total` will wrap when `total` passes the cap. Compare
`total >= max_insn`; never a budget that has to reach zero.

This was reachable only by the two riscv-tests that *depend* on the cap
to terminate, and was invisible for the life of the project because the
interpreter lands on the cap exactly.

## Three outcomes of a translation, not one

"Declined", "overflowed" and "cache full" are three different things and
collapsing any two of them is pathological:

- returning NULL for a block that overran the buffer, exactly as for one
  the translator declined, made the interpreter run one instruction and
  the same oversized block be translated and thrown away again — 957
  times in one CoreMark run, with translation reaching 65% of all host
  cycles;
- sharing a recovery path between "nothing translatable here" and "cache
  full" made every interpreted `div` flush the code cache.

A declined pc is **remembered** — keyed on pc and context, invalidated
by bumping an epoch on flush rather than by clearing a table — because
the second answer costs what the first did and is the first: 636.8M
translation attempts against 638.7M interpreted instructions in a Linux
boot, before. An overflow is not remembered; nothing is wrong with that
pc.

They are counted, not reasoned about. The stats line prints translations,
block entries, interpreted instructions, `declined` (and how many were
answered from memory) and `overflow`, and **that line is what proves a
pass means anything** — a backend that declines everything and falls
back passes every suite while proving nothing.

## Chaining, compaction and committed code

A block's exit to a constant target is emitted as a jump to the block's
own *unlinked* return -- its epilogue, with `EMU_JIT_RET_UNLINKED` set
in the count it returns -- and patched to land in the successor once
both exist and the edge has been taken (`EMU_JIT_MAX_CHAIN` exits per
block). **A link is made the second time an exit is followed**: after
translating a block the dispatcher forgets the one it came from, since
translating may have compacted, so the first traversal of an edge
translates and the second links.

**The dispatcher links only after a return that says so**, and then
searches the chain for the exit. It knows the block it called, not the
one that returned: when the first chained into others, the exit taken
may be a later block's, and linking only the called block's exits left
that later exit going through the dispatcher on every pass -- a G4MH
loop cut into three blocks dispatched each of them 92,000 times. So
`link_exit` asks the called block, then the blocks its linked exits
lead to, eight at most. Searched after every return, that cost about
550 host cycles a dispatch on the F746 and doubled RV32's and ARMv7-M's
CoreMark times, because nearly every return is a computed exit or a
chain that met the loop cap, with nothing to link. The flag costs six
bytes a block on Thumb-2 (`ORR.W`, `B` back to the tail) and the search
runs once or twice an edge: `links ... (missed N)` counts the searches
that found nothing. A link is made only within one context, and an
interrupt between the return and the next block forgets the block
(`test_chain_no_link_across_an_interrupt`). A chain never reaches the dispatcher, so each link carries the
same bound a self-loop does — `EMU_JIT_LOOP_CAP` retired instructions —
or nothing would check for a pending interrupt; an instruction that
must reach the dispatcher at once, as G4MH's EI must for what it
unmasks, exits to a target held in a value, which never chains -- and
is not marked a plain jump, so the stub below does not send it on.

### Computed jumps: the stub (`emu_jit_ibtc`)

**A return cannot be chained, and returns were most of the
dispatching.** On the host, the share of block entries that came back
through a computed exit -- a `jalr`, `blr`, `bx lr`, `jmp [lp]` -- was
200,168 of RV32 Dhrystone's 220,672, 201,063 of ARMv7-M's 241,530,
220,169 of PowerPC's 262,958, and 483,056 of G4MH CoreMark's 801,786.

So a translator marks the computed exits that are plain jumps
(`EMU_IR_EXIT_JUMP`: RV32's JALR; G4MH's JARL [reg1], JMP [reg1], JMP
disp32[reg1] and DISPOSE's return; PowerPC's `bclr` and `bcctr`;
ARMv7-M's interworking BX, POP and LDR pc, and TBB/TBH), and a backend
sends a chainable block's marked exit, its frame given back as a chained
exit's is, to a stub of its own: one per core on x86-64, in the memory
stubs' page; beside `t2_mem_stub` in ITCM on Thumb-2, reached as r11
plus a constant. The stub hashes the target into a 64-entry table
(multiply, top six bits), compares the entry's pc with the target and
its context with the core's own -- read from the core's state, as the
dispatcher reads it, so a block that changed its context on the way out
finds nothing -- and the retired count with `EMU_JIT_LOOP_CAP`, as a
chained exit does. On a match it bumps the entry's block's hit count,
saturating, records the block, and jumps to its chain entry; otherwise
it returns as the block's tail would, and marks the table to be filled.

- **Opt in, never out.** Every computed exit meant "back to the
  dispatcher" before this existed, and some still must: G4MH's EI and
  ARMv7-M's ISB leave through one so that the interrupt they let in is
  taken before the next instruction, and ARMv7-M reads the pc back after
  the interpreter ran an instruction it cannot see into. Marked as jumps
  and sent on, EI's interrupt was taken a block late (test_g4mh's "EI
  four times").
- **Filled only after a miss.** The dispatcher fills the entry for the
  block it runs next after the stub's return said it missed. Filled at
  every dispatch, PowerPC CoreMark paid 362,586 fills for 85,851
  computed exits, and on the F746 a build whose PowerPC jumps were not
  even marked ran 6.8% slower (21.26 host cycles an instruction against
  19.91): the fill and the hash were calls out of the dispatcher.
- **Hit counts and the link search.** Compaction keeps blocks by the
  entries counted at lookup, and a block entered only from the stub
  would read as cold; the stub counts its entries too. And an unlinked
  exit in a block the stub entered is found only by a search starting
  there, so the dispatcher starts `link_exit` from the last such block.
- **Emptied wherever a block moves or goes**: a flush, a compaction, a
  retirement, an unchaining -- every empty entry aimed at the stub's own
  return, so even a false match only returns. And filled only for a
  block after the first of a call, so a caller running one block a call
  -- the gdb stub, unchained -- runs one block.

Host, after the change (block entries; the jumps the stub sent on):

| | entries before | after | sent on |
|---|---|---|---|
| RV32 Dhrystone | 220,672 | 70,611 | 190,049 |
| ARMv7-M CoreMark | 318,175 | 209,482 | 116,540 |
| PowerPC CoreMark | 286,144 | 205,029 | 82,645 |
| G4MH CoreMark | 801,783 | 351,543 | 458,907 |

On the F746 against 3ca1c74, host cycles an instruction under the JIT
at two `.text` layouts, the retired counts and CRCs the same:

| | before | after | |
|---|---|---|---|
| RV32 CoreMark x120 | 13.10 / 13.11 | 12.50 / 12.50 | -4.6% |
| ARMv7-M CoreMark x200 | 16.60 / 16.72 | 15.53 / 15.52 | -6.5%, -7.2% |
| PowerPC CoreMark | 20.06 / 20.06 | 19.97 / 19.97 | -0.45% |
| G4MH CoreMark | 22.00 / 22.02 | 20.37 / 20.36 | -7.4%, -7.5% |
| RV32 Dhrystone | 16.21 | 14.78 | -8.8% (entries 220,674 to 70,616) |
| ARMv7-M Dhrystone | 21.65 | 20.32 | -6.1% |
| PowerPC Whetstone | 35.01 | 34.69 | -0.9% |
| RV32 Whetstone | 26.86 | 26.35 | -1.9% |

CoreMark x650 at 32 KB, the run that compacts, 23.5 s and valid
(`crcfinal 0x25e3`), 1,115,988 jumps sent on.

The stats line reports it at the end of `blk entr` (`ibtc N`), and the
fuzzers' summary as "computed jumps sent on". No fuzzer's random words
reached it once filling waited for a miss; a seed in each frontend's
directory (`calls-and-returns`) does, and `returns-sharing-a-slot.S` puts two
return sites in one entry, which is the only way a stub that skipped
the pc compare shows. `isbpend.S` (`guest-armv7m-isbpend-both`) holds
ARMv7-M's ISB to the dispatcher; the fuzzers cannot, since they set
aside every run whose interrupt may land at different places.

An interrupt the dispatcher takes is counted as a retired instruction
only where the frontend's interpreter counts one: RV32's and PowerPC's
do, ARMv7-M's and G4MH's do not (`emu_ir_frontend_t::irq_not_retired`).
It was counted for all four, and ARMv7-M CoreMark retired 1,114 more
under the JIT than interpreted, a SysTick each.

**A block is charged what its interpreter would have charged**: what it
retired, plus what its helpers spent beside that
(`emu_jit_hot_t::charged`), and one when both are nothing. The budget's
units are the run's total -- `emu-result retired=`, the instruction cap,
a guest clock that counts instructions -- and the interpreters charge
more than a block returns in two places. RV32's, PowerPC's and G4MH's
charge an exception one unit, which is how a fault that faults again
still ends a slice; a block that trapped partway returned only the
instructions before the trap. And ARMv7-M's store with an imprecise bus
error retires, counted by the helper that re-runs it after its block
stopped before it. The architectural counts (`retired` in each core)
always agreed, which is why nothing compared these: G4MH's mmiobench,
once its refused stores were stepped over instead of halting, retired
1,344,477 interpreted and 1,344,461 under the JIT, one short for each,
and ARMv7-M's the same 16; RV32 and PowerPC guests stepping over 100
faulting loads 98 and 100 short (`test_block_charged_its_helpers`,
`test_refused_store_stepped_over`). Each frontend bumps its counter
where its helpers enter an exception, or finish an imprecise store;
interrupts are not counted there.

**And a block that retired nothing is charged one** -- the trap moved
the pc, and charging nothing would let a handler that faults at its own
vector spin the loop -- unless the frontend says nothing is safe
(`emu_jit_ops_t::fault_not_charged`). ARMv7-M does: its interpreter
charges a precise fault nothing, and a fault that cannot be taken locks
up. With the floor, a loop whose head is a faulting load retired 804
interpreted and 806 under the JIT, once for each time the dispatcher,
not a chain, entered that block; without it, 785 on both, and the run
that then points VTOR at a HardFault vector no region covers still
ends, in lockup (`guest-armv7m-faulthead-both`). Six minutes of
fuzz-armv7m-jit found nothing it lets spin.

**A translator may stop before a known entry** (`emu_jit_entry_known`):
during a translation, true for a pc a block begins at in the context
being translated, or did until compaction evicted it -- not one given up
on, which cannot be chained to. All four translators ask before every
instruction after the first, and end the block with a constant exit to
it; a third to two thirds of what ARMv7-M's and PowerPC's CoreMarks
translated at 96 KB had been a copy of code already in the buffer, and
both now fit, 42% and 35% faster on the F746. RV32's and G4MH's
CoreMarks fit their 64 KB, where the stop only splits blocks and costs
about 4%; at 32 KB it is worth 19% and 9%, and 45% to RV32's Whetstone
-- one rule, never slower than no stop on any row measured
([backend/thumb2.md](../backend/thumb2.md#what-a-block-shares-with-the-blocks-already-there)).
Counted as `entry stops` on the `blk entr` line.

When the buffer fills, `compact()` keeps the blocks that have been
looked up more than once, slides them down, and drops the rest. **A
block entered through a chain is never looked up**, so it first gives
every block a linked exit leads to at least the count of the block
exiting to it (`EMU_JIT_CHAIN_HEAT`, on): without that the body of a
hot loop read as cold and was evicted while the block jumping into it
survived. Worth 6-7% and 11% of translations on the F746 at 96 KB; see
[tuning.md](tuning.md#retention-and-chained-blocks). Before
anything moves, **every chained exit is pointed back at its own
unlinked return**: a surviving block's jump otherwise lands in whatever
now occupies the bytes its successor had -- and pointed at the plain
tail instead, it would never say it was unlinked and never be linked
again.

That unlinking silently did nothing whenever the compaction was caused
by a translation *overflowing* the buffer — the hooks that rewrite
committed code went through the emitter's patcher, which refuses to
write while the overflow flag is up. It could only happen with a block
larger than the reserve, which means on a microcontroller and never on a
host; RV32 CoreMark at 120 iterations does not finish on the board when
built from the commit before the fix. The
account is in [backend/thumb2.md](../backend/thumb2.md); the rule is
that **a hook on committed code must not consult the emitter's state**,
and `test_overflow_compaction_unlinks` holds the framework to it with a
stub translator.

The hash is chained too, and has to be. The block table is far larger
than the live set, which makes one entry per bucket look adequate; a
collision instead makes the loser unreachable while it still holds its
code and its slot, so two hot blocks retranslate each other every time
round the loop.

## The passes

`emu_ir_optimise` runs, in order: dead flags, register traffic (a `GET`
of a register the block has just written becomes a move), constant
fusion into the immediate forms, **dead stores**, dead values, multiply-
accumulate fusion, **retirement merging**, and a use count the backends
read. They are
host-independent by construction, and they see the guest only through
`emu_ir_target_t` — which matters, because a pass that rewrites the IR
has to honour every guest invariant the lowerings do. `pass_reg_traffic`
once forwarded a value "written" to RISC-V's `x0`; all three consumers
of the IR then faithfully compiled a guest reading its own discarded
result, and the differential checker agreed with all of them.

**A load or store can be predicated**, and the branch round it lives
*inside* the one IR instruction (`emu_ir_mem_if`: the predicate is the
`c` operand). A guest whose conditional instructions include memory
accesses -- ARMv7-M in an IT block -- cannot compute-then-select an
access the way it does everything else under a condition: the access
can fault, and a read can change a device. Making it a branch inside
the instruction means no pass has to know there is control flow in a
block at all; the passes already walk `c` for the multiply-adds, and
the dead-value pass keeps every access whatever its predicate. Clear
means *no access* -- `test_lower_predicated_memory` uses an address the
target refuses, so an access the predicate failed to stop traps --
and a skipped load's result is unspecified, for the frontend to select
against the old value.

**An operation is removable only if it is on a list of pure ones.**
The dead-value pass deletes anything whose result nothing reads, and it
used to decide that from a list of operations *with* an effect --
everything else could go. `LOAD` was not on it. A load whose
destination was overwritten later in the same block, with no load,
store, helper or exit in between, was therefore deleted: it did not
fault, and on a device it did not *read* -- where reading is how an
interrupt is acknowledged or a flag cleared, and `(void)UART->DR;
return 0;` is how a driver says so. Under a JIT built so a guest can
drive real peripherals.

Nothing in the tree had that shape: ten guests emit byte-identical code
with loads removable and with loads kept, so the optimisation had never
once fired on one and the fix costs nothing. It was found by an
architecture test from a suite nobody had named
(`pmpsm_cfg_A_tor_zero`: `lw a4, 0(zero)`, then `a4` loaded with an
address), which passed interpreted and failed translated. The list is
of the pure operations now, so one nobody has considered is kept, and
`test_only_pure_ops_are_removable` offers every operation in the
enumeration unread. The floating-point operations that raise flags are
kept for the same reason with a different effect.

**The pc is a register, and `SETPC` is the store that writes it.**
Frontends emit one after every guest instruction, because the pc must be
right wherever a fault can be taken and saying so every time is the
version that cannot be got wrong. Measured by emitted bytes per IR
operation, that was 19.3% of RV32 CoreMark's code and 13% of PowerPC's.
The dead-store pass takes it now, with the observer list it already had
— a load, a store, the memory bit operations and both helper calls can
fault or read the pc — and three differences from a register, each with
a test that fails without it:

- `EXIT` writes the pc itself, so it is an overwrite, not only an
  observation;
- `EXIT_IF` is neither: taken it writes the pc, not taken it falls
  through, so the question passes through it unchanged;
- a guest register can *be* the pc (ARMv7-M's r15), so a `GET` of the
  register whose slot is `pc_offset` is an observer.

| | emitted before | after | |
|---|---|---|---|
| RV32 CoreMark, x86-64 | 202,724 bytes | 176,584 | −12.9% |
| PowerPC CoreMark, x86-64 | 405,008 | 364,352 | −10.0% |
| RV32 CoreMark on the F746, 2 iterations | 501,578 ticks | 425,239 | −15.2% |
| RV32 `bench` on the F746 | 413.3 cycles/insn | 199.5 | 2.07× |
| PowerPC CoreMark on the F746 | 472.1 cycles/insn | 393.4 | −16.7% |

`bench` gains far more than the bytes saved because it sits at the edge
of what a 32 KB cache holds: translations fell from 3,563 to 1,873.

**`RETIRE` is the guest's instruction count, and the same observers
read it.** One per guest instruction, as with `SETPC`, and each an add
on the host. The count is read only where the block can be left -- a
trap out of a load or a helper, an exit, the loop cap a back edge tests
-- so `pass_merge_retire` keeps one per stretch between observation
points with `imm` the number it absorbed (0 meaning 1, which is what
every frontend still emits). Which RETIRE of a stretch carries the sum
does not matter, since nothing between them reads it; carrying one
*across* an observer would, and `test_retire_not_merged_across_an_observer`
has a case per observer. The observer list is one predicate now,
`op_observes_state`, shared with the dead-store pass -- two copies of
it were two lists to drift apart.

On the F746 it was 11% of emitted code once the accesses had shrunk:
3,291 adds became 1,429 on CoreMark, 36.0 → 33.9 bytes a guest
instruction, x650 14.29 → 13.94 cycles an instruction at 96 KB and
117.63 → 115.24 at 32 KB. Retired counts under the JIT are what they
were, to the instruction.

**Which suite could see the rule broken was worth a run of its own.**
With the observers no longer keeping a `SETPC` alive, riscv-tests still
pass 77/77 under the JIT. `isatest` fails, the unit tests fail — and the
PowerPC system guest *passed*, because every fault it took was the first
instruction of a block, where the pc is right by construction. It takes
them two instructions in now.

## Floating point

One implementation, SoftFloat, reached from the interpreter and from the
JIT alike — with one bounded exception that had to earn it: the five
operations IEEE 754 specifies exactly are lowered to the host FPU, with
the NaN canonicalisation and the flag hand-over written out. Policy, the
table of what each backend answers, and the measurements are in
[floating-point.md](floating-point.md). Only the RV32 translator emits
the FP class; G4MH, ARMv7-M and PowerPC send every FP instruction to
their helpers.

## Reading a coverage number

Read `interp` against the retired count before believing a passing
suite. `isatest` under the JIT interprets 2,710 of 96,995 instructions
(2026-10-25), and retires the same count on x86-64, the F746 and the
F446.

It used to interpret a third of them on the board, because it arms PMP
early and everything after that went to the interpreter — which is why
three consecutive changes each produced a board run whose counters were
identical to the digit to the run before. The changes were fine; the
guest could not see them. **Identical counters are not "no regression",
they are "the code never ran"**, and before believing a null result,
check that the instrument can represent the difference.
