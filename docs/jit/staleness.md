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
| the page mappings | generation (`vm_gen`), and per page through the framework's page index | watching `satp` alone would miss an edited PTE; a page fence retired one piece of a megapage and left the others' blocks runnable |
| the PMP configuration | generation (`pmp_gen`) | **permission bypass**: the flush watched a flag, not the configuration |
| whether accesses may be inlined | follows from the four above and both generations | an inlined store that ignored PMP |
| the privilege loads and stores are made at | context, bits 37:36 -- it names the data TLB table a block probes, and MPRV moves it apart from the privilege | latent: an M-mode block that inlined the window, entered again under MPRV |
| what a page allows, below M | not in the block at all: the data TLB, which the frontend clears itself | see [The data TLB](#the-data-tlb) |
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

## The data TLB

Below M every load and store was a helper call -- about 190 host
instructions on sv32bench, `rv_pmp_check` alone a third of the run --
because the window is legal only while nothing can deny an access.
`src/frontend/rv32/rv_dtlb.c` caches, per page and per privilege, the
answer to "is an aligned access here a plain host access", and both
backends probe it inline; the interpreter's guarded path asks it too.

A block bakes in only *which* table, so this is the one place in the
JIT where staleness is the frontend's to prevent rather than the
block's. An entry answers three questions at once and is filled only
when all three are the same for the whole page:

| depends on | how it is kept right | the check that fails without it |
|---|---|---|
| the mapping | `rv_mmu_flush_page` and `rv_mmu_flush_tlb` clear it with the TLB it was built from | `dtlb-sfence-page`, `dtlb-asid1` |
| a megapage's other pieces | a fence naming an address in a region marked as translated through a megapage is a global flush, blocks included | `mega-sfence-load-traps`, `mega-sfence-fetch-traps` |
| the PTE's own bits, A and D included | the same: a changed PTE needs an SFENCE.VMA to be seen | `sv32-nod-store-traps` (write without D) |
| SUM | nothing to clear: a U page is never cached for S | `dtlb-nosum-traps` |
| MXR | nothing to clear: a page without R is never cached | `dtlb-nomxr-traps` |
| the privilege | a table each, U and S; the block's context names one | `dtlb-u-after-s`, `dtlb-s-after-u`, `dtlb-mprv-u` |
| the PMP configuration | every `pmpcfg` and `pmpaddr` write clears both tables | `dtlb-pmpcfg-traps`, `dtlb-pmpaddr-traps` |
| PMP over part of the page | not filled: the lowest entry touching the page must hold all of it | `dtlb-pmp-part-traps` |
| the region | RAM for stores, RAM or ROM for loads, the whole page, every width | `dtlb-rom-store-traps` |
| an LR/SC reservation | stores probe only while none is outstanding | `dtlb-sc-broken` |
| alignment | an entry vouches for aligned accesses only | `dtlb-split-value` |

**A megapage is cached a 4 KiB page at a time**, by the TLB, the data
TLB and the translated blocks alike, and an SFENCE.VMA naming an
address has to reach every entry holding that leaf -- all of its
pieces. Nothing records which pieces came from one megapage, so the
walk marks the 4 MiB region (`mega_regions`, a bit per VPN[1]) and a
page fence in a marked region is a global flush. The coarse answer
was measured before it was kept: on the initramfs boot the JIT flushes
were 3,026 with it and 3,026 without, the data TLB's 82 and 82, and
Yocto reached its prompt in 47.4 s against 48.0 -- Linux changes its
megapage leaves by range and by global flush, so no page fence in
either boot named a marked region. `mega-sfence-*` in isatest takes a
megapage away and fences a different piece of it; without the region
test both the load and the fetch succeed, and with the TLBs cleared
but not the blocks only the fetch does, under `--jit` alone.

**SUM and MXR are the design decision.** Linux sets and clears SUM
around every copy to or from user memory, so an entry that depended on
it would have to be flushed thousands of times a second. Refusing to
cache what they decide keeps those copies on the slow path instead --
the kernel's copies, not its own data -- and leaves mstatus writes with
nothing to invalidate.

Every row was confirmed by breaking it: sixteen mutants, each failing a
check that names its rule, on the backend that can see it (the inline
alignment test, the store guard, the store-tag offset and the context
bits fail only under `--jit`). **Three survived the first version of
the test, and the fill was right each time**: the reservation case used
a global in a page holding a locked PMP entry, and the ROM case a
constant in the image's last page, which the ROM window covers only in
part. The fill refused both pages, correctly and silently, so the
checks ran the slow path whatever the probe did. A test of a cache has
to use an entry the cache will hold, and `-- dtlb -- fills` in the
stats line is what says it did.

frm, FS, the PMP configuration and satp share a property that makes the
whole scheme cheap: each changes only through a CSR write, and the
translator declines `SYSTEM`, so the interpreter fallback is the one
place any of them can move. That is a designed invariant, not an
omission — the single exception is *reads* of `time`, lowered because a
Linux boot executed `rdtime` 335 million times.

### The other frontends

| frontend | context | generation |
|---|---|---|
| G4MH | PSW.UM | any MPU register write (`jit_gen`) — both added when the answer to "can a cached block outlive the MPU's execute permission" turned out to be yes: the translator checked nothing, so code the MPU refuses to user mode ran from a block built in supervisor mode, or before the area lost its execute bit. The fetch is now checked per instruction at translation, in the mode the block is keyed on |
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
