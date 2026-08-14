# What a translated block bakes in

A translated block records decisions taken from hart state at the moment
it was built, and then outlives that state. Every such read is a
staleness bug until proven otherwise, and this page is the sweep.

The rule the two backends follow: **specialise, record what was baked in,
and flush when it moves** -- watching the flag that *enabled* a decision
is not the same as watching the decision. Re-derive on the interpreter
fallback rather than per dispatch; CoreMark enters blocks 2.9M times a
run.

## What a block bakes in

A translated block records decisions taken from hart state at the moment it
was built, and then outlives that state. Every such read was swept:

| read at translation | outcome |
|---|---|
| `fcsr` frm, for `rm=dyn` | wrong for `RMM` — fixed |
| `mstatus.FS` | unchecked for OP-FP, stale for loads and stores — fixed |
| `pmp_active` and the `rv_pmp_simple` bounds | **permission bypass** — fixed |
| bus regions | safe: written only at init, before execution |
| the peripheral window's armed flag | safe: flushes when it changes |
| the guest instruction bytes themselves | safe: `FENCE.I` invalidates |

The PMP one was the worst. What a block bakes in is the *bounds* of the single
enabled entry, but the flush compared `pmp_active` — a boolean. Locking a
second entry leaves that flag true while the one-entry assumption stops
holding, so a store to the newly protected region kept taking the inlined
path. The self-test's `pmp2-write-blocked` reported `0xdeadbeef` where the
guest had denied writes: not a slow path taken by mistake, an access that
should have faulted and did not.

All three defects were invisible to `riscv-tests` and `riscv-arch-test`,
because neither runs the JIT, and each needed a hardware run with the fix
reverted to demonstrate.

frm, FS and PMP share a fix and a hook. Each changes only through a CSR write,
and the translator declines `SYSTEM`, so `jit_note_csr` on the interpreter
fallback is the one place any of them can move — which also keeps all three
off the dispatch path, where CoreMark would pay for them 2.9 million times a
run.

## `mstatus.FS` and cached blocks

FS gates the whole extension: with it Off every FP instruction must raise
illegal-instruction, `fmv` included. The JIT decided that when it *translated*
a block, which is half a guard — a block built while FS was on stayed in the
cache and kept running after the guest turned the FPU off. A targeted test
confirmed it: three instructions that had to trap produced no traps at all.
OP-FP and the fused multiply-adds were not consulting FS in the first place.

Fixed the way `frm` is: FS off-ness is recorded when a block is built, and a
change flushes. Both checks sit on the interpreter fallback, because a CSR
write to `mstatus` is the only thing that reaches Off — trap entry and `mret`
do not touch FS, and `emit_fp_dirty` only ever moves it away from Off. It is
*off-ness* that is tracked rather than the two-bit field, since `emit_fp_dirty`
moves Initial or Clean to Dirty on most operations and flushing for that would
discard the cache continuously. A guest with no FP never flushes for either:
CoreMark still reports one flush for its whole run.

**Still declined**, so the block ends there:

- Anything rounding **`RMM`** — ARM has no ties-away mode. Blocks are
  specialised on `frm`, so an `rm=dyn` instruction is resolved at translation
  and declined when it lands on `RMM`, just as a static `rmm` is.

## Cache-block operations

`Zicbom`/`Zicboz` map directly onto ARMv7-M cache maintenance, because both
architectures define the same operations over a block identified by an address:

| RISC-V | ARMv7-M (CMSIS) |
|---|---|
| `cbo.clean` | `SCB_CleanDCache_by_Addr` |
| `cbo.inval` | `SCB_InvalidateDCache_by_Addr` |
| `cbo.flush` | `SCB_CleanInvalidateDCache_by_Addr` |
| `cbo.zero` | stores zeros (no ARM equivalent) |

The guest address is translated to the host address that actually backs it
before the maintenance call, so a guest cleaning a DMA buffer cleans the very
ARM cache lines holding it. On the Cortex-M4 in the STM32F446 there is no data
cache and the maintenance operations are no-ops, which the spec permits; the
code is written against `__DCACHE_PRESENT` so it becomes real cache maintenance
when built for a Cortex-M7.

---
