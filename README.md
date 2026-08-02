# rv32cortex-m

A RISC-V **RV32IMAC** emulator that runs on ARM Cortex-M microcontrollers, with
the emulated guest driving the host's **real peripherals** through an
identity-mapped passthrough window.

Validated against the official
[RISC-V Architecture Test Suite](https://github.com/riscv/riscv-arch-test) and
running on a **Nucleo-F446RE**.

---

## What this is

The emulator turns an ARM Cortex-M part into a RISC-V machine. The interesting
part is not the interpreter — it is the memory map:

```
guest 0x4000_0000..0x5FFF_FFFF   ─── identity map ───▶   STM32 APB1/APB2/AHB1/AHB2
```

That range is where a RISC-V platform leaves room for memory-mapped I/O, and
also exactly where the STM32 puts its peripherals. Mapping it one-to-one means
**a guest driver uses the addresses printed in the vendor reference manual**,
with no translation to reason about.

The consequence is the design's main idea:

> **Peripheral drivers live in the guest, not in the emulator.**

The emulator contains no GPIO driver, no UART driver, no SPI driver. It forwards
loads and stores. Porting to a new microcontroller means writing a clock setup,
a linker script and a region table — not a new driver stack. See
[`tests/guest/stm32drv.c`](tests/guest/stm32drv.c) for a working demonstration:
GPIO and USART2 drivers written entirely as RISC-V guest code, driving real
silicon.

---

## Status

| Area | State |
|---|---|
| RV32IMAFC + Zicsr, Zicntr, Zifencei, Zicbom, Zicboz, **B**, **Zacas**, **Zcf**, **Zcb** | implemented |
| Machine-mode traps, interrupts, CLINT timer | implemented |
| Official `riscv-arch-test` (RVCP) | **224 / 224** with SoftFloat; 172 / 224 with the host FPU |
| **F** (single precision) | implemented, two backends — see below |
| **D** (double precision) | not implemented, and not planned |
| **Zcd** | not implementable without D — see below |
| `riscv-tests` (Berkeley) | **77 / 77 pass** |
| **PMP** | 16 entries, TOR/NA4/NAPOT; inlined in the JIT for the single-entry case |
| **Sdtrig** | mcontrol triggers on execute/load/store; `rv32mi/breakpoint` passes |
| Guest ISA self-test (211 checks) | passes on host **and** on hardware, both backends |
| Nucleo-F446RE firmware | 31–54 KB flash by configuration; guest gets 70–122 KiB of the 128 KiB SRAM |
| Thumb-2 JIT backend | implemented; **19.2× slower than native ARM** on CoreMark with a 48 KB code cache, 15.3× with 64 KB, and *slower than the interpreter* at the 12 KB default — see below |
| **Zacas** (`amocas.w` / `amocas.d`) | implemented |
| V extension | not implemented |

The target the emulator was designed for is Cortex-M7; the board on hand is a
**Cortex-M4F**, which shares the ARMv7E-M instruction set. The core is portable
down to ARMv6-M (Cortex-M0+) and up to ARMv8.1-M.

---

## Guest memory map

Defined once in [`include/rv32/rv_memmap.h`](include/rv32/rv_memmap.h) and shared
by every platform, so a guest binary runs unchanged on the host simulator and on
the target.

| Base | Size | Kind | Contents |
|---|---|---|---|
| `0x0200_0000` | 48 KiB | virtual | CLINT — `msip`, `mtimecmp`, `mtime` (SiFive layout) |
| `0x1000_0000` | 256 B | virtual | Console UART — NS16550 subset |
| `0x2000_0000` | image | ROM | Guest image, executable in place from ARM flash |
| `0x4000_0000` | 512 MiB | **passthrough** | ARM peripherals, identity-mapped |
| `0x8000_0000` | 70–122 KiB | RAM | Guest RAM, whatever the firmware does not use |

### Passthrough policy

The peripheral window is not wide open. A per-region permission table
(`g_periph_map` in [`src/platform/stm32f446/main.c`](src/platform/stm32f446/main.c))
withholds only what would take the emulator down with the guest:

| Region | Access | Why |
|---|---|---|
| `PWR` | read-only | dropping over-drive at 180 MHz stalls the core |
| `RCC` `CR`/`PLLCFGR`/`CFGR`/`CIR` | read-only | reconfiguring the PLL kills the clock the emulator runs on |
| `FLASH` controller | read-only | it can erase this firmware underneath us |
| everything else | read/write | |

Note what is deliberately **not** withheld: the rest of RCC, including
`AHB1ENR` / `APB1ENR` / `APB2ENR`. A guest driver has to be able to ungate its
own peripheral's clock — denying that would push every driver back into the
firmware and defeat the design.

---

## Architecture

```
┌──────────────────────────────────────────────────────────┐
│  platform    host runner  │  STM32F446 firmware           │
│              ELF loader   │  ST HAL: clock, GPIO, USART   │
├──────────────────────────────────────────────────────────┤
│  devices     CLINT (timer)        UART (console)          │
├──────────────────────────────────────────────────────────┤
│  bus         region table, permissions, width checks,     │
│              passthrough translation, fast-path caches    │
├──────────────────────────────────────────────────────────┤
│  backend     rv_backend_t ──▶ interpreter | Thumb-2 JIT   │
├──────────────────────────────────────────────────────────┤
│  core        hart state · RVC expansion · CSRs · traps    │
└──────────────────────────────────────────────────────────┘
```

`src/core/` is portable C11 with no platform dependencies — it compiles
unchanged for ARMv6-M, ARMv7E-M, ARMv8.1-M and for a native host build.

### Backends

The core never calls the interpreter directly; it goes through
[`rv_backend_t`](include/rv32/rv_backend.h). `run` is budgeted rather than
free-running so a JIT can execute a whole translated block and report what it
retired, and `invalidate` lets `FENCE.I` and image loads discard translations.

**Thumb-2 JIT** ([`src/backend/rv_jit_thumb2.c`](src/backend/rv_jit_thumb2.c)).
RV32 basic blocks are translated into Thumb-2 held in a RAM code cache,
eliminating the per-instruction costs the interpreter cannot avoid: the bus call
to fetch, RVC expansion, the dispatch switch, the pc write and the counter
update.

Three design choices worth stating:

- **The guest register file stays in memory.** `hart->x` is at offset 0, so
  every access is a single 16-bit `LDR/STR Rt,[r4,#n]`. That sounds wasteful,
  but it means guest state is coherent at every instruction boundary — a trap,
  an interrupt or a debugger read needs no unwinding. Register allocation across
  a block is the next optimisation, not a prerequisite.
- **A per-block register cache is measured but not built.** Holding hot guest
  registers in `r8`–`r11` only pays if a block reads them enough to repay the
  load that sets each one up. Instrumenting the translator
  (`-DRV32_JIT_HOT_REG_STATS=ON`) says, over CoreMark's 12,602 translated
  blocks:

  | Register | Reads per block that uses it | Blocks using it |
  |---|---|---|
  | `sp` | **7.78** | 3,719 |
  | `a0` | **4.80** | 8,738 |
  | `a1` | 2.55 | 7,499 |
  | `ra` | **1.02** | 6,443 |

  `ra` is clearly not worth caching — read about once per block, so it would
  spend a load to save a load. `sp` and `a0` looked well worth it.

  **They were not.** A write-through cache holding `sp`, `a0` and `a1` in
  `r8`–`r10` was implemented, verified correct (139/139 on hardware, CoreMark
  CRC unchanged) and measured **15.5% slower**: 31.39 → 36.26 cycles per guest
  instruction. It is reverted.

  The read counts were real but measured the wrong thing. A cached read becomes
  `MOV rt, r8` where an uncached one is `LDR rt,[r4,#n]` — the same *one*
  instruction, saving roughly a cycle, not eliminating work. Meanwhile
  write-through adds an instruction to every write, and `a0`/`a1` are written
  about as often as they are read. Add three more registers to the `PUSH`/`POP`
  on all 5.45 M block entries and the arithmetic goes negative.

  The lesson is that reads-per-block was necessary but not sufficient: what
  matters is the *cost difference* between cached and uncached access, and with
  the register file already at a single load off `r4`, that difference was
  never a whole instruction to begin with.
- **PMP is inlined as a range test when it can be.** The inlined memory path
  writes guest RAM directly and cannot consult PMP, so arming a PMP entry
  originally forced every access through the helper. When exactly one entry is
  enabled — a guest protecting one buffer, which is the common shape — the JIT
  instead emits a subtract, a compare and a not-taken branch, sending only
  addresses *inside* that entry to the helper. Everything outside matches no
  entry, and the no-match rule for M-mode is allow, so the fast path stays
  correct. Two or more entries fall back to the helper, because then the lowest
  match wins and one compare cannot express which entry an address hits first.
- **Sdtrig disables translation entirely while a trigger is armed.** An execute
  trigger needs a check before every fetch, which a block cannot express. No
  flush is needed in either direction: blocks are only translated while unarmed
  and only executed while unarmed.
- **The guest-RAM registers are materialised lazily.** `r5`/`r6`/`r7` cost six
  halfwords and a block with no memory access has no use for them, so they are
  emitted at the first access that needs them rather than on entry. No pre-pass
  is required to make that safe: a block has one entry, so every path reaching
  an instruction has flowed through everything emitted before it, and the
  earlier exits leave before the emission point. Worth 3.9% on CoreMark
  (32.65 → 31.39 cycles per instruction).
- **Blocks end at every control transfer**, with no chaining or inline caching.
  Each block writes `h->pc` and returns, which bounds interrupt latency by one
  block rather than by a chain of them.
- **Untranslated encodings end the block and fall back to the interpreter.**
  The JIT is a fast path over the interpreter, never a replacement, so
  correctness does not depend on covering every encoding. `SYSTEM`, atomics and
  the `MULH`/`DIV`/`REM` helpers are currently interpreted.

Memory access stays in C and is reached by a helper call: it needs the region
walk, the permission and width checks and the fault path, none of which is worth
open-coding. A faulting helper enters the trap itself and returns a flag that
makes the block exit immediately.

**Floating point in the JIT.** The `f` registers live in `hart->f[]` rather than
in ARM VFP registers, which means the F operations that are *bit manipulation*
rather than arithmetic translate as ordinary integer code: `FLW`/`FSW` go
through the same inlined guest-RAM path as integer loads and stores, and
`FMV.X.W`, `FMV.W.X` and the `FSGNJ` family become a few loads, masks and
stores. None of them can raise an exception flag or depend on the rounding
mode, so the translation is provably identical to the interpreter's with no
FPSCR handling at all.

**The arithmetic is now real VFP.** `FADD`/`FSUB`/`FMUL`/`FDIV`/`FSQRT` and the
four fused multiply-adds are emitted as `VADD.F32`/`VSUB.F32`/`VMUL.F32`/
`VDIV.F32`/`VSQRT.F32` and `VFMA`/`VFMS`/`VFNMA`/`VFNMS`, wrapped in the FPSCR
handling that makes them architecturally correct:

- **Rounding.** `FPSCR.RMode` is set from the instruction's `rm`, or when that
  is `DYN`, looked up at run time from `fcsr.frm` through a two-bits-per-entry
  packed constant. `RMM` has no ARM equivalent, so instructions using it stay
  on the helper rather than silently rounding ties to even.
- **Flags.** ARM orders the cumulative bits `IOC, DZC, OFC, UFC, IXC` from bit
  0; RISC-V orders them `NX, UF, OF, DZ, NV`. That is the same five flags in
  exactly reversed order, so a 32-bit `RBIT` and a shift down by 27 converts
  one to the other — no table and no branches.
- **NaN.** `FPSCR.DN` is set, so a NaN result is ARM's default NaN, which is
  bit-identical to RISC-V's canonical `0x7FC00000`. `FZ` is cleared, because
  RISC-V requires real subnormals rather than flush-to-zero.

The fused multiply-adds are worth a note. ARM's four accumulate into `Sd` and
negate the *accumulator*; RISC-V's negate the *product*. The correspondence is
therefore not the one the names suggest — RISC-V `FMSUB` subtracts the addend,
which ARM expresses by negating the accumulator, so it maps to `VFNMA` and not
`VFMS`:

| RISC-V | | ARM |
|---|---|---|
| `FMADD` | `rs1*rs2 + rs3` | `VFMA` |
| `FMSUB` | `rs1*rs2 - rs3` | `VFNMA` |
| `FNMSUB` | `-rs1*rs2 + rs3` | `VFMS` |
| `FNMADD` | `-rs1*rs2 - rs3` | `VFNMS` |

The comparisons are translated too, and the correspondence is again exact.
`VCMP` raises the invalid flag only for a signalling NaN, which is `FEQ`'s
rule, and `VCMPE` raises it for any NaN, which is `FLT`'s and `FLE`'s. The
condition codes line up once unordered is accounted for — it leaves `N=0`,
`Z=0`, `C=1`, `V=1`, so `EQ`, `MI` and `LS` are all false for it, which is
RISC-V's requirement that every comparison against NaN be false.

`FCVT.S.W` and `FCVT.S.WU` are translated as `VCVT.F32.S32`/`VCVT.F32.U32`.

`FCVT.W.S` and `FCVT.WU.S` go the other way, as **`VCVTR`** — the rounding
form, since plain `VCVT` would force round-toward-zero whatever `frm` asked
for. ARM and RISC-V agree on more of this than they disagree on: both saturate
an out-of-range value to the limit of the target type, both raise invalid when
they do, and neither adds inexact on top. The entire divergence is one input —
ARM converts a NaN to zero, RISC-V to the *maximum* value of the target type —
so a `VCMP` of the operand against itself, which is unordered exactly for a
NaN, selects between them:

```
VCVTR.S32.F32 s4, s0
VMOV   r1, s4          ARM's answer
MOVW/MOVT r2, #0x7FFFFFFF   the replacement, set up before the compare
VCMP.F32 s0, s0        unordered iff s0 is NaN
VMRS   APSR_nzcv
IT     VS
MOVVS  r1, r2
```

The replacement is materialised *before* the compare, for the reason the
comparison path above documents. The compare is the quiet `VCMP`, not
`VCMPE`: the conversion has already raised invalid for a NaN, and the
signalling form would raise it again for a quiet NaN.

Measured on `isatest`, this moves 22 instructions per run off the helper.

**Routed through a helper rather than open-coded**, which is not the same as
declined — the block continues across them:

- `FMIN`/`FMAX` — ARMv7-M has no scalar `VMINNM`/`VMAXNM`.
- `FCLASS` — no ARM equivalent at all.

Open-coding these would mean reimplementing by hand which NaN wins, that a
signalling NaN raises invalid where a quiet one does not, that `-0.0` ranks
below `+0.0` for min and max although IEEE calls them equal, and the ten-way
split `FCLASS` reports. That is 25–35 emitted instructions each, for
instructions no hot loop contains, spent in the resource this JIT is shortest
of — the code cache sets overall performance more than anything in the
translator. It would also be a second implementation of semantics the core
already owns, which is the drift the conventions forbid; `rv_hart_fp` is the
interpreter's own entry point, validated at 224/224.

What changed is that they no longer **end the block**. Declining costs a
dispatcher round trip, an interpreted instruction and a fresh block beyond
it; a call costs five instructions. Measured by forcing the old behaviour
back, the self-test goes from 146 interpreted instructions and 755 block
entries to 122 and 732.

### What a block bakes in

A translated block records decisions taken from hart state at the moment it
was built, and then outlives that state. Every such read was swept:

| read at translation | outcome |
|---|---|
| `fcsr` frm, for `rm=dyn` | wrong for `RMM` — fixed |
| `mstatus.FS` | unchecked for OP-FP, stale for loads and stores — fixed |
| `pmp_active` and the `rv_pmp_simple` bounds | **permission bypass** — fixed |
| bus regions | safe: written only at init, before execution |
| the peripheral window's armed flag | safe: flushes when it changes |
| the guest instruction bytes themselves | safe: `FENCE.I` invalidates |

The PMP one was the worst. What a block bakes in is the *bounds* of the single
enabled entry, but the flush compared `pmp_active` — a boolean. Locking a
second entry leaves that flag true while the one-entry assumption stops
holding, so a store to the newly protected region kept taking the inlined
path. The self-test's `pmp2-write-blocked` reported `0xdeadbeef` where the
guest had denied writes: not a slow path taken by mistake, an access that
should have faulted and did not.

All three defects were invisible to `riscv-tests` and `riscv-arch-test`,
because neither runs the JIT, and each needed a hardware run with the fix
reverted to demonstrate.

frm, FS and PMP share a fix and a hook. Each changes only through a CSR write,
and the translator declines `SYSTEM`, so `jit_note_csr` on the interpreter
fallback is the one place any of them can move — which also keeps all three
off the dispatch path, where CoreMark would pay for them 2.9 million times a
run.

### `mstatus.FS` and cached blocks

FS gates the whole extension: with it Off every FP instruction must raise
illegal-instruction, `fmv` included. The JIT decided that when it *translated*
a block, which is half a guard — a block built while FS was on stayed in the
cache and kept running after the guest turned the FPU off. A targeted test
confirmed it: three instructions that had to trap produced no traps at all.
OP-FP and the fused multiply-adds were not consulting FS in the first place.

Fixed the way `frm` is: FS off-ness is recorded when a block is built, and a
change flushes. Both checks sit on the interpreter fallback, because a CSR
write to `mstatus` is the only thing that reaches Off — trap entry and `mret`
do not touch FS, and `emit_fp_dirty` only ever moves it away from Off. It is
*off-ness* that is tracked rather than the two-bit field, since `emit_fp_dirty`
moves Initial or Clean to Dirty on most operations and flushing for that would
discard the cache continuously. A guest with no FP never flushes for either:
CoreMark still reports one flush for its whole run.

**Still declined**, so the block ends there:

- Anything rounding **`RMM`** — ARM has no ties-away mode. Blocks are
  specialised on `frm`, so an `rm=dyn` instruction is resolved at translation
  and declined when it lands on `RMM`, just as a static `rmm` is.

### Cache-block operations

`Zicbom`/`Zicboz` map directly onto ARMv7-M cache maintenance, because both
architectures define the same operations over a block identified by an address:

| RISC-V | ARMv7-M (CMSIS) |
|---|---|
| `cbo.clean` | `SCB_CleanDCache_by_Addr` |
| `cbo.inval` | `SCB_InvalidateDCache_by_Addr` |
| `cbo.flush` | `SCB_CleanInvalidateDCache_by_Addr` |
| `cbo.zero` | stores zeros (no ARM equivalent) |

The guest address is translated to the host address that actually backs it
before the maintenance call, so a guest cleaning a DMA buffer cleans the very
ARM cache lines holding it. On the Cortex-M4 in the STM32F446 there is no data
cache and the maintenance operations are no-ops, which the spec permits; the
code is written against `__DCACHE_PRESENT` so it becomes real cache maintenance
when built for a Cortex-M7.

---

## Building

### Host (development and validation)

```sh
cmake -B build/host -DRV32_PLATFORM=host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host
ctest --test-dir build/host -L fast
```

### Nucleo-F446RE firmware

```sh
cmake -B build/stm32f446 -DRV32_PLATFORM=stm32f446 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DRV32_GUEST=isatest
cmake --build build/stm32f446
cmake --build build/stm32f446 --target flash     # probe-rs over ST-LINK
```

Console: USART2 on the ST-LINK virtual COM port, **115200 8N1**.

```sh
picocom -b 115200 /dev/ttyACM0
```

`-DRV32_GUEST=` selects the embedded guest image: `isatest`, `hello`, `bench`,
`stm32drv` or `coremark`.

The options that change what gets built:

| Option | Default | Effect |
|---|---|---|
| `RV32_JIT` | `ON` | Thumb-2 JIT. `OFF` is smaller, and is how a suspected JIT bug is isolated. |
| `RV32_JIT_CODE_BYTES` | `12288` | Code cache. A small value forces compaction and is a useful stress test. |
| `RV32_FPU_SOFTFLOAT` | `OFF` | Berkeley SoftFloat instead of the host FPU: conformant, ~5 KB larger. |
| `RV32_NATIVE_COREMARK` | `OFF` | Run CoreMark natively on the ARM instead of the emulator, for the baseline. |
| `RV32_EXT_PMP` / `RV32_EXT_SDTRIG` | `ON` | Each costs a little even unused; `OFF` removes it. |
| `RV_GUEST_MARCH` | see below | Guest ISA. A **cache variable**, so pass it explicitly when changing it. |

Guest images are built `rv32imafc_zicsr_zifencei_zicbom_zicboz_zba_zbb_zbc_zbs_zacas`.
`Zcb` is supported by the emulator but deliberately absent there — see the
performance section.

### Vendor driver pack

The firmware uses ST's own code wherever it exists — CMSIS-Core, ST's CMSIS
device layer (register definitions, `startup_stm32f446xx.s`,
`system_stm32f4xx.c`) and the STM32Cube HAL for clock, GPIO and USART bring-up.
Nothing reimplements a peripheral the vendor already supports.

These are fetched at configure time from ST's modular repositories. To build
offline, point at local checkouts named `cmsis_core`, `cmsis_device_f4` and
`stm32f4xx_hal_driver`:

```sh
cmake -B build/stm32f446 ... -DSTM32CUBE_LOCAL_DIR=/path/to/checkouts
```

---

## Validation

Two suites, both wired to CMake targets:

```sh
cmake --build build/host --target arch-test        # official RISC-V suite
cmake --build build/host --target arch-test-quick  # base integer only
cmake --build build/host --target riscv-tests      # Berkeley suite
cmake --build build/host --target validate         # everything
```

Current state, all re-run on the tree as it stands:

| | result | runs on |
|---|---|---|
| `riscv-arch-test`, `-DRV32_FPU_SOFTFLOAT=ON` | **224 / 224** | host |
| `riscv-arch-test`, default (host FPU) | 172 / 224 — every failure in `F` | host |
| `riscv-tests` | **77 / 77** | host |
| host unit + guest self-test (`ctest -L fast`) | **2 / 2** | host |
| `isatest`, JIT | **211 / 211** | hardware |
| `isatest`, `-DRV32_JIT=OFF` | **211 / 211** | hardware |
| `isatest`, host, both FP backends | **211 / 211** | host |
| `mmiobench` | **72 / 72** | hardware |
| CoreMark | `crcfinal 0xca90` on all three backends | hardware |

**What these suites do not cover: the JIT.** Both run the host interpreter, so
nothing in `src/backend/rv_jit_thumb2.c` is exercised by either — it only
compiles for ARM. Everything the translator emits is validated by `isatest`
and `mmiobench` on the board, and by CoreMark's CRC agreeing across native
ARM, interpreter and JIT.

That gap is not theoretical. Every JIT defect found so far was found on
hardware, and none of them could have been caught by a signature-checking
suite:

| defect | how it presented |
|---|---|
| inlined store ignored PMP | a protected region was writable under the JIT only |
| inlined store skipped the LR/SC reservation break | a later `SC` wrongly succeeded |
| loop cap compared the wrong register | *no* wrong answer — chained loops simply ran unbounded |
| loop chaining dropped past a branch's reach | *no* wrong answer — one loop shape ran 2.4× slower |
| `rm=dyn` resolved `RMM` as round-to-nearest | ties rounded to even where the guest asked for away |
| `mstatus.FS` decided at translation | FP ran after the guest turned the FPU off |
| PMP flush watched a flag, not the configuration | a store landed in memory PMP had been told to deny |

Two of those produced no wrong answer at all, only performance that made no
sense. Three were staleness: a decision taken when a block was translated,
still in force after the state behind it changed. The self-test grew from 148
checks to 211 chasing them, and the checks that matter are the ones that
re-execute *one* instruction at *one* address after changing the state it was
compiled against — a fresh call site is translated against the current
configuration and proves nothing.

### Floating point (F)

F is implemented — the register file, `fcsr`/`frm`/`fflags`, `mstatus.FS`,
all of OP-FP, the four fused multiply-adds, `FLW`/`FSW`, and `Zcf`'s
compressed FP load/stores (which `C` on RV32F is defined to include).

There are two implementations, chosen with `-DRV32_FPU_SOFTFLOAT`.

**Berkeley SoftFloat (`ON`) passes all 224 tests.** It is the library the
RISC-V FP spec was written against, and the fit is exact rather than
convenient: its rounding modes are numerically identical to `frm`
(`near_even`/`minMag`/`min`/`max`/`near_maxMag` = 0..4) and its exception
flags identical to `fflags` (1/2/4/8/16), so neither needs translating. It
also ships a `RISCV` specialization carrying the canonical-NaN and
NaN-propagation rules. `f32_mulAdd` is a genuine single-rounding fused
multiply-add, which is where most of the previous failures were.

**The host FPU via `<fenv.h>` (`OFF`, the default) passes 172 of 224.** It is
smaller and faster — one hardware instruction per operation on a Cortex-M4F —
but the flags it can report are the ones the hardware happens to raise, and
those differ from RISC-V's rules on the fused multiply-adds and around
subnormals. It remains the default because most guests never look at `fflags`,
and conformance is a build option away.

That split is the useful outcome: correctness when it is wanted, size and speed
when it is not. On the Nucleo, SoftFloat costs about 8.7 KB of flash
(51.3 KB against 42.6 KB with the JIT enabled), which on a 512 KB part is
affordable but is real.

Both are validated on hardware. Note that with the JIT enabled the arithmetic
is translated to VFP and never reaches SoftFloat, so the run that actually
exercises it is `-DRV32_JIT=OFF`; both configurations pass 211/211. That the
two backends agree is worth having deliberately — they use genuinely different
FP implementations, VFP against SoftFloat, so the self-test doubles as a
differential check between them.

**D is not implemented and is not planned**: the Cortex-M4F and M7 FPUs are
single-precision, so D would be entirely soft-float on the intended targets.
**Zcd follows from that** — it is the compressed *double* load/stores
(`c.fld`/`c.fldsp`/`c.fsd`/`c.fsdsp`), which target 64-bit FP registers that do
not exist without D. It is ruled out by the D decision, not separately skipped.

**Zcb is implemented and passes 7/7.** It reuses the `funct6=100111` slot that
RV64 spends on `c.subw`/`c.addw`: bits [6:5] select `c.mul` or a group of unary
operations, and the byte/halfword accesses sit in quadrant 0 under `funct3=100`.
Three of the unary ops (`c.sext.b`, `c.zext.h`, `c.sext.h`) expand to Zbb
instructions, which is why the spec makes Zcb depend on Zbb — without it there
would be nothing to expand them into.

### Official RISC-V Architecture Test Suite — 224/224 with SoftFloat

[`riscv/riscv-arch-test`](https://github.com/riscv/riscv-arch-test), the RVCP
suite governed by RISC-V International. Modern versions are self-checking: the
build runs the **Sail golden model** to compute expected results and bakes them
into each test, which then reports `RVCP-SUMMARY: TEST PASSED/FAILED` and sets
its exit status.

Our device description lives in
[`tests/arch-test/`](tests/arch-test/rv32cortex-m-rv32imac) — a UDB
configuration, a Sail model configuration, the `RVMODEL_*` macros and a linker
script — and is version controlled with the emulator rather than inside a
third-party clone. `scripts/run-arch-test.sh` fetches the suite, the Sail model
and the UDB gems, then builds and runs.

Prerequisites beyond the normal toolchain: `uv`, Ruby, and Bundler
(`gem install --user-install bundler`).

### riscv-tests — 77/77

The older Berkeley suite: `rv32ui`, `rv32um`, `rv32ua`, `rv32uc` and `rv32mi`.
All of it passes, none skipped — `rv32mi/pmpaddr` once PMP was implemented,
and `rv32mi/breakpoint` once Sdtrig was.

It has no `rv32uf`, so it contributes nothing to FP coverage; that comes
entirely from arch-test's `F` family. Two of its tests are load-bearing in a
way the count hides: `rv32mi/csr` fails on purpose when the runner's `-march`
disagrees with what `misa` advertises, and `rv32mi/breakpoint` is the single
test standing between the default build and the 29% interpreter gain that
compiling Sdtrig out would give.

### Bugs these suites caught

Worth recording, because each was a genuine defect:

| Found by | Defect |
|---|---|
| unit test vs. assembler ground truth | `C.ADDI4SPN` took its destination register from bits `[9:7]` instead of `[4:2]`, corrupting every guest stack-frame address computation |
| `riscv-tests` `instret_overflow` | a CSR write to `minstret` must *replace* that instruction's increment, not be followed by it |
| `riscv-arch-test` `Zicntr` | the Sail config declared a clock tick every 100 instructions while the emulator ticks every instruction |
| `riscv-arch-test` `Zacas` | the Sail config declared `atomic_support: AMOArithmetic` on guest RAM, so the golden model **trapped** on `amocas` and baked trap-derived values into the signatures — three sessions were spent looking for an emulator bug that was never there |
| `riscv-tests` `rv32mi/csr` | the suite was built for `rv32imac` while `misa` advertised F. The test detects exactly that mismatch and fails on purpose; the emulator was correct and the runner's `-march` was not |
| hardware `isatest` | the JIT's inlined store wrote guest RAM without consulting PMP, so a protected region was writable under the JIT and not under the interpreter |

The RVC expansion table in [`tests/unit/test_decode.c`](tests/unit/test_decode.c)
is assembler-derived, not hand-computed: each entry was produced by assembling
the compressed form and its 32-bit equivalent and reading both back with
`objdump`.

---

## Performance

Measured on the Nucleo-F446RE (Cortex-M4F @ 180 MHz, code in flash with 5 wait
states and the ART accelerator enabled), using
[`tests/guest/bench.c`](tests/guest/bench.c) — a compute-bound workload with no
I/O between the start and end markers.

### CoreMark: native vs interpreted vs JIT

The same CoreMark sources, 150 iterations, on the same 180 MHz Cortex-M4 —
compiled natively for ARM, and compiled for RV32 and run under each backend.
This is the number that says what emulation actually costs.

These use the full B extension in the guest, which is the best configuration
for each backend.

| | Ticks (µs) | Iterations/s | CoreMark/MHz | vs native |
|---|---|---|---|---|
| **Native ARM** | 336,130 | 446.3 | 2.479 | 1× |
| **JIT**, 64 KB code cache | 5,148,168 | 29.1 | 0.162 | **15.3× slower** |
| JIT, 48 KB | 6,463,217 | 23.2 | 0.129 | 19.2× slower |
| JIT, 32 KB | 8,525,192 | 17.6 | 0.098 | 25.4× slower |
| JIT, 24 KB | 9,329,706 | 16.1 | 0.089 | 27.8× slower |
| JIT, **12 KB — the default** | 10,850,998 | 13.8 | 0.077 | 32.3× slower |
| Interpreter | 10,691,637 | 14.0 | 0.078 | 31.8× slower |

All rows at 150 iterations, `crcfinal 0xca90` throughout.

Re-measured after the correctness work on `frm`, `mstatus.FS`, PMP staleness
and the FP translations: 10,851,525 at 12 KB and 6,375,722 at 48 KB, against
the 10,850,998 and 6,463,217 above. Both inside the ±3% layout noise, with
identical compaction and eviction counts — the translated code is byte for
byte what it was. None of those fixes costs a guest that does not use the
feature anything, which the flush count confirms: CoreMark reports one flush
for a whole run.

**The JIT's speed is set by `RV32_JIT_CODE_BYTES` more than by anything in the
translator**, and the default is not the configuration to quote. CoreMark's
translated working set is about 48 KB; below that the cache thrashes, and the
compaction counts show it directly — 231 compactions at 64 KB, 904 at 48 KB,
8,533 at 12 KB, with evictions going from 8,585 to 94,240.

At the 12 KB default the JIT is **slower than the interpreter**. That is the
number a fresh checkout reproduces, and it means the default currently buys
12 KB of SRAM worth of nothing: a build that small should use
`-DRV32_JIT=OFF` and hand the guest all 122 KiB instead. The JIT starts
earning its RAM at about 24 KB and is worth 2.1× at 64 KB.

The cost is guest RAM, one for one: 122 KiB with no JIT, 106 at 12 KB, 70 at
48 KB, 54 at 64 KB. Which end of that to sit at depends on whether the guest
needs memory or speed, so it is left as a build option rather than decided
here — but every performance figure below is quoted at **48 KB**, and figures
from any other size are not comparable.

### Driver performance: the passthrough window

CoreMark and `bench` are deliberately I/O free, so neither says anything about
the path a guest *driver* takes. `mmiobench` measures that one. Each kernel
runs twice with identical machine code, once against the peripheral window and
once against guest RAM; the RAM form is the control, so dividing removes the
loop overhead and leaves the access path.

Nanoseconds per access on the F446, `RV32_JIT_INLINE_PERIPH` off and on:

| kernel | helper | inlined | speedup | vs its RAM control |
|---|---|---|---|---|
| `read` — load a status register | 1493 | **654** | 2.28x | 1.34x |
| `write` — store a command register | 1236 | **558** | 2.22x | 1.44x |
| `rmw` — read-modify-write | 2286 | **735** | 3.11x | 1.38x |
| `poll` — read, test a bit, branch | 1337 | **479** | 2.79x | 1.15x |

Overall 46.66 to **24.40** cycles per guest instruction. The RAM controls move
by at most 4%, which is inside the noise, and CoreMark by 0.66% — the point of
arming the path from guest behaviour rather than always emitting it.

The residual 1.15-1.44x over RAM is not emulator overhead: a GPIO register on
AHB1 costs more to reach than SRAM on this part, and native ARM code pays that
too.

### Tuning the loop cap

`RV_JIT_LOOP_CAP` is how many guest instructions a chained loop runs before
returning to the dispatcher. Interrupts are delivered between blocks, so it is
directly the guest's worst-case interrupt latency — a throughput-against-
latency knob, not a free parameter. Measured on the F446, cycles per guest
instruction:

| | 64 | 128 | 256 |
|---|---|---|---|
| `bench` | 18.88 | 18.39 | 18.13 |
| `mmiobench` | 24.40 | 23.46 | 22.99 |
| CoreMark | 31.39 | 31.16 | 31.25 |
| worst-case latency | ~11 µs | ~22 µs | ~44 µs |

**CoreMark does not care at all.** Its loops end on branches the translator
cannot chain, so the cap is not what exits them — its 0.7% spread is inside
the noise. The tightest loops care most: `mmiobench`'s block entries halve
exactly with each doubling (25,125 → 12,810 → 6,600), and its RAM-only
kernels gain 5% at 128 and 18% at 256.

Each doubling returns about half of the previous one, which puts **128** on
the knee, and that is the default. The cost is linear and certain where the
gain is small and diminishing: 256 buys a further 2% on aggregate for double
the latency again, which is a poor trade for an emulator whose guest drives
real peripherals. Drop to 64 if a guest ISR has a deadline tighter than
~22 µs; `-DRV32_JIT_LOOP_CAP=` sets it.

**Two bugs surfaced here that had nothing to do with peripherals**, both found
because inlining changed block sizes and made them observable:

*Chaining was silently dropped for large blocks.* The loop back edge was only
emitted when it fit the 16-bit conditional branch's ±254 bytes; past that the
block quietly stopped chaining instead of getting a wider encoding. Inlining
pushed two-access loop bodies over the line and cost them 2.4x. Now the
32-bit form (±1 MB) is used exactly when the short one will not reach, so the
common case pays nothing.

*A silently malformed compare.* `RV_JIT_LOOP_CAP` is enforced by comparing the
retired-instruction accumulator in r8 against the limit — through the 16-bit
`CMP`, which encodes only r0-r7. Passing it r8 set a bit belonging to the
other operand and it assembled as `CMP r0, r1`. The cap therefore never
applied: chained loops ran to completion in one block entry, 3700 guest
instructions where 64 was intended. It read as a throughput win and was
really the interrupt-latency bound being discarded. Fixing it restored 64
(`blk entr` 434 to 25125 on `mmiobench`) at about 10% on tight loops.

The lesson both share is the one this file keeps relearning: **on ARM, a
register number that does not fit the encoding does not fail — it assembles
as a different instruction.** Neither bug produced a wrong result, so no test
caught either; they only showed up as performance that made no sense.

**The interpreter costs 46% for two features almost no guest uses.** Measured
by compiling each out:

| Configuration | cycles/guest insn |
|---|---|
| neither | **35.18** |
| PMP only | 38.36 |
| Sdtrig only | 49.96 |
| both (default) | 51.38 |

So Sdtrig is 14.8 cycles per instruction and PMP 3.2. The JIT pays far less
because it tests `trig_active` once per block dispatch rather than per
instruction.

The obvious fix is not the fix. Hoisting `trig_active` into a local, so the
fetch path tests a register instead of loading hart state, measured *slower*
— 51.38 to 53.66 — because maintaining it across the loop costs more register
pressure than the load did. The cost is not the load; the likely culprit is
the `TRAP` call site the check introduces into the fetch sequence, which
constrains register allocation for the whole dispatch loop. Confirming that
means reading the generated code, not guessing again.

`-DRV32_EXT_SDTRIG=OFF` recovers 29% today, at the price of `rv32mi/breakpoint`
and a 76/77 on `riscv-tests`. Both remain on by default because conformance is
the more defensible default for an emulator, but a deployment that will never
attach a debugger should turn Sdtrig off.

Full extension set — F, B, Zacas — compiled into the emulator. `Zcb` is
supported but deliberately **not** used by the guest; see below.

**CoreMark cannot measure the VFP work.** It is integer-only: our port sets
`HAS_FLOAT 0` to keep soft-float out of the guest, so not one translated FP
instruction executes. Showing the FP translation as a speedup needs an
FP-bearing benchmark, which this is not.

All three produce **`crcfinal 0xca90`** — native ARM and emulated RISC-V agree
bit for bit, which is independent confirmation that the emulation is correct.

The 2.49 CoreMark/MHz native figure is in the expected band for a Cortex-M4,
which is a useful sanity check on the measurement itself.

*(These are not reportable CoreMark scores: EEMBC requires a ≥10 s run and a
specific disclosure format, and the native run takes 0.34 s. They are valid
as a relative comparison, which is what they are used for here.)*

### Per-instruction cost

Host cycles per guest instruction, measured on hardware. The B extension
column is the same guest source rebuilt with `-march=..._zba_zbb_zbc_zbs`.

| Workload | | Interpreter | JIT |
|---|---|---|---|
| CoreMark | RV32IMAC | 35.7 | 35.9 |
| CoreMark | + B | **28.7** | 33.4 |
| `bench`  | + B | 127.2 | **28.7** |

*(The interpreter column is from before PMP and Sdtrig were added; both cost it
a little. The JIT column is current.)*

B is a clear win for the guest: it removes 12% of CoreMark's instructions
(42.94 M → 37.67 M) and takes the interpreter from 35.7 to 28.7 cycles each.
Against native ARM, CoreMark interpreted improves from 26.3× to **17.7×**.

The JIT does not benefit as much, and on CoreMark it is now the slower of the
two. That is an honest open result rather than a tuned one — see below.

### What the JIT needed along the way

Each of these was found by measurement, not by inspection:

1. **Inlining the guest-RAM fast path.** Loads and stores had each been a
   helper call; describing guest RAM in callee-saved registers turned a RAM
   access into a subtract, a compare and a register-offset load.
   CoreMark 47.9 → 35.7.
2. **Translating Zbb.** Untranslated Zbb ended a block every time and fell back:
   307,128 fallbacks. Adding `clz`/`ctz` (`RBIT`+`CLZ`), `min`/`max`,
   `andn`/`orn`/`xnor`, `rol`/`ror`, `sext.*`/`zext.h`, `rev8` → 89 fallbacks,
   44.2 → 35.9.
3. **A helper call for what cannot be translated.** `MULH`/`DIV`/`REM`, `clmul`,
   `cpop` and `orc.b` have no short Thumb-2 form. Ending the block for them cost
   far more than the instruction: CoreMark took 175,305 fallbacks and `bench`
   40,010. Calling a helper instead keeps the block intact — **fallbacks fell to
   1 and 3**, `bench` went 64.5 → 34.3 and CoreMark 46.3 → 40.8.

The recurring lesson is that what a translator *declines* costs more than what
it translates badly.

### Block chaining, and how it was found

The JIT trailed the interpreter on CoreMark, and the first explanation was
wrong. Per-operation counters showed **175,216 `clmul` helper calls** against 88
for multiply/divide, so `clmul` was translated inline — a shift-and-XOR loop
that exits at the highest set bit instead of the helper's fixed 32 iterations.
It worked and it barely mattered: **1.3%**.

The arithmetic said why: an ~11.8 cycle-per-instruction gap over 37.67 M
instructions is ~446 M cycles, which 87 k operations cannot account for. It was
spread across everything.

A block-entry counter found it — **9,141,951 entries for 37,670,524
instructions, an average block of 4.12 instructions.** Every block paid a hash
lookup, `PUSH {r4-r7,lr}`, three constants for the guest-RAM registers, an
epilogue and a return: 30–40 cycles amortised over four instructions.

So blocks were extended rather than given more instruction coverage:

- **Forward `JAL` is simply followed.** Nothing is emitted for it at all and
  translation continues at the target. Backward jumps still end the block —
  they are loop back edges, and returning to the dispatcher there is what
  bounds interrupt latency and stops translation looping over a body.
- **A conditional branch only exits on the taken path.** The fall-through keeps
  being translated behind a short skip-branch over the exit stub. CoreMark is
  full of forward if/else branches that previously ended a block every few
  instructions.

| | Blocks entered | Insns/block | cyc/insn |
|---|---|---|---|
| CoreMark before | 9,141,951 | 4.12 | 40.5 |
| CoreMark after | 5,454,702 | **6.91** | 32.6 |
| `bench` after | 158,025 | **8.07** | 27.4 |

**CoreMark 19.8% faster, `bench` 20% faster**, and against native ARM CoreMark
goes from 25.4× to **20.2×** slower. The JIT still trails the interpreter on
CoreMark (32.6 vs 28.7) but the gap is now 14% rather than 41%; the remaining
per-block cost is the prologue, and the next step is to emit the guest-RAM
registers only for blocks that actually access memory.

Zbc's `clmul` is not translated: ARMv7-M has no carry-less multiply (`PMULL` is
a NEON/crypto instruction, absent on Cortex-M).

### Block retention

The code cache keeps the most-used blocks rather than discarding everything
when it fills. Blocks are relocatable — every guest pc and helper address is an
absolute `MOVW`/`MOVT` constant and the only pc-relative branch is internal —
so compaction slides the hot ones down and drops the cold, with an ageing pass
so past popularity decays.

It matters at realistic cache sizes. CoreMark's working set does not fit in
12 KB:

| Code cache | Translations | Compactions | Ticks |
|---|---|---|---|
| 12 KB | 106,799 | 3,676 | 12,881,419 |
| 48 KB | 3,005 | 168 | 11,462,247 |

### `-Os` versus `-O3`

The STM32F446's ART accelerator holds about 1 KB of instructions, so a smaller
emulator might plausibly fit its hot loop better. Measured, on the same guest
and the same run length:

| Build | Flash | cycles/guest insn |
|---|---|---|
| `Release` (`-O3`) | 49,652 B | **57.2** |
| `MinSizeRel` (`-Os`) | 33,240 B | 62.2 |

**A third smaller, and 8.8% slower.** The accelerator is evidently not the
binding constraint — what `-Os` gives up in inlining and loop structure costs
more than the extra code density recovers.

`-O3` is therefore the default. `-Os` is the right choice on a part where flash
is genuinely scarce: 33 KB against 50 KB is a real difference on a 64 KB device,
and 9% of emulator speed is a reasonable price for fitting at all.

*(These figures come from a short run and include startup, so they are higher
than the 150-iteration numbers above. Only the ratio between them is meant.)*

### Two optimisations that did not work

Recorded because the negative results are as informative as the wins:

**Interpreter in SRAM** (`RV32_INTERP_IN_RAM`, default off) measured *slower* —
162 vs 122 cycles — while also taking 8 KiB from the guest. Executing from flash
lets the Cortex-M4 fetch over the I-bus while data goes to SRAM over the D-bus,
and the ART accelerator keeps a hot loop effectively wait-state free; moving code
into SRAM puts fetch and data on the same interface and serialises them. Kept as
an option because the trade-off is part-specific.

**Lazy interrupt evaluation** (`RV32_LAZY_IRQ`, default on) showed no measurable
gain, which is the expected result: with `mstatus.MIE` clear the eager check
already returns after one load and one test. It pays when a guest enables
interrupts. Measurements across these interpreter builds span 155–160 M cycles
for identical instruction counts, so differences of a few percent are code
layout, not algorithm.

The two optimisations that mattered:

1. **Bus fast paths.** Every emulated instruction did one or two instruction
   fetches plus up to one data access, each an out-of-line call into a region
   walk with permission and width checks. Caching the last plain-memory region
   per access kind turns the common case into a compare and a load.
2. **LTO.** Without cross-translation-unit inlining, the interrupt check and the
   memory helpers stayed real calls on every instruction.

**Measure compute, not the console.** For contrast, on the same build:

| Guest | Reported | What it actually measures |
|---|---|---|
| `bench` | 122 cyc/insn, 1.48 MIPS | the interpreter |
| `stm32drv` | 330 cyc/insn, 545 KIPS | mostly USART2 TX waits and MMIO polling |
| `isatest` | 427 cyc/insn, 421 KIPS | mostly 115200-baud console and the timer spin |

Only `bench` is a throughput figure. The other two are dominated by waiting on
real hardware, which is a property of the workload, not of the emulator.

Remaining headroom, in rough order of expected value: allocating guest registers
to ARM registers across a block (the JIT still loads and stores every operand,
which is the bulk of the remaining 25× gap), chaining blocks so a hot loop stops
returning to the dispatcher, and teaching the translator `DIV`/`REM`/`MULH`.

---

## Debugging

```sh
cmake --build build/stm32f446 --target gdbserver   # OpenOCD on :3333
gdb-multiarch build/stm32f446/src/platform/stm32f446/rv32-stm32f446.elf \
  -ex 'target extended-remote :3333'
```

The host `gdb` on Debian is x86-only; use `gdb-multiarch`, or `probe-rs gdb`.

Useful when a guest misbehaves:

- `report_state()` on the target prints guest `pc`, `mcause`, `mepc`, `mtval`.
- `rv32-host --dump` prints the full guest register file on exit.
- `RV32_ENABLE_TRACE=ON` builds a per-instruction trace hook.
- A `HardFault` on the ARM side usually means the passthrough window let a guest
  access reach an address the ARM bus rejects — the region table is where to
  look.

---

## Porting to another target

Because drivers live in the guest, the port surface is small:

1. **Clock setup** — one function, ideally using the vendor HAL.
2. **Console transport** — two callbacks (`tx`, `rx`) for the virtual UART.
3. **Linker script** — place `.guest_ram` between `.bss` and the stack; it is
   sized by the linker so the guest automatically gets all unused SRAM.
4. **Region table** — the peripheral windows and their permissions.
5. **Cache ops** — `NULL` if the part has no cache.

The guest is told how much RAM it has at reset (`sp`, `a0` = hartid, `a1` = RAM
size), so guest images do not hardcode a size.

For a different ARM core, retarget with
`-DRV32_ARM_CPU=cortex-m7 -DRV32_ARM_FPU=fpv5-d16`, or
`-DRV32_ARM_CPU=cortex-m0plus -DRV32_ARM_FPU=` for ARMv6-M.

---

## Repository layout

```
include/rv32/     public headers — the core's entire API
src/core/         portable interpreter: hart, bus, decode, CSRs, traps
src/backend/      Thumb-2 JIT: emitter, translator, code cache
src/devices/      virtual CLINT and console UART
src/platform/
  host/           native runner, ELF loader (host-only; never built into firmware)
  stm32f446/      Nucleo-F446RE firmware, ST HAL integration, linker script
tests/
  unit/           host unit tests: RVC expansion, bus permissions
  guest/          RISC-V programs that run inside the emulator
  arch-test/      DUT description for the official suite
scripts/          validation runners
docs/<vendor>/    reference documentation
```

Guest images (`tests/guest/`):

| Image | Purpose |
|---|---|
| `isatest` | 104-check RV32IMAC self-test, including traps and CBO |
| `hello`   | smallest useful guest; confirms the console path |
| `bench`   | compute-bound workload for throughput measurement |
| `stm32drv`| GPIO and USART2 drivers written as guest code |
| `coremark`| CoreMark, fetched from upstream and built for RV32 |

---

## Documentation

Reference material, classified by vendor under `docs/`:

| Path | Document | Used for |
|---|---|---|
| `docs/riscv/riscv-spec-unprivileged-2026-07-29.pdf` | RISC-V Unprivileged ISA, release `310a111` | instruction semantics, RVC expansion, the B and F encodings |
| `docs/arm/DDI0403E_e_armv7m_arm.pdf` | ARMv7-M Architecture Reference Manual | Thumb-2 and VFP encodings for the JIT — A4.13 for floating point, A7.7 for the instruction details |
| `docs/arm/DDI0419E_armv6m_arm.pdf` | ARMv6-M Architecture Reference Manual | what the core must avoid to stay portable to Cortex-M0+ |
| `docs/arm/DDI0553B_z_armv8m_arm.pdf` | ARMv8-M Architecture Reference Manual | the other end of the portability range |
| `docs/st/rm0390-...pdf` | RM0390 — STM32F446xx Reference Manual | the peripheral addresses the passthrough window exposes, and which of them the guest may write |
| `docs/st/stm32f446mc.pdf` | STM32F446 datasheet | pin functions and clock limits for the board bring-up |
| `docs/st/pm0223-...pdf` | STM32 Cortex-M0 programming manual | reference for an eventual M0+ port |

Not collected: the RISC-V **Privileged** ISA specification, which the trap,
CSR, PMP and Sdtrig work was written against from the online version. Worth
adding, since it is the reference for most of `src/core/`.

---

## Roadmap

Ordered by what would most repay the effort.

**Performance.** The JIT is 19.2× native at a 48 KB code cache and the remaining cost is structural
rather than a missing optimisation:

- [x] **Chain across loop back edges** — *done*. A backward branch to the block's
      own start now branches within the translated code instead of returning
      to the dispatcher. `bench` went from 158,025 block entries to 40,285 and
      **28.66 → 18.68 cycles per guest instruction, 35%**; CoreMark 33.35 →
      31.04, 7%. `RV_JIT_LOOP_CAP` bounds interrupt latency at 64 guest
      instructions, since delivery happens between blocks.

      That bound was not real until later: the cap compared the wrong
      register (see *A silently malformed compare*, below), so chained loops
      ran to completion in a single block entry. Fixing it cost tight loops
      about 10% and cost `bench` 1% -- 18.68 to 18.88 cycles per guest
      instruction -- which is what bounded interrupt latency actually costs.

      With the bound real, the cap was tuned. `RV_JIT_LOOP_CAP` is now
      **128**; see *Tuning the loop cap* below.

      Getting there took three wrong versions, all of which *ran correctly*
      and miscounted retired instructions — which matters, because that count
      feeds `mcycle`, `minstret` and the run budget, and because dividing host
      cycles by an inflated count once produced an apparent 2.75× win that was
      pure artefact. What fixed it: chain only to the block start, so one
      constant is right on the first pass and every iteration; put the
      accumulation *before* the conditional split, so both paths account for
      the same instructions; and have each exit add only what it retired since
      that point. **The instruction count is the first thing to check when a
      JIT change looks too good.**

- [x] **Fewer helper calls for memory** — *done*. The passthrough window is
      inlined alongside guest RAM, worth **2.2-3.1x** on driver-shaped access
      (see *Driver performance* above). Two things had to be true first: the
      window is an identity map, so the emitted access is a bare load from the
      address register; and its read-only sub-ranges are few and small enough
      to test as holes punched out of one range check.

      It is armed by the guest rather than always emitted, because the code is
      not free -- about 18 bytes per load and 48 per store, which grew
      CoreMark's translated image past the 48 KB cache and cost it 53%. What
      is left here is a *third* window for the guest ROM, which would only
      matter for an execute-in-place guest; the images built here run from
      RAM, so it would be unmeasurable.

**ISA.** What is left is either small or deliberately excluded:

- [x] **`FCVT.W.S` / `FCVT.WU.S` in the JIT** — *done*. `VCVTR` plus a
      compare-against-self for the NaN case, seven instructions; 22 per
      `isatest` run move off the helper. Covered by 25 new self-test checks
      spanning NaN of both signs, both infinities, over- and under-range,
      every rounding mode the translation claims, dynamic rounding, and the
      exception flags — the one pre-existing check (`10.0` with `rtz`) would
      have passed with the NaN fixup deleted entirely.
- [x] **`FMIN`/`FMAX`, `FCLASS` in the JIT** — *done*, by routing them to a
      helper rather than open-coding them. No ARMv7-M equivalent exists, and
      hand-rolling RISC-V's NaN, signalling-NaN and signed-zero rules would
      cost 25–35 emitted instructions each in the resource that turned out to
      set JIT performance overall. The win was never avoiding the call: it is
      that these no longer end the block. 146 interpreted instructions and
      755 block entries on the self-test become 122 and 732.

      Covered by 19 new checks pinning the cases an inline version would get
      wrong — two NaNs giving the canonical NaN rather than either input,
      `-0.0` ranking below `+0.0` for both operations in both operand orders,
      quiet against signalling NaN for the invalid flag, and every one of
      `FCLASS`'s ten bits.
- [x] **`RMM` in the JIT** — *resolved*, by declining it correctly rather
      than by translating it. No ARMv7-M rounding mode expresses ties-away,
      so it stays on the helper; what changed is that it now reliably gets
      there.

      Blocks are **specialised on `frm`**. A `dyn` instruction is resolved at
      translation against the `frm` then in force, so `dyn` under `frm=RMM`
      is declined exactly as a static `rmm` always was. Previously the mode
      was resolved at run time through a packed table whose `RMM` entry was
      `RN`, so such a guest got ties-to-even where it asked for ties-away —
      silently, and only under the JIT.

      The flush that makes specialisation safe costs nothing on the hot
      path. `frm` moves only on a CSR write, the translator declines
      `SYSTEM` entirely, so every write to it lands on the interpreter
      fallback — and that is the only place the check runs. It is skipped
      altogether unless some cached block actually resolved a `dyn`, so a
      guest with no FP never flushes. Specialising also deletes the
      ten-instruction table lookup from the front of every dynamically
      rounded FP operation.

      Caught by six new self-test checks that execute one `fcvt.w.s ..., dyn`
      at one address under five modes in turn: two fail without the fix
      (`2.5` under `RMM` gives 2, not 3), and the rest fail if a block is not
      rebuilt when `frm` changes. The tie value matters — `3.5` rounds to 4
      under both modes and would have passed throughout.
- [x] **APLIC** — *implemented*, direct delivery mode, one domain, one hart,
      at `0x0C00_0000`. Written against the AIA specification 20250312 in
      `docs/riscv/`. `domaincfg`, `sourcecfg`, the pending and enable
      bitmaps with their `*num` forms, `target` priorities, and an IDC with
      `idelivery`/`iforce`/`ithreshold`/`topi`/`claimi`. 24 self-test checks
      drive it through `setipnum`, so they run on the host as well as the
      board. MSI delivery is not implemented and will not be: it targets an
      IMSIC, which needs S-mode CSRs this core does not have.

      **The NVIC bridge is the part that matters** and is what makes an
      interrupt from real silicon reachable by a guest driver. An interrupt
      is the one thing the passthrough window cannot carry: the NVIC vectors
      into the emulator, with the guest nowhere in sight. The handshake is
      forced by the fact that nothing on the host side can service the
      device — only the guest's driver can — so the line is masked on entry
      and unmasked only when the guest clears the APLIC pending bit. One
      table entry and one handler adds a peripheral, the way `g_periph_map`
      works for addresses. TIM6 is wired as the first line; **no guest test
      drives a real interrupt end to end yet**, so that path is built but
      unproven.
- [ ] **ACLINT** — the CLINT is the legacy SiFive layout, which is
      functionally ACLINT's MTIMER and MSWI at the older offsets. Exposing
      the ACLINT offsets alongside is small and not yet done.
- [ ] **U-mode** — invasive rather than large. A second privilege level makes
      `medeleg`/`mideleg`/`mcounteren` real, adds `ECALL` from U as a distinct
      cause, and invalidates the "M-mode only" simplifications throughout
      `rv_csr.c` and `rv_pmp.c`.
- [ ] **S-mode** — the same as U-mode, but with a third privilege level and a second
      set of CSRs. The Cortex-M4 has no MMU, so S-mode would be entirely
      soft-trap and would need a second set of `mstatus`/`mepc`/`mcause`/`mtval`
      to hold the S-mode state.
- [ ] **V** — the largest remaining item and RAM-hungry: `VLEN=128` alone costs
      512 B of register file on a part with 128 KiB. Needs a `VLEN` budget
      decision before any code.
- [ ] **D**, and therefore **Zcd** — *not planned*. The Cortex-M4F and M7 FPUs are
  single precision, so D would be entirely soft-float on the intended targets,
  and Zcd is the compressed double load/stores it would need.

**Measured and rejected**, kept here so they are not retried blind:

| Idea | Result |
|---|---|
| Interpreter loop in SRAM | **slower** — 162 vs 122 cycles, and 8 KiB off the guest |
| Guest registers cached in `r8`–`r10` | **15.5% slower** — a cached read is `MOV` where an uncached one is `LDR`, one instruction either way |
| PMP mapped onto the ARM MPU | **not possible** — the MPU cannot distinguish a guest access from an emulator access, because the JIT's inlined load *is* both |

---

## Licence

Apache-2.0. Vendor code fetched at build time keeps its own licences
(ST: BSD-3-Clause; ARM CMSIS: Apache-2.0).
