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
| RV32IMAC + Zicsr, Zicntr, Zifencei, Zicbom, Zicboz | implemented |
| Machine-mode traps, interrupts, CLINT timer | implemented |
| Official `riscv-arch-test` (RVCP) | **101 / 101 pass** |
| `riscv-tests` (Berkeley) | **75 / 77 pass** (2 need PMP / Sdtrig, not implemented) |
| Guest ISA self-test (104 checks) | passes on host **and** on hardware |
| Nucleo-F446RE firmware | runs; 19–29 KB flash, guest gets 107–123 KiB of the 128 KiB SRAM |
| Thumb-2 JIT backend | implemented; **2.07× over the interpreter** on hardware |
| F / D / B / V extensions | not implemented |

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
| `0x8000_0000` | ~123 KiB | RAM | Guest RAM, carved from ARM SRAM |

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

`-DRV32_GUEST=` selects which guest image is embedded: `isatest`, `hello`,
`bench`, or `stm32drv`.

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

### Official RISC-V Architecture Test Suite — 101/101

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

### riscv-tests — 75/77

The older Berkeley suite. Two failures remain, both for features this core
deliberately does not implement, where accessing the missing CSRs correctly
raises illegal-instruction:

- `rv32mi/breakpoint` — needs the Sdtrig debug trigger module
- `rv32mi/pmpaddr` — needs PMP

### Bugs these suites caught

Worth recording, because each was a genuine defect:

| Found by | Defect |
|---|---|
| unit test vs. assembler ground truth | `C.ADDI4SPN` took its destination register from bits `[9:7]` instead of `[4:2]`, corrupting every guest stack-frame address computation |
| `riscv-tests` `instret_overflow` | a CSR write to `minstret` must *replace* that instruction's increment, not be followed by it |
| `riscv-arch-test` `Zicntr` | the Sail config declared a clock tick every 100 instructions while the emulator ticks every instruction |

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

**1,275,036 guest instructions**, timed with the DWT cycle counter.

| Configuration | Host cycles / guest instruction | Throughput |
|---|---|---|
| interpreter, inline bus fast paths | 166.9 | 1.08 MIPS |
| \+ LTO | 121.9 | 1.48 MIPS |
| interpreter in SRAM | 162.3 | 1.11 MIPS |
| **Thumb-2 JIT** | **58.8** | **3.06 MIPS** |

The JIT is **2.07× faster** than the tuned interpreter: the emulated core runs
at roughly **3 MHz-equivalent** on a 180 MHz host, against ~1.5 MHz interpreted.
It translated 96.9% of executed instructions — 87 blocks in 10.7 KB of a 12 KB
code cache, with 40,010 of 1,275,036 instructions falling back to the
interpreter. Those fallbacks are exactly the `div`/`rem` pairs in the muldiv
kernel, which names the next thing worth teaching the translator.

The JIT costs 12 KB of RAM for the code cache, taking the guest from 123 KiB to
107 KiB. Correctness is validated by the 104-check guest ISA self-test running
on hardware with the JIT active, which covers ALU, shifts, every load/store
width with sign extension, branches, jumps, M, A, CSRs, traps and CBO.

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
to ARM registers across a block (the JIT currently loads and stores every
operand), teaching the translator `DIV`/`REM`/`MULH`, and chaining blocks so a
hot loop stops returning to the dispatcher.

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

---

## Documentation

Reference material is classified by vendor under `docs/`:

| Path | Document |
|---|---|
| `docs/riscv/riscv-spec-unprivileged-2026-07-29.pdf` | RISC-V Unprivileged ISA (release `310a111`) |
| `docs/st/rm0390-...pdf` | RM0390 — STM32F446xx Reference Manual |
| `docs/st/stm32f446mc.pdf` | STM32F446 datasheet |

Not yet collected: the RISC-V **Privileged** ISA specification, and ARM's
ARMv7-M Architecture Reference Manual / Cortex-M7 TRM (ARM's site requires an
account for most PDFs).

---

## Roadmap

Deferred until the current ISA is proven and measured — both of which are now
done, so these are unblocked:

- [ ] **Zacas** — the atomic compare-and-swap instruction.
- **B** (`Zba`/`Zbb`/`Zbc`/`Zbs`) — cheapest of the four; maps well onto Thumb-2
  `CLZ`, `RBIT`, `REV`, `UBFX`.
- [ ] **F / D** — Cortex-M4F and M7 have a single-precision FPU usable for `F`;
  `D` needs soft-float. Needs `f0`–`f31`, `fcsr`, NaN-boxing and correct
  rounding.
- [ ] **V** — largest by far and RAM-hungry (`VLEN=128` alone costs 512 B of
  register file on a part with 128 KiB); needs a `VLEN` budget decision.

---

## Licence

Apache-2.0. Vendor code fetched at build time keeps its own licences
(ST: BSD-3-Clause; ARM CMSIS: Apache-2.0).
