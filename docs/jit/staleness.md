# What a translated block bakes in

A translated block records decisions taken from guest state at the
moment it was built, and then outlives that state. Every such read is a
staleness bug until proven otherwise, and this page is the sweep.

The rule every translator here follows: **specialise, record what was
baked in, and make the block unreachable when it moves** — watching the
flag that *enabled* a decision is not the same as watching the decision.
Re-derive on the interpreter fallback rather than per dispatch; CoreMark
enters blocks 2.9M times a run.

There are two ways to make a block unreachable, and choosing the wrong
one is its own defect:

- **the context** — part of the block's identity. A block is only found
  under the context it was built for; blocks for every context coexist,
  and a change costs nothing.
- **the generation** — a change flushes the cache. For state whose
  change makes existing blocks *wrong for everyone*, not merely not
  applicable now.

## The sweep, for RV32

| read at translation | where it lives now | what went wrong first |
|---|---|---|
| privilege | context | a block built for M-mode found, by address, from U-mode |
| `satp` | context | — |
| `mstatus.FS` off-ness | context | unchecked for OP-FP, stale for loads and stores; then in the generation, flushing twice per Linux system call |
| `fcsr` frm, for `rm=dyn` | context | `RMM` silently rounded to nearest |
| the page mappings | generation (`vm_gen`) | watching `satp` alone would miss an edited PTE |
| the PMP configuration | generation (`pmp_gen`) | **permission bypass**: the flush watched a flag, not the configuration |
| whether accesses may be inlined | follows from the four above and both generations | an inlined store that ignored PMP |
| Sdtrig | `blocked`: nothing translated runs while a trigger is armed | — |
| bus regions | safe: fixed before execution | — |
| the guest instruction bytes | safe: `FENCE.I` invalidates, and a store into a translated page retires its blocks | — |

`rv_jit_ctx_key` builds the context — 64 bits, because satp and the
privilege fill 32 — and `rv_ir_gen_key` the generation, which is the sum
of two monotonic counters since only their changing matters. Both are
re-derived in `rv_jit_after_interp`; the context is also refreshed by
`rv_hart_trap`, because a trap is not always an instruction the
interpreter ran: a load faulting *inside* a block enters the handler
from translated code.

The PMP one was the worst. What a block bakes in is the *bounds* it was
checked against, but the flush compared `pmp_active` — a boolean.
Locking a second entry leaves that flag true while the assumption it
encodes stops holding, so a store to the newly protected region kept
taking the inlined path. The self-test's `pmp2-write-blocked` reported
`0xdeadbeef` where the guest had denied writes: not a slow path taken by
mistake, an access that should have faulted and did not. The counter is
on the CSR writes that alter an entry, and deliberately not on
`rv_pmp_refresh`, which also runs on every privilege change.

Every one of these was invisible to `riscv-tests` and `riscv-arch-test`
at the time, because neither then ran the JIT, and each needed a
hardware run with the fix reverted to demonstrate.

frm, FS, the PMP configuration and satp share a property that makes the
whole scheme cheap: each changes only through a CSR write, and the
translator declines `SYSTEM`, so the interpreter fallback is the one
place any of them can move. That is a designed invariant, not an
omission — the single exception is *reads* of `time`, lowered because a
Linux boot executed `rdtime` 335 million times.

### The other frontends

| frontend | context | generation |
|---|---|---|
| G4MH | **none bound** | **none bound** — whether the MPU's execute permission or a change of PSW.UM can leave a cached block wrong has not been examined; see [../TODO.md](../TODO.md) |
| ARMv7-M | Thumb, inside IT, Handler mode, privileged | any MPU register write; CCR's UNALIGN_TRP and DIV_0_TRP |
| PowerPC | the encoding (VLE or Book E) | none: whatever changes MSR or an SPR is declined, so nothing a block assumes can move under it |

## `mstatus.FS` and cached blocks

FS gates the whole extension: with it Off every FP instruction must raise
illegal-instruction, `fmv` included. The JIT decided that when it *translated*
a block, which is half a guard — a block built while FS was on stayed in the
cache and kept running after the guest turned the FPU off. A targeted test
confirmed it: three instructions that had to trap produced no traps at all.
OP-FP and the fused multiply-adds were not consulting FS in the first place.

Fixed the way `frm` is: FS off-ness is recorded when a block is built, and a
block is never entered under a different one. Both are re-derived on the
interpreter fallback, because a CSR write to `mstatus` is the only thing that
reaches Off — an FP operation only ever moves it away from Off. It is
*off-ness* that is tracked rather than the two-bit field, since most FP
operations move Initial or Clean to Dirty as a side effect.

**"Recorded" means part of the block's identity, not of its generation, and
the first version got that wrong.** FS off-ness and frm were in
`rv_ir_gen_key`, so a change *flushed the whole cache*. The argument that this
was cheap was that "trap entry and `mret` do not touch FS" — true of the
hardware and false of the guest: Linux clears `sstatus.FS` in its trap handler
on every entry to the kernel and restores it before `sret`, so every system
call and interrupt taken from a process that had used a float flipped FS off
and back on, and each flip flushed. Booting a root filesystem, 21,949 of
22,126 flushes were FS; the cache held a median of 438 blocks against ~7,000
while the kernel booted; boot did not reach a login prompt in fifteen minutes. Both now live in the high
word of `rv_jit_ctx_key` — the context had to widen to 64 bits for it, because
satp and the privilege fill 32 — and an FS flip costs nothing.
`test_jit_generation_key` asserts both halves: off-ness changes the context,
and changes the generation not at all.

**A rounding mode the host cannot do goes to the helper**, and the block
stays whole. Neither host has ties-away (`RMM`), and the natively lowered
arithmetic is round-to-nearest only. Blocks are specialised on `frm`
through the context, so an `rm=dyn` instruction is resolved at
translation and routed exactly as a static mode would be — see
[floating-point.md](floating-point.md).

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
