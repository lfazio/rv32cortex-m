# The Nucleo-N657X0-Q (STM32N6)

The third platform, and the first that is neither ARMv7E-M nor
flash-based: a Cortex-M55 with no internal user flash, booted by its
ROM as an FSBL in AXISRAM2. `EMU_PLATFORM=stm32n6`; the firmware rows
`n6-rv32` and `n6-g4` are in the build matrix, FreeRTOS runs on it
([os.md](../os.md#on-a-board)), and `scripts/n6-load.sh` starts the core
through ST-LINK_gdbserver on access port 1, which the vendor's
programmer CLI does not reach. The JIT's code buffer and dispatch path
live in ITCM ([itcm.md](itcm.md)).

**Measured like the F746**: `BOARD_PLATFORM=stm32n6
EMU_PROBE=0483:3754:<serial> scripts/board-measure.sh ...` and
`board-ab.sh` load through the build's `flash` target, which starts the
firmware, so `board-run.py` drains the ST-LINK V3's console before and
reads it while the load runs. There is no `EMU_LAYOUT_PAD` here
(`BOARD_AB_PADS=0`), and a mute console wants `usbreset 0483:3754` as
the F746's does.

The JIT work of 2026-11-14 -- the hot threshold, the computed jumps'
stub, four chain slots -- and ARMv7-M's two loop tests, against
7495445, host cycles an instruction, CRCs the same:

| | before | after | |
|---|---|---|---|
| RV32 CoreMark, JIT | 17.80 | 14.71 | -17.4% |
| ARMv7-M CoreMark, JIT | 23.72 | 19.99 | -15.7% |
| PowerPC CoreMark, JIT | 29.49 | 24.74 | -16.1% |
| G4MH CoreMark, JIT | 29.85 | 25.71 | -13.9% |
| ARMv7-M, interpreted | 177.07 | 169.89 | -4.1% |
| RV32, PowerPC, G4MH, interpreted | 116.32, 172.87, 105.60 | 116.24, 173.43, 104.83 | within 0.7% |

G4MH's retired count differs by 405: its CoreMark times itself by the
LTSC, which follows the host's clock, and the faster run prints a
different line -- the same CRC.

What follows is what the port rests on, established from RM0486, PM0273
and UM3417 in `docs/st/stm32n6/` and from ST's own Nucleo projects
before a line of it was written, and then the plan it was written to --
kept because every part of it was carried out: the shared runner
(`src/platform/common/emu_main.c`), `rv_*` platform names renamed
`emu_*`, and `emu_console_printf` in place of the hand-rolled helpers.

## What the part is

- **Cortex-M55**, Armv8.1-M with Helium. `EMU_HOST_JIT_THUMB2` tests
  `__ARM_ARCH >= 7 && __thumb2__`, which an M55 satisfies, so the
  existing emitter is *selected* and every encoding it emits stays
  valid. The port is expected to run before it is tuned, and the
  risk is that this hides the remaining work rather than failing
  loudly.
- **No internal user flash.** The MCU is an STM32N657X0H3Q and the
  board carries a 512-Mbit external Octo-SPI flash; it boots from
  there or from the boot ROM. Everything this project does with
  internal flash has no equivalent: the guest image embedded in the
  firmware image and served as a ROM region, and the TFTP upload
  arena in sectors 5-7.
- **~4.2 MB of AXISRAM**, which is what makes that easy rather than
  hard: put the guest image, guest RAM and the JIT buffer all in
  RAM. The F746's reason for the `.ro`/`.rw` placement split -- 140
  KiB of 345 saved by serving read-only guest bytes from flash --
  does not apply, so the N6 can map the whole image writable and
  skip it. The split is only an optimisation now that the guest
  initialises its own `.data`.
- Caches again: the M55 has L1 I- and D-cache like the M7, so
  `board_sync_icache` needs the same override. That hook is weak,
  so forgetting it is silent.
- `src/emu/` claims to build for ARMv6-M through ARMv8.1-M. This is
  the first thing that would *check* that claim.
- Helium (MVE) is **not** the port: it is a second, wider emitter.
  Run `-DEMU_PAIR_STATS=ON` first and let the histogram say whether
  any guest can use it, the way the pair stats answered the fusion
  question and the FP histogram answered the lowering one.

## The memory map, from STM32CubeN6

**From STM32CubeN6 v1.4.0** (`github.com/STMicroelectronics/STM32CubeN6`,
689 MB shallow; the modular repos are `cmsis-device-n6` and
`stm32n6xx-hal-driver` -- note **hyphens**, where F4/F7 use
underscores, so `_cube_declare` needs adjusting). ST's own Nucleo
linker scripts settle the memory map:

| image | region | address |
|---|---|---|
| FSBL (boot-ROM loaded) | AXISRAM2 | `0x34180400`, 511K |
| XIP application | ROM (external OSPI) | `0x70100400`, 511K |
| XIP application | RAM (AXISRAM) | `0x34000000`, 2048K |

The `+0x400` on every ORIGIN is the header the boot ROM requires --
not slack, and getting it wrong means the ROM refuses the image
rather than the image misbehaving.

So there are two shapes to choose between, and the choice is the
first design decision of the port, not a detail: an **FSBL in
AXISRAM** (boot ROM loads it from OSPI at reset -- simplest, 511K,
no XIP driver) or an **XIP application** in memory-mapped external
flash with 2 MB of RAM beside it. The FSBL shape is the closer
analogue of what the F746 does and is where to start; the guest
image, guest RAM and JIT buffer all live in AXISRAM either way,
which is what makes the `.ro`/`.rw` placement split unnecessary
here.

Reference projects to read before writing anything:
`Projects/NUCLEO-N657X0-Q/Templates/Template_FSBL_XIP/` for the
boot flow and `Examples/UART/` for the console.

The FSBL's own `main()` is short and its order is the thing to
copy: `SCB_EnableICache()` then `SCB_EnableDCache()` **first**,
before `HAL_Init()` and `SystemClock_Config()` -- the same
caches-before-anything-is-written rule the F746 platform already
follows, and the reason `board_sync_icache` must be overridden
here too. Note it ships a *separate* `system_stm32n6xx_fsbl.c`
rather than reusing the application one, so the CMake cannot just
point at the CMSIS device's system file the way the F4/F7
platforms do.

## Why it keeps lwIP

**CubeN6 ships ThreadX and NetX Duo, not FreeRTOS and lwIP** --
its `Middlewares/ST` holds threadx, netxduo, filex, levelx, usbx
and no lwIP at all. That does *not* constrain this port, and the
reason is worth stating so nobody ports the network stack to NetX
Duo for no reason:

- The stack needs no RTOS. It runs `NO_SYS = 1` -- lwIP is a
  library the run loop calls, and `lwipopts.h` has a compile-time
  `#error` if that is ever overridden, because this port supplies no
  `sys_arch`. (FreeRTOS came later, on every platform, as the task
  per emulated core; the stack is still `NO_SYS`.)
- lwIP does not come from the family pack. `cmake/lwip.cmake`
  fetches `STMicroelectronics/stm32_mw_lwip`, which is
  family-independent -- the same repository already serves both
  the F4 and the F7 platforms and never mentions
  `STM32CUBE_FAMILY`.

So the N6 keeps lwIP, PPP, TFTP and telnet unchanged. NetX Duo
would only be worth reaching for if ST's stack were wanted on its
own merits, which is a different question from this port.

## WFI keeps its clocks here

**The N6 sets no DBGMCU_CR.DBG_SLEEP, unlike the F746, because it was
measured not to matter.** On the F746 a WFI without it stops the cycle
counter the time base reads and shuts the probe out. On this Cortex-M55
neither happens: with `-DEMU_WFI_CHECK=ON` the firmware times a quarter
of a second of WFI by SysTick's millisecond from inside, and read
150,000,030 cycles with the bit set and 150,000,001 with it never
written, 250 wake-ups each; probe-rs then read DHCSR with S_SLEEP set
from the parked core. The register read back 0 after the firmware had
enabled its debug clock (`__HAL_RCC_DBG_CLK_ENABLE`) and set the bit,
so a line setting it would have said what it does not do.
`scripts/check-wfi-attach.sh TREE stm32n6` asks the same of a tree.
(probe-rs read CYCCNT twice, seconds apart, as one value: the probe's
view of the counter is not the firmware's, and was not used.)

## The RNG behind Zkr

RISC-V's `seed` reads the part's RNG through the HAL
(`src/platform/stm32/board.c`, shared with the F746), clock-error
detection on; `isatest`'s `test_zkr` passes here -- ES16 within its
poll bound, values that vary. The driver costs ROM, and the RV32
firmware has about 1.1 KB of its 216 KB to spare now
(`stm32n657xx_lrun.ld`); the RAM half has about 95 KB, so the next
growth moves the line again.

## The plan, as written before the port

What the platform is, concretely -- the F746's nine files, and
which of them are the work:

| file | N6 effort |
|---|---|
| `CMakeLists.txt` (309 lines) | mostly transcribable; set `STM32CUBE_FAMILY n6`, and see the note about it having to precede the `include()` |
| `<part>.ld` (284 lines) | **new**: AXISRAM at `0x34180400`, the `+0x400` boot header, no flash regions, no arena |
| `board.c` (21 KB) | **the real work**: clocks, UART, LED, DWT, `board_sync_icache` |
| `board.h` | interface is already fixed by the other platforms |
| `main.c` (57 KB) | should be *shared*, not copied -- it is the same runner |
| `guest_image.S` | image lives in RAM here; likely simpler |
| `stm32n6xx_hal_conf.h` | from the family's own template, **never a renamed F7 one** -- the F4/F7 accelerator-name divergence is already recorded |
| `stm32n6xx_it.c`, `coremark_native.c` | small |

**The interface is defined; the move is what remains.**
`src/platform/common/emu_board.h` states what only a board can
answer -- its core name for the banner, the guest image and RAM
extents, its passthrough regions, its bridged interrupt lines, and
its clock. Everything else in a runner is the same sequence on
every part.

Already shared and verified on hardware: the console and state dump
(`emu_console.c`), guest cache maintenance (`emu_arm_cache.c`), the
syscall services (`emu_syscall.c`), the run summary and framework
JIT statistics (`emu_stats.c`), and the network wiring
(`cmake/emu_net.cmake`). F446 724 -> 617 lines, F746 1546 -> 1438.

What is left is the sequence itself: `build_address_space`, the
IRQ bridging, the banner and the run loop. They differ between the
two boards *only* in the four things emu_board.h names -- checked,
not assumed: `build_address_space` diffs to the image variables and
the peripheral table and nothing else.

**Share `main.c` before writing a third one.** Measured rather
than assumed: F446 724 lines, F746 1546, host 897. The F746's
extra thousand is almost entirely the network and upload
machinery -- `emu_net_image_begin/data/end`, `gdb_flash_*`,
`start_guest`, `take_uploaded_image`, `console_getc` -- which is
already behind `#if EMU_NET`. The F446 is close to a *subset*, not
a variant.

So the split is: one shared runner (banner, console, stats, guest
state dump, run loop, the JIT diff report) plus a per-platform
part that is genuinely about the part. The N6 needs neither the
flash arena nor `gdb_flash_*`, since it has no internal flash, so
it would take the shared half and almost nothing else.

Do this *before* the N6, not after: the same duplication has
already cost this project three separate fixes to `g4mh_ir.c`'s
copy of the interrupt check, and a third `main.c` would be the
same shape with a thousand lines in it.

Two things to do *while* moving the code, because both are far
cheaper during an extraction than as a later sweep:

**Rename `rv_*` to `emu_*` where the name is not about RISC-V.**
The platform code mixes two kinds, and only one should move:

| name | uses | verdict |
|---|---|---|
| `rv_guest_image`, `rv_guest_image_size`, `rv_guest_ro_size` | 24 | **rename** -- these describe *a guest image*, and a G4MH or PowerPC guest uses the identical symbols today |
| `rv_console_putc` | 4 | **rename** -- a console has no ISA |
| `rv_backend`, `rv_backend_jit`, `rv_backend_interp` | 12 | keep: genuinely the RV32 frontend's |
| `rv_jit_stats_t`, `rv_jit_get_stats`, `rv_pair_report` | 6 | keep: RV32 statistics |

The first two groups are what make a shared runner still say
"RISC-V" while serving three frontends -- exactly the mismatch
`EMU_GUEST_ARCH_*` was renamed to remove.

**Use picolibc's `printf` instead of the hand-rolled console
helpers.** `console_puts`/`console_putu`/`console_puthex` appear
**130 times** in the F746 runner alone, and every stats line is
built by hand from them -- which is why adding one field means
three calls and why the guest-state dump is the length it is. The
toolchain file deliberately avoids `nano.specs`; picolibc is the
small-footprint replacement that gives real formatting. Do it in
the shared runner only, and keep the guests `-nostdlib` -- they
have no libc and must not gain one.

Add a `f746`-style row to `scripts/build-matrix.sh` with the port,
not after it.
