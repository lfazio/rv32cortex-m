# Open work

Moved out of README.md. Status lines here are claims about the code, so
they go stale: `scripts/report-figures.sh` regenerates the ones a host can
check, and anything measured on hardware carries the commit it was
measured at.

## Roadmap

- [ ] **Linux** - Prepare a Linux guest rv32g with mmu and run it on the emulator (x86_64 only). This is a big task, but it would be a good demonstration of the emulator's capabilities. Implement minimal virtio devices to get a shell and run some benchmarks. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] **JIT** - Autovectorisation of the IR pipeline. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] Run doom in Linux so it can be used as a benchmark for the emulator. This is a big task, but it would be a good demonstration of the emulator's capabilities implement a sdl backend for the emulator to run doom in Linux. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] Add simple drivers for the rh850u2b6.
- [ ] Architecture a serial protocol over TCP similar to PCIe so a PC host running the emualtor can access the rh850u2b6's peripherals. This is a big task, but it would be a good demonstration of the emulator's capabilities. First over serial, then maybe over USB or rela ethernet device. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] Implement an emulated GTM device for the stm32f746zg to demonstrate the emulator's capabilities.
- [ ] Finish the ppc emualtor with dual core support and implement a simple driver for the e200z7. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] **Port to the Nucleo-N657X0-Q (STM32N6, board MB1940).** A third
      platform, and the first that is neither ARMv7E-M nor flash-based.
      Facts established from RM0486, PM0273 and UM3417 in `docs/st/stm32n6/`:

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
        Run `-DRV32_PAIR_STATS=ON` first and let the histogram say whether
        any guest can use it, the way the pair stats answered the fusion
        question and the FP histogram answered the lowering one.

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

      **CubeN6 ships ThreadX and NetX Duo, not FreeRTOS and lwIP** --
      its `Middlewares/ST` holds threadx, netxduo, filex, levelx, usbx
      and no lwIP at all. That does *not* constrain this port, and the
      reason is worth stating so nobody ports the network stack to NetX
      Duo for no reason:

      - No RTOS is needed or wanted. The stack runs `NO_SYS = 1` --
        lwIP is a library the run loop calls, and `lwipopts.h` has a
        compile-time `#error` if that is ever overridden, because this
        port supplies no `sys_arch`. FreeRTOS's absence changes nothing
        because it was never used.
      - lwIP does not come from the family pack. `cmake/lwip.cmake`
        fetches `STMicroelectronics/stm32_mw_lwip`, which is
        family-independent -- the same repository already serves both
        the F4 and the F7 platforms and never mentions
        `STM32CUBE_FAMILY`.

      So the N6 keeps lwIP, PPP, TFTP and telnet unchanged. NetX Duo
      would only be worth reaching for if ST's stack were wanted on its
      own merits, which is a different question from this port.

      Add a `f746`-style row to `scripts/build-matrix.sh` with the port,
      not after it.

**Measured and rejected**, kept here so they are not retried blind:

| Idea | Result |
|---|---|
| Interpreter loop in SRAM | **slower** — 162 vs 122 cycles, and 8 KiB off the guest |
| PMP mapped onto the ARM MPU | **not possible** — the MPU cannot distinguish a guest access from an emulator access, because the JIT's inlined load *is* both |

