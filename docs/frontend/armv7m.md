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

**The system suite** (`sys.c`/`sys.S`): 54 tests of what an instruction
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
- **r15 is the pc, and the optimiser has to know it.** The pc slot is
  register 15 in this frontend's register file, and a block that ends
  on a computed target does it with `EXIT` on `GET r15` after something
  has written it. A pass that deletes pc writes nothing observes would
  delete that one — `EXIT` overwrites the pc — unless a `GET` of the
  register whose slot is the pc counts as an observer. It does, and
  `test_setpc_kept_when_a_register_reads_it` is that case.

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
`crypto`, `dhrystone` or `coremark`, embedded as the flat `.bin`: the
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

The ARMv7-M interpreter is **2.7x slower per instruction than RV32's**
on this part (290 host cycles against 107), which is the next thing to
look at for this frontend; and at the default 32 KB the JIT only wins
where the working set fits (Dhrystone 3.4x, `nvic` 3x) and loses where
it does not (crypto 2.2x slower), the same threshold
[../jit/tuning.md](../jit/tuning.md) describes for the other frontends.

## Not done

- **The interpreter's cost per instruction**, 2.7x RV32's on the F746;
  nothing has profiled it on the board.
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
  board attached.
