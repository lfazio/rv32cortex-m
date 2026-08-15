# The G4MH frontend

`include/g4mh/` and `src/frontend/g4mh/` — Renesas RH850 G4MH, the second
frontend, which exists mainly to keep `emu_cpu_ops_t` honest: if a third
ISA needs something neither has, it belongs in the contract rather than
in a platform `#ifdef`.

## Running and debugging one

```sh
cmake -B build/g4 -DEMU_PLATFORM=host -DEMU_FRONTEND_G4MH=ON
./build/g4/rv32-host --frontend g4mh --load 0x80000000 tests/guest/g4mh/guest.bin
./build/g4/rv32-host --frontend g4mh --jit --load 0x80000000 tests/guest/g4mh/guest.bin
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
| double precision, and the `.L`/`.UL` conversions | single precision is there; `G4MH_EXT_FPU` gates the lot |
| the disp23 loads and stores | the 48-bit long-displacement forms; they share the `0x3C`/`0x3D` slot with `LD.BU` and PREPARE and are declined there |
| `LD.DW` / `ST.DW` | the doubleword accesses. CC-RH's assembler does not accept the mnemonics, so their encodings have not been checked against a second encoder and they are *not* guessed at |
| `LDM.MP` / `STM.MP`, `RESBANK` | bank and context-block transfers. `RESBANK` is decoded and reports RIE rather than being mistaken for `DI` |
| `DIVQ`/`DIVQU` with `reg2 == reg3` | the manual leaves the flags undefined there; this treats it as the ordinary case |
| `PREPARE list12, imm5, imm32` | the only 64-bit encoding in the ISA, and past what the length decoder reports — see below |

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
  `rv32-host --jit` selects it. A Thumb-2 translator would be a third
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
