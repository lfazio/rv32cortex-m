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
| MMU: the TLBs, MAS registers, `tlb*`, address spaces | **not modelled** — addresses are physical |
| SPE and the vector half of the EFPU (`ev*`) | **not implemented** — SPE-unavailable while MSR[SPE] is clear, as the core does; an illegal-instruction program interrupt once it is set |
| Classic floating point (`fadd` …) | not on this core |
| Caches, the debug facility (IAC/DAC/DBCR do hold what is written) | not modelled |
| gdb stub, pair statistics | not written for this frontend |
| More than one core | no |

Both backends run everything in the table: the interpreter, and a JIT
through the shared IR on x86-64 and on Thumb-2.

---

## Two encodings, chosen per image

VLE and classic Book E are different encodings of the *same bytes* —
`0x48000009` is `bl` in Book E and a 16-bit `se_` form followed by
something else in VLE — so which one a page holds has to be known
before its first fetch. A real e200 takes it from the VLE attribute of
the TLB entry. There is no TLB here, so it is core-wide and **comes
from the image**:

- an **ELF** says: binutils marks a VLE segment with `PF_PPC_VLE`
  (`0x10000000` in `p_flags`), and the boot hook reads it;
- a **raw binary** cannot say, and is taken as VLE.

So the compiled guests are loaded as ELF and are Book E, and
`isatest.bin` is a flat VLE image. The firmware embeds the ELF rather
than a binary for the same reason (see below).

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
where 48 KiB of `.bss` would be the guest's memory.

Things the manual settles and a reading of the mnemonics would not:

- **Unaligned accesses are performed** (3.4), a byte at a time; only the
  instructions the manual lists take an alignment interrupt — the
  multiple moves, the reservations, `dcbz` with the cache off.
- **The reservation is a flag with no address** (3.5). A store
  conditional to a different address than the load succeeds, and one
  with no reservation is a no-op that raises nothing, whatever its
  alignment.
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
compare, the CR field: 118,208 vectors at `--count 100000`, 0 wrong
(`ctest` runs 38,208, `ppc-efpu-vs-exact`). `ppc-fpu`
(`tests/tools/ppc_fpu.c`) is the unit as a command.

The first run reported 40 wrong square roots, and the emulator was
right: the *model's* scaling was too coarse to round correctly. An
oracle is code too.

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
  block whole — the multiple moves, the reservations, `tw`, `dcbz`, the
  FP unit, and a handful of carrying forms;
- **declined**: the block ends and the interpreter runs it from the
  dispatcher. A short list and a deliberate one — whatever changes MSR
  (`mtmsr`, `wrtee`, the `rfi` family), takes an interrupt on purpose
  (`sc`, an illegal encoding), reads or writes an SPR other than LR, CTR
  and XER, waits, or says code moved (`icbi`). That makes the
  interpreter the single place those happen, which is why a block needs
  **no generation and no privilege in its context**: the context is the
  encoding and nothing else.

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

x86-64 host, medians of seven runs, same retired counts and checksums on
both backends:

| guest | instructions | interpreter | JIT | |
|---|---|---|---|---|
| CoreMark, 40 iterations | 12,170,464 | 258 ms | 43 ms | 6.0x |
| crypto (AES-128, SHA-256) | 2,422,217 | 58 ms | 12 ms | 5.0x |

On the Nucleo-F746ZG it depends entirely on whether the translated
working set fits the code cache, and at the 32 KB default it does not.
Host cycles per guest instruction, with the number of translations the
run needed:

| | CoreMark | crypto |
|---|---|---|
| interpreter | **222.3** | **243.4** |
| JIT, 32 KB (default) | 335.1 (46,199) | 716.5 (28,545) |
| JIT, 64 KB | 212.1 (27,780) | 373.7 (12,532) |
| JIT, 96 KB | 125.8 (15,309) | **35.4** (468) |

**At the default the JIT is 1.5x slower than interpreting CoreMark and
2.9x slower on crypto.** Crypto fits at 96 KB and is then 6.9x *faster*;
CoreMark is still being evicted there and is 1.8x faster.

This is not a PowerPC result — RV32 CoreMark loses to its interpreter at
32 KB on the same board too ([../jit/tuning.md](../jit/tuning.md)) — but
PowerPC's guests translate to more code: about 70 bytes per guest
instruction on x86-64, a quarter to a half of it memory accesses and
much of the rest condition-register arithmetic.

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

| Image | Purpose |
|---|---|
| `isatest.bin` | hand-written **VLE**: the 16-bit forms, the `e_` immediates, the multiple moves, a VLE-only illegal encoding. Exit status is its failure count, and it must print `PPC-ISATEST-END` |
| `ppc-alu` | compiled C against values from Python's integers (`alu_want.py`): the forms a compiler reaches for unasked — `addc`/`adde` chains, `srawi`+`addze`, `rlwinm`/`rlwimi`, `isel`, update forms, `lmw`/`stmw`, `bctr` through a jump table |
| `ppc-sys` | the system level, 183 checks with the manual's section beside each: interrupts, privilege, SPRs, reservations, the timers, `wait`, both FP interrupts |
| `ppc-park` | a `wait` nothing may end: the run must stop there, and the line after it must not print |
| `ppc-coremark`, `ppc-crypto` | the shared benchmarks |

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
| `ppc-coremark`, `ppc-crypto` | their own checksums — CoreMark's CRCs, FIPS vectors — identical across backends and to RV32's |
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

`PPC_GUEST` is `alu`, `sys`, `coremark` or `crypto`. The image is
embedded as a **stripped ELF**, not a binary, because the encoding is a
flag on its segment that a binary has nowhere to carry; stripped,
because the debug sections were four fifths of the file. The guests take
their stack from the loader rather than from the link script, since the
top of RAM is a megabyte on a host and a fifth of that here.

Run on the Nucleo-F746ZG, on both backends: `alu` 68 checks, `sys` 183,
CoreMark `crcfinal 0x65c5` and crypto `0x60ae7a25`, each with the
retired count the host gives — and 600 generated cases, line for line
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
- **No MMU.** A guest that programs the TLB cannot run; MPC57xx start-up
  code does.
- **No VLE compiler**, so compiled code is Book E and VLE is covered by
  the decoder sweeps, the generated cases and hand-written assembly. An
  NXP S32DS or CodeWarrior build would do for VLE what CC-RH did for
  G4MH.
- **On a microcontroller the JIT loses to the interpreter at the default
  cache**; see the table above. That is a property of the shared
  framework's sizes more than of this frontend.
- **A spurious wake from `wait`** — an interrupt that arrives and is
  masked again before delivery — is not tested.
- **No gdb target and no pair statistics.**
- Dhrystone is not built: it needs a 32-bit `libgcc` this toolchain does
  not ship.
