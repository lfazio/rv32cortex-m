# rv32cortex-m

A retargetable 32-bit ISA emulator whose host is an **ARM Cortex-M
microcontroller**, with the emulated guest driving the host's **real
peripherals** through an identity-mapped passthrough window.

Two guest architectures (RISC-V RV32 and Renesas RH850 G4MH), two JIT
backends (Thumb-2 and x86-64), and two platforms (a native host runner
and STM32F4/F7 firmware). Validated against the official
[RISC-V Architecture Test Suite](https://github.com/riscv/riscv-arch-test)
at **378/378** and the Berkeley `riscv-tests` at **77/77**, on hardware
as well as on a host.

---

## The idea

The emulator turns an ARM Cortex-M part into a RISC-V machine. The
interesting part is not the interpreter — it is the memory map:

```
guest 0x4000_0000..0x5FFF_FFFF   ─── identity map ───▶   STM32 APB1/APB2/AHB1/AHB2
```

That range is where a RISC-V platform leaves room for memory-mapped I/O,
and also exactly where the STM32 puts its peripherals. Mapping it
one-to-one means **a guest driver uses the addresses printed in the
vendor reference manual**, with no translation to reason about.

The consequence is the design's main idea:

> **Peripheral drivers live in the guest, not in the emulator.**

The emulator contains no GPIO driver, no UART driver, no SPI driver. It
forwards loads and stores. Porting to a new microcontroller means writing
a clock setup, a linker script and a region table — not a new driver
stack. [`tests/guest/stm32drv.c`](tests/guest/stm32drv.c) is a working
demonstration: GPIO and USART drivers written entirely as RISC-V guest
code, driving real silicon.

Three axes, independent of each other:

| axis | what it decides | selected by |
|---|---|---|
| platform | where it runs | `EMU_PLATFORM=host\|stm32f446\|stm32f746` |
| frontend | what it emulates | `EMU_FRONTEND_RV32`, `EMU_FRONTEND_G4MH` |
| backend | how it executes | `EMU_JIT=ON\|OFF`, `--jit` on the host runner |

---

## Building

### Host — development and both test suites

```sh
cmake -B build/host -DEMU_PLATFORM=host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host
ctest --test-dir build/host -L fast
```

Add `-DEMU_FRONTEND_G4MH=ON` to compile both frontends, so the runner can
pick one with `--frontend`.

### Firmware — Nucleo-F746ZG

```sh
cmake -B build/f746 -DEMU_PLATFORM=stm32f746 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DRV32_GUEST=isatest
cmake --build build/f746 --target flash        # probe-rs over ST-LINK
```

The older Nucleo-F446RE is still supported: `-DEMU_PLATFORM=stm32f446`,
`--chip STM32F446RETx` on `/dev/ttyACM0`. `EMU_PLATFORM` picks the CPU,
the FPU and the vendor pack.

Console: the ST-LINK virtual COM port at **921600 8N1** — USART3 on the
Nucleo-F746ZG (a Nucleo-144 wires the VCP to PD8/PD9), USART2 on the
F446RE.

```sh
picocom -b 921600 /dev/ttyACM1
```

On the F746 the network is **on by default**, so that port carries IP
rather than text once the banner is out: the console becomes telnet, gdb
listens on 1234 and guest images go up by TFTP, all over SLIP on the same
wire. See [docs/network.md](docs/network.md); build `-DEMU_NET=OFF` for a
plain serial console.

```sh
sudo ./scripts/slip-up.sh        # after the board says "net SLIP on this port"
telnet 192.168.7.2
```

The `flash` targets pass `--connect-under-reset` always, because this
firmware never idles and a plain attach races it. **A failed flash leaves
the previous firmware running**, which reports a plausible result for a
change that was never loaded — read the flash log before believing a
board result.

### Options

`-DRV32_GUEST=` selects the embedded guest image: `isatest`, `hello`,
`bench`, `stm32drv`, `irqtest`, `cmsistest`, `coremark`, `dhrystone`,
`whetstone`, `fptest` or `mmiobench`.

| Option | Default | Effect |
|---|---|---|
| `EMU_JIT` | `ON` | The JIT. `OFF` is smaller, and is how a suspected JIT bug is isolated. |
| `EMU_JIT_CODE_BYTES` | `12288` | Code cache. **The dominant performance term** — see [docs/jit/tuning.md](docs/jit/tuning.md). A small value forces compaction and is a useful stress test. |
| `EMU_JIT_LOOP_CAP` | `128` | Guest instructions per block entry: an interrupt-latency knob, not a throughput one. |
| `EMU_NET` | `ON` (F746) | lwIP over SLIP on the console UART: telnet, gdb and TFTP. **The UART stops being a console** -- `OFF` gets it back. |
| `EMU_ENABLE_TRACE` | `OFF` | Per-instruction trace hook. Slow, and the fastest way to find where execution diverges. |
| `RV32_EXT_PMP` / `RV32_EXT_SDTRIG` | `ON` | Each costs a little even unused; `OFF` removes it. |
| `RV32_NATIVE_COREMARK` | `OFF` | Run CoreMark natively on the ARM instead of the emulator, for the baseline. |
| `RV_GUEST_MARCH` | see below | Guest ISA. A **cache variable**, so pass it explicitly when changing it. |

Guest images are built
`rv32imafc_zicsr_zifencei_zicbom_zicboz_zba_zbb_zbc_zbs_zacas`. `Zcb` is
supported by the emulator but deliberately absent there — compressed
guest code is *slower* to interpret, not faster.

**`EMU_JIT_CODE_BYTES`, `RV_GUEST_MARCH` and `COREMARK_ITERATIONS` are
cache variables and silently outlive the build directory they were set
in.** Check `CMakeCache.txt` before quoting any measurement;
`scripts/report-figures.sh` prints them beside the numbers.

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

---

## Running

```sh
./build/host/emu-host --load 0x80000000 build/host/guest/isatest.bin
./build/host/emu-host --jit --quiet --load 0x80000000 build/host/guest/isatest.bin
./build/host/emu-host --frontend g4mh --load 0x80000000 guest.bin
```

`emu-host` picks a frontend from `--frontend`, else from the image's ELF
`e_machine`, else the first compiled in. A flat binary says nothing about
its architecture, so it gets the default. `--jit` selects the translating
backend for either frontend; without it, the interpreter.

Useful flags: `--dump` (register file on exit), `--max-insn N`,
`--trace-skip`/`--trace-count` (with `-DEMU_ENABLE_TRACE=ON`), `--gdb`
(RSP stub on :1234, waits for a client).

### Validation

```sh
./scripts/run-arch-test.sh      # official riscv-arch-test, 378/378
./scripts/run-riscv-tests.sh    # Berkeley suite, 77/77
./scripts/report-figures.sh     # every quoted figure, regenerated
./scripts/check-doc-flags.sh    # every build flag named in the docs exists
```

**Run both suites.** They cover different things, and a regression that
only the Berkeley suite catches will sit unnoticed if only arch-test is
run — which is exactly what happened to `rv32mi/csr` when F was added.

### Debugging

```sh
cmake --build build/f746 --target gdbserver   # OpenOCD on :3333
gdb-multiarch build/f746/src/platform/stm32f746/emu-stm32f746.elf \
  -ex 'target extended-remote :3333'
```

That debugs the *emulator*. To debug the **guest**, use the built-in RSP
stub: `emu-host --gdb`, then `target remote :1234`. Two connections,
two different programs -- [docs/gdb.md](docs/gdb.md) has the rest,
including the three RSP mistakes that fail by hanging rather than
erroring.

The host `gdb` on Debian is x86-only; use `gdb-multiarch`, or
`probe-rs gdb`.

- `emu-host --dump` prints the full guest register file on exit.
- A `HardFault` on the ARM side usually means the passthrough window let
  a guest access reach an address the ARM bus rejects — the region table
  is where to look. An unimplemented address in that window makes the AHB
  signal an error, which kills the *emulator*, not the guest.

---

## Repository layout

```
include/emu/      the frontend contract, and the ISA-agnostic runtime's API
include/rv32/     RISC-V frontend headers
include/g4mh/     RH850 G4MH frontend headers
src/emu/          bus, passthrough, NS16550 console, ELF loader, registry
src/frontend/
  rv32/           hart, decode, CSRs, traps, interpreter, Thumb-2 JIT,
                  CLINT, APLIC
  g4mh/           core, decode, interpreter, INTC
src/platform/
  host/           native runner (frontend-neutral)
  stm32f446/      Nucleo-F446RE firmware, ST HAL integration, linker script
tests/
  unit/           host unit tests: RVC expansion, bus permissions,
                  G4MH decode and the frontend contract
  guest/          RISC-V programs that run inside the emulator
  arch-test/      DUT description for the official suite
scripts/          validation runners
docs/<vendor>/    reference documentation
```

Guest images (`tests/guest/`):

| Image | Purpose |
|---|---|
| `isatest` | RV32 self-test, including traps and CBO |
| `hello`   | smallest useful guest; confirms the console path |
| `bench`   | compute-bound workload for throughput measurement |
| `stm32drv`| GPIO and USART2 drivers written as guest code |
| `irqtest` | a real peripheral interrupt taken by guest code |
| `cmsistest`| the same interrupt through the CMSIS-Core shim |
| `fptest`  | floating point, and deliberately no PMP |
| `mmiobench`| driver-shaped workload for the passthrough window |
| `coremark`| CoreMark, fetched from upstream and built for RV32 |
| `dhrystone`| netlib's Dhrystone 2.1. On the host its clock is derived from the instruction count, so it compares *frontends* and not backends — see [docs/performance.md](docs/performance.md) |
| `whetstone`| Whetstone 1.2, single precision — the floating-point counterpart. Same clock caveat, and its rate depends on `WHET_LOOPS` |

---

---

## Documentation

| | |
|---|---|
| [docs/Architecture.md](docs/Architecture.md) | the three axes and the frontend contract |
| [docs/frontend/rv32.md](docs/frontend/rv32.md) | RV32 scope, memory map, floating point |
| [docs/frontend/g4mh.md](docs/frontend/g4mh.md) | G4MH scope, and what is *not* verified |
| [docs/frontend/ppc.md](docs/frontend/ppc.md) | e200z7 scope, its guest, and what the first running program found |
| [docs/backend/thumb2.md](docs/backend/thumb2.md) | the ARMv7E-M emitter |
| [docs/backend/x86_64.md](docs/backend/x86_64.md) | the x86-64 emitter, which exists for coverage |
| [docs/jit/README.md](docs/jit/README.md) | the IR pipeline, block model, FP policy |
| [docs/jit/staleness.md](docs/jit/staleness.md) | what a translated block bakes in |
| [docs/jit/tuning.md](docs/jit/tuning.md) | every knob, what it is worth, what can see it |
| [docs/jit/floating-point.md](docs/jit/floating-point.md) | host FPU or SoftFloat, and the NaN and flag rules that decide |
| [docs/validation.md](docs/validation.md) | the suites, and the bugs they caught |
| [docs/performance.md](docs/performance.md) | the measured figures |
| [docs/memory.md](docs/memory.md) | ROM, RAM and flash: who backs what, and why the platform does |
| [docs/gdb.md](docs/gdb.md) | the guest stub and the emulator stub -- two connections, different programs |
| [docs/network.md](docs/network.md) | the board over IP: telnet, gdb and TFTP, and what the handover costs |
| [docs/porting.md](docs/porting.md) | porting to another target |
| [docs/TODO.md](docs/TODO.md) | open work |
| [docs/host/](docs/host/README.md), [docs/stm32f446/](docs/stm32f446/README.md) | per-platform and per-platform/frontend notes |

Working notes — what has bitten, and will again — are in
[CLAUDE.md](CLAUDE.md).

---

## Licence

Apache-2.0. Vendor code fetched at build time keeps its own licences
(ST: BSD-3-Clause; ARM CMSIS: Apache-2.0).
