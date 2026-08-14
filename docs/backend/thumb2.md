# The Thumb-2 backend

`src/backend/thumb2/` — ARMv7E-M, which is the whole point of the
project: the emulator's own host is a Cortex-M.

**No host suite exercises it.** Validate by flashing `isatest` and reading
the UART. This has caught real JIT bugs, including an inlined store that
skipped the LR/SC reservation break, a `CMP` that assembled as a
different instruction, and a shift by zero that assembled as a shift by
32. Every one of them was invisible on x86-64.

---

## The guest register file stays in memory

`hart->x` is at offset 0, so every access is a single 16-bit
`LDR/STR Rt,[r4,#n]`. That sounds wasteful, but it means guest state is
coherent at every instruction boundary — a trap, an interrupt or a
debugger read needs no unwinding.

A **register cache** in `r8`–`r10` was built and measured **15.5%
slower**. Reads per block said it should win; it did not, because a
cached read is `MOV` where an uncached one is `LDR` — one instruction
either way — while write-through adds an instruction per write and three
more registers hit every PUSH/POP. Do not retry without a cost model, not
just a frequency count.

The **register allocator** is a different thing and does pay, but only
when the value is used *in place*. The first version routed allocated
temps through `ld_operand`/`st_slot` exactly as x86-64 does and measured
worse: 6040 translations against 5943, 894 compactions against 828, and
410 buffer overflows where there had been 309. On x86-64 that
substitution turns an eight-byte `[rsp + disp32]` into a three-byte
`mov`; on Thumb-2 `LDR.W` and `MOV.W` are four bytes each and it is pure
cost. Computing *into* the allocated register is what pays, and pays more
than on x86-64, because Thumb-2 is three-address: an ADD with both
operands and its destination allocated is **one** instruction where the
frame needs four. Same allocator, same block: 4596 translations, 522
compactions, 788,181 ticks.

## Encoding hazards

An encoder whose wrong answers are other valid instructions needs its
**boundary** values tested, not its typical ones. Three defects, all at
an end of a range, none of which computed a wrong answer that any test
was looking at:

- **A register that does not fit assembles as a different instruction.**
  The 16-bit `CMP`/data-processing form encodes r0–r7;
  `emit_dp_reg(DP_CMP, R8, R1)` set a bit belonging to `rm` and became
  `CMP r0, r1`, so `RV_JIT_LOOP_CAP` never applied and chained loops ran
  unbounded — 3700 guest instructions per block entry where 64 was
  intended. It looked like extra throughput and was the interrupt-latency
  bound being thrown away. Use `emit_cmp_hi` for r8 and above.
- **`imm5 == 0` does not mean "no shift", and only `LSL` reads it that
  way.** `LSR #0` **is** `LSR #32`, `ASR #0` is `ASR #32`, `ROR #0` is
  `RRX`. RISC-V spells a move as `srli rd, rs, 0`, which assembled as a
  shift by 32. Rewriting the type to `LSL` is the fix; *skipping* the
  emit is not, because `rd` and `rm` differ and the move still has to
  happen. Invisible to x86-64, whose `shr r32, 0` is a genuine no-op.
- **Branch range is a silent cliff.** Loop chaining was emitted only when
  the back edge fitted the 16-bit conditional branch (±254 bytes); a
  larger block stopped chaining rather than widening the encoding,
  costing 2.4× on the loops that crossed the line. `emit_bcond_back`
  picks the encoding by reach.

## Calling convention

Cortex-M selects instruction set with the low bit of a branch target, so
a block address needs bit 0 set before it is called. That belongs in the
**framework**, which knows the host, not in the frontend, which knows the
guest. Getting it wrong faults on the first block entry rather than
computing anything wrong: the banner prints and nothing else.

## Caches

The JIT writes instructions as data and branches to them. On a Cortex-M7
that needs a real clean-to-PoU and I-cache invalidate by address, not the
DSB/ISB that sufficed with no caches — getting it wrong executes
arbitrary bytes rather than producing a wrong answer.
`RV_ARM_HAS_CACHES` is set by the **platform**, because nothing in the
compiler flags distinguishes the parts: `-mcpu=cortex-m4` and
`-mcpu=cortex-m7` both define `__ARM_ARCH_7EM__`.

## What `may_run` may gate on

This backend implements PMP and paging — it checks fetch permission per
halfword while translating, walks the page tables, and snapshots the PMP
configuration, `satp` and `vm_gen` — so it must gate on `trig_active`
alone. Gating on `fetch_guard`, copied from the x86-64 backend which
implements neither, costs correctness nothing and coverage everything:
`isatest` arms PMP early, so the whole rest of the run interpreted
(`interp 14660` against 341).

## Inlined fast paths

**JIT fast paths bypass the C helpers and their side effects.** The
inlined store had to drop the LR/SC reservation by hand, and had to be
abandoned once PMP is active, because it writes guest RAM without
checking. When adding anything that `rv_hart_load`/`rv_hart_store` does
beyond the access itself, ask what the inlined path does about it.

Inlining the peripheral window is worth 2.2–3.1× to drivers and −53% to
compute, so **the guest arms it**: always-on grew CoreMark's image past
the code cache and doubled evictions. `pt_note` counts passthrough
accesses through the helper and flushes once at `RV_JIT_PT_ARM_AT`.
Flushing from inside a helper is safe — the running block stays intact
and only the next translation reuses its memory.

## Measured cost of the shared framework

Porting onto the shared IR framework cost 4× at first, in four separate
ways, while every suite passed throughout: CoreMark went 79,502 ticks to
321,035 with `isatest` still 296/296. The causes, in order of size, were
the `fetch_guard` gate above, the collapsed overflow/declined outcomes,
the direct-mapped hash, and six indirect calls per dispatch. Result after
fixing all four: 95,107 ticks, ~16–20% above the pre-framework backend,
which is the price of the shared dispatch loop and is real.

Inlining the emitters was **neutral** (LTO already did it). Do not
re-try it.
