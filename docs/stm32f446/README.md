# Platform: STM32F446 (Nucleo-F446RE)

Cortex-M4F at 180 MHz, 512 KiB flash, 128 KiB SRAM. The reason the project
exists: **the guest drives the host's real peripherals**, so peripheral
drivers live in the guest and not in the emulator. Adding a GPIO or UART
driver to `src/platform/` is almost always the wrong fix.

Bring-up is entirely ST's driver pack — CMSIS device headers, ST's startup
and `system_stm32f4xx.c`, and the STM32Cube HAL for the clock tree, GPIO
and USART. Nothing here reimplements a peripheral the vendor already
supports.

Reference: [`docs/st/rm0390`](../st/), [`docs/arm/DDI0403E`](../arm/).

## Guest memory map

Common to every frontend ([`include/emu/emu_memmap.h`](../../include/emu/emu_memmap.h)):

| guest address | kind | what |
|---|---|---|
| `0x1000_0000` | MMIO | NS16550 console, bridged to USART2 |
| `0x2000_0000` | ROM | the guest image, execute-in-place from ARM flash |
| `0x4000_0000` | passthru | 1:1 onto the ARM peripheral space, 512 MiB |
| `0x8000_0000` | RAM | guest RAM carved out of ARM SRAM |

Each frontend adds the interrupt controller and timer its architecture
defines; see the per-frontend pages.

Guest RAM is not a fixed array. The linker script places it between the
firmware's `.bss` and the stack and gives it everything in between, so the
guest automatically receives all the SRAM the ARM side is not using.

## The passthrough window

Guest `0x4000_0000..0x5FFF_FFFF` maps one-to-one onto the STM32's own
peripheral space, and that is not a coincidence — it is exactly the range a
RISC-V platform leaves free for memory-mapped I/O. A guest driver therefore
uses the addresses printed in RM0390 with no translation to reason about.

Policy table in `src/platform/stm32f446/main.c`. Only what would take the
emulator down with the guest is withheld:

| region | permission | why |
|---|---|---|
| APB1 to PWR | RW | timers, RTC, SPI2/3, USART2/3, UART4/5, I2C |
| PWR | **R** | dropping over-drive at 180 MHz stalls the core |
| rest of APB1, all APB2, GPIO, CRC | RW | |
| RCC CR/PLLCFGR/CFGR/CIR | **R** | reconfiguring the PLL kills the clock the emulator runs on |
| RCC resets and clock enables | RW | a driver must be able to ungate its own peripheral |
| flash interface | **R** | the flash controller can erase this firmware underneath us |
| BKPSRAM, DMA1/2, USB OTG HS, AHB2 | RW | |

Note what is deliberately *not* withheld: the rest of RCC. Denying clock
gating would push every driver back into the firmware.

**Reserved peripheral addresses are not a harmless near miss.** An
unimplemented address in the window makes the AHB signal an error, which is
a HardFault in the *emulator*, not a fault delivered to the guest — the
firmware dies rather than the test failing. Every hole in the table begins
just above reserved space, so there is no register immediately below one to
probe with. Test the window with registers that exist.

## Interrupts

An interrupt is the one thing the passthrough window cannot carry. A guest
driver reaches a peripheral by address, but when that peripheral raises an
interrupt the NVIC vectors into the *emulator*, with the guest nowhere in
sight.

The handshake is forced by one fact: nothing on the ARM side can service
the device — only the guest's driver knows how, and it will not run until
the emulator returns from the ISR. A level-triggered peripheral therefore
re-asserts the moment the handler exits. So the line is **masked on entry**
and stays masked until the guest says it has dealt with the device, which
it does by unmasking the source in its own controller; that reaches the
platform through `emu_cpu_ops_t::set_unmask_hook`.

A source number *is* the NVIC line number. A driver that would call
`HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn)` writes that same 54 to its own
controller, and the two numbering spaces never have to be reconciled.

Adding a peripheral is one table entry (`g_bridged`) and one handler.

## Cache maintenance

The M4 in the F446 has no data cache — only the ART flash accelerator,
which is transparent — so every maintenance operation is a no-op, which
both architectures permit. The code is written against `__DCACHE_PRESENT`
so the same file does the right thing on a Cortex-M7, where these become
real operations on the lines backing the guest block.

## Measured, and settled

- **`RV32_JIT_CODE_BYTES` dominates JIT performance, and the 12 KB default
  is worse than no JIT at all.** CoreMark's translated working set is
  ~48 KB. 12 KB: 10,850,998 ticks (8533 compactions, 94240 evictions);
  24 KB: 9,329,706; 32 KB: 8,525,192; 48 KB: 6,463,217 (904); 64 KB:
  5,148,168 (231). The interpreter is 10,691,637 — so at the default the
  JIT *loses*. Guest RAM pays one for one: 122 KiB with no JIT, 106 at
  12 KB, 70 at 48 KB, 54 at 64 KB.
- **`-Os` is 33% smaller and 8.8% slower.** The ART accelerator is not the
  binding constraint, so the code-density argument does not pay. Use
  `MinSizeRel` only when flash is actually scarce.
- **Layout noise is ±3%.** Ignore differences below that.

## To do

- **A second STM32.** Moving to another part should be the clock setup and
  the policy table, not new drivers. Never tried.
- **Cortex-M7 validation.** The cache maintenance path is written for it
  and has never run on one.
- **DMA from a guest driver.** Zicbom translates guest addresses to the
  host addresses that actually back them, so a guest cleaning a DMA buffer
  cleans the right ARM cache lines — on a part that has a cache. Untested.

## Investigate

- **Whether the passthrough policy should be data rather than code.** It is
  a table already; making it a linker section would let a board be added
  without touching `main.c`.
- **Interrupt latency under the JIT.** `RV_JIT_LOOP_CAP` bounds it to
  ~22 µs at the default 128, derived rather than measured end to end.

## Discarded

- **Interpreter in SRAM** (`RV32_INTERP_IN_RAM`). Measured *slower*: 162 vs
  122 host cycles per guest instruction, while also taking 8 KiB from the
  guest. Executing from flash lets the M4 fetch over the I-bus while data
  goes to SRAM over the D-bus, and the ART keeps a hot loop effectively
  wait-state free; moving code into SRAM puts fetch and data on the same
  interface and serialises them. Kept as an option because that trade-off
  is part-specific.
- **Peripheral drivers in the firmware.** The whole design is the opposite.
