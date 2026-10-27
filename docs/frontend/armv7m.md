# The ARMv7-M frontend

`include/armv7m/` and `src/frontend/armv7m/` — ARMv7E-M with FPv5-SP,
the Cortex-M7 as the Nucleo-F746ZG has it.

```sh
cmake -B build/m7 -DEMU_PLATFORM=host \
      -DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_ARMV7M=ON
cmake --build build/m7
./build/m7/emu-host        build/m7/guest/armv7m-coremark.bin
./build/m7/emu-host --jit  build/m7/guest/armv7m-coremark.bin
```

It started as the way to execute the Thumb-2 backend's encoder on a
host (`tests/guest/armv7m/t2exec.c`), and is now a complete core,
**checked instruction by instruction against a real Cortex-M7** rather
than against a reading of the manual. The board is the reference: where
this file says what the core does, the number beside it is what the
silicon was asked.

| Area | State |
|---|---|
| Thumb-2, the whole ARMv7E-M space including the DSP extension | implemented |
| FPv5-SP: arithmetic, fused multiply-add, conversions, half precision (IEEE and AHP), VRINT/VCVTx/VSEL/VMAXNM | implemented on SoftFloat |
| Exceptions: priorities with PRIGROUP, BASEPRI/BASEPRI_MAX, PRIMASK, FAULTMASK, escalation to HardFault, lockup | implemented |
| MSP/PSP, CONTROL, unprivileged execution, LDRT/STRT | implemented |
| Fault status: CFSR, HFSR, MMFAR, BFAR | implemented |
| MPU: 8 regions, subregions, AP/XN, PRIVDEFENA, HFNMIENA | implemented |
| Lazy FP stacking: FPCCR, FPCAR, FPDSCR, EXC_RETURN bit 4 | implemented |
| Exception return from a 0xFxxxxxxx pc in Handler mode, written by BX, LDR, POP or LDM -- and only those: MOV pc, ADD pc and BLX branch to it, as exception entry and unstacking do, and the fetch faults (`pc_not_exc_return`) | asked of the F746 (`sys.c`, `excret-*`) |
| NVIC (98 lines, 4 priority bits), SysTick | implemented |
| Local exclusive monitor | implemented; keeps no address, as the M7's does |
| Caches, the debug architecture, DWT/ITM | not modelled |

---

## How it is checked: the board

`tests/armv7m-diff/` builds the *same* test program twice — as a guest
for this frontend and as native firmware for the Nucleo-F746ZG — runs
both, and compares what each printed. No other emulator is involved:
the reference is silicon.

```sh
tests/armv7m-diff/run.py --seed 1 --count 6000          # generated instructions
tests/armv7m-diff/run.py --suite sys                    # the system level
tests/armv7m-diff/run.py --suite raw --from x.elf ...   # encodings, not instructions
tests/armv7m-diff/run.py --rerun --emu-args=--jit ...   # a stored board run, again
tests/armv7m-diff/run.py --self                         # no board: JIT vs interpreter
```

Needs `arm-none-eabi-gcc`, `probe-rs` and pyserial; the board's ST-LINK
virtual COM port at 460800.

Three suites, because they answer different questions.

**Generated instructions** (`gen.py`). Each case loads registers, APSR
and the FP state from a seeded generator, runs one instruction and
prints everything. A quarter of operands come from a table of special
values, because a random 32-bit operand is almost never where carry,
overflow and saturation live. Classes cover data processing, shifts,
multiplies and DSP, every addressing mode, the exclusives, branches,
IT, and the FPU — and a growing set written because the random ones
*missed something*, each confirmed by reintroducing the defect:

| class | what a random operand did not reach |
|---|---|
| `fpspecial` | NaN operand order; 3,000 random FP cases did not detect a reversed one |
| `fphalf` | AHP, a directed rounding mode and a negative value at once |
| `carry` | ADC/SBC whose second addition carries: needs `a + b == ~0` exactly |
| `shedge` | register shifts by exactly 0, 32, 33 and 256 |
| `qflag` | Q, which a random APSR has set half the time already |
| `adr` | `Align(PC, 4)`, at both pc alignments |

**The system suite** (`sys.c`/`sys.S`): 62 tests of what an instruction
generator cannot reach — the identification and writable bits of every
configuration register, exception ordering and preemption, BASEPRI and
FAULTMASK, every fault and its status bits, escalation, the MPU, the
process stack, lazy FP stacking, and a set of *encoding probes*.

**Raw encodings** (`rawgen.py`). The generated suite only contains what
an assembler writes, so it says nothing about the rest of the encoding
space — 150 million of the 402 million 32-bit encodings are UNDEFINED.
This writes `.inst.w`: uniform random encodings, and real instructions
with one bit flipped, each filtered by the frontend's own decoder to
what is safe to run. The harness takes the fault, records it, and
resumes, so "UNDEFINED" is a result compared like any other. About
54,000 have been run.

**sp and pc as operands** (`--regs N`, `--regs-sp`, `--enc`). The
filter keeps every operand in r0-r12, because an arbitrary write to sp
or pc wanders off -- so no encoding naming either had ever been
asked, and that is where `fuzz-armv7m-jit` found the interpreter
executing `bfi pc, lr, #0, #13`. `--regs` takes real register-only
instructions from the corpus and puts 13 or 15 in each register field,
keeping only what still decodes as a register-only operation (a field
of 15 can turn `and.w` into `bl`); `--regs-sp` adds sp as a
destination where the decoder already calls the encoding UNDEFINED,
because one the board executed would end the run. The memory and
system forms were asked by name, with any that could write sp or pc
last.

### What the board corrected

Every one of these was in an interpreter that had already passed the
generated suites:

| | the board's answer |
|---|---|
| a should-be-zero or should-be-one bit that is not | **UNDEFINSTR**, everywhere it was asked -- where the manual says UNPREDICTABLE |
| except a hint (`NOP.W`, `WFI.W` ...) | executes |
| except VRINT's bit 7 | ignored |
| `LDR` with bit 24 set | UNDEFINSTR; it executed as `LDR` |
| `LDRD rt, rt`; a long multiply with RdLo == RdHi; `VMOV r, r, d` into one register | UNDEFINSTR |
| `MRS`/`MSR` naming a register that does not exist | UNDEFINSTR; it read zero |
| D16 and up, on a 16-register FPU | UNDEFINSTR |
| the Advanced SIMD space and an unallocated coprocessor row | UNDEFINSTR, not NOCP |
| CCR at reset | `0x00040200`: BP is RAO/WI |
| BASEPRI | reads back 8 bits, masks with 4 |
| ICSR.RETTOBASE in Thread mode | 0 |
| FPCCR.HFRDY at lazy stacking | set |
| `STREX` to another address than the `LDREX` | succeeds |
| AHP conversion of a negative value under RM | rounded as if positive -- fixed |
| **sp or pc where the manual says UNPREDICTABLE**: Rd, Rn or Rm of the 32-bit data-processing, shift, extend, multiply, divide, saturate and bit-field forms; Rt of a narrow load or any store; LDRD/STRD through pc or onto sp; the exclusives; TBB/TBH; VMOV, VMRS, VMSR, MSR | **UNDEFINSTR**: 557 of 557 in one sweep and every named encoding after. The frontend executed them all (2026-10-26) |
| ...and a write-back onto the transfer register, LDRD with W through pc, STM writing back over a base it stores, VSTR through pc | UNDEFINSTR |
| except the forms that may name sp: ADD, SUB, CMP and CMN of sp, MOV to or from it, `str sp` | execute |
| except an LDM of one register, or one writing back over a base in its list, and REV/CLZ with their two Rm copies differing | execute -- UNPREDICTABLE, and not refused |
| 16-bit CMP naming pc, and BLX pc | UNDEFINSTR; CMP with two low registers in the hi-register form executes, as does `add pc, pc` |
| **IT, every encoding**: firstcond 1111, AL with an else, an IT inside an IT block | execute: 1110 and 1111 both mean always, and an inner IT replaces the block. The frontend refused all of them (2026-10-26) |
| B\<cond\> (16- and 32-bit) and CBZ/CBNZ inside an IT block | execute **on their own condition**, ignoring the block's: under `it eq` with Z clear, `bne` branches |
| B, B.W and BL inside an IT block but not last | execute under the block's condition |
| **MOV pc, ADD pc and BLX to EXC_RETURN in Handler mode** | branch: the fetch takes IACCVIOL with the target, bit 0 clear, stacked, and the handler is still active. Only BX, LDR, POP and LDM return, as the manual says. The frontend read the value as a return wherever it came from, and hung (2026-10-26) |
| **any instruction that can write the pc, inside an IT block** | ends the block whatever happens to it -- taken, not taken, or skipped by the block's condition. Under `itt eq` with Z clear, a skipped `bx`, `blx`, `mov pc`, `add pc`, `pop {pc}`, `pop.w {..,pc}`, `ldm.w {..,pc}`, `ldr pc`, `bl`, `tbb` or `tbh` is followed by an instruction that runs outside the block, unconditionally and setting flags; so is an untaken `bne` or `cbz`; and every one of them *taken* lands on an instruction that runs outside it too |

The IT rules are `it_ignores_condition`, `it_writes_pc` and `it_after`
beside `step()` in the interpreter, which decides them all: the
translator gives back any IT block holding a branch other than a last
B, a CBZ or another IT. Each was asked as
a named halfword sequence (`--enc nbfe4+bf08+2001`), six times over, so
that the flags differ between repetitions, with a `mov` placed to show
which way a branch went; `test_it_board_answers` holds eleven of them
for both values of Z and checks the flags after an untaken `bne`, and
the sp/pc table seven 16-bit ones (29 and 5 failures against ad70a01,
the commit before). The ten pc-writers the first runs left out -- ADD
pc, POP and POP.W, LDM.W and LDR.W into pc, TBB, TBH, and BX, BLX and
MOV pc *taken* -- were asked afterwards (`build/armv7m-it7`), skipped
and taken, every target computed from `mov r5, pc` and the stack
balanced, so they run wherever the probe lands: the board ends the
block for all twenty, as `it_writes_pc` had by analogy, and the
emulator agrees on every field but r5, the address it took from the
pc. `test_it_pc_writers_board_answers` holds them; with the encoding
test removed the ten skipped ones fail, and with the pc test as well
the ten taken ones. The harness resumes a faulting probe at
its end now, not four bytes on, which inside a 16-bit probe was the
middle of the next instruction.

**Exception return is decided by the instruction, not the value.** The
run loop recognises EXC_RETURN in the pc before fetching, so it took
any 0xFxxxxxxx value in Handler mode as a return, whatever wrote it.
The manual names BX, LDR, POP and LDM; MOV pc and ADD pc are
ALUWritePC and BLX is BLXWritePC, plain branches. `sys.c` asks each
from an SVCall handler with lr's EXC_RETURN: the board returns for the
first three and, for the others, fetches the address and takes
IACCVIOL, the target stacked with bit 0 cleared and SVCall still
active (`excret-*`; `xn-fetch` and `excret-mov-scan` show the stacked
pc is the target for ten System-region addresses, from either mode).
The frontend read `mov pc, lr` as a return to 0xFFFFFFE8 -- lr held
0xFFFFFFE9, an FP frame, less bit 0 -- which is invalid, so it raised
INVPC and deactivated SVCall under its own handler, and the suite's
recovery then looped. Those three writers now set `pc_not_exc_return`,
out of line (`branch_not_exc_return`), and the run loop fetches; the
JIT declines all three in Handler mode, so the interpreter decides for
both backends. `test_handler_pc_writers_branch` holds it (13 failures
with the fix reverted); interpreted CoreMark on the F746 read +0.35%,
-0.14% and +0.08% at three layouts, which move the old build by 0.22%
on their own.

The sp and pc rules are one predicate per group in `armv7m_decode.h`,
called by both decoders, and the board's answers to 36 of them are
held without the board by `test_sp_pc_board_answers` (26 fail with the
rules removed).

**What they cost was measured twice, and the two instruments
disagreed by a factor of four.** Callgrind on x86-64 said 2.3% of the
interpreter's host instructions on CoreMark. The F746 said 10.3%:
143.44 host cycles an instruction at 375f9bf, 158.27 with the rules as
first written, 160.65 with the IT rules beside them, the wall clock
agreeing every time. GCC had moved `armv7m_dp_regs_ok` and the
plain-immediate handler out of the loop, which runs from ITCM, into
calls in flash -- a cost x86-64 has no counterpart for. The fast test
is inline and forced now, the rules out of line and called only by an
encoding naming sp or pc, the handler inlined again: 153.69, so 7.1%
above 375f9bf. A PC profile (`scripts/board-pc-profile.py`) puts about
2% in the predicates and the rest in the loop and the handlers around
them, unchanged code among them (`t16_shift_add` +97 samples), which
is code shape. Callgrind against 375f9bf: +4.45% on CoreMark and
+3.37% on Dhrystone interpreted, +3.67% and +2.52% in one partition,
+0.04% and +0.02% under the JIT -- which is this frontend's default
backend and checks a block once. Variants that read dearer on the
board: the handler placed in ITCM out of line (153.65, no better than
in flash), and a pre-filter on the pc-writer test (155.13).

---

## The decoder, twice

The interpreter decodes for itself, and that decode is the one the board
checked. `armv7m_decode` is a second, structured description of the
encoding space, for the consumers that *talk about* an instruction —
the disassembler, the pair statistics, the translator. Two descriptions
drift, so they are held together:

- **One copy of each field rule**: the length rule, DecodeImmShift,
  Shift_C, ThumbExpandImm_C and VFPExpandImm are `static inline` in
  `armv7m_decode.h`, and the interpreter calls them.
- **A property test over the whole space**: `test_decode_agrees_on_undefined`
  runs every 16-bit encoding and a spread of 32-bit ones through both,
  asking each whether it is an instruction, UNDEFINED or NOCP.
  `ARMV7M_UNDEF_FULL=1` runs all 402,712,576 (about a minute); they
  agree on every one. It found twelve slots where they did not, and the
  board settled each.

## The disassembler

`armv7m_disasm` prints unified syntax and is tested by **assembling what
it prints**: `tests/armv7m-diff/disasm-check.py` disassembles every
instruction in an ELF's `$t` ranges, feeds the text to
`arm-none-eabi-as`, and requires the original bytes back. That needs no
second disassembler, and a wrong register, immediate, branch target or
width qualifier produces a different encoding. About 170,000
instructions from the generated cases and two firmware images
round-trip. What it cannot see is an instruction the decoder does not
know, since `.inst` assembles to itself — those are counted and are a
failure.

It follows IT blocks itself (a 16-bit ALU instruction is `adds` outside
one and `addeq` inside), because the trace hook cannot pass ITSTATE.

## The JIT

`--jit` selects `armv7m_backend_jit`, built from `armv7m_ir.c` through
the shared IR pipeline. CoreMark on the x86-64 host, 600 iterations,
interleaved medians:

| | time |
|---|---|
| interpreter | 6,116 ms |
| JIT | 555 ms |

Three outcomes per instruction, counted in the run's report:
**lowered** to IR; **helper**, the interpreter's own single-instruction
core called from inside the block, which keeps the block whole; and
**declined**, which ends the block — the masks, CONTROL, MSR/MRS, CPS,
SVC, BKPT, WFI and IT blocks that cannot be lowered whole. Declining
those is a design decision: it makes the interpreter the single place
the context can change.

What the measurements decided, in order:

| step | CoreMark |
|---|---|
| first version: flags packed into xPSR | 1,994 ms |
| flags as four lazy words (`jit_nf/zf/cf/vf`) | 1,943 ms -- and 21% less code |
| SysTick: subtract per block, walk only a block that wraps | 1,446 ms |
| inlined RAM access | 707 ms |
| native SMULxy/SMLAxy, divide, register shifts, TBB/TBH, CLZ, flash literals | 620 ms |
| the shared optimiser deleting pc writes nothing observes | 555 ms |

The flag representation was the obvious target and the smallest win;
the profile said a loop decrementing SysTick once per instruction was
16% of all host instructions. **Profile before choosing.**

What a block may assume, and what invalidates it:

- **Context** (`armv7m_jit_ctx`): Thumb, inside IT, Handler mode,
  privileged. A block is only entered under the context it was built
  for. Anything that changes it from inside a block — a fault from a
  load, a helper, an interworking branch — rewrites `jit_ctx` before
  the dispatcher looks.
- **Generation** (`jit_gen`): any MPU register write, and CCR's
  UNALIGN_TRP or DIV_0_TRP changing. The inlined memory window and the
  native divide are specialised on those.
- **Self-modifying code**: nothing on this architecture announces it, so
  a store into a page translated code came from (`smc_*`, a bitmap of
  writable pages) discards the translations. Code in flash costs one
  compare per store and nothing else.
- **ISB ends a block through the dispatcher**, so an exception the
  guest has just pended is taken before the next instruction — two
  board tests failed until it did.
- **IT blocks are lowered whole** as straight-line code with
  conditional writes, or not at all. A load or store in one is
  *predicated* rather than selected (`emu_ir_mem_if`): skipped outright
  when the condition fails, because an access cannot be done and then
  undone. And it is the one member that can be observed part way --
  a fault stacks xPSR and the handler returns into the rest of the
  block by the ITSTATE it finds there -- so xPSR carries the member's
  ITSTATE across the access and gets its old value back after it.
  The dead-put pass drops the restore between two accesses. `itblock`
  checks both: a skipped store that lands, and a misaligned `LDRD`
  that faults inside `itee eq` and must come back with the else half
  still skipped.

  **The cheap half was measured first, and took none of it.** These
  were 25 blocks the translator declined (`declined IT:25`), 6.4% of
  Dhrystone under the JIT. A literal load from flash is a constant and
  could select like any ALU result -- and none of the 25 was that:
  Dhrystone's hot blocks hold real accesses, `ittte eq; ldreq; moveq;
  strbeq; movne` in Func_1 and `strls` and `ldreq` elsewhere. With the
  predicate, x86-64 JIT Dhrystone went 0.076 s to 0.050 s.
- **r15 is the pc, and the optimiser has to know it.** The pc slot is
  register 15 in this frontend's register file, and a block that ends
  on a computed target does it with `EXIT` on `GET r15` after something
  has written it. A pass that deletes pc writes nothing observes would
  delete that one — `EXIT` overwrites the pc — unless a `GET` of the
  register whose slot is the pc counts as an observer. It does, and
  `test_setpc_kept_when_a_register_reads_it` is that case.
- **Reads of the image that are not literals go through the image
  window.** A literal from flash is folded to a constant; what is left
  is reads through a pointer -- Dhrystone's string literals byte by
  byte, CoreMark's constant table in `.text` as halfwords -- and they
  used to be a helper call each: 621,495 on Dhrystone, 57,512 on
  CoreMark x40. The frontend counts them and arms the window as RV32
  does ([../backend/x86_64.md](../backend/x86_64.md#the-image-window)).
  On the F746 at 96 KB, Dhrystone 36.78 host cycles an instruction to
  22.73, crypto 30.27 to 26.81, CoreMark 34.65 to 33.51. On Thumb-2 the
  window is in the memory stub rather than in line, because in line it
  took this CoreMark to 49.19
  ([../backend/thumb2.md](../backend/thumb2.md#the-guest-ram-window)).

### The peripheral window

A guest on this frontend is a Cortex-M program, and on the F746 the
peripherals it drives are the board's own, identity-mapped. Translated
blocks inline them on Thumb-2 once the guest has made
`EMU_JIT_PT_ARM_AT` (64) accesses through the bus -- RV32's rule and the
backend's lowering unchanged; the frontend sets `has_pt`, and arms in
`bind` with one flush between slices. The window is under the RAM
window's conditions, so an enabled MPU refuses both.

`mmiobench` (`tests/guest/armv7m/`) runs RV32's four kernels on GPIOB
and on a RAM word with the same code, timed by TIM2, which the guest
starts itself: SysTick counts instructions here, so no guest clock
could tell the kernels apart. TIM2 ticks per kernel on the F746 at
aff384e and after, at three buffer offsets (`EMU_LAYOUT_PAD_BSS` 0, 64,
256):

| kernel | helper | inlined | speedup | after, against its RAM control |
|---|---|---|---|---|
| read | 9.02-9.19M | 1.64M | 5.5-5.6x | 1.22x |
| write | 7.19-7.20M | 1.23M | 5.8x | 1.36x |
| rmw | 21.4-22.8M | 1.76M | 12.2-13.0x | 1.30-1.36x |
| poll | 9.12-9.13M | 1.63-1.66M | 5.5-5.6x | 1.17-1.18x |

62.87-64.78 host cycles an instruction to 16.40-16.52 over the run; the
interpreter is 170.16. The RAM controls move by -1.2% to +2.7% with the
buffer offset, and ARMv7-M CoreMark, which drives no peripheral and
never arms, read 30.60-30.70 before and 29.89-30.01 after -- inside the
29.64-30.86 that the code offset alone gives it at aff384e.

Its checks run before arming and after: a store into a read-only hole
(the RCC clock tree) and one past the window must each raise a precise
BusFault with BFAR the address stored to; the hole's readable, and the
word above it writable. 208 checks, 0 failures, both builds. With the
backend's hole tests removed, the second round alone fails, on exactly
the two hole checks, eight times each -- and BFAR is what showed it,
holding the address of the previous round's fault.

How it is checked: every stored board run again under `--jit`
(`--rerun --emu-args=--jit`), the `armv7m-jit-vs-interp` ctest, which
runs 3,000 generated cases both ways *and requires block entries*, and
JIT variants of every guest. Mutation runs confirm the instrument: each
of seventeen deliberate mistakes in the translator — carry, overflow,
condition codes, IT selection, writeback, shift edges, Q, TBH scaling,
divide by zero — fails at least one of the three. Three first passed
everything, and the `carry`, `shedge` and `qflag` classes exist because
of them.

---

## Guests

`tests/guest/armv7m/`, built with the ARM toolchain:

| Image | Purpose |
|---|---|
| `hello` | hand-written, 16-bit encodings only |
| `alu` | compiled C; found three decode defects the assembly could not |
| `itblock` | IT, ITT, ITE, ITTTT and the in-block flag rule |
| `nvic` | SysTick, exception entry and return, PRIMASK, an external source |
| `t2exec` | the real Thumb-2 encoder, emitting into RAM and executing it |
| `coremark`, `dhrystone`, `crypto` | the shared benchmarks, with SysTick as the clock |
| `mmiobench` | GPIOB against RAM through the passthrough window, timed by TIM2 -- see [the peripheral window](#the-peripheral-window) |

The benchmarks keep time with SysTick (`systick_clock.h`), which in this
emulator counts instructions. That makes them tests as well: the
interrupt lands thousands of times a run in the middle of compiled
code, and the benchmark's own checksum has to survive it.

**Each runs as an ELF too, and until recently none of them did.** Reset
reads the vector table at VTOR, and VTOR followed the reset pc -- the
load address for a `.bin`, which is the table, and `e_entry` for an
ELF, which is the reset *handler* with the Thumb bit set. So an ELF read
its stack pointer and reset vector from an odd address inside its own
code and ran 0 to 2 instructions from an address that changed with the
build. ctest ran only the `.bin`s. `boot` now points VTOR at the base of
the segment holding `e_entry`, where the table is, and lets `e_entry`
win when that table's reset vector disagrees; `guest-armv7m-alu-elf`
runs the ELF path. gdb, which needs the ELF for its symbols, is what
found it.

## gdb

`--gdb` serves this frontend: `src/frontend/armv7m/armv7m_gdb.c`
describes r0..r12, sp, lr, pc and xpsr as gdb's
`org.gnu.gdb.arm.m-profile`, so gdb knows it is talking to a Cortex-M.
`xpsr` is packed from the JIT's split flags before it is read. The FP
registers, MSP/PSP and the masks are not described -- see
[../gdb.md](../gdb.md) -- and `scripts/gdb-check.sh` attaches a real gdb
to `armv7m-alu.elf` and checks what it sees.

## Pair statistics

`-DEMU_PAIR_STATS=ON` histograms adjacent executed pairs, as for the
other frontends. Register numbers are biased by one inside the table,
because the shared histogram reads 0 as "no register" and r0 is the
busiest register on ARM. Flags are not registers there, so `cmp; bne`
counts as independent. CoreMark: 29.0% of pairs are dependent, 8.2%
with a dead intermediate, 2.6% address generation into an access.

## Firmware

```sh
cmake -B build/f746-armv7m -DEMU_PLATFORM=stm32f746 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_ARMV7M=ON -DEMU_NET=OFF \
      -DARMV7M_GUEST=alu
cmake --build build/f746-armv7m --target flash
```

`ARMV7M_GUEST` is `hello`, `alu`, `itblock`, `nvic`, `t2exec`,
`mmiobench`, `crypto`, `dhrystone` or `coremark`, embedded as the flat `.bin`: the
vector table is its first word, so the load address is VTOR.

Run on the Nucleo-F746ZG -- an emulated Cortex-M7 on a real one -- on
both backends, every guest passing with **the retired count the host
gives, to the instruction**, and the firmware's cycle count agreeing
with the wall clock on the host end of the UART:

| guest | interpreter | JIT, 32 KB cache |
|---|---|---|
| `hello`, `alu`, `itblock`, `t2exec` | 22, 190, 218, 2,197 retired | the same |
| `nvic` | 92,709 retired, 288 host cycles each | 81,030 (SysTick is block-granular), **95.5** |
| `crypto` | 2,329,855, **296.0** | 659.6 -- 19,788 translations: the cache is a queue |
| `dhrystone` | 9,056,320, **309.9**, 12.8 s | 9,057,193, **91.2**, 3.7 s; 579,463 interpreted |
| `coremark`, 40 iterations | 11,144,363, **289.8**, 14.7 s, "Correct operation validated" | 11,145,477, **279.2**, 14.2 s |

That table is the first board run. Once IT blocks holding a load or a
store were lowered, every guest was run again on both backends -- all
passing, the retired counts the host's -- and the two benchmarks under
the JIT against a worktree at the commit before, since much else had
moved in between:

| JIT, 32 KB | before | after |
|---|---|---|
| `dhrystone` | **78.31**, 3.20 s, 579,310 interpreted | **40.17**, 1.63 s, 1,033 interpreted |
| `coremark`, 40 iterations | 234.82, 11.90 s, 13,114 interpreted | 238.19, 12.08 s, 1,115 interpreted |

CoreMark at 32 KB is translation-bound (23,000 translations), and +1.4%
on it is inside what layout alone moves on this part.

Two things in the *platform* stood between this frontend and its first
board run, and neither is in the frontend:

- **The board's clock hook called `set_time` unconditionally.** Every
  hook in the contract is optional, ARMv7-M supplies none (SysTick
  counts retired instructions in the core), and the host runner had
  always tested for NULL. The first firmware printed `ARMV7M-ALU-OK`,
  then branched to address 0 at the end of the slice: a HardFault with
  INVSTATE, read off the probe in one `bt`.
- **No staging for an ARMv7-M guest**, so the firmware would not
  configure. `ARMV7M_GUEST` is in `cmake/guest-image.cmake` now, and
  `f746-armv7m` is a build-matrix row.

And one in the guests: `start.S` put the initial stack at 0x8010_0000,
the top of a host's megabyte, where the F746 gives a guest a fifth of
that. The table carries 0 now, meaning **the loader's stack** -- the top
of whatever RAM the machine has, as the RV32 and PowerPC guests already
take it -- and `boot` keeps that rather than loading zero.

At the 32 KB of that first run the JIT only won where the working set
fitted (Dhrystone 3.4x, `nvic` 3x) and lost where it did not (crypto
2.2x slower), the same threshold [../jit/tuning.md](../jit/tuning.md)
describes for the other frontends. At 96 KB it is 34.7 host cycles an
instruction on CoreMark and 31.3 on crypto. **At 64 KB CoreMark does
not fit**: 106.41, with 10,226 translations and 289 compactions in a
40-iteration run, still ahead of the interpreter's 178.3, while crypto
is 32.39. So an ARMv7-M firmware defaults to 96 KB, where an RV32 one,
whose CoreMark fits 64, takes 64.

### The interpreter, profiled on the board

The interpreter cost **2.7x RV32's per instruction** on the F746, and
the profile said why in one run: **a third of the time was the
instruction fetch**, which went through two out-of-line calls per
halfword -- `armv7m_mem_fetch16`, then `mpu_permits`, which re-derived
the privilege level to answer, with the MPU off, nothing more than "not
in an execute-never space". The data accesses took the same route for
another sixth.

Tick-sampled (`-DEMU_PC_PROFILE=8192`), CoreMark interpreted,
`RelWithDebInfo`, both frontends in the same firmware configuration:

| | share of samples |
|---|---|
| `armv7m_run` (the loop, decode and the inlined execute) | 52.3% |
| `mpu_permits` | 16.7% |
| `armv7m_mem_fetch16` | 15.8% |
| `armv7m_mem_read`, `rd_u`, `armv7m_mem_write` | 6.8% |
| the rest of execute (`add_c`, `cond_holds`, `dp_op`, `ldm_stm`) | 7.0% |

RV32's interpreter met this as `fetch_guard` (CLAUDE.md: 9.3% for one
branch), and the shape of the answer is the same: the common case
inline. `armv7m_mem_read`, `armv7m_mem_write` and `armv7m_mem_fetch16`
are inline now, answering from the bus's cache when the MPU is off --
every data access permitted, a fetch refused only from the
execute-never spaces -- and otherwise calling the checked forms, which
raise the fault. They read `mpu_ctrl` itself rather than a flag derived
from it, so there is nothing to keep in step.

Against a worktree at the commit before, same configuration, retired
counts and results identical:

| interpreted | host cycles / guest instruction, before | after | |
|---|---|---|---|
| `nvic` | 307.7 | 211.2 | 1.46x |
| `crypto` | 318.0 | 169.4 | 1.88x |
| `dhrystone` | 334.9, 13.82 s | 212.5, 8.77 s | 1.58x |
| `coremark`, 40 iterations | 302.5, 15.38 s | 178.3, 9.07 s | 1.70x |

The JIT does not move (CoreMark 34.4 to 34.7, crypto 31.5 to 31.3),
which is what to expect: translated code has its own inline RAM path,
and the helpers it calls are a small share. RV32's interpreter, in the
profiling configuration, is **107.5**; ARMv7-M's is 182.5 there, so the
gap is 1.7x now. What is left of it is spread thin -- the scan of the
pending words before every instruction (about 3%), the IT state and
the EXC_RETURN test, SysTick -- and the largest of those would need a
summary of `pending[]` kept by its five writers, which is the derived
state CLAUDE.md warns about.

**A second round** took three per-instruction costs out of the loop,
each measured on the board at three `-falign-functions` layouts against
the one before -- every layout of each step below every layout of the
step before:

- the **whole instruction in one fetch**, both halfwords from the bus's
  window at once (`armv7m_fetch_insn_hit`) where a 32-bit instruction
  made the MPU, execute-never and window tests twice;
- **SysTick's common tick inline**, a decrement when the counter is
  above 1, where every instruction called `armv7m_systick_tick`;
- **no pending-exception scan unless something is pending.** Removing
  the scan outright, as an upper bound, was worth 6-8% -- more than its
  3.3% of samples, through the loop's code and registers. It is gated
  on `pending_any` now, the OR of the pending words, which one helper
  recomputes on every write of them. `test_pending_summary` walks the
  exception model at random -- pend, unpend, a level line, takes onto
  good stacks and bad, returns that re-pend -- and requires the summary
  to equal the OR after every step; each of the five writers bypassed
  fails it, two of which nothing else in the tree noticed. Its first
  version reached none of those paths, because nothing in it was
  enabled, and said so only because it counts what it reached.

Against a worktree at b6724fd, results and retired counts identical:

| interpreted | host cycles / guest instruction, before | after | |
|---|---|---|---|
| `nvic` | 211.7 | 205.9 | -2.7% |
| `crypto` | 169.0 | 158.9 | -6.0% |
| `dhrystone` | 206.9 | 192.7 | -6.9% |
| `coremark`, 40 iterations | 177.9 | 161.9 | -9.0% |

CoreMark has gone 302.5 to 161.9 over both rounds, 1.87x; in the
profiling configuration ARMv7-M is 165.2 against RV32's 107.5, a gap of
1.54x where it was 3.1x. What is left, by the same profile: decode and
execute 53% of samples, the loop and `step` 29%, fetch 9%, data
accesses 9%. The next lever looked like the decode itself -- the
execute functions take raw halfwords and decode as they go -- and was
not: see [the next section](#the-run-loop-in-itcm-and-the-decode-cache-that-lost).

### The run loop in ITCM, and the decode cache that lost

**`armv7m_run` is in the F746's ITCM** (`EMU_INTERP_TEXT_SECTION`, on by
default there and nowhere else). With the decode it inlines it is
8.6 KB, twice the part's 4 KB I-cache, and from flash its figure moved
with where the linker put it. Against a worktree at 22efa80, retired
counts, checksums and `crcfinal` identical, and the wall clock taken
from the host end agreeing:

| interpreted, F746 | before | in ITCM | |
|---|---|---|---|
| `coremark`, 40 iterations, three function alignments | 161.9 / 172.9 / 164.3 | 143.6 / 143.8 / 144.3 | -11 to -17% |
| `coremark`, wall clock | 8.23 s | 7.28 s | |
| `nvic` | 206.0 | 177.8 | -13.7% |
| `crypto` | 158.9 | 139.8 | -12.0% |
| `dhrystone` | 192.7, 7.96 s | 169.9, 7.01 s | -11.9% |

The spread across alignments went from 11 cycles to under 1, which is
the other half of the result: the figure is a property of the code now
and not of the link. The JIT does not move (CoreMark 34.71 against
34.61), since it runs from its buffer.

**The N6 is the opposite**, which is why the option is per platform and
separate from `EMU_HOT_TEXT_SECTION`: 167.2-167.3 from ordinary memory
and 175.2-175.3 from ITCM, 4.8% slower at all three alignments. Its M55
has a 32 KB I-cache, which holds the loop; why ITCM then costs is not
established.

**The decode cache was built three ways and measured worse every time**,
which is a result about the M7 rather than about the idea: PowerPC's
cache is worth 35% on a host, and its decode is a table lookup and an
extended-opcode search. Each version kept the interpreter's own decode
-- the one checked against the M7 -- and moved the fields a leaf
extracts into a 16-byte entry keyed on the pc and the bits:

| CoreMark, F746 | host cycles / instruction |
|---|---|
| no cache, from flash | 161.9 / 172.9 / 164.3 |
| 16-bit instructions cached | 178.1 |
| 32-bit only, inlined into the loop | 182.9 / 184.1 / 182.3 |
| 32-bit only, out of line | 164.8 / 169.9 / 187.4 |
| ...the same binary, every entry "run the leaf" | 190.9 / 194.2 / 223.3 |
| no cache, in ITCM | 143.6 / 143.8 / 144.3 |
| 32-bit only, in ITCM | 148.7 / 147.7 / 148.8 |

Callgrind said the 32-bit cache saved 3.5% of host instructions (441.2M
to 425.7M), and the 16-bit one cost 4.6%. On the board the 16-bit
decode is a few compares and the field extraction is one `UBFX` each,
which costs less than the lookup and the indirect branch that replace
them. The decoded forms do save what they were for -- the
same-binary row, which only toggles whether an entry is decoded, is
25-35 cycles behind -- but the lookup and the code it adds give it all
back, and with the I-cache taken out of the question the cache is still
3% behind. Misses were not the reason: 3.5% of lookups on CoreMark and
3.1% on Dhrystone at the board's 256 entries.

**The execute-never test on the fast path had nothing that could fail
it.** `tests/armv7m-diff`'s `xn-default` fetches from the peripherals,
and passed with the inline test removed: no cache ever holds an address
the checked path refused, so a fetch from one always misses and is
refused there. The only case the inline test decides is a region that
*straddles* an execute-never boundary -- fetching its lower half fills
the cache with all of it -- and `test_fetch_execute_never` builds
exactly that, a RAM region across 0x40000000. With the inline test
removed it fails 5 checks; with the MPU test removed the stored
`sys` answers differ on 4.

## How it began: the encoder, executed

hardware.** It began as a way to test the Thumb-2 backend without hardware, and
that worked first: the real
`src/backend/thumb2/encode.c` compiles into a Cortex-M guest,
runs on this frontend on an x86 host, emits instructions into
guest RAM and **branches to them** --
`tests/guest/armv7m/t2exec.c`, asserting `T2EXEC-OK`.

That closes a gap this file has complained about since the
backend existed. `scripts/t2-check-encodings.sh` compares emitted
bytes against `arm-none-eabi-as`, which catches a wrong
*encoding* and cannot catch a wrong *choice* of encoding -- an
emitter that assembles perfectly and is the wrong instruction for
its operands. Confirmed against the two historical defects by
reintroducing them: removing the shift-by-zero rewrite gives
`FAIL lsr-0-is-a-move`, and reversing the imm3:imm2 split kills
the guest. The first of those originally cost a Nucleo, a flash
cycle and two architecture tests.

Named `armv7m`, not `thumb2`: `src/backend/thumb2/` owns every
`t2_` symbol and those are an *encoder*. This is a decoder.

**What it decodes**: the 16-bit integer set, the 32-bit
encodings a compiler actually emits (chosen by compiling C and
reading the disassembly rather than by working down the manual),
IT blocks, SysTick, the NVIC, and exception entry and return.
Guests in `tests/guest/armv7m/`: hello, alu, itblock, nvic,
t2exec -- 6/6 in an `EMU_GUEST_ARCH_ARMV7M=ON` tree.

**Five defects, every one found by running a program and none by
a unit test**, which is this file's most-repeated lesson arriving
again. Four were the same class -- a field read too few bits wide,
aliasing one group of instructions onto another:

| | read | needs | symptom |
|---|---|---|---|
| shift/add/sub group | 2 bits | 3 | `movs r0,#16` ran as `lsls r0,r2,#0` |
| modified immediate | bits 10:9 | bit 9 | rejected every immediate with `i` set |
| extend group | 6 bits | 8 | swallowed **CBZ**; a branch became a zero-extend |
| MOVW/MOVT | masked 0x7B vs 0x24 | — | provably constant; neither ever decoded |

The fifth is the one worth keeping separately: the whole
0xE8xx-0xEFxx range sat behind `op1 == 3` and is `op1 == 1`, so
`add.w r5, r5, r5` reported as unimplemented while its handler
was forty lines below in a block the decoder could not enter. Two
more of the same shape followed -- the barriers checked *after*
the branch test that swallows them, and a dead duplicate of
MSR/MRS. **An unreachable handler reads exactly like a missing
one**, and only the fault report tells them apart.

Only MOVW/MOVT was caught by the compiler
(`-Wtautological-compare`). The other four needed a guest.

And the instruments were wrong more often than the code:

- an A/B harness that grepped the guest for `FAIL` scored a
  *crashed* guest as a pass, so three real catches read as
  misses. Detection is `ctest` now, which asserts a line only a
  completed run prints.
- the exception-frame check was inline asm and passed with the
  restore of r0 and of r12 broken, because gcc needs registers
  for the operands and chose the ones under test. It is
  `frame.S` now, where nothing is allocated, and it checks `sp`
  too -- the 8-byte alignment bit is otherwise invisible until a
  handler takes a *second* exception.
- three tests used the one input where correct and broken agree:
  `ITTTT EQ` cannot see an ITAdvance that shifts the whole byte
  (EQ is condition 0b0000, so the top three bits are already
  zero), and an IT block with a *true* condition cannot see a
  handler that inherits ITSTATE. The awkward input is the whole
  test.

The question the frontend was also meant to answer -- whether a
Thumb-2 guest on a Thumb-2 host could copy a block rather than
translate it -- is untouched and still a measurement rather than
a plan. The register file is in memory and the guest's pc is not
the host's, so "copy it" is unlikely to survive contact.

## Not done

- **The interpreter is still behind RV32's** on the F746 -- CoreMark
  143.6 host cycles an instruction against RV32's 103.0, both from
  ITCM now ([performance.md](../performance.md#the-interpreters-loops-in-itcm)). A decode cache is not the way to close it; see
  [the run loop in ITCM](#the-run-loop-in-itcm-and-the-decode-cache-that-lost).
- **AIRCR.SYSRESETREQ stops the run** rather than restarting the
  guest -- no platform here restarts one -- and the run summary says
  so: `emu: core 0 stopped: system reset by SYSRESETREQ`
  (`emu_cpu_status_t::reset_by`, which the PowerPC watchdog uses too).
  It used to halt with nothing printed.
- **Guests below 0x10000000 get no inlined memory.** The window is the
  RAM the stack is in; a guest whose RAM is mapped low takes the checked
  path for every access.
- **Interrupt latency under the JIT is a block**, and SysTick advances at
  block boundaries; a guest reading `SYST_CVR` inside a block sees the
  value from its start. The run total also counts one more per interrupt
  taken under the JIT than the interpreter does (the framework's
  convention).
- **IT blocks ending in a computed branch** -- `LDR pc`, `POP {pc}`,
  `BX` under a condition -- are given back to the interpreter. Those
  holding a load or a store are not any more: they were 6.4% of
  Dhrystone's instructions under the JIT, and are 1,033 of 9.06M. What
  is left is measured and is not worth a lowering yet: `declined IT:19`
  on Dhrystone, every one cold, and none on CoreMark or crypto.
- **The board's answers are not in the repository.** They live under
  `build/armv7m-*` and are regenerated by running the suites with the
  board attached. Kept as fixtures, `--rerun` would be a regression
  suite anyone could run without the board -- a decision about 3 MB of
  test data, not a technical one. The sp and pc answers are the
  exception, as a table in `test_sp_pc_board_answers`.
- **The IR and the register allocator are not exercised by `t2exec`**,
  which tests the encoder end to end and nothing above it. Running
  `emu_ir_lower` in the guest is the next layer.
- **The whole F746 firmware as a guest**, which was the original
  phrasing of this frontend's purpose and a much larger job: it drives
  366 distinct STM32 peripheral registers, so it needs an STM32F746
  machine model -- a direction change for a tree whose drivers live in
  the guest and reach real silicon through passthrough.
