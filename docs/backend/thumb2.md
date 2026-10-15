# The Thumb-2 backend

`src/backend/thumb2/` — ARMv7E-M, which is the whole point of the
project: the emulator's own host is a Cortex-M.

It is a lowering of the shared IR and nothing else. `encode.c` spells
instructions, `ir_lower.c` turns one IR block into one host block, and
neither knows what guest it is working for: everything guest-specific
arrives through `emu_ir_target_t`. All four frontends reach it through
the one macro in `src/emu/emu_ir_jit.c`.

| frontend | run on a board under this backend |
|---|---|
| RV32 | yes — `isatest` (748 checks, every B instruction among them), CoreMark, `bench`, on the Nucleo-F746ZG |
| PowerPC e200z7 | yes — `alu`, `sys`, CoreMark, crypto and 600 generated cases, on the F746 |
| G4MH | yes — its small guest on the F746, the board and the host agreeing to the digit; three-PE `smp.bin` on the N6, its line exactly the model's |
| ARMv7-M | yes — all eight guests, both backends, on the F746, each with the host's retired count ([../frontend/armv7m.md](../frontend/armv7m.md#firmware)) |

---

## How it is checked

**No host suite exercises the lowering.** It compiles only for ARM, so
the architecture suites and `ctest` say nothing about it. Validate by
flashing a guest and reading the UART. That has caught real defects that
every host suite passed: an inlined store that skipped the LR/SC
reservation break, a `CMP` that assembled as a different instruction, a
shift by zero that assembled as a shift by 32, and a compaction that
moved blocks with their chained jumps intact (below).

The *encoders* are a different matter, and are checked on a host twice:

- `scripts/t2-check-encodings.sh` compiles `encode.c` for the host —
  it is plain C that writes bytes — and compares what it emits with
  `arm-none-eabi-as`. `scripts/check-thumb2-vfp.sh` does the same for
  the FP forms.
- `tests/guest/armv7m/t2exec.c` compiles the same encoder into a
  Cortex-M guest, emits instructions into guest RAM and **branches to
  them**, on the ARMv7-M frontend. That frontend exists because of this.

Neither reaches `ir_lower.c`'s decisions — which registers, which
frame slot, which branch gets patched.

**`emu_ir_can_lower` answers yes by default, and every yes is a
promise.** It has been broken twice, both times found when a frontend
started asking: the rotations and the four value bit operations, then
the memory bit operations (`EMU_IR_BITOP_*`), which `lower_one` has never
lowered. G4MH emitted those anyway and lost each block containing one at
emission; it asks now, and a no sends each to a helper that keeps the
block whole.

## Registers and the frame

| | |
|---|---|
| `r4` | the `emu_cpu_t` pointer, callee-saved so it survives a helper |
| `r5` | the retired count the block returns |
| `sp` | the temp frame: every IR value has a home in it |
| `r0`–`r3`, `r12` | scratch, and the AAPCS argument registers |
| `r6`–`r11` | handed out by `emu_ir_regalloc`, on top of the frame |

**The guest register file stays in memory.** A guest register is one
`LDR.W`/`STR.W` off `r4`, and guest state is coherent at every point a
fault can be taken — a trap, an interrupt or a debugger read needs no
unwinding. Data processing and memory access use the 32-bit forms
throughout: allocated registers are frequently above `r7`, and a 16-bit
encoding handed one does not fail.

A **register cache** in `r8`–`r10` was built and measured **15.5%
slower**. Reads per block said it should win; it did not, because a
cached read is `MOV` where an uncached one is `LDR` — one instruction
either way — while write-through adds an instruction per write and three
more registers hit every PUSH/POP. The histogram that would have
predicted this exists now (`EMU_JIT_HOT_REG_STATS`): 88–93% of (block,
register) pairs are referenced once or twice, and pinning breaks even at
two.

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
frame needs four.

## Encoding hazards

An encoder whose wrong answers are other valid instructions needs its
**boundary** values tested, not its typical ones. Three defects, all at
an end of a range, none of which computed a wrong answer that any test
was looking at:

- **A register that does not fit assembles as a different instruction.**
  The 16-bit `CMP`/data-processing form encodes r0–r7;
  `emit_dp_reg(DP_CMP, R8, R1)` set a bit belonging to `rm` and became
  `CMP r0, r1`, so the loop cap never applied and chained loops ran
  unbounded — 3700 guest instructions per block entry where 64 was
  intended. It looked like extra throughput and was the interrupt-latency
  bound being thrown away.
- **`imm5 == 0` does not mean "no shift", and only `LSL` reads it that
  way.** `LSR #0` **is** `LSR #32`, `ASR #0` is `ASR #32`, `ROR #0` is
  `RRX`. RISC-V spells a move as `srli rd, rs, 0`, which assembled as a
  shift by 32. Rewriting the type to `LSL` is the fix; *skipping* the
  emit is not, because `rd` and `rm` differ and the move still has to
  happen. Invisible to x86-64, whose `shr r32, 0` is a genuine no-op.
- **Branch range is a silent cliff.** Loop chaining was once emitted only
  when the back edge fitted the 16-bit conditional branch (±254 bytes); a
  larger block stopped chaining rather than widening the encoding,
  costing 2.4× on the loops that crossed the line. Every branch this
  backend emits is the wide form now.

## Calling convention

Cortex-M selects instruction set with the low bit of a branch target, so
a block address needs bit 0 set before it is called. That belongs in the
**framework**, which knows the host, not in the frontend, which knows the
guest. Getting it wrong faults on the first block entry rather than
computing anything wrong: the banner prints and nothing else.

## Committed code is not emission

**A compaction caused by a buffer overflow unlinked nothing**, and the
board hard-faulted on a branch to 0x7A4 bytes *below* the code buffer.

The emitter's branch patcher declines to write while the buffer has
overflowed — rightly, during emission, because the slot it was handed
may lie past the end. But the hooks the framework calls to link, unlink
and retire blocks went through that same patcher, and an overflow is
exactly when the framework compacts: the flag was still set, `unlink_all`
marked every chained exit unlinked and rewrote none of them, and the
survivors were then moved with jumps aimed at blocks that had just been
evicted. A relative branch moves with its block, so the target moved
too — out of the buffer.

It needs a block larger than `EMU_JIT_BLOCK_RESERVE`, because otherwise
the reserve triggers the compaction first and the flag is clear. That is
8192 bytes on a host, which no block reaches, so no host run had ever
overflowed — and 512 here, where every guest that fills the cache does.
A host runner built with this backend's reserve segfaults on the same
guest (x86-64's patcher carried the same guard), so it was never a
Thumb-2 defect, only one that could not be reached anywhere else.

**It was not a PowerPC defect either, though PowerPC found it.** RV32
CoreMark at 120 iterations overflows 982 times on the F746. Built from
the commit before the fix, in a worktree, it prints its banner and never
finishes; with the one fix applied to that tree it completes with
`crcfinal 0xd340`. The defect arrived with block chaining and was there
for three weeks. `isatest`, which is what a board run usually is, never
overflows the buffer and so could not meet it.

Two fixes, each sufficient alone and each reverted alone to show it:

- the framework reports an overflow by value and clears the emitter's
  flag when the attempt ends;
- the hooks use `t2_write_branch`, which does not consult it.
  `t2_patch_branch` is for a branch in the block being emitted and
  nothing else.

`emu_jit_invalidate_page` already cleared the flag by hand before
calling the same hooks, with a comment saying why. `compact()` called
them too and had no such line. **A fix applied at one of two call sites
is a note about where the other one is.**

How it was found is the reusable part: the stacked pc was in `.bss`, the
range checks on every block pointer and link patch had not fired, and
dumping the code buffer over gdb showed the last block entered ending in
`b.w` to an address outside it — while the block table said that exit
was *unlinked*. The table and the bytes disagreeing is the whole
diagnosis.

## Caches

The JIT writes instructions as data and branches to them. On a Cortex-M7
that needs a real clean-to-PoU and I-cache invalidate by address, not the
DSB/ISB that sufficed with no caches.

The framework calls `emu_jit_ops_t.sync` after every translation, after
a compaction and after patching a link; on this host that is
`t2_sync_code`, which issues the barriers itself and delegates the
maintenance to `board_sync_icache`. That split is deliberate: the
barriers are a property of the *host*, and whether there are caches to
maintain is a property of the **platform** — nothing in the compiler
flags distinguishes the parts, since `-mcpu=cortex-m4` and
`-mcpu=cortex-m7` both define `__ARM_ARCH_7EM__`. So `board_sync_icache`
is weak and a no-op, and the F746 overrides it.

**It was missing entirely, and this section described it anyway.** When
the hand-written translator was replaced by the IR backend, the new one
inherited `.sync = NULL` from a macro shared with x86-64 — whose caches
*are* coherent with instruction fetch, so the comment justifying it was
true where it was written and false where it was copied.

**It has not been shown to fail without the maintenance.** CoreMark on
the F746 with `.sync` back at `NULL` — 25 iterations, 55,452 translations
into a 12 KB buffer with 55,441 evictions — came out identical to the
fixed build. The change is kept because the architecture requires it,
**not** because a failure was observed. Do not read "the A/B showed
nothing" as "the maintenance is unnecessary"; read it as "this workload
cannot see it".

## Sizes

On a microcontroller the framework's tables are the guest's memory, so
they follow the target (`EMU_HOST_JIT_THUMB2`):

| | here | on a host |
|---|---|---|
| blocks, hash buckets | 256, 256 | 65,536 each |
| block reserve | 512 bytes | 8192 |
| code buffer | `EMU_JIT_CODE_BYTES`, default 32 KB | 32 MB |
| IR instructions, temps per block | 512, 256 | 2048, 1024 |

On the F746 a JIT build has 77 KiB less guest RAM than an interpreter
one at the default: 232 KiB against 309.

## Measured

Nucleo-F746ZG, 216 MHz, `RelWithDebInfo`, host cycles per guest
instruction. Each figure is the firmware's own count and agrees with a
wall clock on the host end of the UART.

RV32, CoreMark at 120 iterations (30,027,635 instructions, a run
CoreMark accepts as valid) and `bench`:

| | CoreMark | translations | guest RAM | `bench` |
|---|---|---|---|---|
| interpreter | **108.2** | — | 309 KiB | **98.2** |
| JIT, 32 KB (default) | 169.6 | 82,075 | 232 KiB | 199.5 |
| JIT, 48 KB | 143.5 | 60,171 | 216 KiB | |
| JIT, 64 KB | 97.7 | 29,682 | 200 KiB | |
| JIT, 96 KB | **53.1** | 588 | 168 KiB | |

**At the default cache the JIT is slower than the interpreter**, on both
guests — 1.57× on CoreMark, 2.0× on `bench`. CoreMark's translated
working set is about 83 KB (82,896 bytes in use at 96 KB), and until it
fits the cache is a queue: 82,075 translations for a guest with a few
hundred blocks. Once it fits the JIT is 2.04× the interpreter.

That is the same statement this project once made about a 12 KB cache,
moved up: the IR backend emits more per guest instruction than the
hand-written translator it replaced, and the default did not follow.
PowerPC on the same board has the same shape with larger guests — see
[../frontend/ppc.md](../frontend/ppc.md).

**Any board figure from a run longer than 19.9 seconds, measured before
the counter was fixed, is a wrap short.** DWT CYCCNT is 32 bits and the
firmware subtracted an epoch from it, for the guest's clock and for the
performance figure alike. A PowerPC CoreMark run printed 42.0 cycles per
instruction — and the JIT 5× ahead of the interpreter — while taking
21.8 s against the interpreter's 12.3; the true figure is 393.4.
`emu_cycles.h` accumulates wrap-correct deltas into 64 bits now. The
tell, before the wall clock settled it, was a profile that priced a
translation at 105,000 cycles in one configuration and 865 in another.

## Where the bytes go

`-DEMU_JIT_BYTE_STATS=ON` prints emitted bytes per IR operation after a
run, on the board as on a host (`include/emu/emu_ir_bytes.h`). It had
been measured once on x86-64 and not kept, and nobody had measured this
backend at all -- while code size is the thing the cache, which is the
guest's RAM, pays for.

RV32 CoreMark x120 on the F746 at 96 KB, per translation:

| | bytes / guest insn | LOAD | STORE | GET | PUT | EXIT_IF |
|---|---|---|---|---|---|---|
| x86-64, for comparison | 59.3 | 100 | 104 | 7 | 7 | 42 |
| Thumb-2, as it was | **60.8** | 92 | 105 | 5 | 4 | 38 |
| Thumb-2, short encodings | **44.6** | 77 | 86 | 3 | 2 | 30 |
| ...and the one-compare window guard | **36.0** | 54 | 59 | 3 | 2 | 27 |

**The encoders always chose the 32-bit form.** `MOV`, `CMP`, and the
loads and stores of the frame and of the guest register file were
`MOV.W`, `CMP.W`, `LDR.W` and `STR.W` whatever their operands, though
Thumb-2 has 16-bit encodings for all four: `MOV` and `CMP` for any pair
of registers but SP and PC, `LDR`/`STR` for a low register at a word
offset from SP (to 1020) or from a low base (to 124) -- which is every
frame slot a small block has and the head of every frontend's register
file, reached through r4. They choose by operand now, and an access's
displacement is one `ADDW`/`SUBW` rather than `MOVW` and an `ADD`, and
a trap check compares with an immediate zero rather than materialising
one.

**CMP is the one with a trap in it**: T1 holds two low registers and T2
any pair, UNPREDICTABLE when both are low, and T1 handed r8 is the
defect this file's encoding hazards already record. `t2exec` executes
every pairing with values for which a compare of the wrong registers
gives other flags, and checks each form's *length* as well as its
answer, so a fall back to 32 bits is seen too; a mutant using T1 for
every pair fails three cases by name, and one scaling the 16-bit
offset wrongly fails three more. `t2-check-encodings.sh` holds the new
forms to `arm-none-eabi-as`.

27% fewer bytes, and **CoreMark's working set went from not fitting the
96 KB default to fitting it with room** -- which is a cliff, not a
slope. CoreMark x650, the firmware's count, each against a worktree at
the commit before and agreeing with the host's clock:

| | translations | compactions | cycles / insn | wall |
|---|---|---|---|---|
| 96 KB, before | 29,862 | 816 | 26.86 | 19.92 s |
| 96 KB, after | **294** | **2** | **15.71** | **11.65 s** |
| 32 KB, before | 346,178 | 19,453 | 151.68 | 112.56 s |
| 32 KB, after | 299,402 | 15,273 | 135.82 | 100.73 s |

**1.71x at the default** and 1.12x at 32 KB, where the hot set still
does not fit and the JIT still loses to the interpreter. `isatest` on the
board: 748 checks, 1,129 of 65,829 interpreted, as before; `crcfinal
0x25e3` and "Correct operation validated" in every row.

**Then the window guard, which was a third of every access.** It
subtracted the window's base, compared with the limit, tested the
alignment and branched twice, with the base and the limit each a
`MOVW`/`MOVT`: 34 bytes on every load and store. Now the offset is
rotated right by log2(size) after the subtraction, which leaves an
aligned offset as offset >> k and carries a misaligned one's low bits to
the top, so one unsigned compare against size >> k is the bounds test and
the alignment test together -- and the access undoes the rotation with
`[host, r2, LSL #k]`. The base and the limit are modified immediates
when they encode (RV32's 0x80000000 and a 164 KiB window both do), the
branch into the slow path and the one over it are the 16-bit forms
because each target is a few instructions away, the frame is set up and
given back with 16-bit `SUB/ADD SP`, and the slow path loads its
arguments with `MOVS` and `ADD rd, SP, #imm` -- the flags being dead
before a call. 16 bytes of guard where there were 34.

x650 against a worktree at the commit before:

| | translations | cycles / insn | wall |
|---|---|---|---|
| 96 KB, before | 294 | 15.70 | 11.66 s |
| 96 KB, after | 281 | **14.29** | **10.61 s** |
| 32 KB, before | 299,402 | 135.82 | 100.62 s |
| 32 KB, after | 264,568 | **117.63** | **87.18 s** |
| interpreter, same tree | | 111.22 | 82.42 s |

At 32 KB the JIT is now within 6% of the interpreter, from 22% behind.
`t2exec` runs the guard sequence at every edge -- the first and last
aligned slot, one past the end, below the base, each misalignment, at
all three sizes -- and the shifted access; it is a copy of
`emit_fast_guard`'s sequence built from the same encoders, not the
function itself, so the board's `isatest` (748 checks, 1,129
interpreted, unchanged) is what holds the lowering to it.

What is left in an access is the host pointer (`MOVW`/`MOVT`, 8 bytes)
and the call into the checked accessor with its trap exit: `MOVW`/`MOVT`
and `BLX` for the call, a compare and a branch for the verdict -- once
per access. See [../TODO.md](../TODO.md).

## The guest-RAM window

A frontend may offer a window (`emu_ir_fastmem_t`) that a `LOAD` or
`STORE` can reach without calling a helper: one unsigned compare against
the window and an alignment test, then a plain access, with the checked
accessor as the slow path. **The frontend refuses whenever an access
could mean more than a move** — RV32 offers it only with PMP, paging and
Sdtrig all idle, because the inlined path writes guest RAM without
asking anyone.

That is the rule the hand-written backend learned on hardware and it
still holds: anything `rv_hart_load`/`rv_hart_store` does beyond the
access itself — a reservation, a permission, a translation — has to be
answered by whoever inlines the access. The window is guest RAM only.
Peripheral accesses go to the helper; the hand-written backend's inlined
peripheral window, and the 2.2–3.1× it bought driver-shaped code, did
not survive the move to the IR.

## History: the framework port

Porting onto the shared IR framework cost 4× at first, in four separate
ways, while every suite passed throughout: CoreMark went 79,502 ticks to
321,035 with `isatest` still passing. The causes, in order of size, were
a gate copied from the x86-64 backend that sent everything after the
first PMP entry to the interpreter, overflowed blocks being treated as
declined, a direct-mapped hash, and six indirect calls per dispatch.
Each is written up in [../jit/README.md](../jit/README.md).

Inlining the emitters was **neutral** (LTO already did it). Do not
re-try it.
