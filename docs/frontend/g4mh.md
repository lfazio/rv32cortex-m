# The G4MH frontend

`include/g4mh/` and `src/frontend/g4mh/` — Renesas RH850 G4MH, the second
frontend, which exists mainly to keep `emu_cpu_ops_t` honest: if a third
ISA needs something neither has, it belongs in the contract rather than
in a platform `#ifdef`.

## Running and debugging one

```sh
cmake -B build/g4 -DEMU_PLATFORM=host -DEMU_FRONTEND_G4MH=ON
./build/g4/emu-host --frontend g4mh --load 0x80000000 tests/guest/g4mh/guest.bin
./build/g4/emu-host --frontend g4mh --jit --load 0x80000000 tests/guest/g4mh/guest.bin
```

`tests/guest/g4mh/` holds a CC-RH-built guest and its listing, so the one
cross-check this frontend has -- interpreter against JIT -- is
reproducible from the tree. `scripts/report-figures.sh` runs both and
prints them side by side.

`--gdb` works: `src/frontend/g4mh/g4mh_gdb.c` supplies the target
description, and the register layout was taken from gdb rather than
inferred (`maint print registers` under `set architecture v850:rh850`).
Two things about it are worth knowing before trusting a session -- gdb
carries `fp` as a *raw* register that the architecture does not have, so
it is reported as the EABI's r29; and gdb's v850 backend **rejects**
target-supplied registers, so its built-in numbering is the contract and
the served XML documents it without being able to enforce it.
`test_gdb_layout` is what holds the two together.

---

**There is no reference model.** RV32 has riscv-arch-test, the Berkeley
suite and Sail to disagree with. G4MH has hand-written unit tests, a
second *encoder* (CC-RH), and a compiled guest — and nothing at all that
will tell you an **answer** is wrong. Treat any G4MH semantic result as
verified only as far as `tests/unit/test_g4mh.c` reaches.

Runs on a host and as firmware. What it *backs* of the U2B6's memory map
is a build setting -- 128 KiB of cluster RAM and 64 KiB of local RAM by
default, and code flash supplied by the platform rather than allocated.
See [../memory.md](../memory.md).


**Implemented.** Formats I and II (the 16-bit reg-reg and imm5 ALU),
III (`Bcond disp9`), IV (`SLD`/`SST` .B/.H/.W through EP), V (`JR`/`JARL
disp22` and `disp32`), VI (the imm16 ALU group), VII (`LD`/`ST` .B/.H/.W
`disp16`), VIII (the memory bit ops), `MOV imm32`, and the Format X
system group: `LDSR`, `STSR`, `TRAP`, `EIRET`/`FERET`/`CTRET`, `HALT`,
`DI`/`EI`, `CLL`, the register-form shifts, `MUL`/`MULU`, `DIV`/`DIVU`,
`SETF`, `CAXI`, `LDL.W`/`STC.W`, the swap and bit-search group.

Plus the set a compiler actually emits, added later: `PREPARE`/`DISPOSE`
with the full list12, `CALLT`, the unsigned loads `LD.BU`/`LD.HU` and
`SLD.BU`/`SLD.HU`, the branchless `CMOV`/`ADF`/`SBF`/`SASF`, `MAC`/`MACU`,
`BINS`, `ROTL`, `LOOP`, `PUSHSP`/`POPSP`, `JARL [reg1], reg3`, and
`JMP disp32`.

And the set a *compiled* guest needs, which is a different set again --
found by running one rather than by reading the manual: the three-operand
register shifts `SHR`/`SAR`/`SHL reg1, reg2, reg3`, the high-speed divides
`DIVQ`/`DIVQU`, the halfword divides `DIVH reg1, reg2, reg3` and `DIVHU`,
and the imm9 multiplies `MUL`/`MULU imm9, reg2, reg3`. Single-precision
floating point is implemented on SoftFloat behind `G4MH_EXT_FPU`.

Then the slots that had to be *enumerated* rather than extended:
`CLIP.B`/`.BU`/`.H`/`.HU`, the narrow atomics `LDL.BU`/`LDL.HU` and
`STC.B`/`STC.H`, `FETRAP`, `SYSCALL`, and `CACHE`/`PREF` as no-ops.
`RESBANK` is decoded and reports RIE, because register banks are not
modelled and running it as `DI` -- which is what decoding on reg2 alone
did -- is worse than saying so.

Everything else raises `G4MH_EXC_RIE` -- which is the correct report for
an unimplemented encoding **only if something catches it**. See the entry
below on what RIE actually does in a flat guest, because for the whole
life of this frontend it did not report anything at all.

**Not implemented, roughly in the order a real guest would miss them:**

| gap | why it matters |
|---|---|
| `LDM.MP` / `STM.MP` | **blocked on the MPU, not on the encodings.** They transfer `MPLA`/`MPUA`/`MPAT` entries, and no access check here consults those registers — so executing them would move guest memory into registers nothing enforces, which is worse than RIE. A guest configuring protection would get silence instead of a report |
| `RESBANK` | needs the register banks modelled. Decoded and reports RIE rather than being mistaken for `DI` |
| `DIVQ`/`DIVQU` with `reg2 == reg3` | the manual leaves the flags undefined there; this treats it as the ordinary case |

**`PREPARE list12, imm5, imm32` is implemented** — the ISA's only 64-bit
encoding, and the last thing the length decoder could not reach.
`g4mh_insn_is_64` answers whether a *fourth* halfword follows, which
only `ff = 11` in this one instruction does.

Its encoding came from CC-RH, which had never been asked, because the
checking script capped its own listing regex at twelve hex digits and
so dropped every 64-bit line — **the fourth width that script has
silently swallowed**:

```
prepare 0x3, 4, 0x12345678  ->  82 07 7B 00 78 56 34 12
                                w0     w1     w2    w3
imm32 = (w3 << 16) | w2
```

One thing to know before writing a test against it: CC-RH's `imm5`
operand is a **byte** count and the field holds words, so
`prepare 0x3, 4` encodes 1, and `prepare 0xFFF, 31` warns "immediate
must be a multiple of 4" and encodes 7.

**Format XIV — the disp23 loads and stores, and `LD.DW`/`ST.DW` — are
implemented, in both backends.** They had been listed here as two
separate gaps, the second of them blocked on a claim that turned out to
be about this repository's own tooling rather than about CC-RH:

> `LD.DW` / `ST.DW`: CC-RH's assembler does not accept the mnemonics.

It accepts them. `scripts/g4mh-check-encodings.sh` was matching listing
lines of 4 or 8 hex digits with a numeric line-number column, and a
48-bit form is 12 digits on a continuation line marked `--`, so the
whole width came back empty — which is indistinguishable from an
assembler refusing the input. That is the third class of encoding this
script has silently dropped; its comment now enumerates all three.

The field split, which is the part worth writing down, came off the
bytes rather than off the manual's diagram:

```
ld.b 0x123456[r6],r7   ->   86 07 65 3D 68 24

w0   00000 11110x RRRRR      reg2 = 0, reg1 = base
w1   wwwww ddddddd ssss      reg3, disp[6:0], opcode
w2   DDDDDDDDDDDDDDDD        disp[22:7]
```

| `ssss` | op6 `0x3C` | op6 `0x3D` |
|---|---|---|
| `0x5` | `LD.B` | `LD.BU` |
| `0x7` | `LD.H` | `LD.HU` |
| `0x9` | `LD.W` | `LD.DW` |
| `0xD` | `ST.B` | `ST.H` |
| `0xF` | `ST.W` | `ST.DW` |

The manual draws the *aligned* forms with a five-bit opcode and six
displacement bits, because their `disp[0]` is architecturally zero:
`LD.DW` is `dddddd01001` where `LD.B` is `ddddddd0101`. Read as one
rule that bit is `disp[0]` for the byte forms and required-zero for the
rest, and setting it on an aligned form raises RIE — which is the
architectural report for a reserved encoding, and not the misaligned
address exception a uniform reading would eventually produce.

`LD.DW` masks `reg3` even, which the manual states and no assembler can
exercise (CC-RH aligns an odd operand down and warns). It is two word
accesses rather than one eight-byte one, because the caution under
`LD.DW` says no MAE occurs when the address is on a *word* boundary —
and both loads complete before either register is written, so a fault
on the second leaves the first intact. The JIT does the same, in the
same order, for the same reason.

`MOV imm32` is lowered by the JIT as well. It is in the same 48-bit
width and was ending a block at every large constant.

`tests/guest/g4mh/disp23.asm` is the end-to-end check, and the half
that is not this project's own encoder: CC-RH produces the bytes, the
emulator runs them, 8 checks and 0 failures on both backends.

## Floating point

Single **and** double precision, behind `G4MH_EXT_FPU`, on Berkeley
SoftFloat — the same library the RV32 side uses, for the reason
CLAUDE.md records: an FP unit built on the host's own instructions is a
second implementation of semantics that has to be exact, and the two
disagree exactly where the architecture is fussiest.

That this frontend has D where RV32 has only F is not a policy
difference. RH850 keeps doubles in general-register **pairs** — low 32
bits in rN, high in rN+1, N even, the same convention as `LD.DW` — so
the only new machinery is reading and writing two registers instead of
one. There is no separate register file to add.

**The `.D` sub-opcodes are the `.S` ones with bit 4 set**: `0x460`
ADDF.S is `0x470` ADDF.D, `0x448` is `0x458`, `0x420` CMPF.S is
`0x430`. Off CC-RH, and recorded as a fact about the encodings that
*exist* rather than as a rule for generating new ones — the fused
multiply-adds have no double form at all (CC-RH rejects `fmaf.d`), so
`0x4F0` is not FMAF.D and is not decoded. A test asserts that.

The four float-to-integer groups share one `reg1` encoding: the low
nibble selects the rounding (0 nearest, 1 truncate, 2 ceil, 3 floor,
4 whatever FPSR says) and bit 4 selects the unsigned form. Which
widths are involved comes from the sub-opcode — `0x440` single→word,
`0x444` single→long, `0x450` double→word, `0x454` double→long.

Still declined: the half-precision conversions at `reg1` `0x02`/`0x03`
of sub `0x442`, whose storage format has no other use here.

Double precision costs the F746 firmware **9,680 bytes** — 129,560 to
139,240 of text — which is why CMakeLists.txt adds SoftFloat's f64
entry points only when this frontend's FPU is on. An RV32-only build
links none of them.

`tests/guest/g4mh/fpdouble.asm` is the end-to-end check: CC-RH's own
encodings, 8 checks and 0 failures on both backends.

**`CHECK_EQ` in the unit tests casts to `uint32_t`**, so every
assertion about a double compared only its low half. Six tests of the
arithmetic passed with `SUBF.D`'s operands reversed — 2.0 against -2.0
differ in the sign bit, which is the top of the *high* word. `CHECK_EQ64`
exists now; use it for anything wider than a register.

## The disassembler

`g4mh_disasm` takes the whole encoding — up to 64 bits — and its width
in bytes, so every form renders in full. That needed widening
`emu_cpu_ops_t.disasm` and `emu_trace_fn` together, which is why it was
a separate piece of work: it is the shared contract and all three
frontends.

**The encoding is passed by value, not fetched.** Handing the
disassembler the cpu so it can read as far as it likes is more general
and is wrong here twice over: it can disagree with what actually ran,
because the guest may have rewritten those bytes since, and it can
fault — turning a diagnostic into a second failure at the moment
something is already going wrong. The caller has the bytes; it fetched
them to execute them.

`len` is *derivable* from the value on this ISA, so it is not
information the function lacks. It is a **second opinion**: the caller
ran `g4mh_insn_len`/`is_48`/`is_64` to fetch the instruction, and a
disagreement means the caller and the decoder have diverged. That
prints `.short` with both numbers rather than an authoritative-looking
mnemonic.

What it must not do is *guess*, and it did. CLAUDE.md already records
this file printing "confident nonsense"; there was more of it, all in
slots where `reg2 == 0` selects a different instruction:

- **`jr` for the whole `0x3C`/`0x3D` slot** — `LD.BU`, all three
  `PREPARE`s and every `disp23` load and store, with a target computed
  from their operands. A reader chasing that goes looking for a
  control-flow bug in a load.
- **the `disp22` split in the wrong order.** High bits first:
  `w0[5:0]` is `disp[21:16]` and `w1[15:1]` is `disp[15:1]`. This file
  had low bits first, as RISC-V does it — which is the same mistake the
  interpreter records having made and fixed, left uncorrected here. It
  gives a plausible target for a small forward jump and garbage for
  everything else.
- **`mulh` and `mulhi` for `JR`/`JMP disp32`**, which share those two
  slots at `reg2 == 0`.

The rule this leaves: **the disassembler has to make the same
discrimination the interpreter does, in the same order.** It is not a
lookup table over opcodes, because the ISA is not one.

### `JR`/`JARL disp32` could never execute

Found by poking op6 `0x17` while testing the above, and it is the
worst class of defect this frontend records.

`JARL disp32, reg1` is `00000 010111 RRRRR` — reg2 zero, op6 `0x17`,
reg1 the link register, and reg1 zero makes it `JR`. It shares MULH
imm5's opcode and is **48 bits wide**. The interpreter has always known
that and carries a full implementation of both.

`g4mh_insn_len` answers from the first halfword and said 2 bytes for
every op6 below `0x30`. So the second stage never ran, `g4mh_insn_is_48`
— which has handled `0x17` since it was written — was never asked, `w1`
and `w2` read as zero, and the jump went to `pc + 0`. **An infinite
loop, not a wrong answer.** The comment naming the slot was accurate and
load-bearing and the code it described was unreachable.

`g4mh_is_16bit` had the same rule spelled a second way and is now one
line calling `g4mh_insn_len`. A property test asserts they agree across
all 65536 first halfwords — because what makes this a defect is the
duplication, and because nothing downstream can currently see the
divergence: the JIT's translator declines `0x17` for other reasons, so
a wrong answer there changes no result today.

## The inter-CPU peripherals

BARR, IPIR and TPTM are implemented, from the U2B hardware manual
(R01UH0923EJ0130) rather than the software manual — section 3.3 groups
them as one subsystem and they are one piece of work here for the same
reason: they share a shape.

| | base | what it is |
|---|---|---|
| BARR | `0xFFFB_8000` | hardware barrier, 16 channels |
| IPIR | `0xFFFB_9000` | inter-processor interrupts, 4 channels × 6 PEs |
| TPTM | `0xFFFB_B000` | per-PE timer set: 2 interval, 1 free-run, 2 up |

**BarrierSync is not an instruction**, and it is worth saying because it
is easy to go looking for an `HBARR` opcode. There is none: CC-RH
V2.08.00 rejects the mnemonic at `-Xcpu=g4mh`, which is its only valid
setting, and the manual lists BARR beside IPIR and the TPTM as modules
with register maps.

**Each has a *self* region** whose meaning is "the PE doing the access"
— "when PE1 accesses the IPI0REQS register, PE1 can also access the
IPI0REQ1 register". That costs almost nothing here because every core
already has its own bus, so a self region is an ordinary MMIO region
bound to a per-core port naming the PE, and the access path never learns
that multicore exists. It is the third use of that arrangement after
INTC1-self and LRAM-self, and it is what a bus per core buys.

Four rules that are easy to lose, each of which a test pins:

- **A write to `BRnCHKm` sets it whatever the value written**, while
  `BRnSYNCm` takes the value. Arriving at a barrier is a write, not a 1.
- **`BRnEN` has two different rules.** A PE that is not enabled may
  arrive and is ignored; but if the *whole* register is zero, the check
  bit cannot be set at all. Implementing only the first leaves a stale
  arrival on an unconfigured channel with nothing able to clear it.
- **The barrier is a level condition**, so a write to `BRnEN`
  re-evaluates it. Narrowing participation can complete a barrier.
- **IPIR's enable gates the transfer, not the request.** A request
  raised while the receiver has not enabled the sender is remembered in
  `REQ` and never arrives — not even once the enable is written, which
  is why the manual's bring-up sequence clears with `FCLR` first. That
  is the opposite of the barrier's behaviour on its enable, and the two
  sit ten lines apart in the code.

`TPTMSEL` (manual 6.3.15, at `<INTIF_base> + 0x200` = `0xFF09_0200`)
routes each PE's interval interrupt to EIINT31 or to FEINT, and **its
reset value selects FEINT** — so the default path is the FE one. That
needed an FE-level delivery: `g4mh_cpu_pending_fe` honours PSW.NP and
deliberately not PSW.ID, which is the whole point of the level.

**Both backends deliver it, and that was not free.** The JIT has its own
interrupt path, so the FEINT delivery added to the interpreter was
simply absent there — and the JIT is the default backend, so nothing
took an FEINT at all. Identical in shape to the performance-counter bug
(#38), found immediately after it, and caught only because the test
asserts the *cause register* rather than that a handler ran: the zeros
between the vectors run through to the same handler whatever exception
vectored there.

Not modelled, in all three: the guard registers
(`BRGPROT`/`IPIGPROT`/`TPTGPROT`), address EDC and data ECC, the
debug-mode counter stop signals, and the TPTM's global up-timer control
channels (`TPTMGgURUN` and friends — `UTRG` is stored and read back but
setting `GTRGEN` does not detach a counter from the local run
registers). The TPTM's up-timer interrupts are raised on channels
413 and up, which is past `G4MH_INT_CHANNELS` in every configuration
built here, so they set their `UCSTR` flags and the raise is dropped;
that is deliberate and a guest polling the flags sees the timer work.

The TPTM runs off the platform's tick rather than a separate `cpu_clk`,
because the emulator has no separate CPU clock to offer. The dividers
are relative so they behave; a guest computing an absolute period from a
datasheet frequency will be wrong by that ratio.

**The performance counters are implemented, and the bank file was widened
to reach them.** `PMCTRL0-7` and `PMCOUNT0-7` are at selID 14 and
`PMUMCTRL` at selID 11, so with `G4MH_SR_BANKS` at 3 they were not merely
unimplemented -- an `LDSR` to them had nowhere to be stored and
`g4mh_sr_write` dropped it, which reads to a guest exactly like a register
hardwired to zero. The file is 16 banks now: 2 KiB per PE against 384
bytes, flat rather than sparse because the alternative is a switch on
every `LDSR` and `STSR`.

Only two events are sourced -- cycles and instructions retired -- and that
is deliberate. They are the two an emulator can report honestly; cache
misses, branch mispredictions and stall cycles have no counterpart here,
and a guest tuning against a fabricated miss rate would be tuning against
nothing. Channels selecting them are left alone rather than counting
something plausible.

Counting happens **once per run slice, from the retired delta**, not per
instruction in the interpreter. That is not a shortcut, it is the fix for
a real bug: ticking in the interpreter's retire path counts only
*interpreted* instructions, and under the JIT that is a small and
arbitrary subset. The same seven-instruction program counted 5
interpreted and 2 translated -- and 2 is the worse answer, because it is
plausible. `retired` is maintained by both backends, so the delta is the
one quantity already correct either way, and taking it in
`g4mh_ops_run` costs nothing per instruction.

The price is granularity: an `STSR` reading `PMCOUNT` inside a slice sees
the value as of the previous slice boundary, not as of the instruction
before it. `test_perf_counters` asserts that rather than a precision the
implementation does not have.

`test_perf_counters_agree` runs the same program on both backends and
compares. With no reference model, interpreter-against-JIT is the only
cross-check this frontend has, and it is the only kind of test that could
have caught the bug above -- no single-backend run can see it.

**Architectural features not modelled:**

- **The MPU.** `MPLA`/`MPUA`/`MPAT`/`MPM` and the protection checks behind
  them — the analogue of RISC-V PMP. `MIP` and `MDP` are raised today only
  by a bus fault, never by a protection region.
- **User mode.** `PSW.UM` is defined and nothing enforces it: `LDSR`,
  `STSR`, `DI`, `EI`, `HALT` and `RETI` do not check privilege. Note what
  the RISC-V side of this repo learned the hard way — U-mode turned three
  latent M-mode PMP bugs into failures at once. Expect the same here.
- **Coprocessor gating, beyond CU0.** `PSW.CU0` *is* consulted: the FPU
  raises `UCPOP` when it is clear, which is what a part without the
  option does. `CU1` and `CU2` have no unit behind them and are not
  checked.
- **Interrupt priority.** The INTC has a 4-bit priority per channel but does
  not maintain `ISPR` or honour `PMR`, so nesting is not modelled. `INTBP`
  and the table-reference entry method are absent; entry uses the single
  direct vector only.
- **Register banks and hardware context save**, `GMCFG`, the guest modes.
- **Debug level.** No `DBPC`/`DBPSW`, `DBTRAP`/`DBRET` — the analogue of
  Sdtrig.
- **No Thumb-2 JIT.** There *is* an x86-64 one:
  `g4mh_backend_jit` comes from the shared IR framework, and
  `emu-host --jit` selects it. A Thumb-2 translator would be a third
  `emu_backend_t` and is what the firmware would need, since the host
  backend exists for coverage rather than for speed.

  Run both and diff them. With no reference model, interpreter against
  JIT on the same guest is the only cross-check this frontend has --
  which is exactly how the CC-RH guest's `puthex` bug was established as
  shared semantics rather than a translator fault: both backends produced
  the *same* wrong bytes.

**Simplifications to be aware of before trusting a result:**

- The INTC is now the real thing: INTC1 (core-local, channels 0-31) at
  `0xFFFC_0000` SELF and `0xFFFC_4000` PE0, INTC2 (global, channels 32 up)
  at `0xFFF8_0000`, with the EICn bit layout from the U2B manual Section
  6.3. Modelled on the **U2B6**, which has three PEs -- the manual's base
  table runs to PE5 because the larger parts have six. The `OSTM` at
  `0xFFEC_0000` is still a stand-in rather than the real register set.
  What is *not* modelled: `EEIC`, table-reference delivery, and `EIBD`
  is stored but does not route.
- Exception vectors use the compact offsets in `handler_address()`; a real
  part's table is larger and `RBASE`/`EBASE` flag bits are masked off rather
  than honoured.
- Misaligned data accesses raise `MAE`. Most real G4MH parts permit them.
- `PID`, `HTCFG0` and `MCFG0` read as zero rather than identifying a part.

**The encodings are now checked against the manual, and the first pass was
wrong in six places.** `docs/renesas/rh850g4mh-users-manual-software.pdf`
(R01US0209EJ0220) is the authority; it settled that

- `LDSR` and `STSR` use their two register fields in *opposite* senses, so
  implementing one by analogy with the other gets it backwards;
- `JR`/`JARL disp22` carries its *high* displacement bits in the first
  halfword, the reverse of the RISC-V habit;
- G4MH has no `RETI` at all -- V850's single return was split into
  `EIRET`, `FERET` and `CTRET`, which name their level in the opcode
  rather than inferring it from PSW;
- sub-opcode 0x160 is shared by `DI`, `EI`, `PUSHSP`, `POPSP` and `CLL`,
  told apart by the whole reg2 field and not by its top bit;
- `HALT` and `SNOOZE` share 0x120;
- **reg2 == 0 is an opcode extension throughout.** `CALLT` hides in the
  `MOV imm5` slot, `DISPOSE` in `MOVHI`/`SATSUBI`, `MOV imm32` in `MOVEA`,
  `JMP disp32` in `MULHI`, `JR disp32` in `MULH imm5`, `PREPARE` in `JR`.
  Decoding on the opcode alone made six unimplemented instructions retire
  silently as writes into r0 instead of raising RIE -- which is far worse
  than not implementing them, because the guest gets a wrong answer rather
  than a clean exception. Every such slot now tests reg2.

The lesson generalises: **an ISA that reuses a register field as an opcode
extension will not tell you when you ignore it.** Before adding an
encoding, grep the manual for every instruction sharing its opcode, not
just the one being added.

**And it happened again, twice, in the same shape.** Adding the compiler
set found both:

- `SATADD imm5` never tested reg2, so half of every `CALLT` -- the half
  with bit 5 of the vector set, because the opcode's low bit *is*
  imm6[5] -- retired as a saturating add into r0. Discarded, call never
  made, nothing to show for it. Its neighbour `MOV imm5` had the check.
  One slot of a straddling pair being guarded is not the pair being
  guarded.
- **A shared opcode can hold two instructions of different lengths, and
  that is worse than two of the same length.** `g4mh_insn_is_48` answered
  "48-bit" for the whole of `0x37` with reg2 == 0 and for the whole of
  `0x3C`/`0x3D` with bit 0 of the second halfword set. `LOOP` is 32-bit
  and lives in the first; `PREPARE`'s short form is 32-bit and lives in
  the second, alongside 48-bit disp23 loads *and* a 64-bit `PREPARE`. A
  wrong length is not a wrong answer, it is a desynchronised instruction
  stream -- every instruction after it is garbage. The length decoder and
  the execute switch have to make the *same* test, and the only way to
  know is to enumerate everything in the slot including its width.

Both were invisible while the instructions raised RIE, which is the
argument for implementing a slot completely rather than one encoding of
it at a time.

**There is no reference model, but there is now a toolchain.** RV32 has
riscv-arch-test, the Berkeley suite and Sail to disagree with; G4MH still has
nothing that will tell you an *answer* is wrong. Its tests are hand-assembled
halfword arrays in `tests/unit/test_g4mh.c`, deliberately not sharing an
encoder with the interpreter — a shared one would pass while both were wrong
the same way. Treat any G4MH *semantic* result as verified only as far as
those tests reach.

**Encodings are a different matter, and are now checkable.** Renesas CC-RH
builds in the `ccrh:latest` image (`docs/renesas/Dockerfile`), and its
assembler is a second, independent encoder —
`scripts/g4mh-check-encodings.sh` assembles a set of instructions and prints
the fields this frontend decodes them into. **Run it before hand-writing an
opcode constant.**

It found a real bug on its first run. `CMPF.S` carries its condition in the
**reg3 field** and its target CC bit in the sub-opcode's low bits; this
frontend had the two the other way round, inferred from a manual diagram
that draws the field as `0FFFF` and names neither part:

```
cmpf.s 0x4, r6, r7, 3   E7372624   sub=0x426  reg3=4
cmpf.s 0xC, r6, r7, 5   E7372A64   sub=0x42A  reg3=12
```

The second settles it — `0xC` does not fit in three bits. Every
hand-written test passed either way, because they all used fcbit 0, where
the two readings coincide. `CMOVF.S` and `TRFSR` are genuinely the other
way round (fcbit in the sub-opcode, reg3 the destination), which is what
made the wrong reading look plausible.

The compiler is also worth using for what it *chooses*: `-Xcpu=g4mh
-Xfloat=fpu` on `a < b` emits `cmpf.s` / `trfsr` / `setf`, which is the
combination a guest will actually contain, and confirms the operand order
`reg2 < reg1` that the manual states and that is easy to implement
backwards.

## INTC1 / INTC2

Implemented from the RH850/U2B hardware manual R01UH0923EJ0130, tables
6.15 (EICn), 6.16 (IMRm), 6.20 (EEICn) and section 6.4.6 (priority).

**EICn, EEICn and IMRm are three windows onto one per-channel word, not
three registers**, and the manual says so:

- "All bits except EIP[5:4] of EEICn are shared with EICn."
- IMRm registers "are an aggregation of the EIMK bits from the EIC
  registers. Setting of the EIMK bit in the EIC register is reflected in
  this register. Also the setting of a bit in this register is reflected
  in the EIMK bit of the corresponding EIC register."

They were three separate stores here, and they disagreed in the
direction that matters: `imr[]` was written by the guest, **never
consulted when choosing a channel**, and reset to zero against an
architectural `FFFF_FFFFH`. A guest that masked a channel through IMRm
was not masked at all, and one that unmasked through IMRm never got the
host-line callback. The state is now held once, in EEICn's layout
because it is the superset, and the other two are computed.

What that fixed, each confirmed by reverting it:

| | was | is |
|---|---|---|
| IMRm | a dead array, reset 0 | the EIMK bits, reset FFFF_FFFFH |
| priority | 4 bits compared | 6 (EEICn.EIP[5:0]); 16 no longer ties with 0 |
| EEICn | read as zero | the canonical word |
| EICn 16-bit write | cleared EIP[5:4] | preserves them |
| EICT | writable | read-only, as the table says |
| EIOV | never set | set when an edge arrives with EIRF already 1 |
| EIRF on acknowledge | always cleared | cleared only in edge detection |
| IMRn address | — | `+1000H + 04H*n` for n=1..31; 0x1000 is IMR0's slot and belongs to INTC1 |

**Still open, and deliberately so.** The controller now selects the
right channel, but the *core* does not compare its priority against
anything: `ISPR` and `INTCFG` exist as system-register numbers and
nothing reads them, so an interrupt is delivered whenever PSW.ID and
PSW.NP allow rather than only when it outranks what is in service.
Nesting is therefore unmodelled. That is a CPU-side gap rather than a
controller one, and it wants its own change.

Level detection is implemented as a rule -- EIRF read-only, no
acknowledge clear -- but no modelled source sets EICT, so the path is
unreachable today. It is written down rather than left out so that a
level-sensitive device has somewhere to land.

## MPU

Implemented from R01UH0923EJ0130 section 3 "CPU System" -- tables 3.65
(MPM), 3.66 (MPCFG), 3.74-3.77 (MPLA/MPUA/MPAT/MPIDn) and the rules in
3.2.5.1(4) and (5). The virtualization manual R01UH0865EJ0140 gives the
same register map in its table 3.12 and defers every bit layout to the
above, which is why it alone is not enough to build from.

Thirty-two entries, each three registers reached through a **window**:
`MPIDX` selects the entry and `MPLA`/`MPUA`/`MPAT` refer to whichever one
that is. Same shape as the INTC's `EICn`/`EEICn`/`IMRm`, and held the
same way -- the entry array is the state, the system registers are a
view of one element.

An access must satisfy **two independent gates**, and this is the part a
mode-only model gets wrong:

| gate | what it is |
|---|---|
| mode | `SR`/`SW`/`SX` in supervisor, `UR`/`UW`/`UX` in user |
| SPID | `RMPIDn`/`WMPIDn` indexed by which `MPIDn` holds the accessing SPID, or bypassed by `RG`/`WG` |

Both must allow. Treating them as alternatives opens every area to every
master the moment `SW` is set -- the A/B for that is 8 failures.
`RMPIDn` covers *execution and reading together*, so there is no
`XMPIDn`; the mode group still keeps them apart, and an area that is
readable and not executable refuses a fetch.

Other rules worth stating because each has a test that fails without it:

- **`MPM.SVP` clear means "enable all accesses in SV mode"** -- all,
  fetch included, with no entry configured. A supervisor guest that never
  sets `SVP` runs unprotected however its entries look, which is the
  reset arrangement.
- **`MPUA` is inclusive and its low two bits read as 1**, and the *whole
  span* of an access must be inside. An access straddling the top is a
  violation even though its first byte is not.
- **Overlapping areas: permitted by any one of them is permitted.**
  Stopping at the first match would make entry order significant, which
  it is not.
- **Matching nothing denies.** The opposite of RISC-V PMP's M-mode rule,
  and worth stating because this project carries that habit: a guest that
  sets `MPM.MPE` with no entry covering its own code stops immediately.

Violations raise `MIP` on a fetch and `MDP` on an operand access, with
the address in `MEA` -- both already existed.

### What it costs when off

One predicted branch on the fetch path and one per data access, testing
`mpu_active`, which is false until a guest sets `MPM.MPE`. Nothing else
may join it: the RISC-V side measured **9.3% of CoreMark** on an
unguarded second condition in the fetch sequence and had to fold two
flags into one `fetch_guard` to get it back.

The fetch check covers the first halfword only. Every MPU bound is
4-byte aligned (`MPLA` and `MPUA` keep bits 31:2), so an instruction
cannot straddle a boundary that its first halfword is on the right side
of. If an area ever becomes finer than 4 bytes this needs the per-halfword
treatment the RISC-V fetch guard already has.

`-DG4MH_EXT_MPU=0` compiles it out; `-DG4MH_MPU_ENTRIES=n` shrinks the
entry file. Both are C macros with defaults in `g4mh_config.h`, not CMake
options.

**The out-of-range `MPIDX` guard is unreachable at 32 entries** --
`MPIDX` is five bits and every value is then a valid entry -- so it is
tested in a `-DG4MH_MPU_ENTRIES=8` build, where removing it costs 2
failures. The first version of that test asserted nothing at all, being
`#if`-ed out in the default configuration, and the second checked the
wrong neighbour: with the guard removed `mpla[N]` runs off the end and
lands on `mpua[0]`, which the test never read. It now snapshots every
entry and requires that none moved.

### Not implemented

The setting-check function (`MCA`/`MCS`/`MCC`/`MCR`/`MCI`) is stored and
readable but performs no check -- a guest can write the command and will
read back whatever it wrote rather than a result. `MPBK` reads 0 (one
bank, which is what `MPCFG.NBK` reports). The host/guest entry split
(`MPCFG.HBE`) is not enforced, because there is no hypervisor mode here
to enforce it against.
