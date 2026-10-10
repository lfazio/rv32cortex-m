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
| interpreter | 6,025 ms |
| JIT | 620 ms |

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
  conditional writes, or not at all.

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

## Pair statistics

`-DEMU_PAIR_STATS=ON` histograms adjacent executed pairs, as for the
other frontends. Register numbers are biased by one inside the table,
because the shared histogram reads 0 as "no register" and r0 is the
busiest register on ARM. Flags are not registers there, so `cmp; bne`
counts as independent. CoreMark: 29.0% of pairs are dependent, 8.2%
with a dead intermediate, 2.6% address generation into an access.

## Not done

- **Guests below 0x10000000 get no inlined memory.** The window is the
  RAM the stack is in; a guest whose RAM is mapped low takes the checked
  path for every access.
- **Interrupt latency under the JIT is a block**, and SysTick advances at
  block boundaries; a guest reading `SYST_CVR` inside a block sees the
  value from its start. The run total also counts one more per interrupt
  taken under the JIT than the interpreter does (the framework's
  convention).
- **IT blocks containing a load, a store or a conditional return** are
  given back to the interpreter: about 6% of Dhrystone's instructions.
- **The board's answers are not in the repository.** They live under
  `build/armv7m-*` and are regenerated by running the suites with the
  board attached.
