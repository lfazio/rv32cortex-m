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
      S-mode at 0x80400000. That address is where a kernel goes, and one
      goes there now: `scripts/run-linux.sh` builds fw_payload with a
      kernel inside it. Run bare, with no payload, it still spins to the
      instruction cap, which is the expected outcome rather than a
      failure.
- [~] **Linux** - rv32 with MMU on the host emulator, to a shell, then
      benchmarks.

      **A full distribution boots to a login prompt.** Yocto
      `core-image-minimal` for `qemuriscv32` (poky scarthgap), root
      filesystem on virtio-blk:

          EXT4-fs (vda): mounted filesystem ... r/w, ordered data mode
          Run /sbin/init as init process
          INIT: Entering runlevel: 5
          Poky (Yocto Project Reference Distro) 5.0.20 qemuriscv32 /dev/ttyS0
          qemuriscv32 login:

      udev, syslogd, network interfaces configured, getty on ttyS0 and
      on the virtio console. Built with `MACHINE=qemuriscv32`, which is
      a first-class machine in poky and needs none of the config
      fragments the usual write-ups describe. Run it with:

          LINUX_INITRAMFS=0 \
          LINUX_BOOTARGS="earlycon=sbi console=ttyS0 root=/dev/vda rw rootwait" \
          LINUX_DISK=<rootfs.ext4> scripts/run-linux.sh

      Three things that cost time and would cost it again:

      **A built-in initramfs containing /init wins over `root=`.** The
      kernel runs it and never looks at the root device. No warning.
      `LINUX_INITRAMFS=0` builds without one.

      **Yocto's own kernel does not work on this machine.** It builds
      6.6 for qemuriscv32 and the APLIC driver landed in 6.10, so every
      device needing an interrupt defers for ever. Its *rootfs* on our
      6.12 kernel is the pairing, and the right one: a rootfs is
      userspace and only needs virtio-blk and ext4 in whatever kernel
      runs it.

      **The host build needs two Debian packages Yocto assumes**,
      `libcrypt-dev` and `rpcsvc-proto`. Both fail late and obscurely --
      `crypt() not found` inside shadow-native, and a HOSTTOOLS error
      naming `rpcgen` -- and `dpkg -s` reports "ok" for a package in
      `deinstall ok config-files`, which is how both were missed on the
      first check. Use `dpkg-query -W -f='${Status}'`.

      Still open: a shell session driven from the host end (the login
      prompt is there; nothing types into it yet), and benchmarks. Broken into the order the pieces actually unblock each
      other, because most of them are only testable once the one above
      works:
  - [x] OpenSBI in M-mode, above.
  - [x] **Kernel boot to userspace. Done.** Linux 6.12 rv32 boots on
        OpenSBI, reaches `Run /init as init process`, and the init
        process makes user-mode `ecall`s that land in `do_trap_ecall_u`
        -- so Sv32, the S-mode trap path and the U-mode boundary all
        work. ~1.5e9 instructions interpreted, about 10 seconds of guest
        time. Rootfs is an initramfs built into the image.

        **`scripts/run-linux.sh` builds and runs the whole thing**, and
        that is most of what this entry was worth. It had been assembled
        by hand, which is how the tree came to be running a kernel image
        six hours *older* than the init binary supposedly inside it --
        every run was exercising a previous userspace, and nothing said
        so. The script rebuilds init, regenerates the cpio manifest,
        rebuilds the kernel and OpenSBI, and runs, in one command.

        Two things it had to fix to get this far:

        **The initramfs had no `/dev/console`.** A directory handed to
        CONFIG_INITRAMFS_SOURCE cannot carry a device node -- creating
        one needs root -- so the kernel reported "unable to open an
        initial console" and userspace had no file descriptors. The
        manifest form of INITRAMFS_SOURCE lets gen_init_cpio write the
        node as a cpio record, which needs no privilege.

        **`--max-insn` could not express the budget this needs.** Its
        field is uint64_t and it was parsed through a uint32_t, so
        anything over 4,294,967,295 was rejected as a *usage error* --
        printing the help, which reads as a typo in the command line.
        A kernel needs more than 3G instructions to reach init, so the
        first budget anyone would want was the first one refused.
  - [~] **The JIT translates under paging now, and is still not a win
        on wall time.** It used to decline the moment Sdtrig, PMP or
        paging was armed, because `blocked` pointed at `fetch_guard`,
        which folds all three -- so under Linux it translated nothing
        after the first page table and was 12% *slower* than no JIT.

        `ir_fetch16` walks the page tables and checks execute permission
        per halfword now, as the interpreter's fetch does, and `blocked`
        is Sdtrig alone. Same 600M budget, same image:

        | | wall | interpreted | translations |
        |---|---|---|---|
        | originally | 52.5 s | 580,180,660 (96.7%) | 4,786 |
        | translating | 61.9 s | 228,272,019 (38.0%) | 951,343 |
        | + batched fallback | 53.1 s | 236,069,284 (39.3%) | 931,905 |
        | + FENCE lowered | **49.4 s** | 161,193,429 (26.9%) | 931,160 |
        | interpreter | 46.8 s | all | -- |

        Coverage is transformed and the clock is nearly level. **What
        remains is measured rather than guessed.**

        **SFENCE.VMA flushes the whole code cache** -- 129,286 times in
        a boot. An A/B with the invalidation simply removed (incorrect,
        for measurement only) runs 55.7 s with 97,646 translations, so
        this is worth about 6 s. Fixing it properly means page-granular
        invalidation: a block would record the guest page it was
        translated from, and a targeted SFENCE would invalidate only
        matching blocks. The handler currently ignores its operands,
        which is legal and maximally coarse.

        **The fallback interpreted one instruction per call. Fixed.**
        Every declined instruction paid the interpreter's whole run-loop
        entry, which the interpreter proper amortises over thousands.
        It now interprets a run in one call, with an adaptive size:
        doubling while translation keeps failing, reset to one on block
        entry, so an isolated declined instruction still costs exactly
        one. 61.9 s to 53.1 s with coverage unchanged.

        Resetting that size in the wrong place is worth knowing about.
        The first version reset only on a *fresh* translation, which in
        steady state never happens -- a warm cache answers from the
        lookup -- so the batch pegged at its maximum and ran past
        translatable code. Interpreted rose to 443M of 600M, block
        entries fell 99M to 22M, and it was **faster**: 45.5 s, because
        an efficient interpreter beat a JIT that had stopped
        translating. A speed-up is not evidence.

        **Plain FENCE was not lowered. Fixed.** It is a no-op on this
        machine and the translator declined it, so every barrier ended a
        block and took a fallback -- and kernel code is full of them.
        53.1 s to 49.4 s, interpreted 39% to 27%.

        **Since then: the memory fast path and block chaining.** What
        the ratio measurement said, and what each step was worth on
        Dhrystone against an interpreter at 0.231 s:

        | | wall | ratio | block entries |
        |---|---|---|---|
        | before | 0.104 s | 44.21 | 1,665,337 |
        | + self-loop chaining | 0.084 s | -- | 741,085 |
        | + inlined RAM access | 0.057 s | 33.02 | 741,085 |
        | + cross-block linking | **0.046 s** | **29.65** | **320,738** |

        2.0x to **5.0x**. The order came from measuring rather than
        guessing: 40% of guest instructions were a memory access and
        each was a C call (3,728,397 of them), which was far larger than
        the dispatcher; only after that did linking exits become the
        biggest remaining term. 134 links removed 420,000 dispatches.

        **The next lever is a minimum block length.** Lowering FENCE
        took block entries from 100M to 180M: the instructions that
        stopped being interpreted land in roughly one-instruction
        blocks, because a block starting at a FENCE lowers it and then
        declines on whatever follows. At about 300 cycles of dispatch
        per entry -- which is what differencing the two runs above
        gives -- entering a block to run a single no-op is a loss.
        Refusing to keep a block below some length would send those back
        to the (now efficient) fallback. Not done here because guests
        with genuinely short hot blocks, CoreMark at 4.12 instructions,
        have to be measured against it.

        Also worth knowing: the JIT attempts a full translation before
        almost every interpreted instruction and gets nothing back,
        which is ~230M attempts for 951k successes.

        Guests that never page are unaffected and still twice the
        interpreter: bench 0.028 s against 0.057, dhrystone 0.104
        against 0.203.

        **What made any of this checkable is `EMU_EXTRA_ARGS` on
        run-arch-test.sh.** Nothing validated a translating backend
        under the privileged features before -- the suite runs the
        interpreter, and the JIT passed the Sv32 and PMP tests by
        declining to execute them. It is 378/378 both ways, and with
        `--jit` those tests now translate for the first time.

  - [x] **Userspace output reaches the console. Done**, and the cause
        was the emulator stealing the guest kernel's syscalls.

        `emu_guest_syscall` is semihosting: it answers `write` and
        `exit` so a bare-metal guest can print and terminate with no
        kernel underneath it. The interpreter consulted it for *every*
        ECALL, at any privilege -- so a Linux process calling write(2),
        which traps from U-mode, was answered by the emulator and the
        kernel never saw the call. It is M-mode only now, which is where
        every bare-metal guest here runs. An S-mode ECALL is an SBI call
        and belongs to the firmware.

        **It imitated success almost perfectly**, which is why it took
        so long. The hook returns the length it was given, so write()
        reported every byte written; it ignores the descriptor, so
        `write(-1, ...)` reported success too; and it reads the buffer
        with a *physical* bus access, which under Sv32 is not where a
        user pointer points -- so the read faults on the first byte and
        nothing is printed. Userspace was mute while every call
        succeeded.

        Four things were ruled out first, and all four were sound
        conclusions from a broken instrument: the transmit interrupt
        (requested 3 times a boot, all start-up probe), the tty layer
        (/dev/kmsg bypasses it and behaved identically), the process
        exiting too early (a busy loop bought 85 seconds of guest time),
        and printk from process context (a `pr_emerg` in `do_group_exit`
        printed perfectly).

        **What broke the deadlock was testing the instrument**, which
        this file already tells you to do: `write(-1, buf, 29)` must
        return -EBADF and returned 29. Everything measured with that
        wrapper was worthless, and the contradiction it had produced --
        "the write succeeds" against "the kernel function it must go
        through is never entered" -- was the shape of a lying
        instrument rather than a strange kernel.

        `tests/unit/test_ecall.c` pins all three privileges. Reverting
        the fix gives 6 failures.

  - [~] **An interrupt controller -- and it does not have to be a PLIC.**
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

        **Done, and the part that was missing was not the controller.**
        The APLIC drove MEIP unconditionally, which is right for every
        bare-metal guest here and invisible to Linux: it runs in S-mode
        under OpenSBI and never sees MEIP. `--supervisor` moves
        delivery, carried as emu_boot_info_t::supervisor because it is a
        property of the machine. The device tree now describes the
        APLIC in direct mode and the virtio nodes.

        What is *not* done is seeing it fire under Linux -- the unit
        test proves delivery moves between privileges, and nothing has
        yet driven a real queue completion through the whole path.
  - [x] **virtio-mmio transport**, done by importing rather than
        writing: TinyEMU's `virtio.c` is vendored byte-identical under
        `third_party/tinyemu/` (MIT), and the porting layer is four
        functions -- `cpu_register_device` to `emu_bus_add_mmio`,
        `phys_mem_get_ram_ptr` to `emu_bus_host_ptr`, `set_irq` to
        `emu_raise_irq`, and a dozen inline helpers. That brings block,
        console, net, input and 9p with it. The PCI transport is refused
        loudly rather than stubbed, because a stub lets `virtio_pci_init`
        appear to succeed and return a device that never answers.
  - [~] **The devices on it.** `--9p [TAG:]DIR`, `--virtio-input` and
        `--disk FILE` / `--disk-ro FILE` work. 9p was first on purpose:
        it needs no image to build and no partition table to get right,
        so the host directory *is* the filesystem. The input pair is a
        keyboard and a mouse, fed from the same SDL events as the simple
        polled devices and from the same converted evdev codes, so the
        two families cannot disagree about what a key is.

        virtio-blk is a file-backed `BlockDevice`: 512-byte sectors,
        synchronous, because the completion path already handles it --
        `read_async` returning 0 means "done" and the request ends
        inline. `guest-virtiotest-blk` reads sector 0 and asserts its
        *content*, since a device that signalled and filled nothing
        would pass every other check in the guest.

        virtio-net is `--net loop` and `--net tap:NAME`. The loopback
        backend exists because a tap cannot be tested: it needs a device
        node, a persistent interface and an address on it, none of which
        a suite can assume, so a device offered only over tap would ship
        having never moved a packet. `guest-virtiotest-net` is also the
        first test to drive two queues at once.

        **All three probe under Linux**, which is the check that matters
        and which no unit test can make: `virtio_blk virtio0: [vda]
        32768 512-byte logical blocks (16.8 MB/16.0 MiB)`.

        Next is the display. And note what a *missing* device does:
        every virtio node in the device tree needs one behind it, or
        virtio_mmio_probe reads the magic, the bus refuses an unmapped
        address, and the kernel takes a load access fault and panics
        inside driver_attach. Not a driver quietly finding nothing.

        **What virtio-blk cost was a barrier in the guest, not a bug in
        the device.** A volatile access does not order the ordinary
        stores around it, so GCC sank the descriptor ring and
        `avail->idx` past the volatile write to QueueNotify. The device
        read an avail ring still holding zero and did nothing; every
        register was right and nothing faulted. The console queue had
        the identical defect and passed anyway, because its one
        descriptor happened to be scheduled first -- one weak test is
        worse than none, again.

        **The interrupt path is proven.** `tests/guest/virtiotest.c`
        drives the console queue to completion from guest code and takes
        the interrupt: descriptor ring in guest RAM, QueueNotify, the
        device printing the bytes and advancing the used ring, the APLIC
        delivering, and the handler acknowledging both device and
        controller. It runs under `ctest -L fast`.

        Its first version failed in the way this whole entry was written
        to expect: it spoke the *legacy* transport at a device reporting
        version 2, every write was accepted, and QueueNotify did nothing
        at all. No register was wrong.

        And the device tree has to name them: 0x1000_1000 upwards,
        0x1000 apart, interrupts from 1. Nothing checks that the tree
        and the emulator agree -- they are two descriptions of one
        machine, and the usual failure is a driver finding nothing.
- [x] **Find out what the 50x is. Answered, and it was two things.**

      `--rate` reports host instructions per guest instruction directly,
      and on Dhrystone the JIT was **44.21** against the interpreter's
      131. A translated ALU operation should be 4-8.

      **40% of guest instructions were a C call.** Every load and store
      went to rv_hart_load/rv_hart_store, which walk the region table:
      3,728,397 calls in a 9.4M-instruction run. Inlining the ones that
      are plain RAM took the ratio to 33.02.

      **The rest was the dispatcher**, at about 80 cycles a round trip
      for blocks of a handful of instructions -- measured by differencing
      two runs that differed only in block entries, not by reasoning.
      Linking exits took it to **29.65** and Dhrystone from 0.104 s to
      0.046 s, 2.0x to **5.0x** the interpreter.

      What is left at 29.65 is the *register file*: every GET is a load
      from the cpu struct and every PUT a store, because the allocator
      covers temps rather than guest registers. That is the next lever
      and it needs measuring before it is built -- a guest-register
      cache was tried on Thumb-2 with three registers and measured
      **15.5% slower**, and x86-64 with fifteen is a different
      proposition. `EMU_JIT_HOT_REG_STATS` is the histogram that would
      answer it.

      The original reasoning below was wrong in an instructive way, and
      is kept for that:

- [ ] ~~**Find out what the 50x is.**~~ The runner now reports host
      instructions per guest instruction live, and it is 46-53 across
      every guest measured -- far above what a translated block should
      cost.

      What it is *not*: SoftFloat, which was the first theory and is
      refuted by Dhrystone, which has no floating point and costs more
      per instruction than Quake, and by Whetstone, which is the most
      FP-heavy and costs least. Nor interpreter fallback: 98% of
      instructions run translated in both guests measured.

      What is left is the shape of the blocks. Quake enters one every
      7.1 guest instructions, so every dispatch is amortised over very
      little -- which is the same conclusion the pair-statistics work
      reached from the other end, and the reason this file already says
      to go after longer blocks rather than more registers.

- [~] **Doom, then Quake** -- as benchmarks with a real frame rate
      rather than a checksum, and as the first guests big enough to make
      the JIT's figures mean something.

      **DOOM and Quake 1 both run, on rv32 and on G4MH**, at 800x600:
      DOOM loads E1M1 and plays, Quake initialises, loads `demo1.dem`
      and runs it. Recipes are in the README; the game data is supplied
      rather than fetched. What they bought beyond being playable was
      four emulator defects neither test suite could see -- a JIT that
      read a load's displacement as a floating-point opcode, a guest
      runtime with no thread pointer, misaligned access the C library
      assumes, and a flash window sized for smaller images.

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

  - [x] **A framebuffer device**, portable C in `src/emu/` with no SDL in
        it -- the guest writes pixels, the host presents them. Same split
        as the NS16550 and the console: the *device* is portable and the
        platform owns the window, or it will not build for the F746. A
        plain framebuffer rather than `virtio-gpu`, which buys nothing
        until something wants 3D acceleration this emulator does not have.
        **Done** -- `src/emu/emu_fb.c`. Why it is not an emulated VGA, and
        what one would cost, is in [`docs/vga.md`](vga.md): the deciding
        factor is that VGA's write modes live in the store path, so every
        pixel would become a device dispatch.

        `--fb WxH` picks the mode it starts in, 1024x768 by default, and
        refuses a geometry the device's table does not hold -- otherwise
        the geometry registers would report a size MODE_GET matches
        nothing for, and a guest enumerating modes could not find the
        one it is already in. It only moves the *starting* mode: the
        buffer is sized for the largest either way, so a guest that sets
        its own through the mode registers is unaffected.
  - [x] **An SDL host backend** to present it. `src/platform/host/
        sdl_display.c`, behind `-DEMU_SDL=ON`, against **sdl3** rather
        than the SDL2 this entry was written expecting. Off by default,
        because the device above must build and be testable on a host
        that has neither -- which is how the F746 gets a framebuffer at
        all.
  - [x] **Input**, keyboard and mouse, from SDL events. Two devices and
        not one: a host binds a separate evdev to each, and a combined
        descriptor claiming both is not what any driver expects. There
        are now two *pairs* -- these simple polled rings, which DOOM and
        Quake use, and the virtio pair an operating system's driver
        binds to. Both are fed from the same converted evdev codes, so
        they cannot disagree about what a key is.
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
  - [x] **Quake on rv32 -- it runs.** Initialises, loads `demo1.dem`
        and plays it, on rv32 and on G4MH, at 800x600. It was the
        harder target and it answered the question it was set: the JIT
        holds up under floating point at scale, and the host/guest
        ratio on it (46.6) is *better* than Dhrystone's (52.6), which
        has no floating point at all -- which is what refuted the
        theory that SoftFloat was the cost.
        https://github.com/lfazio/quake-embedded
  - [ ] Doom on ppc, once it has a JIT.
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
  - [ ] Option bytes: impact on clocks and startup.
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
