# Open work

Moved out of README.md. Status lines here are claims about the code, so
they go stale: `scripts/report-figures.sh` regenerates the ones a host can
check, and anything measured on hardware carries the commit it was
measured at.

## Roadmap

- [x] LWIP's PPP stack in the host runner. `--ppp` opens a pty, `pppd`
      attaches to it exactly as it attaches to /dev/ttyACM0, and the same
      lwIP, telnet, TFTP and gdb stub the board runs become debuggable
      with the tools a development machine already has. See
      `scripts/ppp-host.sh`; the default subnet is 192.168.8.x so it can
      be up alongside a board on 192.168.7.x.
- [x] Unify host main and the firmware main. Done as far as it should go.
      `emu_session.c` is the whole middle -- open the cores, choose the
      backend, install the hooks, place the image, reset, boot, report --
      and `emu_run.c` is the loop. What stays per-platform is acquisition
      (argv and a file against an incbin and TFTP) and termination (an exit
      status a suite reads against a park loop serving a link). Those are
      genuinely different and merging them would put an `#if` per platform
      inside one `main()`, which is the arrangement all of this undid.
- [ ] Add FreeRTOS support to the host emulator, lwip in one task and emulator in another one (it will allow to instanciate later a tinyusb network device over USB). https://github.com/STMicroelectronics/x-cube-freertos/tree/main (https://github.com/hathach/tinyusb)
- [x] Make rv32 frontend works with opensbi (https://github.com/riscv-software-src/opensbi/tree/master)
      `scripts/run-opensbi.sh`. OpenSBI v1.9 `generic` boots on the
      device tree in `boot/rv32-emu.dts`, finds the uart8250, the
      aclint-mswi and aclint-mtimer, sets up its PMP domains and jumps to
      S-mode at 0x80400000 -- which spins to the instruction cap because
      no payload is loaded there yet. That address is where a kernel goes.
- [ ] **Linux** - rv32 with MMU on the host emulator, to a shell, then
      benchmarks. Broken into the order the pieces actually unblock each
      other, because most of them are only testable once the one above
      works:
  - [x] OpenSBI in M-mode, above.
  - [~] Kernel boot to userspace with **no devices at all**. Linux 6.12
        rv32 now boots on OpenSBI to driver init -- 447 lines of dmesg,
        SBI v3.0 detected, TIME/IPI/RFENCE/DBCN found, memory and zones
        set up, io schedulers registered. Reaching `init` needs more than
        3G instructions on the interpreter; still to confirm. `earlycon=sbi`
        and `hvc0` go through SBI calls OpenSBI already serves, so this
        needs no PLIC, no virtio and no interrupt controller -- which is
        the point of doing it first: it isolates Sv32, the S-mode trap
        path and SBI from every device question. Rootfs is an initramfs
        built into the image (`boot/initramfs/`), so no block device
        either. A static `-nostdlib` init prints and exits.
  - [ ] **An interrupt controller -- and it does not have to be a PLIC.**
        Established by booting without one: the kernel reaches driver
        init with *no* interrupt controller in the device tree, because
        the timer comes from the SBI TIME extension, IPIs from SBI IPI
        and the console from SBI DBCN. So this blocks devices, not boot.
        Use the **APLIC the emulator already has** rather than writing a
        PLIC: a 6.12 kernel has `CONFIG_RISCV_APLIC=y` as well as
        `CONFIG_SIFIVE_PLIC=y`. It must be wired in **direct mode** --
        `interrupts-extended` to the cpu intc and no `msi-parent` --
        because MSI mode needs an IMSIC and the AIA CSRs, which this
        emulator does not implement.
  - [x] **virtio-mmio transport**, done by importing rather than
        writing: TinyEMU's `virtio.c` is vendored byte-identical under
        `third_party/tinyemu/` (MIT), and the porting layer is four
        functions -- `cpu_register_device` to `emu_bus_add_mmio`,
        `phys_mem_get_ram_ptr` to `emu_bus_host_ptr`, `set_irq` to
        `emu_raise_irq`, and a dozen inline helpers. That brings block,
        console, net, input and 9p with it. The PCI transport is refused
        loudly rather than stubbed, because a stub lets `virtio_pci_init`
        appear to succeed and return a device that never answers.
  - [~] **The devices on it.** `--9p [TAG:]DIR` works and is the first
        one on purpose: it needs no image to build and no partition
        table to get right, so the host directory *is* the filesystem.
        Next is `virtio-blk` (the one that makes throughput measurable),
        then `virtio-net`, then input and display.

        **None of them can interrupt yet**, which is the blocker above
        rather than here: the devices are mapped and answer their magic,
        version and device id, and a driver that probes will find them
        and then wait for ever on a queue whose completion nothing
        signals. The APLIC is the next piece.

        And the device tree has to name them: 0x1000_1000 upwards,
        0x1000 apart, interrupts from 1. Nothing checks that the tree
        and the emulator agree -- they are two descriptions of one
        machine, and the usual failure is a driver finding nothing.
- [~] **Doom II, then Quake III** -- as benchmarks with a real frame rate
      rather than a checksum, and as the first guests big enough to make
      the JIT's figures mean something.

      **DOOM and Quake 1 both run, on rv32 and on G4MH**, at 1024x768:
      DOOM loads E1M1 and plays, Quake initialises, loads `demo1.dem`
      and runs it. Recipes are in the README; the game data is supplied
      rather than fetched. What they bought beyond being playable was
      four emulator defects neither test suite could see -- a JIT that
      read a load's displacement as a floating-point opcode, a guest
      runtime with no thread pointer, misaligned access the C library
      assumes, and a flash window sized for smaller images.

      Still to do here: Doom II and Quake III, which are the larger
      data sets rather than new ports.

      Order is rv32, then g4mh, then ppc -- but **the second and third are
      blocked on toolchains rather than on the emulator**, which is worth
      knowing before planning around them:

      | frontend | JIT | C guest possible | blocker |
      |---|---|---|---|
      | rv32 | yes | yes | none -- CoreMark already runs 1.5G instructions |
      | g4mh | yes | **yes** | none known -- CC-RH is installed, see below |
      | ppc  | **no** | **no** | `powerpc-linux-gnu-gcc` is not built with VLE (`-mvle` is rejected), so only hand-written assembly compiles; and there is no IR translator |

      **CC-RH is installed** -- V2.07.00 and V2.08.00 under
      `/usr/local/Renesas/CC-RH/`, with `ccrh`, `asrh` and `rlink`. Both
      report *"Paid license of CC-RH V2 is not found, and the evaluation
      period has expired"* at link time, and both link anyway: a warning,
      not an error. An 8,000-line source compiled to a 927 KB object
      without complaint, so there is no obvious size cap -- but nothing
      Doom-sized has been linked, and that is the thing to test before
      planning around it rather than after.

      So rv32 first because nothing blocks it, and g4mh is a real second
      rather than a hypothetical one. ppc is the far one: it needs a VLE
      C compiler *and* a JIT, and the compiler is the harder to acquire.

      Note `scripts/g4mh-check-encodings.sh` reaches CC-RH through a
      Docker image and hardcodes the same V2.08.00 path the native
      install uses, so it may work directly against the local toolchain.

  - [ ] **A framebuffer device**, portable C in `src/emu/` with no SDL in
        it -- the guest writes pixels, the host presents them. Same split
        as the NS16550 and the console: the *device* is portable and the
        platform owns the window, or it will not build for the F746. A
        plain framebuffer rather than `virtio-gpu`, which buys nothing
        until something wants 3D acceleration this emulator does not have.
        **Done** -- `src/emu/emu_fb.c`. Why it is not an emulated VGA, and
        what one would cost, is in [`docs/vga.md`](vga.md): the deciding
        factor is that VGA's write modes live in the store path, so every
        pixel would become a device dispatch.
  - [ ] **An SDL host backend** to present it. SDL2 is **not installed on
        the build machine**; that is one apt away but it is a real
        prerequisite, and the device above must build and be testable
        without it.
  - [ ] **Input**, keyboard and mouse, from SDL events. Two devices and
        not one: a host binds a separate evdev to each, and a combined
        descriptor claiming both is not what any driver expects.
  - [x] **DOOM on rv32 -- it plays.** Full startup, E1M1, frames through
        the framebuffer, keyboard and mouse live.

        `-DEMU_DOOM=ON`, then `scripts/doom-gentables.sh`, then
        `guest-doom`; run with `--jit --ram 0x2000000 --timer-hz 6`.
        The port layer is GPL and lives in the port
        (https://github.com/lfazio/embeddedDOOM, branch `rv32cortex-m`);
        nothing of DOOM's is in this tree.

        `--timer-hz` is the dial that matters. The guest renders a few
        frames a second while `mtime` runs at wall-clock, so DOOM
        advances many world-tics per drawn frame and the game appears to
        fast-forward. Dividing guest time back down matches its own
        rate -- 6 suits this machine, another will differ.
  - [ ] **Quake III on rv32**, after Doom II works. It is the harder
        target and the one that will say whether the JIT holds up under
        floating point at scale.
        https://github.com/lfazio/quake-embedded
  - [ ] Doom II on g4mh. The toolchain is there; what is untested is
        whether an expired-evaluation `rlink` will produce a binary that
        large.
  - [ ] Doom II on ppc, once it has both a VLE C compiler and a JIT.
- [ ] **PowerPC debug infrastructure**, which is three files where the
      other two frontends have fifteen. Every G4MH defect this project
      found was found with a trace and a disassembler; the PowerPC ones
      were found by bisecting by hand with external `objdump`, which is
      the same job done slowly.
  - [ ] `ppc_decode.c` -- decoding is inline in the interpreter, so
        nothing else can ask what an instruction is. A JIT needs this
        before it needs anything else.
  - [ ] `ppc_disasm.c` -- and it is worth remembering that this tree's
        disassemblers have twice been the *weaker* instrument: rv32
        printed every OP-FP as `illegal` and turned a hard-float profile
        into "there is no floating point here", and g4mh printed
        confident nonsense for a slot it did not know. Write it against
        the assembler, not against the interpreter.
  - [ ] `ppc_gdb.c` -- the register layout gdb expects, which is a fixed
        per-architecture order and not a choice.
  - [ ] `ppc_pairstats.c` -- the histogram that answers "which
        instruction next" with a measurement instead of an opinion.
- [ ] **JIT** - Autovectorisation of the IR pipeline. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] Add simple drivers for the rh850u2b6.based on their specification in the reference manual.
  - [ ] Option bytes: impact on clocks
  - [ ] Clock tree
  - [ ] ltsc
  - [ ] ostm
  - [ ] gpio/on emulated on top of stm32 gpio hal, emualated to output the binary state of output on a udp/ip connexion.
  - [ ] kcrc
  - [ ] wdtb
- [ ] Architecture a serial protocol over UDP/TCP similar to PCIe so a PC host running the emualtor can access the rh850u2b6's peripherals. This is a big task, but it would be a good demonstration of the emulator's capabilities. First over serial, then maybe over USB or rela ethernet device. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] Implement an emulated GTM device for the stm32f746zg/stm32n657 to demonstrate the emulator's capabilities.
- [ ] Implement TAUD peripheral in rh850u2b6 with the remaining stm32 timer availble.
- [ ] Finish the ppc emualtor with dual core support and implement a simple driver for the e200z7.
- [x] **Port to the Nucleo-N657X0-Q (STM32N6, board MB1940).** A third
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
        Run `-DEMU_PAIR_STATS=ON` first and let the histogram say whether
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
