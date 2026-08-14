# The JIT

Three documents:

- this one — the shared framework, the block model, and what the two
  emitters have in common
- [staleness.md](staleness.md) — what a translated block bakes in, and
  why every translate-time read of mutable hart state is a bug until
  proven otherwise
- [tuning.md](tuning.md) — the knobs, what each one is worth, and which
  workload can see it

Emitter-specific notes live beside the code they describe:
[backend/thumb2.md](../backend/thumb2.md) and
[backend/x86_64.md](../backend/x86_64.md).

---

## The shape

A frontend never calls its interpreter directly; it goes through
[`emu_backend_t`](../../include/emu/emu_backend.h). `run` is budgeted
rather than free-running so a backend can execute a whole translated
block and report what it retired, and `invalidate` lets `FENCE.I` and
image loads discard translations.

Translation is a pipeline, not a switch statement per host:

```
frontend->translate()      guest instructions  ->  emu_ir
emu_ir_optimise()          four passes over the IR
emu_ir_lower()             IR  ->  host code, per backend
```

`src/emu/emu_ir.c` owns the IR and its passes; `src/emu/emu_ir_jit.c` is
the framework that binds a frontend to a host emitter and defines both
frontends' JIT backends from one macro. The emitters are
`src/backend/thumb2/` and `src/backend/x86_64/`, and
`src/backend/common/interp.c` is an IR interpreter used as a reference.

The frontend states where its state lives, once, in `bind` —
`emu_jit_hot_t` then holds `pc`, `state`, `generation` and `blocked` as
*pointers*, not callbacks. That is not a stylistic choice: as
`emu_jit_ops_t` callbacks they cost 4.99M of the 7.03M host cycles the
framework added on a Cortex-M7, because the dispatch loop runs once per
block entry and an M7 cannot predict an indirect call.

## A budget is a floor, not a ceiling

A block backend may retire **more** than the budget it was given: it can
only stop between blocks. Any caller sizing the next slice as
`max_insn - total` will wrap when `total` passes the cap. Compare
`total >= max_insn`; never a budget that has to reach zero.

This was reachable only by the two riscv-tests that *depend* on the cap
to terminate, and was invisible for the life of the project because the
interpreter lands on the cap exactly.

## Three outcomes, not one

"Declined", "overflowed" and "cache full" are three different things and
collapsing any two of them is pathological:

- returning NULL for a block that overran the buffer, exactly as for one
  the translator declined, made the interpreter run one instruction and
  the same oversized block be translated and thrown away again — 957
  times in one CoreMark run, with translation reaching 65% of all host
  cycles;
- sharing a recovery path between "nothing translatable here" and "cache
  full" made every interpreted `div` flush the code cache.

They are counted, not reasoned about. The host runner prints
`xlat/entries/interp/declined/overflowed`, and **that line is what proves
a pass means anything** — a backend that declines everything and falls
back passes every suite while proving nothing.

## The hash chain

The block table is far larger than the live set, which makes one entry
per bucket look adequate. It is not: a collision makes the loser
unreachable while it still holds its code and its slot, so two hot blocks
retranslate each other every time round the loop. Keep the chain.

## Floating point goes to the helper

**There is one FP implementation and both backends reach it.** Everything
that rounds, classifies or reports a flag goes to `rv_hart_fp`, from the
interpreter and from the JIT alike. Only `FMV.X.W` and `FMV.W.X` are
lowered, because they move bits and cannot round.

This replaced an arrangement where the JIT emitted host FP instructions,
which was a *second* implementation of semantics the core already owns,
and the two disagreed exactly where the architecture is fussiest — NaN
propagation, subnormals, and which of `fflags` an operation may raise.
`rv32i/F` was interpreter 78/78, JIT 55/78, same binary. Routing the
arithmetic to the helper made it 78/78 both ways, and the JIT is still
ahead of the interpreter on FP work (`fptest` ×5: 38 ms against 54 ms),
because the block stays whole and only the arithmetic becomes a call.

To make it faster, bring operations back **one at a time, each measured
against the F suite** — not the whole table on the argument that the host
has an FPU.

A helper call is a translation; declining is not. `FMIN`/`FMAX` and
`FCLASS` have no ARMv7-M equivalent, but routing them to `rv_hart_fp`
keeps the block whole and is worth 24 interpreted instructions and 23
dispatches on the self-test. Open-coding them costs 25–35 emitted
instructions each in the code cache, which sets performance more than the
translator does.

The corollary bites when a backend cannot emit the fallback:
`emu_ir_can_lower` lets a frontend turn an operation the host lacks into
a helper call rather than a declined block, and is worth nothing if
`EMU_IR_HELPER_TRAP` is itself declined. It presented as a *perfect* null
result — a board run identical to the previous one to the digit, tests
still passing — while the same commit moved an F architecture test from
452 interpreted to 275 on the host. **Identical counters are not "no
regression", they are "the code never ran".**

## Reading a coverage number

`isatest` cannot measure JIT coverage: it arms PMP early, and
`rv_jit_bind` points the framework's `blocked` at `h->fetch_guard`, so
everything downstream goes to the interpreter whatever the backend could
have lowered. Measured on the board, same firmware, one guest apart:

| guest | interpreted | of | share |
|---|---|---|---|
| `isatest` | 14,700 | 45,399 | 32% |
| `bench` | 40,521 | 1,274,518 | **3.2%** |

Use `bench` or `coremark` to measure translation. Read `isatest` only as
a correctness check.
