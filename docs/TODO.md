# Open work

Moved out of README.md. Status lines here are claims about the code, so
they go stale: `scripts/report-figures.sh` regenerates the ones a host can
check, and anything measured on hardware carries the commit it was
measured at.

## Roadmap

- [ ] Unify the main runner for all platforms, so the F746 and N6 can share `main.c` with the host. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] Add usage of LWIP's PPP stack to the host runner, so the host can run the same code as the F746.
- [ ] Unify host main and the stm32F{4,7} main, so the host can run the same code as the F746. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] Add FreeRTOS support to the host emualtor, lwip in one task emulator in another one (it will allow to instanciate later a tinyusb network device over USB). https://github.com/STMicroelectronics/x-cube-freertos/tree/main (https://github.com/hathach/tinyusb)
- [ ] **Linux** - Prepare a Linux guest rv32g with mmu and run it on the emulator (x86_64 only). This is a big task, but it would be a good demonstration of the emulator's capabilities. Implement minimal virtio devices to get a shell and run some benchmarks. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] Run doom in Linux so it can be used as a benchmark for the emulator. This is a big task, but it would be a good demonstration of the emulator's capabilities implement a sdl backend for the emulator to run doom in Linux. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] **JIT** - Autovectorisation of the IR pipeline. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] Add simple drivers for the rh850u2b6.
  - [ ] kcrc
  - [ ] ltsc
  - [ ] ostm
  - [ ] wdtb
- [ ] Architecture a serial protocol over UDP/TCP similar to PCIe so a PC host running the emualtor can access the rh850u2b6's peripherals. This is a big task, but it would be a good demonstration of the emulator's capabilities. First over serial, then maybe over USB or rela ethernet device. This is a big task, but it would be a good demonstration of the emulator's capabilities.
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

**Measured and rejected**, kept here so they are not retried blind:

| Idea | Result |
|---|---|
| Interpreter loop in SRAM | **slower** — 162 vs 122 cycles, and 8 KiB off the guest |
| PMP mapped onto the ARM MPU | **not possible** — the MPU cannot distinguish a guest access from an emulator access, because the JIT's inlined load *is* both |

