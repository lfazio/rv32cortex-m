# host / g4mh

The Renesas RH850 G4MH frontend on the native runner, on the interpreter.
The only pair where G4MH has actually executed anything.

```sh
cmake -B build/both -DRV32_PLATFORM=host -DEMU_FRONTEND_G4MH=ON
cmake --build build/both
./build/both/tests/unit/rv32-unit     # includes the G4MH tests
```

Reference: [`docs/renesas/rh850g4mh-users-manual-software.pdf`](../../renesas/)
(R01US0209EJ0220) is the ISA authority;
[the U2B hardware manual](https://www.renesas.com/en/document/mah/rh850u2b-hardware-users-manual-rev-130-r01uh0923ej0130)
(R01UH0923EJ0130, 181 MB, gitignored) is the authority for the INTC.

## Devices

Modelled on the **RH850/U2B6** — three G4MH cores, PE0 to PE2; the manual's
base table runs to PE5 because the larger U2B parts have six, and §40
states plainly that "CPU3, CPU4, CPU5 are not implemented in RH850/U2B6".
This frontend is single-core, so only SELF and PE0 are mapped.

| guest address | what | role |
|---|---|---|
| `0xFFFC_0000` | INTC1 SELF | core-local controller, channels 0–31 — the CLINT's role |
| `0xFFFC_4000` | INTC1 PE0 | the same registers named absolutely |
| `0xFFF8_0000` | INTC2 | global controller, channels 32–1023 — the APLIC's role |
| `0xFFEC_0000` | OSTM0 | time base (a stand-in, not the real OSTM register set) |

The elegant part of the real design: `EICn` is one 16-bit register at
`base + 0x02 × n` in **both** units, so the channel array is a single
address space that happens to be implemented by two devices. A guest
computes the address from the channel number and never cares which side of
the boundary it lands on. `EIBD` (interrupt bind — which PE takes it) is
the APLIC target analogue.

`EICn` bits, from U2B §6.3: `EIP[3:0]` priority (0 highest), `EIOV` 5,
`EITB` 6, `EIMK` 7, `EIRF` 12, `EICT` 15. Reset `0x008F` — masked, lowest
priority, edge detection — so nothing is delivered until software has both
lowered `EIMK` and set a priority.

A channel number is the host's interrupt number, exactly as an APLIC source
is.

## Implemented

Formats I and II (16-bit reg-reg and imm5 ALU), III (`Bcond disp9`), IV
(`SLD`/`SST` .B/.H/.W through EP), V (`JR`/`JARL disp22`), VI (the imm16
ALU group), VII (`LD`/`ST` .B/.H/.W `disp16`), `MOV imm32`, and the Format
X system group: `LDSR`, `STSR`, `TRAP`, `EIRET`, `FERET`, `CTRET`, `HALT`,
`SNOOZE`, `DI`/`EI`, register-form shifts, `MUL`/`MULU`, `DIV`/`DIVU`,
`SETF`, the `SYNC*` barriers, and the atomics `CAXI`, `LDL.W`, `STC.W` and
`CLL`. Plus both exception levels with their own save registers, the PSW
and system register file, and INTC1/INTC2.

Everything else raises `G4MH_EXC_RIE`.

## Testing

191 checks in `tests/unit/test_g4mh.c`, covering instruction length
classification, the condition-code table, ALU and flags, branches,
load/store, `MOV imm32`, `MUL`/`DIV`, system registers, the reserved-
instruction path, `TRAP` with and without a syscall hook, and the frontend
contract itself.

The test programs are **hand-assembled halfword arrays**, deliberately not
sharing an encoder with the interpreter — a shared one would pass while
both were wrong the same way. That is not hypothetical: the first version
of the `LDSR` test built its encodings symmetrically with `STSR` and would
have passed against the backwards implementation it was meant to catch.

**There is no reference model and no toolchain.** RV32 has riscv-arch-test,
the Berkeley suite and Sail to disagree with; G4MH has none of that here.
Treat any G4MH result as verified only as far as those 191 checks reach.

## Things that have bitten

Checking the first, from-memory implementation against the manual found six
errors:

- **`LDSR` and `STSR` use their two register fields in opposite senses.**
  `LDSR` has regID in bits[15:11] and the GPR source in bits[4:0]; `STSR`
  is the other way round. Implementing one by analogy with the other gets
  it backwards.
- **`JR`/`JARL disp22` carries its *high* displacement bits in the first
  halfword** — `disp[21:16]` in `w0[5:0]`, `disp[15:1]` in `w1[15:1]`. The
  RISC-V habit is the reverse, and assuming it gave plausible-looking short
  forward jumps and garbage for everything else.
- **G4MH has no `RETI`.** V850's single return was split into `EIRET`
  (0x148), `FERET` (0x14A) and `CTRET` (0x144), which name their level in
  the opcode instead of inferring it from PSW.
- **Sub-opcode 0x160 is shared** by `DI`, `EI`, `PUSHSP`, `POPSP` and
  `CLL`, told apart by the whole reg2 field — testing bit 15 alone reads
  `PUSHSP` and `POPSP` as `DI`.
- **`HALT` and `SNOOZE` share 0x120.**
- **`reg2 == 0` is an opcode extension throughout.** `CALLT` hides in the
  `MOV imm5` slot, `DISPOSE` in `MOVHI`/`SATSUBI`, `MOV imm32` in `MOVEA`,
  `JMP disp32` in `MULHI`, `JR disp32` in `MULH imm5`, `PREPARE` in `JR`.
  Decoding on the opcode alone made six unimplemented instructions **retire
  silently as writes into r0** instead of raising RIE — far worse than not
  implementing them, because the guest gets a wrong answer rather than a
  clean exception.

The lesson generalises: **an ISA that reuses a register field as an opcode
extension will not tell you when you ignore it.** Before adding an
encoding, grep the manual for every instruction sharing its opcode.

**Instruction length is two-stage** (`g4mh_insn_len`, then
`g4mh_insn_is_48`) because the 48-bit forms cannot be identified from the
first halfword — they are exactly the `reg2 == 0` cases of opcodes 0x17,
0x31, 0x37, and 0x3C/0x3D disambiguated by bit 0 of the second halfword. A
wrong length does not produce a wrong result, it desynchronises the
instruction stream.

## To do

**Multicore is implemented** — see [`multicore.md`](multicore.md). Three
PEs at `-DG4MH_PE_COUNT=3` (host only; 64 KiB of local RAM per core rules
out the F446), round-robin with a `--quantum` knob, cross-core LL/SC, CAXI,
and INTC2 routing by `EIBD.PEID`. Still to do there: per-PE local RAM with
its SELF alias, and a spinlock test passing a token round the three cores.

Missing instructions, in the order a real guest would hit them:

| gap | why it bites |
|---|---|
| `PREPARE` / `DISPOSE` | **every non-leaf function** a compiler emits — the biggest blocker to running compiled code |
| `LD.BU`/`LD.HU`, `SLD.BU`/`SLD.HU` | only the sign-extending loads exist |
| `CALLT` / `CTRET` | `CTBP`/`CTPC`/`CTPSW` are storage with no instructions behind them |
| `CMOV`, `ADF`/`SBF`, `SASF` | the branchless idioms a compiler prefers |
| `BSW`/`BSH`/`HSW`/`HSH`, `SCH*` | byte swaps, bit search |
| Format VIII `SET1`/`CLR1`/`NOT1`/`TST1` | bit manipulation on memory (opcode 0x3E) |
| 48-bit `JMP`/`JR`/`JARL disp32`, disp23 loads/stores | long-range code and data |
| `MAC`/`MACU`, imm9 `MUL`/`DIV`, 3-operand `DIVH` | |
| `CACHE`, `PREF` | |
| the FPU | the FP system registers exist as storage; no FP encoding is decoded. `G4MH_EXT_FPU` is the switch |

Architecture not modelled:

- **The MPU** — `MPLA`/`MPUA`/`MPAT`/`MPM` and the checks behind them, the
  PMP analogue. `MIP`/`MDP` are raised only by a bus fault today.
- **User mode.** `PSW.UM` is defined and nothing enforces it. Note what the
  RISC-V side learned: U-mode turned three latent PMP bugs into failures at
  once. Expect the same here.
- **Coprocessor gating.** `PSW.CU0-2` and `UCPOP` defined, never consulted.
- **Interrupt nesting.** Per-channel priority exists; `ISPR` is not
  maintained and `PMR` not honoured. `INTBP` and table-reference entry are
  absent; entry uses the single direct vector.
- **Register banks / hardware context save**, `GMCFG`, guest modes.
- **Debug level** — no `DBPC`/`DBPSW`, `DBTRAP`/`DBRET`.

## Investigate

- **Where a G4MH guest image would come from.** Without a toolchain, the
  ceiling on this frontend is hand-assembled tests. GCC has a `v850` target
  that may be close enough to bootstrap against.
- **Whether the exception vector offsets match a real part.**
  `handler_address()` uses a compact layout; a real table is larger and
  `RBASE`/`EBASE` flag bits are masked off rather than honoured.

## Discarded

- **Inventing an INTC register map.** The first version put one 16-bit word
  per channel at a convenient `0x0FF0_0000`. The bit layout turned out to
  be right by luck — `EIMK` at 7, `EIRF` at 12, priority in [3:0] — but the
  addresses were fiction, and an RH850 guest reaches these by the numbers
  in the manual. Using the architectural addresses is what makes a vendor
  driver work unported, which is the same argument as the STM32
  passthrough window.

## Building a guest with the real toolchain

The unit tests assemble their own instruction words, which means they agree
with whatever the frontend already believes. Building with Renesas CC-RH
does not, and the first RH850 binary to run here found two things the unit
tests could not:

- **CC-RH emits `e_machine` 36 (`EM_V800`)**, the number the RH850 ABI
  document specifies, where the frontend accepted only 87 (`EM_V850`),
  which is what GNU's v850 target emits. `readelf` prints both as "Renesas
  V850 (using RH850 ABI)". A frontend that takes one rejects half the
  world's binaries, and `ccrh`'s own linker output would not load.
- **The syscall number cannot be the `TRAP` vector.** That field is five
  bits, so it can say 0-31, while the host harness answers to newlib's 64
  and 93 -- no guest could ever name them, and every syscall fell through
  to the architectural trap. The number now comes from `r11`, which is
  caller-saved and not an argument register: the role `a7` plays on
  RISC-V.

The ABI, then: number in `r11`, arguments in `r6`-`r9`, result in `r10`,
entered with `TRAP`. A hook that does not recognise the number returns
false and the architectural trap happens after all, so `TRAP` stays usable
for its own sake.

`tests/guest/g4mh/hello.asm` is the smallest guest that exercises it, and
carries its own build line.

## The memory map

Taken from the Y-ASK board package's linker scripts
(`Source/Make/CSP/r7f7025*.csp.ld`), which is what the vendor's own
toolchain reads and therefore the authority a real guest is built
against. The three device variants differ in how much of each region
exists, never in where it starts:

| | flash banks | local RAM | cluster RAM | PEs |
|---|---|---|---|---|
| U2B6 | 2 x 3M | 3 x 64K | 384K | 3 |
| U2B10 | 3M, 3M, 2M, 2M | 4 x 64K | 1M | 4 |
| U2B24 | 6 x 4M | 6 x 64K | 1.5M + 2.5M | 6 |

Modelled: code flash at `0x00000000`, local RAM, cluster RAM at
`0xFE000000`. A guest linked by the vendor's scripts loads and runs from
flash at the real reset address.

Local RAM is where this gets interesting, and it is the same shape as
INTC1: every PE sees its own at the SELF alias `0xFDE00000`, and every
PE's at an absolute address. The absolute windows run *downwards* from PE0
at `0xFDC00000`, 2 MiB apart, so PE n is at `PE0 - n * stride` -- easy to
get backwards, and backwards puts PE1's RAM where nothing is mapped rather
than producing a wrong answer. One image can therefore be shared by every
PE and still get its own data, which is what the SELF window is for.

Not modelled: the second flash bank and the boot clusters, the retention
and ERAM regions, and the flash sequencer -- flash is plain RAM here, so a
guest that writes to its own code succeeds where a real part would refuse.

`tests/guest/g4mh/lram.asm` checks the aliasing from the guest side, and
was A/B'd by collapsing the three local RAMs into one.

## The PE number

PE *n* reports *n*, and the frontend returns exactly that. The U2B manual
§3.2.2.10, "Acquiring the CPU Number", says a program identifies the core
it is running on by reading the PEID register, which holds "a unique
number within a multi-processor system ... according to the specification
of the product"; Table 3.1's column is headed "CPU (PEID)" and runs 0
upwards. `lram.asm` reads it with `stsr 0, rN, 2` and gets 0, 1, 2 on the
three-PE build.

One loose thread, and it is naming rather than behaviour: the U2B manual
calls this the PEID register, while the frontend and this document call
the system register HTCFG0. The value is confirmed; whether PEID occupies
the whole of HTCFG0 or a field within it is not settled by the text that
extracts cleanly from these PDFs. Nothing here depends on the surrounding
bits, so it matters only to a guest that reads them.

There is no JIT for this frontend. `g4mh_backend_interp` is the only
backend; the x86-64 JIT is an RV32 one.

## The JIT

`g4mh_jit_x86_64.c`, on the shared framework -- see docs/Architecture.md.

The hard part of this ISA is not arithmetic, it is PSW: almost every
instruction writes Z, S, OV and CY, and the interpreter does four tests
per instruction to produce them. x86 has the same four in EFLAGS for free,
and they line up exactly -- ZF, SF, OF, CF against Z, S, OV, CY, with CF a
borrow on subtract just as CY is -- so translation pays off precisely
where interpreting is slowest.

Capturing them is the fiddly part, because everything that combines them
destroys them. `seto al` then `lahf` gets all four into one register with
no flag-clobbering instruction in between; only then is it safe to shift
and mask. The sequence must confine itself to eax, ecx and edx: the caller
stashes the operation's result in esi across it, and the first version
used esi as a scratch, so the block stored the flag word where the result
belonged. `lram.asm` caught that on its first run.

Translated: the register-register and imm5 forms of MOV, the logical ops,
ADD, SUB, CMP. Not translated, and so ending the block: loads, stores,
branches, TRAP, the system registers, and every long-form encoding. On
`lram.asm` that is 6 of 22 instructions -- honest for a first cut against
a guest that is mostly `mov imm32` and memory.
