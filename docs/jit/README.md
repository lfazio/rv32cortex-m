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

## What a frontend decides, per instruction

Three outcomes, and every frontend counts them:

- **lowered** to IR;
- **helper** — the interpreter's own single-instruction core, called
  from inside the block. A helper call is a translation; declining is
  not. Ending a block for an awkward instruction fragments hot code, and
  what is declined costs more than what is translated badly;
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
for a while promised operations it refuses, which costs the block.

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
own tail, and patched to land in the successor once both exist and the
edge has been taken (`EMU_JIT_MAX_CHAIN` exits per block). A chain never
reaches the dispatcher, so each link carries the same bound a self-loop
does — `EMU_JIT_LOOP_CAP` retired instructions — or nothing would check
for a pending interrupt.

When the buffer fills, `compact()` keeps the blocks that have been
looked up more than once, slides them down, and drops the rest. Before
anything moves, **every chained exit is pointed back at its own tail**:
a surviving block's jump otherwise lands in whatever now occupies the
bytes its successor had.

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
accumulate fusion, and a use count the backends read. They are
host-independent by construction, and they see the guest only through
`emu_ir_target_t` — which matters, because a pass that rewrites the IR
has to honour every guest invariant the lowerings do. `pass_reg_traffic`
once forwarded a value "written" to RISC-V's `x0`; all three consumers
of the IR then faithfully compiled a guest reading its own discarded
result, and the differential checker agreed with all of them.

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
suite. `isatest` under the JIT interprets 829 of 54,087 instructions, on
x86-64 and on the F746 alike.

It used to interpret a third of them on the board, because it arms PMP
early and everything after that went to the interpreter — which is why
three consecutive changes each produced a board run whose counters were
identical to the digit to the run before. The changes were fine; the
guest could not see them. **Identical counters are not "no regression",
they are "the code never ran"**, and before believing a null result,
check that the instrument can represent the difference.
