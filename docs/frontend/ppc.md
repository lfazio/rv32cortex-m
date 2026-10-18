# The e200z7 frontend

`include/ppc/` and `src/frontend/ppc/` — the NXP e200z759n3: Power ISA
embedded (Book E) with VLE and the embedded floating-point unit, the
core in the MPC57xx family, and this project's big-endian guest.

```sh
cmake -B build/ppc -DEMU_PLATFORM=host \
      -DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_PPC=ON
cmake --build build/ppc
./build/ppc/emu-host        build/ppc/guest/ppc-coremark.elf
./build/ppc/emu-host --jit  build/ppc/guest/ppc-coremark.elf
ctest --test-dir build/ppc
```

**Nothing here has been run against an e200.** There is no board for
this frontend and no other emulator is consulted, so every statement
below about what the core does is the manual's — *e200z759n3 Core
Reference Manual*, Rev. 2 — and every check is against something that is
not this code: binutils for encodings, Python's integers and rationals
for arithmetic, the manual's own tables for the system level. What none
of them can catch is a shared misreading of the manual.

| Area | State |
|---|---|
| Book E, 32-bit: integer, logical, rotate and shift, CR logic, `isel`, the update and indexed forms, `lmw`/`stmw`, byte-reversed access, the reservations (`lwarx`/`stwcx.` and the byte and halfword forms) | implemented |
| VLE: every 16-bit `se_` form, the `e_` forms, SCI8 and the I16 immediates, `e_lmvgprw`-family multiple moves | implemented |
| EFPU2, scalar single precision (`efs*`): arithmetic, the multiply-adds, square root, min/max, compares, every conversion including half precision | implemented, on SoftFloat |
| Interrupts: IVPR and IVOR0–15, 32–35; the four save/restore pairs (SRR, CSRR, DSRR, MCSRR); ESR; `sc`, `tw`, `rfi`/`rfci`/`rfdi`/`rfmci` | implemented |
| Privilege: MSR[PR], SPR access by number, the user-readable SPRGs | implemented |
| Time base, decrementer with auto-reload, TSR/TCR, HID0[TBEN] and [SEL_TBCLK], `wait` | implemented |
| The MPC5675K's INTC, STM and SWT, one of each per core: priorities, the LIFO, software and hardware vector modes; a counter on the core clock with four compare channels; a watchdog with fixed and keyed service, window mode, locks, and interrupt-then-reset | implemented ([below](#the-interrupt-controller-and-the-system-timer)) |
| MMU: the 32-entry TLB, MAS0–4 and 6, PID0, MMUCSR0, the `tlb*` instructions, address spaces, per-page VLE and per-page byte order | implemented ([below](#the-mmu)) |
| SPE and the vector half of the EFPU (`ev*`) | **not implemented** — SPE-unavailable while MSR[SPE] is clear, as the core does; an illegal-instruction program interrupt once it is set |
| Classic floating point (`fadd` …) | not on this core |
| Caches, the debug facility (IAC/DAC/DBCR do hold what is written) | not modelled |
| gdb stub | `src/frontend/ppc/ppc_gdb.c`: r0..r31, pc, msr, cr, lr, ctr, xer as `org.gnu.gdb.power.core`, big-endian, as `powerpc:e500` -- the nearest core gdb has, which names the Book E SPRs and the SPE where `powerpc:common` named 603 BATs and AltiVec; not the SPE halves or the SPRs ([../gdb.md](../gdb.md#the-guest-stub)) |
| pair statistics | not written for this frontend |
| More than one core | **two**, as on the MPC5675K (`PPC_CORE_COUNT`, 2 by default): core 1 held until the SSCM's key sequence releases it, a TLB each, reservations broken by the other core's stores ([below](#two-cores)) |

Both backends run everything in the table: the interpreter, and a JIT
through the shared IR on x86-64 and on Thumb-2.

---

## Two encodings, chosen per image

VLE and classic Book E are different encodings of the *same bytes* —
`0x48000009` is `bl` in Book E and a 16-bit `se_` form followed by
something else in VLE — so which one a page holds has to be known
before its first fetch. A real e200 takes it from the VLE attribute of
the TLB entry. Out of reset that is the one entry the model starts
with ([the MMU](#the-mmu)), whose VLE bit **comes from the image**:

- an **ELF** says: binutils marks a VLE segment with `PF_PPC_VLE`
  (`0x10000000` in `p_flags`), and the boot hook reads it;
- a **raw binary** cannot say, and is taken as VLE.

So the compiled guests are loaded as ELF and are Book E, and
`isatest.bin` is a flat VLE image. The firmware embeds the ELF rather
than a binary for the same reason (see below). Once a guest programs
the TLB, the encoding is each page's own.

The loader had to learn two things for this: ELF headers are in the
file's byte order, and it read them little-endian only ("machine 5120"
is 20 with its bytes swapped); and the frontend's `set_image` hook is
called on a *reload* and never on first start, so the image reaches the
boot hook through `emu_boot_info_t` instead.

## One decoder

`ppc_decode(insn, len, vle, &out)` reduces either encoding to one
`ppc_insn_t`: a mnemonic (313 of them), a **semantic class** (125), and
fields already in architectural terms — real register numbers after the
`se_` forms' four-bit mappings, the branch condition in Book E's BO/BI
whatever encoding it came from, immediates sign-extended and scaled.
Everything else consumes that:

| consumer | file |
|---|---|
| the interpreter | `ppc_interp.c` — `ppc_exec()` switches on the semantic class |
| the translator | `ppc_ir.c` — the same switch, to IR |
| the disassembler | `ppc_disasm.c` — a format per mnemonic |

There is no VLE in the interpreter or the translator and no Book E
either. It was not always so: the interpreter used to decode inline,
which was a second description of the encoding space with nothing
holding it to the first — `e_cmpi` compared into CR0 whatever field it
named, the SCI8 record forms never recorded, `se_btsti` wrote the wrong
CR bits.

Reserved fields follow manual 3.16: a non-zero reserved field is an
illegal instruction, *except* bit 31 of the X-form loads and stores and
the `z` bits of BO, which the core ignores; the invalid forms of the
update instructions, `lmw` and `bcctr` are executed, as it executes
them.

### How it is checked: binutils, both ways

`tests/ppc-check/check.py` asks two questions of the same encodings.
**Round trip**: everything the decoder names is disassembled, the text
is assembled again, and the result must decode to the same *fields* —
fields rather than bytes, because an SCI8 immediate can often be placed
by more than one scale and the assembler may pick its own. **Validity**:
whether the decoder calls an encoding an instruction must agree with
`objdump`, except where the manual says otherwise, and each exception is
listed in the script with its reason.

| set | encodings | named | round trip wrong | objdump-only | ours-only |
|---|---|---|---|---|---|
| VLE 16-bit, exhaustive | 49,152 | 41,131 | 0 | 0 | 0 |
| VLE 32-bit, swept | 1,498,005 | 676,155 | 0 | 0 | 0 |
| Book E, swept | 2,520,850 | 899,826 | 0 | 0 | 0 |
| random, VLE / Book E | 14,910 / 60,000 | 10,287 / 32,404 | 0 | 0 | 0 |

The full run is about two minutes; `ctest` runs the exhaustive 16-bit
set and the random one (`ppc-decode-vs-binutils`). With `--elf` the
encodings are a compiled program's — every word of its executable
sections must be an instruction this core has, which is the question a
sweep cannot ask (`ppc-decode-compiled`, over `alu`, `coremark` and
`crypto`).

`ppc-dis` (`tests/tools/ppc_dis.c`) is the disassembler as a command, and
`emu-host` uses the same one for `-DEMU_ENABLE_TRACE=ON`.

## The interpreter

`ppc_exec()` is the instruction semantics, and it is also what a
translated block calls for an instruction the translator does not lower
— so a block and the interpreter cannot disagree about anything the
block hands back.

A decode cache keyed on pc, instruction word and encoding is worth 35%
of the interpreter on a host. It is 1024 entries there and 64 on ARM,
where 48 KiB of `.bss` would be the guest's memory -- one per core; see
[two cores](#two-cores) for what one between two cost.

Things the manual settles and a reading of the mnemonics would not:

- **Unaligned accesses are performed** (3.4), a byte at a time; only the
  instructions the manual lists take an alignment interrupt — the
  multiple moves, the reservations, `dcbz` with the cache off.
- **The reservation is a flag with no address** (3.5). A store
  conditional to a different address than the load succeeds, and one
  with no reservation is a no-op that raises nothing, whatever its
  alignment. That is the core alone; with a second core running, the
  manual's "external reservation logic" gives it an address -- see
  [two cores](#two-cores).
- **SPR privilege goes by number, before existence** (2.5.1, 3.15):
  bit 4 of the SPR number set means privileged, so problem state gets a
  privilege exception for an SPR that does not exist, and supervisor
  state an illegal instruction.
- **The time base and decrementer count nothing out of reset**:
  HID0[TBEN] gates both. With it set they count instructions (the
  processor clock, one tick each), or platform time with
  HID0[SEL_TBCLK].
- **`wait` ends only on an enabled interrupt** (3.12). The run ends when
  the only core is waiting with MSR[EE] and [CE] both clear, because
  nothing can then wake it.
- The semihosted `write`/`exit` are answered **in supervisor state
  only**; an `sc` from problem state is the guest's, and goes to IVOR8.

## The MMU

`ppc_mmu.c`: chapter 10 as the e200z759n3 has it — one TLB of 32
entries, fully associative, pages of 2^TSIZE KB from 1 KB to 4 GB, the
MAS registers and the five TLB instructions. Every rule is the manual's,
with its section beside it in the code; nothing here has met an e200.

| | |
|---|---|
| TLB1, 32 entries; TLB0 null; MMUCFG, TLB0CFG, TLB1CFG as tables 10-3 and 10-5 | implemented |
| MAS0–4, MAS6, PID0 (eight bits), MMUCSR0[TLB1_FI] | implemented; unimplemented bits read 0 |
| `tlbre`, `tlbwe`, `tlbsx`, `tlbivax` (TLBSEL, INV_ALL, IPROT, EPN only), `tlbsync` (no-op) — all privileged | implemented |
| the MAS registers after a TLB error, a `tlbsx` hit or miss, and `tlbre` (table 10-15) | implemented |
| matching: V, TS against MSR[IS]/[DS], TID against PID0 with 0 shared, EPN on the page's bits | implemented |
| UX/SX/UW/SW/UR/SR, as data storage and instruction storage interrupts | implemented |
| DTLB and ITLB errors; ISI[MIF] for an odd halfword of a Book E page; ITLB[MIF] and ISI[BO] for a 32-bit VLE instruction straddling into a missing page or one of the other encoding | implemented |
| the encoding per page, MAS2[VLE] | implemented |
| WIMG | held, and does nothing: no cache is modelled |
| E, little-endian pages: data in true little-endian order at every width, Book E instructions as little-endian words, DSI[BO] for a misaligned access across pages that disagree, ISI[BO] for a VLE page marked E, MAS4[DE] on a miss | implemented ([below](#little-endian-pages)) |
| cache instructions taking TLB errors (`dcbf`, `dcbst`, `icbi`, `dcbt`, …) | no: they stay no-ops; `dcbz` is translated, because it stores |

**Where the model is not the manual**, each written down where it is
made:

- **The reset entry.** The manual's entry 0 is one 4 KB page at the reset
  base, which a part's boot ROM extends before user code runs. There is
  no ROM here, so entry 0 is the whole 4 GB, identity, every permission
  — with the manual's other attributes: valid, IPROT, TS 0, TID 0,
  cache-inhibited, and the image's encoding. A guest that programs the
  TLB overwrites it as it would the ROM's entries.
- **A multi-hit** is "a programming error" with an undefined result; the
  lowest-numbered entry wins. A TSIZE above 22 is undefined and matches
  nothing. MAS0[TLBSEL] is "ignored", and reads back as written.
- **A misaligned access across a page** is translated byte by byte, and
  a store translates every byte before writing any. 3.4 lets the core
  write the first page and redo it; this does not. DEAR is the first byte
  of the page that faulted, which 7.7.14's "a byte of the access" allows.
- **Branch targets on the identity map.** 3.3: `bclr`, `bcctr` and the
  `rfi` family keep bit 62 on this core, and an odd halfword on a Book E
  page is ISI[MIF]. Under a programmed TLB that is what happens. With the
  reset TLB the whole map is one encoding, and a Book E image still has
  its targets masked to a word, as before — only a broken Book E program
  can tell, and doing better would put a check on both backends' branch
  paths.

**What the identity map pays.** `mmu_on` is true unless the TLB is in its
reset state with MSR[IS] and [DS] clear, and only `ppc_mmu_refresh`
writes it — after a TLB write, an MSR change and every interrupt. The
interpreter reads it into a local once per stretch and branches on that
to an out-of-line translating fetch; a change sets `irq_dirty`, which
ends the stretch. Loads and stores test the flag. CoreMark, identical
retired counts and CRCs against the commit before:

| | before | after | |
|---|---|---|---|
| x86-64 interpreter, callgrind | 1,473,600,779 | 1,462,832,203 | −0.73% |
| x86-64 `--jit`, callgrind | 263,305,060 | 263,984,261 | +0.26%, all in the dispatcher |
| F746 interpreter, three layouts | 224.05 / 231.50 / 225.15 | 225.50 / 230.95 / 223.77 | host cycles an instruction |

The JIT's share is reading `blocked`, which for this frontend was NULL:
two or three host instructions a block entry. "Three layouts" is the
same source built with `-falign-functions` default, 32 and 64, because
this part moves by up to 10% with placement alone.

**The first version cost the F746 10%, and x86-64 could not see it.** It
moved the inner loop into a `stretch()` function taking `&reason` and
`&stop`, inlined once per mode: callgrind said +0.10%, the board said
224–231 became 242–256 across the same three layouts. Every guess was
wrong, each by measurement: the MMU's tests compiled out changed the
board run by under 0.01%; the TLB code LTO had inlined into the
interpreter's switch (1.2 KB, kept out of line now anyway) was not it;
padding the old `ppc_cpu_t` to the new size, which moves guest RAM by
the same 416 bytes, reproduced the old figure to the digit. Bisecting by
file did it in two steps — everything new except `ppc_interp.c` ran at
216.96 — and putting the loop back in its old shape, inline with a
`goto out`, gave the figures in the table. On the Cortex-M7 the
difference is register allocation in the hottest loop; an instruction
count on another architecture has no way to show that.

**The JIT runs under a programmed TLB.** It used to stand aside --
`blocked` pointed at `mmu_on`, so from the first `tlbwe` everything was
interpreted -- and three things lifted that:

- **the translator fetches through the TLB**, with `ppc_mmu_probe_exec`:
  `ppc_mmu_xlate`'s answer for a fetch with none of its side effects,
  because the translator reads ahead of what will run and a miss there
  must not touch the MAS registers. A miss, a page the mode may not
  execute, a page of the other encoding, an odd halfword on a Book E
  page or a straddle into any of those *stops the block*, and the
  interpreter, arriving, raises what the manual says;
- **what a block was fetched under is its identity**: under the MMU the
  context carries `mmu_on`, MSR[IS], MSR[PR] (execute permission is per
  mode) and PID0 (`ppc_cpu_ctx`); and **a TLB write retires the blocks
  whose code the entry mapped**, before the change and after it,
  because the mapping itself is not identity but validity -- see
  [per-entry invalidation](#per-entry-invalidation);
- **the RAM window is a page**: on the identity map a block's loads
  and stores go straight to the RAM region the stack is in, and under
  the MMU to the page the stack is on -- when `ppc_mmu_data_page` says
  every address on it translates alike: read and write for the mode,
  big-endian, and no lower-numbered entry overlapping it, since the
  lowest entry wins a multiple hit. Clipped to the RAM region its real
  address is in. Anything outside, misaligned, or on another page goes
  to the helper, which translates. What a window was chosen under is in
  the key: a TLB change touching a window discards every block, and
  MSR[DS] -- the data address space it was looked up in -- joined
  MSR[IS], MSR[PR] and PID0 in the context for it.

`coremark-tlb` is CoreMark with a start-up that maps RAM and the console
the MPC57xx way and overwrites the reset entry (`crt0.S` assembled with
`PPC_CRT0_TLB` defined -- a guest define, not a build option), so the whole benchmark runs under the MMU. x86-64, same
retired count and CRCs:

| | interpreter | JIT, blocked | JIT, every access a call | JIT, page window |
|---|---|---|---|---|
| `coremark`, identity | 0.26 s | 0.051 s | 0.052 s | 0.048 s |
| `coremark-tlb` | 0.56 s | 0.596 s, 12,170,424 interpreted | 0.112 s, 2,480 interpreted | **0.047 s**, 2,480 interpreted |

The last two columns were measured together, from a worktree at the
commit before and this one, median of five interleaved runs: 0.139 s
against 0.047 s that day. Callgrind, which counts and does not time:
853,507,674 host instructions against 258,631,521 -- within 0.5% of
identity CoreMark's 257,293,430, which moved by 7,914.

On the F746 at 96 KB, host cycles an instruction and the wall clock:
`coremark-tlb` interpreted **582.99** (32.3 s); translated with every
access a call **104.69** (5.79 s), and with the page window **52.68**
(2.89 s), beside identity `coremark` at 51.66 (2.84 s). Same CRC and
retired count throughout, and `ppc-mmu` passes all its checks on the
Thumb-2 backend both ways.

**The window's checks had to be built so the block under test runs at
all.** `stub_win` moves r1 onto a test page and loops a store and a load
there, so that the loop's block is the one whose window is that page.
The first version fell into the loop, and its first iteration ran in
the block that moved r1 -- whose window was the stack's page -- so a
store refused on the test page faulted through the helper before the
loop's block was ever built, and a mutant dropping the permission test
passed. A `b` into the loop gives it a block of its own from the first
iteration; with it each of four mutants -- no permission test, no E
test, no lower-entry test, MSR[DS] out of the context -- fails the
`jit-win-` check named for it, under `--jit` only. Same lesson as the
caller below, from the data side.

**Its checks could not see the machinery at first: six of eight
single-point defects passed all 150.** Every one of them was written for
the interpreter, which re-checks the mode on every fetch, so none ran
one address twice under different conditions -- and a block exists only
if something runs twice. The `jit-` cases do: each runs code, changes
one thing the block was built under, and runs it again. Two of those
first passed too, for a reason worth keeping: after an `rfi` the
fallback interprets a *batch*, which ran straight into the target and
checked the mode itself. `mmu_caller` loops first and calls the target
from a translated block, which is the only path through the dispatcher's
lookup -- and with that, all ten mutants of the new code fail on the
check named for them.

### Per-entry invalidation

A TLB change used to move the generation, so every block went with
every `tlbwe`, `tlbivax` and flash invalidate: a start-up pays that
once, and a guest refilling its TLB from a miss handler paid it for
each refill. Now a change retires only the blocks whose code the entry
mapped -- `emu_jit_invalidate_range` over the span the entry covered
before the change and the span it covers after, because either can be
the only one: removing an entry changes the first, and writing a
lower-numbered entry over an address another maps changes only the
second, the lowest entry winning. TS and TID are not consulted; a few
more blocks retired is always right.

Two things a block holds are not in its code range. **Its RAM window**
is a page its loads and stores reach whatever page its code is on, so
each core remembers the windows it handed out under the MMU, and a
change touching one moves the generation after all. **Its pc's
refusal**: a pc whose fetch faulted was declined and remembered, and a
mapping is what makes it translatable -- so the negative cache forgets
the refusals in the range, and only those. Forgetting them all at each
refill made the translator try the miss handler again every time, an
instruction at a time.

`ppc-coremark-tlbmiss` is the guest that measures it: `crt0.S` with
`PPC_CRT0_TLBMISS` keeps three protected entries -- the vectors and the
handler, the stack, the console -- and a miss handler refills two more
round robin, a 4 KB page at a time, for a working set of five pages.
26,652 refills in a run. x86-64 JIT, same retired count and CRC:

| | flushes | translations | translation attempts refused | host instructions | wall, median of 5 |
|---|---|---|---|---|---|
| every write discards everything | 26,652 | 111,979 | 188,825 | 7,889M | 1.815 s |
| retire by entry | 8 | 48,255 | 11,802 | 6,828M | **1.409 s** |

**The first version was slower than what it replaced**: 2.41 s
against 1.97 s, with translations halved. Retiring a range scanned the
whole block table, and at a refill every few hundred instructions that
scan was 35% of all host instructions -- more than the flushes had
cost. The framework indexes blocks by the page they start on now, so a
page retire walks the blocks on that page and on the few before it
that could run onto it. With 8 refill entries or more CoreMark's
working set fits after the first touch and there is nothing to measure,
which is why the guest has two. ctest's
`guest-ppc-coremark-tlbmiss-retires` holds the claim by its flush
count: with every write discarding everything it reads 26652.

The mutants: a `tlbwe` retiring only the old span fails `jit-map-remapped`,
only the new one `jit-map-back`; a `tlbivax` retiring nothing fails
`jit-ivax-code`; windows unrecorded or ignored fail `jit-win-le` -- all
under `--jit` only. In the framework's tests a walk that does not look
back for blocks running onto the page, a block never indexed, refusals
kept and refusals all forgotten each fail. One survives by design: the
index is rebuilt after a wide range, and the walk also drops retired
blocks it meets, so leaving the rebuild out costs only time.

### Little-endian pages

MAS2[E] makes a page "accessed in true little-endian byte order"
(table 10-11): the same bytes, read the other way round. All of it
lives in the two places a translated access already goes through --
`load_xlate` and `store_xlate` in `ppc_cpu.c`, and the two fetches --
so the flat path, which cannot have an E page, pays nothing:

- **Data** swaps at the access's width; an unaligned access assembles
  its bytes from the low end. `lwbrx` and its relatives come out
  big-endian on such a page, because their reversal undoes the page's.
- **Across a page boundary** both pages must agree. When they do not
  there is no order to put the bytes in, and the core takes a DSI with
  ESR[BO] (11.16.5) -- DEAR the first byte on the page that differs, and
  a store writes nothing. The access path has one bit to hand the
  interrupt that its callers do not know about, so it leaves it in
  `dsi_bo` and `ppc_cpu_raise` moves it into ESR and clears it.
- **Code**: a Book E page marked E holds little-endian words, reversed
  as they are fetched -- by the interpreter, and by the translator,
  which reads the page's E through the same probe as its VLE bit, so a
  TLB write's generation covers both. A VLE page marked E cannot be
  executed: ISI with ESR[BO] (table 7-14), whether the fetch is the
  first halfword or the second of a straddling instruction.
- **A TLB miss** loads MAS2[E] from MAS4[DE] with the rest of WIMGE.

E was refused until this -- read as 0, so a guest could see that no
page was little-endian -- rather than accepted and ignored. `ppc-mmu`
holds all of it, 45 checks, on both backends and on the F746, and
twelve of thirteen single-point mutants fail on the check named for
them. The thirteenth is the translator's own refusal of a VLE page
marked E: without it, the translator reverses the word as for Book E,
the VLE halfwords become something it declines at once, and the
interpreter raises the same ISI. It is kept because a 32-bit VLE
instruction can reverse into one that decodes; the test that reaches it
from a translated VLE block says it cannot tell.

**How it was checked.** `ppc-mmu` is 150 checks whose expectations are
the manual's tables or bytes the guest wrote to physical memory; two
were marked as the model's choices (the reset entry, and MAS2[E] until
little-endian pages were modelled). Then 37
single-point defects — the TS and TID compares, the page mask, the RPN
mask, each permission, each MAS update, INV_ALL, IPROT, the MIF and BO
cases, VLEMI on an ITLB error, the per-byte split of a crossing access,
the loop choice, the JIT gate, the decoder's `tlbsx` address — each
fails on the check named for it; two end the run instead, which ctest
sees by the missing `PPC-MMU-END`.

One survived at first: the line that sets `irq_dirty` when translation
turns on. Every test reached its first translated fetch through an
`mtmsr`, an `rfi` or an interrupt, and each of those ends the stretch
for reasons of its own. The case that needs the line is a start-up that
writes the TLB and branches straight into the new mapping — and the
first version of that check failed against the *correct* emulator,
because the 4 GB reset entry still covered the target and the
lowest-numbered entry wins. It lives in the start-up now, after entry 0
is overwritten, and fails under the interpreter alone: under `--jit`
every interpreter entry after the declined `tlbwe` reads the flag
afresh. Reading it once per *run* instead of once per stretch is caught
the same way.

Two defects came out of the decoder work beside it. **`dcbz` ignored
rB** for the life of the frontend: its decode never set the indexed
flag, so the address was `(rA|0) + 0`. The only test used `dcbz 0, r3`
and checked the vector and ESR — never DEAR, which said 0 where it
should have said the buffer. And `check.py` had the TLB instructions
listed as "the MMU, which this model does not have"; taking them out
showed objdump naming XO 978 `tlbld` — the 603e's — under the Book E
option set, which the script's first-name-wins rule then filed as not
on this core. It maps that name to `tlbwe` now, and the reserved fields
of all five are held to 3.16.4.

## Two cores

The MPC5675K has two e200z7 cores, and so does this model by default
(`PPC_CORE_COUNT`, 1 or 2). In its decoupled parallel mode -- the
part's default, each core independent -- **core 0 runs out of reset and
core 1 is held** until core 0 writes the address core 1 starts at to
`SSCM_DPMBOOT`, with DVLE saying whether in VLE, and then 0x5AF0 and
0xA50F to `SSCM_DPMKEY` (*MPC5675K Reference Manual*, Rev. 10, in
`docs/nxp/`: 5.6.2.2 and 49.3.1.6-7). `ppc_sscm.c` is the SSCM, and
models those two, STATUS reading decoupled mode, MEMCONFIG at its reset
value, ERROR and DEBUGPORT holding what is written, and the user
options reading as all zero; not what those registers act on, and not
lockstep mode.

Two choices where the manual says nothing, each the conservative one:
**any other key ends the sequence**, and **the sequence releases a core
in reset and does nothing to one that runs** -- the manual describes
releasing core 1 from reset, not restarting it, so a start is once per
reset.

**The cores here are e200z759n3s, and the MPC5675K's are e200z760n3s**
(1.4.1): a 64-entry TLB where this has 32, and SPE2, which this
implements none of. The core is the one `docs/ppc/e200z759CRM.pdf`
describes; the SoC is the one `docs/nxp/` describes, which is where the
start sequence and the peripherals to come from -- the STM and the SWT,
chapters 51 and 52 -- are read from.

- **Each core is a whole core**: its own registers, SPRs, time base and
  decrementer, and TLB. `tlbivax` "invalidates local TLB entries only.
  No broadcast" (10.5.4). PIR is the `p_cpuid` input, "defined by a
  hardwired input vector to the individual cores" (47.6): 0 and 1.
  `p_tbclk`, which either core's time base may count, is one input for
  the part: the platform gives it to core 0, and each core takes the
  latest value at the top of its own run -- core 1's time base did not
  move at all until it did, which `core1-tb-on-tbclk` now holds.
- **Core 1 starts itself.** The core that writes the register cannot
  reset the other's state while the other's thread may be running, so
  the second key leaves a request -- start at this address in this
  encoding -- and core 1 applies it at the top of its next run. The
  request is also what the scheduler sees, whatever the core's own last
  slice wrote: a `wait` executing as a release arrived used to write WFI
  over it, and a waiting core with interrupts off is one the scheduler
  never runs again. (That was found when the model was the MPC5777C's
  `SIU_RSTVEC1`, which could hold core 1 again and restart it; the
  MPC5675K's keys release it once.)
- **The reservation gets an address.** On its own the e200 compares
  none (3.5), and "external reservation logic" on the bus is left to
  the system. While a second core runs, this model is that logic: a
  reservation is on the word, physical, that `lwarx` read, any other
  core's store to the word breaks it, and on RAM `stwcx.` is a
  compare-and-swap against the bytes `lwarx` read -- G4MH's arrangement
  for `LDL.W`/`STC.W`, because another core's plain store can land
  between the check and the write when the cores run in parallel. A
  reservation held when core 1 is started is dropped, which a
  store-conditional is allowed to see as a failure.
- **The JIT**: the RAM window's inlined stores test `smp_live` and take
  the helper once a second core runs, since an inlined store breaks no
  one's reservation; under a programmed TLB a block's context carries
  the core, because where the cores share one block table core 1 would
  otherwise find core 0's block at the same address under a different
  mapping; and the generation is one counter for all cores, because two
  that differed would discard everything at every switch between them.

**In parallel only when asked.** The host runs the two as FreeRTOS
tasks, one after the other on one thread -- deterministic, and the
round's order is asserted every round -- and in parallel with
`-DEMU_OS_SMP=ON`. That is the default only for a multicore G4MH,
because the SMP port makes the JIT's state per thread for every
frontend in the runner, and callgrind measured RV32 under the JIT
paying 1.7% on Dhrystone and 2.1% on crypto for it. A core that cannot
run this round -- held, halted, parked -- is not woken for it, which
was half a percent of a single-core PowerPC guest's whole run in task
switches.

What a single-core guest pays for the company, callgrind against the
commit before (`-DEMU_OS_SMP` at its new default):

| guest | interpreted | JIT |
|---|---|---|
| PowerPC CoreMark | -0.02% | +0.84% |
| RV32 CoreMark | -0.61% | +0.18% |
| RV32 Dhrystone | -0.58% | +0.05% |
| RV32 crypto | -0.45% | +0.08% |

The PowerPC JIT's 0.84% is the store guard -- a byte test before every
inlined store -- in a build that can start a second core; with
`PPC_CORE_COUNT=1` there is none. The interpreter took four rounds to
get to nothing: the reservation paths had to be kept out of line (LTO
folded them into the switch, and `ppc_exec` and `ppc_run` each grew by
two host instructions per guest instruction), and the decode cache's
base had to come from the core rather than a local, which held a
register across the loop.

### How it is checked

`ppc-smp` (smp.c, smp_stubs.S, `crt0.S` with `PPC_CRT0_SMP`), 41
checks, run by ctest seven ways: interpreted and translated; in
parallel (`host-ppc-smp`) and one core after the other on one thread
(`--serial`), where the two share a block table; and at quanta of 1, 4
and 64. Core 1 takes commands through shared words and only core 0
prints. Every wait is bounded, so a core that never answers fails a
check rather than hanging the run. `ppc-smp-vle` is the same source
built to start core 1 in VLE instead, which only a reset can choose
again.

Each mechanism was broken on its own and the run read:

| mutant | fails |
|---|---|
| the key sequence releases nothing | `core1-up` and everything after it |
| the second key alone releases it | `held-without-sequence` |
| a key between the two keeps the sequence | `held-without-sequence` |
| a sequence restarts a running core | `restart-ignored-vle`, `restart-ignored-answers` |
| a store breaks no reservation | `resv-broken-same-value` -- a value written back is invisible to the compare-and-swap, so only the break can see it |
| the core is not in the context | `tlb-code-core1`, under `--serial --jit` only: in parallel each core has its own block table |
| inlined stores unguarded | `resv-broken-same-value`, under `--jit` only |
| the start request never applied | `core1-up` |
| `stwcx.` a plain store, not a compare-and-swap | `race-no-lost-store`, in parallel only, 10 runs of 10 |
| `lwarx` takes no address | eight checks, `inc-total` among them |
| core 1 runs from reset | `pir`: it ran `main` too |
| DPMBOOT[DVLE] ignored | `vle-start` in `ppc-smp-vle` |

`race-no-lost-store` exists because the compare-and-swap's mutant
passed every other check: core 1 stores rising even values and reads
each back while core 0 adds one to whatever it reserves, and an odd
read-back below the value just stored is core 0 writing over it from an
older one. It can only fire when the cores really run at once.

**Two races were in shared state that a single core never reaches.**
The first parallel runs failed one in five and then one in twenty, and
neither failure was in the new code: the interpreter's decode cache
was one array for both cores, so a core found its own pc and bytes in
an entry whose decoded half the other had just rewritten -- a `beq`
raised an illegal-instruction interrupt on core 1 -- and the decoder's
X-form index was built on first use, under a test of one of its
entries that a second core saw true half way through the build. Each
core has its own cache now and the index is built before any core
runs. Neither has a deterministic mutant: they are races, and what
shows them is repetition -- 40 parallel runs each way, interpreted and
translated, without a failure.

### On the F746

**The interpreter stays in flash**, where the other three frontends'
loops went to ITCM: moved there it was 2-5% slower at every alignment,
because `ppc_run` calls `ppc_exec`, the decoder, the accessors and the
address translation for every instruction and each call from ITCM to
flash goes through a veneer -- 28 of them, 31 with the accessors moved
too ([performance.md](../performance.md#the-interpreters-loops-in-itcm)).

`ppc-smp` passes all 41 checks on the Nucleo-F746ZG on both backends,
the cores two tasks of the board's cooperative scheduler as under the
host's `--serial`, and `ppc-smp-vle` passes too. Measured at 8409e5b,
96 KB of code cache, and timed from the host end of the UART as well:

| `ppc-smp` | host cycles an instruction | wall clock |
|---|---|---|
| interpreter | 258.25 | 10.40 s |
| JIT | 77.24 | 3.09 s |

8,831,273 and 8,792,828 instructions: the bounded waits spin a
different number of times on each backend, as on the host.

**What a build that can start core 1 costs a single-core guest is less
than placement moves it, and the store guard is not what moves.** Host
cycles an instruction, each at `-falign-functions` default, 32 and 64;
the same retired counts and checksums throughout:

| build | CoreMark | crypto |
|---|---|---|
| one core | 51.82 / 51.35 / 51.35 | 25.67 / 25.25 / 25.27 |
| two cores | 52.31 / 51.70 / 51.96 | 27.58 / 27.13 / 27.24 |
| two cores, the store guard removed | 52.04 / 51.38 / 51.61 | 28.65 / 28.43 / 28.47 |

Two cores read as 7.4-7.8% on crypto, consistently across the three --
and taking the guard out, the one thing a second core adds to
translated code (2,392 of crypto's 77,872 bytes), made it *slower*. The
second core's state is 5,472 bytes of `.bss`, which moves the JIT's
code buffer, its tables and guest RAM; `-falign-functions` moves the
emulator's functions and none of that. Moving `.bss` alone, in a
one-core build (`-DEMU_LAYOUT_PAD_BSS`), with byte-identical
translations:

| `.bss` moved by | 0 | 2,736 | 5,472 | 8,192 |
|---|---|---|---|---|
| CoreMark | 51.82 | 51.79 | 52.07 | 52.36 |
| crypto | 25.67 | 26.35 | 28.74 | 28.00 |

Crypto spans 12% on placement alone, and the two-core builds sit inside
it. So the guard costs CoreMark at most the half percent between the
second and third rows of the first table, which callgrind's 0.84% on
x86-64 agrees with, and on crypto nothing this board can resolve.

## The interrupt controller and the system timer

The MPC5675K's INTC (chapter 32 of the Reference Manual), its System
Timer Module (chapter 51) and its Software Watchdog Timer (chapter 52),
one of each per core as in decoupled parallel mode, each set mapped on
its own core's bus only:

| | core 0 | core 1 | |
|---|---|---|---|
| INTC | `0xFFF4_8000` | `0x8FF4_8000` | `ppc_intc.c` |
| STM | `0xFFF3_C000` | `0x8FF3_C000` | `ppc_stm.c`, INTC sources 30-33 |
| SWT | `0xFFF3_8000` | `0x8FF3_8000` | `ppc_swt.c`, INTC source 28 or 29 ([below](#the-watchdog)) |

**The INTC** is all of chapter 32 that a single core sees: 337 sources,
8 of them software-settable through SSCIR, a 4-bit priority each in
PSR, the current priority in CPR with the 14-entry LIFO behind it --
which overwrites its oldest entry when pushed full and pops zeros when
empty, 32.4.2.2 -- and both handshakes. In **software vector mode** the
core takes the external input through IVOR4 and the handler's read of
IACKR is the acknowledgement: it pushes CPR, installs the request's
priority and negates the request. In **hardware vector mode** the
core's taking the interrupt is the acknowledgement, and it branches to
IVPR[0:15] || vector << 4 (the core manual's table 7-16) instead of
IVOR4. EOIR pops. Its request sits beside the platform's own external
line (`set_irq`), which keeps its old edge-like behaviour.

**The STM counts the core's clock**, one tick per instruction -- the
same clock HID0 can give the time base -- divided by CPS + 1, and a
channel matches when the counter *reaches* its compare value. Its next
match, and the watchdog's next expiry, are published to the core as
one word (`soc_until`), so the stretch the interpreter runs ends on that
instruction, exactly as it ends on the
decrementer's expiry, and a core in `wait` is carried to it. Reading
STM_CNT syncs the clock first, as reading the time base does.

Where the manual says nothing, the files state the reading taken: a
request stays asserted until acknowledged, with the vector it asserted
with ("no spurious vector support", 32.4.1), and only a CPR write
re-decides it, which is what the priority ceiling protocol needs; an
IACKR read with no request has no side effect; writing CNT onto CMP
matches nothing; CIF is the request whatever CEN says. And one place
where the manual contradicts itself: two sentences have *SSCIR* writes
popping the LIFO, against EOIR in 32.3.2.4, 32.4.2.2 and both example
handlers, read here as a slip for EOIR.

**Translated, a device read is a chain behind.** A chain of blocks
carries its retired count in a host register until it returns to the
dispatcher, where it becomes cycles, so a load from STM_CNT inside a
chained loop sees the clock as of the chain's start -- up to
`EMU_JIT_LOOP_CAP` iterations, 128. Interpreted it is exact. A match
still fires, a chain late rather than on its instruction: from a
2,000-tick compare to the ISR reading the counter, 41 instructions
interpreted against 97 translated in `ppc-intc`. The time base has no
such lag only because `mfspr` is one of the instructions the translator
gives back. Making a translated device read exact would mean every
helper call storing that register, on both hosts.

**On the F746** `ppc-intc` passes on both backends with the host's
retired counts to the instruction, 107,835 interpreted and 107,923
translated, and so does `ppc-swt`, reset included, at 43,119 and
43,315 -- so the Thumb-2 path agrees with x86-64's.

**What they cost a guest that does not use them**, by callgrind on
PowerPC CoreMark, the same 12,170,464 instructions either way: +0.035%
host instructions interpreted and +0.52% translated, the STM and INTC
+0.017% and +0.44% of that and the SWT the rest. The translated cost is
`ppc_jit_count` comparing one more word after every chain -- one word
however many devices there are, which is why the STM and the SWT
publish a single `soc_until` -- and it is what makes a translated loop
stop at a match or an expiry at all.

**Not in the DTCM, by measurement.** The F746's DTCM is full, and in a
JIT build the INTC and STM land just past it, behind the D-cache; in an
interpreter build they are inside it already, by link order. On a
scratch workload that is nearly all STM and INTC -- an interrupt every
200 ticks re-arming its channel, then 400,000 polls of STM_CNT -- moving
them into `.dtcm` measured 98.66 host cycles an instruction against
99.24 for a control that shifts `.bss` by the same 1 KiB
(`EMU_LAYOUT_PAD_BSS=1024`) and moves nothing, and 98.91 as built: 0.6%,
of the order of the shift alone and far inside the 10% placement moves
on this part. A kilobyte touched that often stays in the D-cache; what
costs is the bus's slow path per device access, not where the state
sits. Interpreted, all three were 273.6. The DTCM's bytes stay with the
decode cache and the JIT's tables.

### The watchdog

The SWT is chapter 52 whole: the down-counter loaded with SWT_TO (at
least 0x100) on enabling and on service, expiry a system reset or with
ITR an interrupt first and a reset on the second consecutive one, the
fixed and the keyed service sequences -- the key generator held to
52.4's own example, SK 0x0100 giving 0x1103 then 0x2136 --, window
mode, the soft and hard locks, SWT_CO, and the invalid accesses: a
narrow access, a reserved address, a write to a locked or read-only
register, an incorrect service value while enabled, a service outside
the window. Each is a bus error, or with RIA and WEN a reset.

**A system reset stops the core, and the run says so**: `emu: core 0
stopped: system reset by SWT_0 timeout`. That is
`emu_cpu_status_t::reset_by`, new for this and generic -- ARMv7-M's
SYSRESETREQ, which used to halt in silence, names itself through it
too. There is no reset generation module to restart the part, and a
guest that started again silently would hide the one event a watchdog
test is about.

Readings, each stated in `ppc_swt.c`:

- **The service sequence is 0xA602, 0xB480.** Table 52-7 says so and
  gives 0xC520, 0xD928 as the unlock; SWT_CR[KEY]'s description and a
  paragraph of 52.4 give 0xC520, 0xD928 for the service too. The same
  paragraph says the service logic "ignores unlock sequence writes",
  meaningless if they were the same values.
- **Off out of reset.** Table 52-2 has WEN set and 52.4 calls it
  device-specific; on this part a flash option decides it for SWT_0,
  and SWT_1 "is always off until enabled by software". Off here, or
  every existing guest would be reset 261,600 instructions in.
- **Both of CSL's clocks are the processor clock**; there is no
  oscillator.
- **Each SWT requests through its own core's INTC.** Table 32-10 gives
  both timeouts, 28 and 29, to both INTCs (the page has to be read as
  an image: the text layout loses which column the cell spans).
  Reaching the other core's INTC would need a lock between cores.

`tests/unit/test_ppc_soc.c` holds the three devices to the manual
register by register, 486 checks; `ppc-intc` (49 checks) and `ppc-swt`
(14, then the reset) run them under a core, interpreted and translated.
`ppc-swt` passes only if `emu: core 0 stopped: system reset by SWT_0
timeout` follows its last line *directly*: the first pattern allowed
anything between, and a watchdog left out of the stretch bound reset the
guest only after it had printed `FAIL still running` -- and passed.

**And both must give the same run at any quantum**, which ctest checks
as a property (`tests/guest/same-at-any-quantum.py`, at 509, 4096 and
65536). The first `ppc-swt` failed it: the watchdog's register writes
recomputed `soc_until` without ending the interpreter's stretch, so a
deadline moved *earlier* -- enabling it -- was noticed only when the
stretch ran out by itself, and the reset landed at retired 42,924 at
`--quantum 512` and 43,000 at 4096. The STM's writes did end it; now
`ppc_cpu_soc_changed` does, for every device. A latency window did not
find this: it passes whenever the slice boundary happens to fall
inside the window -- `ppc-intc` caught the mutant at 4096 and missed it
at 512 -- and the watchdog's reset, where it showed, is past the end of
anything the guest can print.

Each mechanism was broken on its own to see that something fails:

| broken | unit | `ppc-intc`, interpreted | translated |
|---|---|---|---|
| the core's stretch bound ignores the STM | -- | `match-latency`, `wait-latency` | the same |
| the clock never reaches the STM | -- | hangs in the `wait` | hangs |
| no hardware vector: IVOR4 always | -- | never finishes | never finishes |
| the INTC's request not consulted | -- | hangs | hangs |
| a match only when the counter lands on CMP | 1 | -- | 10 |
| `until` ignores the prescaler's count | 2 | -- | -- |
| a device write does not end the stretch (`ppc_cpu_soc_changed`) | -- | `match-latency` at 4096 only; `-quanta` fails | -- |
| equal priorities to the higher vector | 1 | `ties-*` | `ties-*` |
| the LIFO does not saturate | 1 | -- | -- |
| an IACKR read does not acknowledge | 49 | never finishes | never finishes |
| a CPR write does not re-decide | 2 | -- | -- |
| EOIR does not pop | 20 | hangs | hangs |

| broken | unit | `ppc-swt`, both backends |
|---|---|---|
| the service sequence is the unlock's codes | 18 | fails |
| ITR ignored: every timeout resets | 9 | fails |
| the clock never reaches the SWT | -- | hangs |
| the stretch bound ignores the SWT | -- | fails: the reset comes late |
| the window inverted | 5 | -- |
| the key generator off by 2 | 7 | -- |
| RIA resets with WEN clear | 7 | fails |
| the locks ignored | 5 | `locked-write-faults`, `locked-to-unchanged` |
| SWT_CO shows the counter while enabled | 1 | -- |
| enabling does not load the counter | 19 | `serviced-no-irq` |
| WTO below 0x100 used as it is | 3 | -- |
| TIF requests the wrong source | 4 | fails |

Each was a single-point change, asserted to match once and to relink
both binaries; the tree restored passes all three columns.

## The embedded floating-point unit

`ppc_fpu.c` is EFPU2 in mode 0, the only mode the core has: single
precision in the low word of a GPR, and a *default result* for every
input that is not a normal number — an infinity or NaN operand gives
the largest magnitude, a denormal counts as a zero of its sign, overflow
gives the largest normal and underflow a signed zero, each decided after
rounding with an unbounded exponent (tables 5-2 to 5-5).

It computes in SoftFloat's double precision toward zero, takes its
sticky bit from the inexact flag, and rounds to single itself. SPEFSCR
is whole: the per-instruction status, the sticky bits, FG and FX, the
enables and the rounding mode. An enabled invalid, divide-by-zero,
overflow or underflow **suppresses** the instruction and takes the data
interrupt (IVOR33, SRR0 the instruction); an enabled inexact writes the
result **truncated** and takes the round interrupt (IVOR34, SRR0 the
next instruction). MSR[SPE] does not gate the scalar instructions.

`tests/ppc-check/efpu.py` computes the same operations in **rationals**
— Python's `Fraction`, which does not round — and rounds once, itself.
The two share the manual and nothing else. Every vector compares the
result, the whole of SPEFSCR, which interrupt was asked for and, for a
compare, the CR field: 1.36 M vectors over three seeds, 0 wrong
(`ctest` runs 74,048, `ppc-efpu-vs-exact`). `ppc-fpu`
(`tests/tools/ppc_fpu.c`) is the unit as a command.

The first run reported 40 wrong square roots, and the emulator was
right: the *model's* scaling was too coarse to round correctly. An
oracle is code too.

### What uses it, and what it costs

**Nothing compiled used the unit until clang.** GCC dropped its SPE
back end in version 9, so the cross compiler here builds soft-float:
every `float` operation a call into compiler-rt. LLVM still has the
back end, and `-mefpu2` is exactly this core's half of it -- single
precision in the GPRs, `double` through the same compiler-rt calls,
because EFPU2 has none. Plain `-mspe` would emit `efd*`, which this
core does not have. Its calling convention is the soft-float one, so a
clang-built guest links the GCC-built picolibc and compiler-rt
unchanged ([games.md](../games.md#quake-on-powerpc)):

```
clang --target=powerpc-unknown-eabi -mcpu=e500 -mspe -mefpu2
```

Quake built that way has 5,421 EFPU2 instructions and no `efd*` or
`ev*`. Sampled mid-demo, soft-float routines went from **63.2%** of what
the guest executed (`__addsf3` alone 35.1%) to **0.1%**.

**The C library has a build for the unit too.** Quake calls little of
libm; Whetstone is libm -- its modules 7 and 11, the transcendental
ones, are nearly all of its run -- and against the GCC-built picolibc
it would measure compiler-rt. `scripts/ppc-build-runtime.sh` builds
picolibc a second time into `efpu2/`, its C by clang with `-mefpu2`
and its assembly by GCC as before: `setjmp.S` uses a `bclr+` clang's
assembler refuses, and under the `__SPE__` clang defines it would save
the registers with `evstdd`, which this model raises as illegal. EFPU2
uses only the low half of a GPR, so the 32-bit save is the right one.
The clang half has 1,917 EFPU2 instructions and no `ev*` or `efd*`.

`ppc-whetstone` links it, and its per-module digest at LOOP 10 -- four
digits of each module's four results -- is identical to RV32's under
both backends of each (`whetstone-digest-rv32-ppc`): two FP
implementations in this emulator, through two compilers' builds of one
libm. Four digits cannot see an ulp. Of three defects in the integer
fast path the digest fails two -- a sum taking the smaller operand's
sign, a quotient's exponent off by one -- and passes an inexact quotient
rounded as exact; `ppc-efpu2` fails all three.

That moved the cost to the host, and three changes took it back,
measured on Quake under the x86-64 JIT -- callgrind over the same 300 M
guest instructions, and frames a second over 4e9:

| | host instructions | frames/s |
|---|---|---|
| GCC soft-float | -- | 6.6 |
| EFPU2, through the generic step helper | 9.14 G | 11.3 |
| ...its own helper | 8.08 G | 13.2 |
| ...exact results in integers | 7.48 G | 14.0 |
| ...zero addends too | -- | 14.3-14.6 |

RV32 runs the same demo at 17.3 on the same host.

- **Its own helper.** A block reached the unit through `ppc_ir_step`,
  which decodes the word again and passes through the interpreter's
  switch: `ppc_decode` alone was 11.3% of host instructions.
  `ppc_ir_efs` takes the fields the translator already decoded, packed
  into one constant, and calls `ppc_fpu_exec` -- the `PPC_S_EFS*` case
  of `ppc_exec` with the decode taken out, raising the same two
  interrupts. Swapping either interrupt's address fails `ppc-sys` under
  the JIT; dropping `crf` or reading `rb` as `ra` fails the
  interpreter-against-JIT model.
- **Exact results in integers.** SoftFloat was asked for the exact
  result truncated to double, and whether anything was lost below it --
  two conversions and a general double operation, about four hundred
  host instructions an operation. For two normal singles that value is
  small enough to have in integers: a product of 24-bit significands is
  48 bits; a sum whose exponents are at most 39 apart is exact in 64,
  the larger significand at bit 62 and the smaller losing nothing until
  it passes the 39 zeros beneath it; a 40-bit quotient and its remainder
  hold every bit rounding reads. Each goes to the same `round_f64`, so
  rounding, FG, FX, overflow and underflow stay one implementation. A
  zero added to anything is the other operand; a sum further apart than
  39 bits, the multiply-adds and the square root keep SoftFloat.

  The oracle's random operands almost never meet what that depends on,
  so `efpu.py` builds **relations** as well: exact and near
  cancellation at equal and adjacent exponents, a carry out of the top,
  exponent differences around 24, 39 and 40, 53 and 64, zero sums under
  every rounding mode, exact quotients, quotients near 1, and inexact
  quotients whose 40 bits are all zero below the guard -- where only
  the remainder says so. Ten deliberate defects in the fast path: eight
  fail it, by 308 to 13,870 vectors, and seven of those also fail
  `ppc-efpu2`, the compiled guest, on both backends. The other two cannot change a
  single-precision answer -- a bound of 40 instead of 39, and ignoring
  what the truncation to 53 bits dropped -- because in both the bits in
  question sit far enough below the guard that no carry reaches it,
  and the smaller addend's leading bit keeps the sticky bit set either
  way.

What is left is the unit's own bookkeeping: `round_f64`, `finish` and
the dispatch are 14.7% of the host's instructions, which is what
modelling SPEFSCR exactly -- status, sticky bits, FG and FX, two
interrupts -- costs, and the reason for having the unit.

## The JIT

`--jit` selects `ppc_backend_jit`, built from `ppc_ir.c` through the
shared IR pipeline ([../jit/README.md](../jit/README.md)). Three
outcomes per instruction, counted in the run's report:

```
-- ppc translator (per instruction translated) --
  lowered  4993  helper 5  declined 56
  helper   TW:5
  declined MTSPR:50 SC:2 RFI:2 MFSPR:2
```

- **lowered** to IR;
- **helper**: `ppc_exec`, called from inside the block, which keeps the
  block whole — the multiple moves, the reservations, `tw`, `dcbz` and a
  handful of carrying forms; the FP unit's instructions go to
  `ppc_ir_efs`, which calls the unit with the fields already decoded
  ([above](#what-uses-it-and-what-it-costs));
- **declined**: the block ends and the interpreter runs it from the
  dispatcher. A short list and a deliberate one — whatever changes MSR
  (`mtmsr`, `wrtee`, the `rfi` family), takes an interrupt on purpose
  (`sc`, an illegal encoding), reads or writes an SPR other than LR, CTR
  and XER, waits, changes the TLB, or says code moved (`icbi`). That
  makes the interpreter the single place those happen. On the identity
  map the context is the encoding and nothing else; under a programmed
  TLB it carries what the code was fetched under too, and a TLB write
  retires what it mapped -- see the MMU section.

CR and XER are guest registers, and an instruction that writes a CR
field or XER[CA] says so in IR — compares, shifts and an insert; nothing
is lazy. The ARMv7-M frontend built the lazy alternative first and
measured it at under 4%.

**Byte order.** The window a backend may access directly is bytes on a
little-endian host, so an IR `LOAD` here means *the bytes as the host
reads them* and the translator follows each with a byte swap — except
`lwbrx` and its relatives, whose whole meaning is that there is none.
The two functions a backend calls outside the window keep the same
contract by swapping what the bus gave them. One rule on both paths.

### What it is worth

x86-64 host at ce02bd0, `Release`, medians of three, same retired
counts and checksums on both backends:

| guest | instructions | interpreter | JIT | |
|---|---|---|---|---|
| CoreMark, 600 iterations | 182,243,221 | 3.76 s | 0.58 s | 6.5x |
| crypto (AES-128, SHA-256) | 2,422,217 | 62 ms | 18 ms | 3.4x |

The runner's start-up is 6-7 ms, which is most of crypto's JIT row --
less it, about 5x. CoreMark used to be quoted at 40 iterations, 12M
instructions and 43 ms, where start-up was a sixth of the figure; it
shares `ARMV7M_COREMARK_ITERATIONS` with the ARMv7-M guest.

On the Nucleo-F746ZG it depends on whether the translated working set
fits the code cache. Host cycles per guest instruction at ce02bd0, with
the number of translations the run needed (CoreMark at 40 iterations,
`crcfinal 0x65c5` and validated in every row):

| | CoreMark | crypto |
|---|---|---|
| interpreter | **212.29** | **282.49** |
| JIT, 32 KB | 263.95 (35,487) | 336.76 (10,559) |
| JIT, 64 KB (the default) | 148.35 (18,688) | **29.57** (362) |
| JIT, 96 KB | **53.63** (5,077) | 29.14 (347) |

**At the 64 KB default the JIT is 1.4x the interpreter on CoreMark and
9.6x on crypto; at 96 KB, 4.0x and 9.7x.** The default is set by RV32,
whose CoreMark is as fast at 64 KB as at 96, and the guest's 32 KiB was
chosen over PowerPC CoreMark: build `-DEMU_JIT_CODE_BYTES=98304` to run
it at 53.63. At 32 KB the JIT is 1.24x and 1.19x *slower* than
interpreting, where it was 1.5x and 2.9x when 32 KB was the default.
Crypto fits from 64 KB. CoreMark is still being evicted at 96 KB, with
97,524 of 98,304 bytes in use. The
interpreter's crypto figure is 282.49 where it was 243.4, and this page
has not apportioned that; [On the F746](#on-the-f746) records crypto
moving 12% with nothing but where `.bss` lands.

This is not a PowerPC result — RV32 CoreMark loses to its interpreter at
32 KB on the same board too ([../jit/tuning.md](../jit/tuning.md)) — but
PowerPC's guests translate to more code: about 70 bytes per guest
instruction on x86-64, a quarter to a half of it memory accesses and
much of the rest condition-register arithmetic.

### What it still interprets

`-DEMU_PAIR_STATS=ON` counts executed instructions by kind and adjacent
pairs, on the interpreter -- and under `--jit` that is exactly the set
the translator handed back. `ppc_pairstats.c` is the table that reads an
e200 encoding for it, over `ppc_decode` and per semantic class, so the
16-bit VLE forms land on the same register roles as the Book E ones.
x86-64 JIT:

| guest | retired | interpreted | what |
|---|---|---|---|
| CoreMark | 12,170,464 | 2,462 | `mtspr` 1,240, `rfi` 1,215: the decrementer's interrupt |
| crypto | 2,422,217 | 514 | the same two |
| sys | 15,209 | 2,194 | `mfspr` 1,687 -- the SPRs under test |
| mmu | 25,427 | 4,496 | **everything after the TLB is programmed** |

The last row was the JIT standing aside from the first `tlbwe`, which it
no longer does -- see [the MMU](#the-mmu). Interpreted, CoreMark's adjacent pairs are
26.1% data dependent (6.7% with a dead intermediate) and 2.6% address
generation feeding an access, which is RV32's shape.

**Blocks are capped at 16 instructions on a microcontroller and 64 on a
host**, and that was measured rather than argued: while the cache is
thrashing a long block is the expensive one to lose. At 32 KB the
shorter cap took crypto from 1,567 cycles per instruction to 716 and
CoreMark from 393 to 335; where the working set fits (crypto at 96 KB)
it costs 6.6%. The table is in `ppc_ir.c`.

## Guests

`tests/guest/ppc/`. One is assembly and VLE; the rest are C and Book E,
because **GCC has no VLE code generator** (`-mvle` is rejected by
Debian's `powerpc-linux-gnu-gcc`) while the assembler and `objdump`
handle VLE fine. They are built `-m32 -mbig-endian -mcpu=8540
-msoft-float` and linked without a C library.

A guest that wants one links picolibc and LLVM compiler-rt's builtins,
which `scripts/ppc-build-runtime.sh` fetches and builds -- the cross
compiler ships no 32-bit `libgcc`, and neither library is GPL. DOOM is
the first such guest, and the first big-endian target its port has had
([games.md](../games.md#doom-on-powerpc)); Quake is the second, and the
first compiled with clang so that its `float` uses this core's FPU.
Dhrystone and Whetstone are the ones in this tree: a configure builds
them when `PPC_RUNTIME` (default `build/ppc-runtime`) holds a runtime,
and says it skipped them when it does not.

| Image | Purpose |
|---|---|
| `isatest.bin` | hand-written **VLE**: the 16-bit forms, the `e_` immediates, the multiple moves, a VLE-only illegal encoding. Exit status is its failure count, and it must print `PPC-ISATEST-END` |
| `ppc-alu` | compiled C against values from Python's integers (`alu_want.py`): the forms a compiler reaches for unasked — `addc`/`adde` chains, `srawi`+`addze`, `rlwinm`/`rlwimi`, `isel`, update forms, `lmw`/`stmw`, `bctr` through a jump table |
| `ppc-sys` | the system level, 184 checks with the manual's section beside each: interrupts, privilege, SPRs, reservations, the timers, `wait`, both FP interrupts |
| `ppc-mmu` | the MMU, 239 checks: the TLB programmed as an MPC57xx start-up does, then translation, address spaces, PIDs, permissions, every TLB interrupt and the MAS registers it leaves, per-page VLE and byte order, invalidation, and the JIT's `jit-` cases |
| `ppc-smp`, `ppc-smp-vle` | the second core, 41 checks: held at reset and released only by the SSCM's key sequence, PIR, atomics and reservations between the cores, a TLB each for data and code, a second sequence ignored; and, built again, a start in VLE. Run nine ways by ctest |
| `ppc-intc` | the INTC and the STM under a core, 49 checks: the access rules, the counter against the processor clock, a match taken through a software-vector handler and a C ISR, preemption and the LIFO both ways round, CPR masking and equal priorities, the hardware vector from a page of its own, and a `wait` only the STM can end |
| `ppc-swt` | the watchdog under a core, 14 checks and then the reset it ends in: the locks and access rules, servicing in time, the initial timeout's interrupt, a `wait` only the watchdog can end, SWT_CO -- and the second timeout, which must stop the run at once |
| `ppc-coremark-tlbmiss` | CoreMark with the TLB filled on demand by a miss handler in `crt0.S`, two refill entries for a working set of five pages: 26,652 `tlbwe` in a run |
| `ppc-park` | a `wait` nothing may end: the run must stop there, and the line after it must not print |
| `ppc-efpu2` | **compiled by clang for the FP unit**, the only guest that is: the four operations and three compares over every pair of 22 values chosen for EFPU2's defaults and the fast path's edges, conversions both ways, a polynomial, and the sticky bits -- 2,475 checks against `efpu.py`'s model, through `efpu2_want.py`. Built where a clang can emit EFPU2, skipped with a message where none can |
| `ppc-coremark`, `ppc-crypto` | the shared benchmarks |
| `ppc-dhrystone` | Dhrystone 2.1 by GCC, its report's doubles compiler-rt's: 577 instructions a run, where RV32 takes 472 and ARMv7-M 453 (retired over runs). The time base counts instructions, so "microseconds" are instructions and the figure is the same on both backends |
| `ppc-whetstone` | Whetstone 1.2 by clang for the FPU, against the EFPU2 picolibc: 8,371,269 instructions at LOOP 100, where RV32 with F takes 10,665,816. `ppc-whetstone-digest` is LOOP 10 with the per-module reports |

Every compiled guest links `crt0.S`, which **installs a vector table
that reports**: an interrupt the guest does not expect prints its IVOR,
address and syndrome and exits 99. It also sets the decrementer ticking
every 10,007 instructions, so an interrupt lands in the middle of
compiled code thousands of times a run and the benchmark's own checksum
has to survive it, on both backends.

```sh
./build/ppc/emu-host --load 0x80000000 --max-insn 2000000 \
    build/ppc/tests/guest/ppc/isatest.bin
```

**`--load 0x80000000` is not optional for the flat image, and leaving
it off fails quietly**: loaded at the default it runs, reads every
string from memory that holds none, and exits 0. And mind the path:
`build/*/guest/isatest.bin` is the *RV32* self-test, which this frontend
will execute without complaint.

## How it is checked

| instrument | what it is held against |
|---|---|
| `check.py` | binutils: every encoding, both directions |
| `cases.py` | a Python model on unbounded integers, the interpreter and the JIT, three ways |
| `efpu.py` | exact rational arithmetic |
| `ppc-alu` | Python's integers, through a compiler |
| `ppc-sys` | the manual, section by section |
| `ppc-mmu` | chapter 10 and tables 7-13 to 7-24, and memory the guest wrote itself |
| `ppc-coremark`, `ppc-crypto` | their own checksums — CoreMark's CRCs, FIPS vectors — identical across backends and to RV32's |
| `whetstone-digest-rv32-ppc` | RV32's F extension and its libm, to the four digits Whetstone prints |
| `tests/unit/test_ppc.c` | hand-assembled words |

`cases.py` generates instructions with their operands — a quarter from
a table of special values, because a random 32-bit number is almost
never where carry, overflow or a shift by 32 live — runs each image on
both backends, and requires the two outputs identical and equal to the
model. It also requires **block entries**: a JIT that declined
everything would agree with the interpreter perfectly. `ctest` runs
3,000 per encoding (`ppc-model-interp-jit`); 48,000 more across eight
seeds agree, as do 100,000 from an earlier tree.

**Each instrument was itself tested, by breaking the thing it checks.**

| deliberate defects | caught |
|---|---|
| 44 in the translator and the interpreter, against `cases.py` | 44 |
| 30 in the FP unit, against `efpu.py` | 28; the other two change nothing observable |
| 22 in the system level, against `ppc-sys` | 21; the other wakes a `wait` with nothing pending |
| 37 in the MMU, the fetch path and the JIT's gate, against `ppc-mmu` | 37, after one was found surviving (below) |
| 8 in the SETPC rule of the shared optimiser, against the unit tests | 8 |

Several first passed everything, and the directed cases exist because
of them: a register equal to the immediate it is subtracted from (the
one input that tells a carry computed as `>=` from one computed as `>`),
`INT_MIN / -1`, a zero divisor, equal compares.

The mid-block faults in `ppc-sys` exist for the same reason. Every
fault it took was the first instruction of a stub, where a block starts
and the pc is right by construction — so a translator that recorded the
*block's* address for a fault passed all of it. It now takes a load, a
store and a trap two instructions in, and checks SRR0 and a register
the block wrote first.

## Firmware

```sh
cmake -B build/f746-ppc -DEMU_PLATFORM=stm32f746 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_PPC=ON -DEMU_NET=OFF \
      -DPPC_GUEST=alu
cmake --build build/f746-ppc --target flash
```

`PPC_GUEST` is `alu`, `sys`, `mmu`, `coremark` or `crypto`. The image is
embedded as a **stripped ELF**, not a binary, because the encoding is a
flag on its segment that a binary has nowhere to carry; stripped,
because the debug sections were four fifths of the file. The guests take
their stack from the loader rather than from the link script, since the
top of RAM is a megabyte on a host and a fifth of that here.

Run on the Nucleo-F746ZG, on both backends: `alu` 68 checks, `sys` 184,
`mmu` 150, CoreMark `crcfinal 0x65c5` and crypto `0x60ae7a25`, each with
the retired count the host gives — `mmu` with the same 4,496
instructions falling back under the JIT — and 600 generated cases, line for line
identical to the host under the Thumb-2 JIT.

That last run is what found a defect in the *shared* JIT framework: a
compaction caused by a buffer overflow unlinked no chained exits, and
the survivors were moved with jumps into evicted blocks. See
[../backend/thumb2.md](../backend/thumb2.md).

## Lessons, and how each was found

The first program this frontend ever ran found four defects that 353
passing unit checks had not, and the rest of the list has the same
shape: an instrument that could not see the thing it was pointed at.

**A mode flag whose only writer is a unit test is not a mode flag.**
`vle` defaulted to false and the sole assignment in the tree was a unit
test reaching into the struct, so the whole 16-bit half of the
interpreter was unreachable by any guest. Grep for the *writers*.

**Enumerate the slot, against the assembler.** Two groups had holes, and
in both the missing entries sat *between* implemented neighbours —
`se_cmpli` between `se_addi` and `se_subi`, `se_srawi` between `se_srwi`
and `se_slwi`. And `e_add2i.` is XO 0x11, not the 0x10 the group's start
suggests, so a guessed base shifts every entry by one.

**A two-bit field read as one bit aliases half the instructions onto the
other half.** `e_bc`'s BO32 selects true, false, `bdnz` and `bdz`; only
its low bit was read, so a counted loop never terminated.

**Adding a table entry must not replace its neighbour.** IVPR was
missing from the SPR table, and the edit that added it overwrote ESR's
slot. `mfspr rN, ESR` then raised an illegal instruction — inside the
interrupt handlers, which recursed. Found by `ppc-sys`, not by the test
that had wanted IVPR.

**A capability answer that defaults to "no" is a silent decline.**
x86-64's `emu_ir_can_lower` did not name the byte swaps, the leading-zero
count or the high multiplies, so the translator routed 165 of 1,332
instructions to the helper while every test passed. The stats line is
what showed it.

**An expectation written from the shape of a number will disagree with a
correct implementation.** `0xFFFFFF9C / 7` is `0x24924916`; the test said
`0x24924924`, a plausible repeating pattern.

**Give the guest a vector table on day one.** With IVPR and the IVORs
zero an interrupt vectors to address 0, the guest's own entry point, and
every defect presents as "runs to the instruction cap with no output".

**A test declared is not a test that can run.** `ctest` in a
PowerPC-only tree queued three RISC-V tests, gated on the guest *image*
existing rather than on the frontend being compiled in.

## Not done

- **Nothing has been compared with an e200.** Where the manual leaves a
  choice — a negative value converted to unsigned, for one — the code
  documents the choice it made, and the checker agreeing there is two
  readers of one sentence.
- **No VLE compiler**, so compiled code is Book E and VLE is covered by
  the decoder sweeps, the generated cases and hand-written assembly. An
  NXP S32DS or CodeWarrior build would do for VLE what CC-RH did for
  G4MH.
- **On a microcontroller the JIT loses to the interpreter at the default
  cache**; see the table above. That is a property of the shared
  framework's sizes more than of this frontend.
- **A spurious wake from `wait`** — an interrupt that arrives and is
  masked again before delivery — is not tested.
- **No pair statistics.** The gdb target exists; it does not describe
  the SPE upper halves, which gdb wants alongside a 64-bit `acc` the
  stub cannot carry, nor the SPRs.
- **Only the STM and the SWT request through the INTC.** Its other
  300-odd sources are peripherals this model does not have; lockstep
  mode, where one set of the three serves both cores, is not modelled;
  the SWT reaches only its own core's INTC (above); STM_CR[FRZ] and
  SWT_CR's FRZ, STP and MAPn are held and do nothing.
- **A translated STM read is a chain behind**
  ([above](#the-interrupt-controller-and-the-system-timer)).
- **Dhrystone and Whetstone need a runtime from outside the build**,
  so a tree where `scripts/ppc-build-runtime.sh` has not been run skips
  them, and their ctest entries with them.
