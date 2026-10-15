# Open work

Moved out of README.md. Status lines here are claims about the code, so
they go stale: `scripts/report-figures.sh` regenerates the ones a host can
check, and anything measured on hardware carries the commit it was
measured at.

## Open, from the board runs of October 2026

Found by putting the PowerPC frontend on the Nucleo-F746ZG and timing
long runs. The defects are fixed; these are what the measurements left
standing. Figures are in [jit/tuning.md](jit/tuning.md).

- [x] **FreeRTOS on every platform: a task per emulated core, the
      cores synchronised at every quantum** ([os.md](os.md)). The POSIX
      port on the host, CM7, CM4F and CM55 on the boards; cooperative, so
      a multicore round is still core 0, 1, 2 -- asserted every round,
      because two order mutants left `barrier3`'s output and trace
      byte-identical. Identical to the commit before at quanta 1, 4, 64
      and 4096 on both backends; on the F746 the three-PE guest matches
      the host core by core. Left open by it:

      - [x] **A multicore host runs the cores in parallel.** FreeRTOS
        has no SMP POSIX port, so one is written here
        (`src/platform/host/freertos_smp`); the emulator got per-core JIT
        state, locks on G4MH's shared devices and the console, host
        atomics for CAXI, the bit operations and LL/SC, and a stack per
        PE ([os.md](os.md#smp-on-the-host)). `--serial` is the
        reproducible round-robin. Three PEs of independent work run
        2.79x (interpreted) and 2.67x (translated) faster at the default
        parallel quantum on a 20-CPU host.
      - [x] **Quantum and SMP.** Every round is a barrier across host
        threads, so a small quantum costs most of what parallelism buys:
        the JIT goes 1.85x at 4,096, 2.38x at 16,384, 2.67x at 32,768 and
        2.66x at 65,536. The default is **32,768 when an SMP host runs
        a multicore guest's cores in parallel** and 4,096 otherwise. A
        single default of 32,768 failed two Zicntr architecture tests:
        the guest clock moves once per quantum, and they read `time`
        twice ~11,000 instructions apart ([os.md](os.md#the-quantum)).
        The boards keep 4,096 (`EMU_RUN_SLICE`), unmeasured there.
      - [x] **The JIT under SMP declined memory bit operations**, so
        each one ended a block. They are a helper call into
        `g4mh_bitop`, the interpreter's host atomic, and the register
        form is translated as well as the immediate one. The same helper
        serves Thumb-2, which cannot lower `EMU_IR_BITOP` and used to
        lose the whole block to it -- its `emu_ir_can_lower` promised it
        could, and x86-64's denied what it does.
      - [x] **FreeRTOS runs on the N6**, for the first time, and what
        stopped it was not the port. (A box here was ticked in 18c54e1 on
        a misreading: the board had worked *before* FreeRTOS.) RV32
        `isatest` passes with 1,129 of 65,829 interpreted, as on the F746
        and x86-64, and three-PE G4MH `smp.bin` prints the model's line,
        3,776 rounds of 4,096 -- the host's cooperative count exactly.

        **picolibc's `errno` is thread-local, and nothing on the boards
        set the thread pointer.** ST's Reset_Handler is not picolibc's
        crt0, so `__tls` stayed 0 and every `errno` store went to
        address 8 -- ITCM on the N6. `strtoull`, parsing `--quantum`,
        zeroed the `sub sp, #228` at the head of `emu_jit_run`; every
        slice then returned through `emu_main`'s frame and the first one
        HardFaulted on a pc of 0. A gdb hardware watchpoint on the one
        word that differed from the ELF named the writer. The link
        scripts now lay out `.tdata`/`.tbss` as picolibc's own does and
        `board_tls_init()` points `__tls` at the block, first thing,
        checking that `&errno` lands inside it.

        And the `flash` target starts the core now: it loaded a signed
        image through `STM32_Programmer_CLI -g`, which left the core in
        the boot ROM -- the CLI is on access port 0 and the M55 on 1.
        `scripts/n6-load.sh` does it through `ST-LINK_gdbserver -m 1` and
        gdb, resetting a crashed core to the ROM first
        ([os.md](os.md#on-a-board)).
      - [ ] **The F446 has built and not run** under FreeRTOS.
      - [x] **The F746's hot path was at the mercy of the linker
        because at 32 KB the hot path is the translator.** Measured
        ([jit/tuning.md](jit/tuning.md#layout-on-the-f746--what-the-140-to-153-swing-was)):
        moving all of `.text` by up to 1.5 KB changes nothing (152.67 to
        152.85), and so does moving guest RAM; changing the *order* of
        functions with `-falign-functions` takes the pre-FreeRTOS 140.01
        to 147.9 and 149.3 and HEAD's 152.85 to 148.5 and 150.5. So
        140.0 was a lucky order, not a baseline. A PC profile from the
        tick puts 70% of the time in `emu_ir_optimise`, `memset`,
        `lower_one` and `rv_translate` and 7.6% in translated code: with
        63,733 translations the cache is a queue, and tens of kilobytes
        of translator behind a 4 KiB I-cache are exactly the code that
        cares what sits next to it. At 96 KB the same layouts spread 2%,
        not 9%. The ITCM hang this entry used to record was `errno` at
        address 8 (fixed in ced1558); ITCM and DTCM now work, and are
        worth 1.6% and 1.8%, 3.1% together, stable across layouts.
      - [x] **The F746's JIT buffer is 96 KB, and its dispatch path is in
        ITCM** -- the defaults now. RV32 CoreMark x120 is 29.85 host
        cycles an instruction against 152.85, 5.1x, `crcfinal 0xd340`;
        ITCM is 8% of that at this size, against 1.6% at 32 KB, because
        translation no longer drowns it. A second layout gives 29.10, so
        the swing is 2.5% where it was 9%. The guest has 164 KiB of RAM
        where it had 232. ITCM's contents start at 0x100, so a store
        through a near-NULL pointer -- `errno`, until ced1558 -- hits
        nothing that runs.
      - [x] **Translate only hot code** -- built
        (`EMU_JIT_HOT_THRESHOLD`, `--jit-hot`), tested, and off by
        default: no measured workload gains. The F746's translations do
        not fall at any threshold, because what it retranslates is hot
        code it evicted; on x86-64 a translation is cheaper than
        single-stepping cold code, and Linux kernel init went 4.4 s to
        8.0 s at 16 ([jit/tuning.md](jit/tuning.md)).

- [x] **The RV32 translator declined the B extension, and that was
      nearly all of what the JIT still interpreted. Lowered.** Counted
      with `-DEMU_PAIR_STATS=ON`, which reports executed instructions by
      extension and now by B instruction:

      | guest | interpreted under the JIT, before | after |
      |---|---|---|
      | CoreMark | 23,339 of 518,234 -- 98.8% B | **1** |
      | Dhrystone | 120,566 of 9,431,232 -- 99.6% Zba | **1** |
      | crypto | 65,776 of 2,720,538 | **1** |
      | a Linux boot, 3e9 instructions | 1,308,143, 331,550 of them Zbb | 913,551, **no B** |

      Every guest here executes the same nine: the three shifted adds,
      `sext.h`, `zext.h`, `andn`, `rori`, `rev8` and `bexti`, then
      nothing (`minu` eight times, `bext` three). Those and the rest of
      Zba/Zbb/Zbs are open-coded from operations *both* backends lower;
      `clmul*`, `cpop`, `orc.b`, `min`/`max` and `rol`/`ror` go to one
      helper built on `rv_bitmanip.h`, which the interpreter shares.

      On the F746, CoreMark ×120, `crcfinal 0xd340` throughout: **32 KB
      172.3 → 140.0** host cycles an instruction (wall 23.6 s → 19.1 s),
      **96 KB 47.6 → 31.1** (6.5 s → 4.2 s). On x86-64, medians of
      fifteen interleaved: CoreMark −11.5%, Dhrystone −15.8%, crypto
      −19.7%, with `bench` and Whetstone -- which execute almost none --
      at +4.1% and +0.4%.

      **This was a capability the old translator had.** The
      hand-written Thumb-2 one translated Zbb and measured it
      ([performance.md](performance.md#what-the-jit-needed-along-the-way):
      307,128 fallbacks to 89); the IR that replaced it lowered `rori`
      and nothing else, and nothing said so but the `interp` line.
      Checked by `isatest`'s 230 new cases (expectations from
      `tests/guest/bitmanip-expect.py`, an independent model), on the
      board as on x86-64, and by 34 mutants of the lowering, the helper,
      the shared arithmetic and the interpreter: every one that changes
      an answer on a backend fails on that backend
      ([validation.md](validation.md#what-the-host-suites-do-not-cover)).
- [ ] **The default code cache makes the JIT slower than the
      interpreter on the F446, and on G4MH builds.** Decided for the
      F746 (915f583: 96 KB, CoreMark 152.85 → 27.82 with chain heat)
      and the N6 (128 KB in AXISRAM2, 175.46 → 20.21, and 17.09 at x1500
      since the smaller Thumb-2 code, the guest paying
      nothing), each except with G4MH compiled in, whose RAM leaves no
      room. The F446 has 128 KiB of RAM in all. What follows is the
      original entry. At
      `EMU_JIT_CODE_BYTES=32768` RV32 CoreMark is 140.0 host cycles an
      instruction against the interpreter's 107.4, `bench` 194.6
      against 100.1, and a JIT build pays 77 KiB of guest RAM for it.
      Lowering the B extension narrowed it (from 172.3) and did not
      close it. The JIT wins from 64 KB (67.4), and at 96 KB it is
      3.45× ahead. Three ways out,
      and they are not exclusive: raise the default and say what it
      costs the guest; default the firmware to the interpreter and make
      the JIT a choice for guests known to fit; or make the code
      smaller, which is the next two items. **Short encodings** took
      32 KB from 151.68 to 135.82 (CoreMark x650, worktree A/B), and the
      one-compare window guard to 117.63 -- 6% behind the interpreter,
      111.22 measured on the same tree, where it had been 22%. The
      shared slow path took it to 109.2-109.3 against 110.46: **level**,
      on the F746. The F446 has not been measured since.
- [~] **A memory access is a hundred bytes of emitted code.** Measured
      on Thumb-2 now, with `-DEMU_JIT_BYTE_STATS=ON`: 60.8 bytes a guest
      instruction on RV32 CoreMark, a load 92 and a store 105, together
      53% of everything emitted. **Short encodings took it to 44.6** --
      the emitters had always chosen the 32-bit MOV, CMP, LDR and STR --
      and CoreMark's working set then fits the F746's 96 KB: 1.71x, x650
      19.92 s to 11.65 s ([backend/thumb2.md](backend/thumb2.md#where-the-bytes-go)).
      **Then one compare for the window guard** (offset rotated right
      by log2(size), so bounds and alignment are one unsigned test):
      36.0 bytes a guest instruction, a load 54 and a store 59; x650
      15.70 → 14.29 at 96 KB and 135.82 → 117.63 at 32 KB, against the
      interpreter's 111.22 on the same tree. **Then one shared slow
      path** (`t2_mem_stub`, called through r11, which each block takes
      from a second argument the framework now passes): 31.1 bytes a
      guest instruction, a load 43 and a store 51; at 32 KB 117.31 →
      109.17 and, under a second layout, 115.06 → 109.30, translations
      256,320 → 239,432 either way -- level with the interpreter's
      110.46 on the same tree
      ([backend/thumb2.md](backend/thumb2.md#where-the-bytes-go)).
      Left: the host pointer, `MOVW`/`MOVT` per access, which cannot be
      shared because a window may change between translations without
      a flush; a literal per block would save 4 bytes an access.
- [x] **`RETIRE` was one operation per guest instruction** -- 11% of
      what the Thumb-2 backend emitted once the accesses had shrunk.
      `pass_merge_retire` keeps one per stretch between observation
      points, carrying the count; the observer list is now one
      predicate, `op_observes_state`, shared with `pass_dead_puts`.
      3,291 adds became 1,429 on CoreMark, 33.9 bytes a guest
      instruction; x650 14.29 → 13.94 at 96 KB, 117.63 → 115.24 at
      32 KB. Retired counts under the JIT are what the commit before
      gave, to the instruction.
- [x] **Retention ranked blocks by dispatcher lookups, and a chained
      block got none** -- and it cost: at compaction every block a
      linked exit leads to now inherits the count of the block exiting
      to it (`EMU_JIT_CHAIN_HEAT`, on). RV32 CoreMark on the F746 at
      96 KB: 29.85 → 27.80 cycles an instruction, 6,413 → 5,731
      translations, and 29.10 → 27.25 under a second layout. At 32 KB it
      costs 3%, compaction running 35% more often
      ([jit/tuning.md](jit/tuning.md#retention-and-chained-blocks)).
- [x] **The G4MH translator bound no context and no generation, and
      that was a permission bypass.** The interpreter checks MPU execute
      permission on every fetch; the translator checked nothing, so
      code the MPU refuses to user mode ran from a block translated in
      supervisor mode, or translated before its area lost the execute
      bit. Now: PSW.UM is the context, any MPU register write the
      generation, and the fetch is checked per instruction at
      translation. And exception entry did not clear PSW.UM -- only
      SYSCALL did -- so a user-mode fault ran its handler as user and,
      for a fetch fault, faulted again there for ever.
      `test_mpu_exec_permission_both_backends` catches each of the four
      pieces removed alone; its first version passed with the context
      removed, because the declined-pc cache answered for it
      ([jit/staleness.md](jit/staleness.md)).
- [x] **The ARMv7-M frontend has run on a board**: all eight guests on
      the F746 under both backends, every retired count equal to the
      host's ([frontend/armv7m.md](frontend/armv7m.md#firmware)). It
      took a NULL check the boards' clock hook had never needed, guest
      staging for the firmware, and a guest stack that was not the top
      of a host's megabyte. Left open from it: the interpreter is 2.7x
      RV32's cost per instruction there.
- [ ] **Board figures that predate `emu_cycles.h`.** Any of them from a
      run longer than 19.9 s at 216 MHz (23.9 s on the F446) is a whole
      wrap short, and the documents do not record run lengths. The ones
      in `jit/tuning.md`, `backend/thumb2.md` and the first section of
      `performance.md` are after the fix. The second half of
      `performance.md` and both `stm32f446/` pages are not, and are
      marked as history.
- [x] **ARMv7-M declined an IT block that held a memory access.**
      `-DEMU_PAIR_STATS=ON` over the x86-64 JIT: CoreMark interpreted
      0.1%, crypto nothing, **Dhrystone 6.4%** (579,310 of 9.06M), from
      25 IT blocks the translator declined (`declined IT:25`), because an
      instruction inside IT is lowered as "compute, then select against
      the old value" and an access cannot be done and then undone.

      The cheap half was measured first, as this entry said to: a
      literal load from flash is a constant and can select like any ALU
      result, and it took **none** of the 25 -- Dhrystone's hot blocks
      hold real accesses (`ittte eq; ldreq; moveq; strbeq; movne` in
      Func_1, `strls` and `ldreq` elsewhere). So the second half: a
      LOAD or STORE takes a **predicate** in its third operand
      (`emu_ir_mem_if`), and each backend branches round the whole
      access inside the one IR instruction, so no pass sees control
      flow ([jit/README.md](jit/README.md#the-passes)). xPSR carries the
      member's ITSTATE across the access, which is what a fault stacks
      ([frontend/armv7m.md](frontend/armv7m.md)).

      Dhrystone interprets 1,033 instructions instead of 579,310:
      x86-64 JIT 0.076 s → 0.050 s, and on the F746 at 32 KB **78.31 →
      40.17** host cycles an instruction, 3.20 s → 1.63 s, against a
      worktree at the commit before. CoreMark's 13,114 interpreted fell
      to 1,115 and its figure moved 234.82 → 238.19 (+1.4%), which is
      within what layout alone does on that part. `itblock` checks a
      skipped store, a skipped faulting `LDRD`/`LDM`, writeback both
      ways, and a fault inside `itee eq` returning with the else half
      still skipped; each of the two mechanisms was reverted alone and
      the test named it. Still declined: an IT block ending in a
      computed branch (`LDR pc`, `POP {pc}`, `BX`).
- [x] **G4MH's translator had no ADDI.** Opcode 0x30 fell to the
      `default` and declined, and `LD.HU disp16` was declined on purpose
      and never lowered: together 94% of what a G4MH DOOM run interpreted,
      in one rendering loop. Both lowered; interpreted 36.8M → 2.54M of
      300M, DOOM 3.35x faster on the x86-64 JIT
      ([frontend/g4mh.md](frontend/g4mh.md#what-the-translator-declined)).
      Found with `-DEMU_PAIR_STATS=ON`, once it counted every kind rather
      than adjacent pairs and stopped naming ZXB SATSUBR.
- [x] **G4MH had no guest-RAM fast path in the JIT**: every load and
      store was a helper call. It has the window the others have now,
      under the MPU off and with the reservation rules in
      [frontend/g4mh.md](frontend/g4mh.md#guest-ram-in-the-jit); G4MH
      DOOM 7% faster on the x86-64 JIT. And the Thumb-2 store guard was
      a word load of a one-byte flag, so whatever followed the flag in
      the struct could send every store to the slow path; it is `LDRB`.
- [x] **The IR backends inline the peripheral window again** (Thumb-2,
      RV32), armed after 64 passthrough accesses: `mmiobench` read
      2.47x, write 4.71x, rmw 6.96x, poll 5.47x, now 1.3-2.2x its RAM
      control ([performance.md](performance.md#driver-performance-the-passthrough-window)).
      G4MH, PowerPC and ARMv7-M do not opt in yet: each would set
      `has_pt` beside `has_fast` and arm in its `bind` as RV32 does --
      PowerPC only for a little-endian window it does not have.
      Before, measured on the F746
      (`mmiobench`, ticks per kernel, 91df012):

      | kernel | interpreter MMIO / RAM | JIT MMIO / RAM |
      |---|---|---|
      | read | 152,233 / 101,144 | 70,009 / 11,532 -- **6.1x** |
      | write | 94,330 / 70,624 | 42,092 / 6,801 -- **6.2x** |
      | rmw | 205,154 / 124,091 | 94,044 / 9,206 -- **10.2x** |
      | poll | 128,149 / 96,248 | 51,188 / 7,307 -- **7.0x** |

      The JIT was 2.2x the interpreter on peripheral access, but every
      one left the block through the checked accessor and a bus walk.
      The hand-written translator inlined the window and was
      2.2-3.1x better for it: the window is an identity map, so it is
      the guest-RAM fast path with host equal to base, plus a test of
      the read-only holes on a store (PWR, the RCC clock tree, the
      flash controller), armed only once a guest has made passthrough
      accesses, because always-on code grew CoreMark's image past the
      cache.
- [x] **Is `-ffp-contract=off` on `rv_fpu.c` still doing anything?
      No, and it is gone.** This entry said the file was "SoftFloat
      throughout", which was not quite true: it still held the
      host-float square root, in a `case 0xFF` no seven-bit funct7 can
      reach. With that removed, the object code is identical with and
      without the flag -- x86-64, x86-64 with `-mfma`, and the
      Cortex-M7 -- instruction for instruction, and with no fused
      multiply in any of them. The first attempt at that comparison
      hashed the objects CMake had built, which are LTO and contain no
      code: two architectures "matched".

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
- [x] **A host runner that accepts any frontend already supported.**
      Done: every `EMU_GUEST_ARCH_*` defaults ON for `EMU_PLATFORM=host`
      and OFF but RV32 for a firmware, so a fresh host build is the
      `host-all` configuration -- 56 ctests -- and the matrix rows that
      meant "this frontend alone" now say so. A flat image still runs as
      RV32 unless `--frontend` names another, rather than being refused:
      every RV32 flat test relies on it, and the runner's banner names
      the ISA it chose. Measuring what RV32 paid for the company found
      6.1% on its interpreter: the bus tested the guest's byte order on
      every fetch and RAM access whenever PowerPC was compiled in. The
      caches carry the order now, and PowerPC reaches its own directly:
      RV32 65.30M host instructions on CoreMark with all four against
      65.15M alone, PowerPC 1,462.8M to 1,381.0M. What the entry said
      before -- one `emu-host` that runs an RV32, G4MH, PowerPC or ARMv7-M guest
      without reconfiguring. The mechanism exists: the runner picks a
      frontend from the ELF's `e_machine` or from `--frontend NAME`, and
      the `host-all` row of `scripts/build-matrix.sh` builds all four and
      passes its 54 ctests. What is missing is that it is not what a
      plain build gives you: `EMU_GUEST_ARCH_G4MH`, `_PPC` and `_ARMV7M`
      default OFF (`CMakeLists.txt`), so `build/host` -- the tree
      `run-riscv-tests.sh`, `gdb-check.sh` and the docs' recipes default
      to -- accepts RV32 alone, and the other three are exercised only by
      configurations someone has to ask for. To do:
      - default every frontend ON for `EMU_PLATFORM=host`; the firmware
        keeps one, because there a frontend costs flash;
      - a flat `.bin` carries no architecture and runs as the first
        frontend compiled in -- which is how 20 tests failed when
        `host-all` was first built. With more than one compiled in,
        refuse a flat image without `--frontend` rather than guess;
      - check what grows with the set: `EMU_MAX_REGIONS` is computed per
        frontend (27 with all four), and the build matrix once kept a
        stale 16 in a cached row;
      - measure that RV32 pays nothing for the company -- CoreMark on
        both backends, interleaved, before and after, since layout alone
        moves a host figure by about 3%;
      - then the validation table, README and BUILD.md say one runner.
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

      **The prompt can be typed at now**, and two CMake targets stand
      the whole thing up:

          cmake -B build/host -DEMU_LINUX_ROOTFS=<rootfs.ext4>
          cmake --build build/host --target linux-shell

      `linux` is the same without a root filesystem, on the built-in
      initramfs.

      **Output had a fallback and input did not.** `board_console_putc`
      drops through to stdout when no pty is configured, so every guest
      in this tree printed; `board_console_getc` returned -1
      unconditionally. Every bare-metal guest here only ever prints,
      which is why nothing noticed for the life of the project -- and
      then a distribution booted to a login prompt that could not be
      answered. stdin is pumped into a ring from `board_poll`, once per
      slice rather than per LSR read: a guest polls that register far
      more often than it retires an instruction, and a `poll(2)` there
      would put a syscall on the hottest path a device has.

      **`USES_TERMINAL` is what makes the target work, not a tidiness
      flag.** Without it the build tool hands the command a pipe, raw
      mode is skipped for anything that is not a terminal, and the line
      arrives assembled and echoed instead of keystroke by keystroke.

      Ctrl-C ends the emulator rather than reaching the guest: ISIG is
      left on deliberately. That costs a guest shell its interrupt key,
      which is a real limitation; an emulator a person cannot escape
      from is a worse one. QEMU's answer is an escape sequence, which
      is a state machine rather than a flag.

      **The udev stall: fixed, and it was three things, two of them
      the JIT's.** This entry used to say the guest was "waiting on
      something that never completes rather than grinding -- which
      makes it a device question, not a JIT one". Half right. Boot to
      a login prompt is **109 s** now, from over 900 s (a 15-minute
      run did not reach it):

      | | 8192 blocks | 65536 blocks |
      |---|---|---|
      | FS out of the generation only | 202 s | 188 s |
      | ...and GET_ID answered | 202 s | **109 s** |

      - **The JIT flushed on every system call.** Linux clears
        `sstatus.FS` on each trap into the kernel and restores it at
        the return, and FS off-ness was in `rv_ir_gen_key` -- so every
        trap from a process that had touched a float threw the whole
        cache away. 21,949 of 22,126 flushes; a median of 438 resident
        blocks against ~7,000 while the kernel booted; guest 2.4 M
        instructions/s with 1,776 host instructions each (medians over
        the run, first and last lines dropped). FS and frm are part of a block's *identity*
        now, in a context widened to 64 bits. The telemetry showed it
        from the start -- `jit` (resident) collapsing while `blk`
        (translations) climbed -- once the columns were read the right
        way round.
      - **The block table filled first.** Blocks are keyed on the
        address space, so every process udev starts translates its
        code afresh. At 8192 blocks: 2,578 compactions, 13.7M blocks
        evicted, translation 81% of host time, with the 32 MiB code
        buffer a fifth used. At 65536 the buffer fills first, as it
        should.
      - **virtio-blk dropped GET_ID.** Reading
        `/sys/block/vda/serial` sends `VIRTIO_BLK_T_GET_ID`, and
        TinyEMU lets every type but IN and OUT fall through a
        `default: break;`. The worker blocked in the kernel and
        `udevadm settle` waited out its 120 s. One dropped request
        per boot, measured: type 8, 16 bytes out, 21 back. Its
        signature in the console log is udevd's own, 180 s after the
        worker started:

            [   81.562662] udevd[81]: worker [88] .../block/vda is taking a long time
            [  199.198842] udevd[81]: worker [88] .../block/vda timeout
            [  199.217723] udevd[81]: seq 1280 '.../block/vda' killed

        That is the control -- every fix but this one. With it, none of
        the three lines appears and the root filesystem is remounted at
        guest 65.8 s. The `killed` at guest 1153 s or 1276 s in slower
        boots is the same event, dispatched late because udev itself
        was crawling.

      **The last two hid each other**, which is why each looked
      worthless alone. With the request dropped, udev ends when the
      settle timeout does however fast the CPU work runs; with 8192
      blocks the CPU work takes about as long as the timeout. Both
      single fixes measure 202 s; both together, 109 s. An A/B that
      shows nothing is a result about the workload -- here, about a
      second bottleneck the same size as the first. What separated
      them was the telemetry's guest rate: **executing at 4.8 M/s and
      idle at 0.1 M/s are different stalls**, and one boot had both.

      Still costing time, measured and not yet done:

      - **FENCE.I flushes the whole cache**: ~3,770 a boot, nearly
        all before udev. A store into a page holding translated code
        is what it has to catch; tracking that per page would make it
        local.
      - **Every process translates its own copy of libc.** The key is
        satp, so identical physical pages in two address spaces are
        two sets of blocks. Keying on the physical page would share
        them, at the cost of a translation on every lookup.

      Found on the way, and **fixed**: a trap taken inside a block left
      `jit_ctx` stale. `rv_ir_load` raises a fault from inside the
      block and the interrupt hook raises one between blocks, and the
      context -- which carries the privilege -- was refreshed only on
      the interpreter fallback. So the dispatch looked the handler up
      under the context it had trapped *from*, and filed the handler's
      first block there. `rv_hart_trap` refreshes it now, beside the PMP
      and MMU refreshes it already did. isatest's `jit-ctx-fetch-tval`
      shows the consequence: S-mode jumps to a machine-mode trap stub on
      a page PMP denies it, and with the fix reverted the fetch fault
      arrives at stub+8 -- S-mode had run the stub's first block -- where
      it must arrive at stub+0. Interpreter unaffected, as it should be.

      **The device tree understates the machine, and the cost is
      coverage.** The guest reports

          isa : rv32imafc_zicntr_zicsr_zifencei_zca_zcf
          mmu : sv32

      while the emulator's own banner says
      `RV32IMAFDCB_zicsr_zifencei_zicntr_zbc`: `boot/rv32-emu.dts`
      declares `riscv,isa = "rv32imafc_zicsr_zifencei"` and omits B and
      D. Nothing is wrong at run time -- the kernel is being
      conservative about instructions it is told do not exist -- but it
      means **no Linux guest can reach the Zba/Zbb/Zbc/Zbs lowerings in
      either backend**, so the largest guest in the tree gives those
      paths no coverage at all. This is the "two descriptions of one
      machine, and disagreeing is silent" rule in its quiet direction:
      the disagreement costs testing rather than correctness, so
      nothing will ever report it. **Resolved in 6cd92f5:** the device
      tree declares all of it, and the kernel executes Zbb -- counted
      below, and translated since the B lowering.

      **The banner itself was understating the core, and finding that
      took a third description to notice.** It read `RV32IMAFCB_zbc`:
      no D, though `rv_fpu.c` implements it; and no Zicsr, Zifencei or
      Zicntr, though all three are implemented and the 20191213 spec
      requires them to be *named* -- a string without Zicsr describes a
      core that cannot execute `csrr`. CMake's configure summary had
      been printing `_zicsr_zicntr_zifencei` and `D` all along, so the
      two descriptions this tree generates disagreed with each other
      and the one a user sees at run time was the wrong one. Both then
      read `RV32IMAFDCB_zicsr_zifencei_zicntr_zbc` -- which still named
      three multi-letter extensions out of a dozen implemented. They
      name all of them now, and the `rv32-isa-string` test fails if the
      two differ. The device tree is the third description and nothing
      holds it to the other two.

      `mmu : sv32` is the kernel's own confirmation of the paging work.

      **Whether the Zbb alternatives actually patch: they do.** This
      was open for as long as the device tree has declared B, because a
      boot that never executes a Zbb instruction cannot fail on one and
      nothing counted them. A `-DEMU_PAIR_STATS=ON` runner does now:
      **331,550 Zbb instructions in one boot**, with 36,225 `cbo.zero`,
      4,096 cache-block maintenance operations and 3,270 `clmul`. That
      was counted under `--jit`, which then gave every one of them in
      twenty seconds because the translator declined all of Zbb but
      `rori`. **It does not any more**, so the same run now reports no
      B at all: the kernel's Zbb executes translated. Counting it again
      means `LINUX_JIT=0`, which takes several minutes to userspace.

      Still open:

      - **benchmarks**, still not run under Linux;
      - **the run ends in a kernel panic**, because the test init exits
        and there is nothing in the device tree to power off through.
        It is read as the terminator, and it means every run goes to
        the instruction cap or a timeout to print its figures.

      Broken into the order the pieces actually unblock each other,
      because most of them are only testable once the one above works:
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

        **Re-measured after the fast path and chaining, and the JIT is
        still behind.** Same image, same 2e9 instruction cap both ways
        so the guest work is identical, two interleaved rounds:

        | | round 1 | round 2 |
        |---|---|---|
        | `LINUX_JIT=1` | 190 s | 189 s |
        | `LINUX_JIT=0` | 177 s | 175 s |

        About 8% *slower* translated. The headline above therefore still
        holds, and it is worth saying plainly that none of the work in
        the entries below moved it: Dhrystone went 2.0x to 5.0x over the
        same period and the Linux boot did not follow, because what
        costs here is not what costs there.

        **What costs here, from the same run's counters:**

            flushes  129285
            interp   638699763 instructions fell back
            declined 636822916
            blocks   1764 resident, 957612 optimised

        **636.8M declined translations against 638.7M interpreted
        instructions is 99.7%**: essentially every instruction the
        interpreter runs was preceded by a translation attempt that
        produced nothing. The note further down guessed "~230M attempts
        for 951k successes"; at this budget it is 637M, and it is the
        largest single term in the run.

        **Built and measured.** A translator that declines at a pc
        declines there again, so the refusals are remembered, keyed on
        (pc, context) and invalidated by an epoch the flush bumps:

        |  | with | without |
        |---|---|---|
        | wall, 2e9 cap | **148 / 146 s** | 210 / 207 s |
        | declined | 258,596 | 624,309,893 |
        | cached | 668,408,667 | -- |
        | translations | 1,025,340 | 1,024,706 |

        **1.42x**, two interleaved rounds, both binaries against the
        same kernel. Translations unchanged across 134,860 flushes is
        what says nothing legitimate was suppressed; arch-test is
        378/378 with `--jit` and riscv-tests 77/77.

        So the JIT is no longer behind the interpreter here -- 148s
        against the 175/177s an interpreted boot took, though note that
        comparison crosses a kernel change and has not been re-run.

        **Then page-granular SFENCE.VMA, and then `time`.** The first
        collapsed flushes 134,857 to 2,968 and translations 1,023,251 to
        174,903 -- and moved the clock 2%, which is what said
        translation was never the cost. The second is where it was:

        | | wall | interp | block entries |
        |---|---|---|---|
        | before | 210 s | 626.4M | 641.7M |
        | negative cache | 148 s | 673.7M | 682.0M |
        | + page-granular | 137 s | 674.6M | 685.8M |
        | + `time` lowered | 80 s | 358.1M | 366.6M |
        | + `div` lowered | **38 s** | — | — |

        **210 s to 38 s, 5.5x**, and each step was found by asking the
        same question and getting a different answer:

        `rdtime` was **335,195,899** of the boot's 2e9 instructions,
        polled in kernel delay loops; `DIV` was **356,462,035** --
        99.4% of everything still interpreted after `time` -- declined
        because open-coding it means reproducing RISC-V's divide-by-zero
        and overflow rules per host. That is an argument against
        open-coding and none at all against a call.

        **Native divide was then built, measured, and does not pay**:
        39 s against the helper's 38 s, two interleaved rounds. A
        hardware `idiv` is 20-40 cycles, so the call was never the
        dominant term, while the normalisation adds ~8 IR instructions
        per divide. It is kept -- correct, suite-clean, and the balance
        differs on Thumb-2 where an M7 `SDIV` is 2-12 cycles against a
        call with register save/restore -- but **that is unmeasured**,
        and the x86 figure is the only one in hand. See
        `EMU_IR_DIVS` in emu_ir.h for the precondition it carries.

        What is left: **`interp` is still 358M of 2e9 after `time`, and
        the divide work removed most of the rest.** The question to ask
        next is the same one, a third time -- *which instruction, not
        which class* -- against a current boot, because the last two
        answers were each a single instruction and neither was
        guessable from the class histogram.

        **SFENCE.VMA flushes the whole code cache** -- 129,286 times in
        a boot, and **99.994% of those requests were not global**:

            SFENCE   global 8   page 131890   asid 2

        Eight whole-address-space flushes in an entire boot, against
        131,890 carrying a virtual address in rs1. Every one of the
        131,890 discards all 1,750 resident blocks, which is what makes
        the run translate 1,026,399 times -- roughly 585 retranslations
        per block. So the operands are not a detail to be conservative
        about here; they are almost the whole population.

        The design that follows, and the thing that makes it feasible:
        a chained exit is patched to `chain_entry`, which is *past* the
        prologue and shares the caller's frame, so an invalidated block
        can be made harmless by writing a jump from its own
        `chain_entry` to its own `tail`. Every inbound link then falls
        into the dispatcher and the pc is retranslated -- **no reverse
        index from target back to the blocks that jump into it**, which
        is the part that would otherwise make this cost more than it
        saves: `unlink_all` is O(blocks x links) and would run 131,890
        times.

        What it needs that does not exist: a hook that *writes* an
        unconditional jump. Both `patch_link` implementations only
        rewrite the displacement of a branch already there
        (`t2_patch_branch`, `x86_patch_rel32`), which is the wrong
        primitive for turning ordinary code into a jump. An A/B with the invalidation simply removed (incorrect,
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

        **A minimum block length was tried and is much worse.**
        Refusing to keep a block below N instructions, so those go to
        the (now efficient) fallback instead of paying a dispatch:

        | minimum | Dhrystone | block entries |
        |---|---|---|
        | **1 (kept)** | **0.044 s** | 320,735 |
        | 2 | 0.071 s | 300,725 |
        | 3 | 0.138 s | 280,713 |
        | 4 | 0.305 s | 320,639 |
        | 6 | 0.643 s | 160,426 |

        Monotonic, and by a lot. The reasoning behind it -- "entering a
        block to run one no-op costs 80 cycles and is a loss" --
        compared a short block against *free* rather than against what
        actually happens instead, which is interpreting those
        instructions. Even a one-instruction block beats the
        interpreter by enough to pay for its own dispatch. Entries fall
        as intended and the clock rises anyway, which is the shape of
        an optimisation measuring the wrong thing.

        Also worth knowing: the JIT attempts a full translation before
        almost every interpreted instruction and gets nothing back,
        which is ~230M attempts for 951k successes at a 600M budget --
        counted since at a 2e9 budget as **636.8M declined against
        638.7M interpreted, 99.7%**, and it is the largest term in the
        run. See the re-measurement above.

        Guests that never page are unaffected and still twice the
        interpreter: bench 0.028 s against 0.057, dhrystone 0.104
        against 0.203.

        **What made any of this checkable is `EMU_EXTRA_ARGS` on
        run-arch-test.sh.** Nothing validated a translating backend
        under the privileged features before -- the suite runs the
        interpreter, and the JIT passed the Sv32 and PMP tests by
        declining to execute them. It is 378/378 both ways, and with
        `--jit` those tests now translate for the first time.

        **The inlined memory path is off under every operating system,
        and for two reasons rather than one.** `rv_ir_fast_mem`
        declines on `vm_active`, `pmp_active` or `trig_active`, and
        `rv_pmp_refresh` sets `pmp_active` **from the privilege alone**
        -- before it looks at a single entry:

            h->pmp_active = false;
            if (h->priv != RV_PRIV_M || rv_hart_data_priv(h) != RV_PRIV_M) {
                h->pmp_active = true;

        So a kernel gets no inlined access even with no PMP region
        configured, and "add an inline TLB probe" -- which is how this
        item was written -- would leave the second gate shut and change
        nothing. Anything built here has to answer both questions
        inline: virtual to physical, *and* whether the access is
        permitted.

        **What it is worth, measured.** `tests/guest/sv32bench.c` is
        one source built twice, differing in one `-D`: the same
        memory-bound kernel in M-mode under Bare, and in S-mode under
        an identity-mapped Sv32 with a background PMP entry. Retired
        counts agree exactly (8,579,610), block entries agree to 0.2%,
        so the difference is the per-access helper call and not
        dispatch:

        | | interpreter | JIT | JIT speedup |
        |---|---|---|---|
        | bare, fast path on | 129 ms | **21 ms** | 6.1x |
        | paged, fast path off | 351 ms | **85 ms** | 4.1x |

        Three interleaved rounds, 21/21/22 against 84/85/85 -- far
        outside the +/-3% this host is noisy by.

        **4.0x is the ceiling, and the realistic prize is smaller.**
        The interpreter pays 2.7x for the same move (129 to 351) while
        inlining nothing either way, so a large part of that 4.0x is
        address translation and permission work that an inline probe
        still has to do. Do not quote 4x as what a TLB probe would
        recover; it is what the *call* plus the walk costs together.

        The pieces exist: a 32-entry direct-mapped TLB tagged by full
        VPN with the PTE bits stored, and `rv_pmp_simple`, which is
        already written to report a single entry's bounds "so a caller
        can inline a range test instead of calling the full check".
        What is missing is IR surface to express a probe-with-fallback,
        and on Thumb-2 the code-size question is real -- the
        passthrough test alone is ~18 bytes a load and ~48 a store, and
        always-on grew CoreMark past a 12 KB code cache.

        **And this configuration now has a test, which it did not.**
        isatest arms PMP early and from then on everything interprets,
        so the JIT's behaviour under Sv32 and PMP was covered by
        nothing. `guest-sv32bench` and `guest-sv32bench-jit` run the
        paged guest on both backends and assert `SV32BENCH-END`, the
        line only a completed run prints. Confirmed by breaking it:
        both fail with the trap reported.

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

        **Seen firing under Linux now**, which is what was missing: the
        unit test only proved delivery moves between privileges, and
        nothing had driven a real queue completion through the whole
        path. `/proc/interrupts`, from a distribution at its shell:

            10:   554825  RISC-V INTC   5 Edge    riscv-timer
            12:      124  APLIC-DIRECT 10 Level   ttyS0
            13:        0  APLIC-DIRECT  3 Level   virtio2
            14:      824  APLIC-DIRECT  1 Level   virtio0
            15:        0  APLIC-DIRECT  2 Level   virtio1

        824 completions on virtio-blk, delivered to S-mode. The
        controller reports **APLIC-DIRECT**, so the direct-mode wiring
        above is what the kernel actually bound to rather than what the
        device tree merely offered. The two zeros are right: nothing
        used the console or sent a frame at the loopback net device.

        The 124 on ttyS0 are a second thing confirmed for free -- the
        NS16550 receive path is **interrupt-driven end to end**, not
        polled. Those are keystrokes typed from the host reaching a
        getty through `emu_uart_poll`'s line.
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
      covers temps rather than guest registers.

      **Measured, and the answer is no.** `EMU_JIT_HOT_REG_STATS` was
      an option nothing read -- declared in CMakeLists, defaulted in
      `rv_config.h`, consulted by no line of code -- so the histogram
      it names had never existed. It does now, in `emu_regstats.c`,
      counting what a block leaves behind *after* the optimiser:

      | | Dhrystone | CoreMark | bench |
      |---|---|---|---|
      | refs to one register in one block: 1 | 67.5% | 67.6% | 54.0% |
      | 2 | 21.1% | 25.0% | 26.9% |
      | **3 or more** | **11.5%** | **7.4%** | **19.1%** |
      | surviving reads | 594 | 1122 | 451 |
      | surviving writes | 1300 | 1678 | 910 |
      | distinct registers per block | 5.79 | 5.61 | 12.27 |
      | busiest register's share | 12.3% | 14.8% | 9.2% |

      Pinning costs a load at block entry and a store at exit, so it
      breaks even at two references and only pays above them: **7-19%
      of the traffic can pay for itself and the rest is a loss.**

      **And what survives is writes, by 1.5 to 2.2 to one** -- which is
      exactly the traffic a register cache cannot help. This is the
      cost model the Thumb-2 attempt lacked, and it explains the 15.5%
      rather than contradicting it: a cached read is a MOV where an
      uncached one is an LDR, one instruction either way, while a
      write must still be written through. `pass_reg_traffic` has
      already taken the redundant reads; the residue is stores that a
      short block has to make before it exits.

      There is also no small hot set to choose: the busiest register is
      9-15% and the top eight reach 46-69%, so a three-register pin
      covers about a third of a third.

      **The histogram is weighted per translation, not per block
      entry**, and that limit is stated in the header. It does not
      weaken this conclusion -- benefit and cost both scale with
      entries, so a register that cannot pay in a block cannot pay in a
      hot one -- but it would have to be fixed before any *positive*
      result was believed.

      Two things about building it are worth keeping.

      **The first version reported RV32's `x0` as the busiest register
      in every guest**, at 19-24% of all references. A GET of x0
      survives the passes deliberately and every backend answers it
      with a constant at lowering, so it is not traffic at all -- the
      instrument was counting a quarter of its own histogram into a
      bucket that cannot be optimised, and would have made a pinned x0
      the obvious first choice. `emu_reg_note_block` takes the target
      and skips hardwired-zero registers now.

      **Its `#include` landed inside `#if EMU_PAIR_STATS`**, an
      unrelated option, so the declaration was absent in exactly the
      configuration the header was written for and the build failed on
      an implicit declaration. A conditional include belongs to its own
      condition.

      So: **do not build a guest-register allocator.** The lever this
      entry was looking for is longer blocks, which is what this file
      already says two entries down.

      The original reasoning below was wrong in an instructive way, and
      is kept for that:

- [x] ~~**Find out what the 50x is.**~~ (The first answer, kept under
      the one above that replaced it.) The runner now reports host
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
      | ppc  | yes | **yes, as Book E** | `powerpc-linux-gnu-gcc` is not built with VLE (`-mvle` is rejected), but the core runs classic Book E too and GCC generates that: CoreMark and crypto run on both backends. What is missing for a game is a 32-bit `libgcc` and a C library for that build |

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
  - [ ] Doom on ppc. It has a JIT now and runs compiled C; what stops
        it is that the Book E guests are `-nostdlib` with no 32-bit
        `libgcc` in this toolchain (the same reason Dhrystone is not
        built), and that the image lives in guest RAM rather than
        executing in place.
- [~] **PowerPC debug infrastructure.** It was three files where the
      other frontends had fifteen, and every PowerPC defect was found by
      bisecting by hand with external `objdump`.
  - [x] `ppc_decode.c` — one decoder, 313 mnemonics to 125 semantic
        classes, consumed by the interpreter, the translator and the
        disassembler. Checked against binutils over four million
        encodings, in both directions.
  - [x] `ppc_disasm.c` — written against the assembler, as this entry
        asked: it is tested by assembling what it prints.
  - [x] The embedded FP unit, against exact rational arithmetic, and an
        IR translator — neither was on this list, and both were what
        "finish" turned out to mean. See [frontend/ppc.md](frontend/ppc.md).
  - [x] `ppc_gdb.c` — the register layout gdb expects, which is a fixed
        per-architecture order and not a choice. Written with the
        ARMv7-M one; `scripts/gdb-check.sh` attaches gdb-multiarch to it.
  - [x] `ppc_pairstats.c` — the histogram that answers "which
        instruction next" with a measurement instead of an opinion.
        Its first run named what the JIT still interprets: two
        instructions, `mtspr` and `rfi`, on CoreMark and crypto, and
        everything after the TLB is programmed on `ppc-mmu`
        ([frontend/ppc.md](frontend/ppc.md#what-it-still-interprets)).
        No matrix row had ever built with `EMU_PAIR_STATS=ON`;
        `host-pairs` does now, for all four frontends' tables.
  - [x] **An MMU.** Chapter 10, with a guest that programs the TLB as
        MPC57xx start-up code does; see the entry below.
  - [ ] **Anything to compare it with.** No e200 is attached and no
        other emulator is consulted, so a shared misreading of the
        manual is invisible to every check there is.
- [~] **An ARMv7-M frontend, so the Thumb-2 backend is testable without
      hardware.** Built, and the thing it was for works: the real
      `src/backend/thumb2/encode.c` compiles into a Cortex-M guest,
      runs on this frontend on an x86 host, emits instructions into
      guest RAM and **branches to them** --
      `tests/guest/armv7m/t2exec.c`, asserting `T2EXEC-OK`.

      That closes a gap this file has complained about since the
      backend existed. `scripts/t2-check-encodings.sh` compares emitted
      bytes against `arm-none-eabi-as`, which catches a wrong
      *encoding* and cannot catch a wrong *choice* of encoding -- an
      emitter that assembles perfectly and is the wrong instruction for
      its operands. Confirmed against the two historical defects by
      reintroducing them: removing the shift-by-zero rewrite gives
      `FAIL lsr-0-is-a-move`, and reversing the imm3:imm2 split kills
      the guest. The first of those originally cost a Nucleo, a flash
      cycle and two architecture tests.

      Named `armv7m`, not `thumb2`: `src/backend/thumb2/` owns every
      `t2_` symbol and those are an *encoder*. This is a decoder.

      **What it decodes**: the 16-bit integer set, the 32-bit
      encodings a compiler actually emits (chosen by compiling C and
      reading the disassembly rather than by working down the manual),
      IT blocks, SysTick, the NVIC, and exception entry and return.
      Guests in `tests/guest/armv7m/`: hello, alu, itblock, nvic,
      t2exec -- 6/6 in an `EMU_GUEST_ARCH_ARMV7M=ON` tree.

      **Since then, everything that was listed as missing** -- the
      FPU, the MPU, the process stack, BASEPRI, FAULTMASK, escalation,
      real priorities and the whole 32-bit space -- plus a decoder, a
      disassembler, pair statistics and an IR translator, all checked
      against a Cortex-M7 board rather than a reading of the manual.
      Scope, the board-differential suites and what they corrected are
      in [frontend/armv7m.md](frontend/armv7m.md).

      Still open:

      - **The whole F746 firmware as a guest**, which was the original
        phrasing and is a much larger job than the encoder. It is
        linked at 0x08000000 with ITCM at 0 and drives **366 distinct
        STM32 peripheral registers** -- RCC, PWR, the flash controller,
        GPIO, USART, DWT -- so it needs an STM32F746 machine model.
        Worth deciding deliberately: this tree's whole design is that
        peripheral drivers live in the *guest* and reach real silicon
        through passthrough, so adding peripheral *models* is a
        direction change rather than an increment.
      - **The IR and the register allocator**, which t2exec does not
        reach: it tests the encoder end to end and nothing above it.
        Running `emu_ir_lower` in the guest is the next layer; the
        32-bit space it needed is there now.
      - **The JIT's own open items**: IT blocks ending in a computed
        branch are given back to the interpreter (those holding a load
        or store are lowered now, which was 6% of Dhrystone), RAM below
        0x10000000 gets no inlined access, and interrupt latency is a
        block. See frontend/armv7m.md.
      - ~~**No gdb target.**~~ Done, with PowerPC's, and a real gdb
        checks all four frontends (`scripts/gdb-check.sh`, the
        `gdb-check` ctest). Writing it found the stub encoding
        registers little-endian for a big-endian guest, the stop reply
        naming the wrong register for every target but RV32, and every
        ARMv7-M ELF booting from a garbage address. Still not
        described: the FP registers and MSP/PSP ([gdb.md](gdb.md)).
        The real-silicon comparison it makes possible -- the same gdb
        against the M7 and against the emulator -- has not been run.
      - **The board's answers live in `build/`**, not the repository.
        Kept as fixtures, `--rerun` would be a regression suite anyone
        could run without the board; that is a decision about 3 MB of
        test data, not a technical one.

      **Five defects, every one found by running a program and none by
      a unit test**, which is this file's most-repeated lesson arriving
      again. Four were the same class -- a field read too few bits wide,
      aliasing one group of instructions onto another:

      | | read | needs | symptom |
      |---|---|---|---|
      | shift/add/sub group | 2 bits | 3 | `movs r0,#16` ran as `lsls r0,r2,#0` |
      | modified immediate | bits 10:9 | bit 9 | rejected every immediate with `i` set |
      | extend group | 6 bits | 8 | swallowed **CBZ**; a branch became a zero-extend |
      | MOVW/MOVT | masked 0x7B vs 0x24 | — | provably constant; neither ever decoded |

      The fifth is the one worth keeping separately: the whole
      0xE8xx-0xEFxx range sat behind `op1 == 3` and is `op1 == 1`, so
      `add.w r5, r5, r5` reported as unimplemented while its handler
      was forty lines below in a block the decoder could not enter. Two
      more of the same shape followed -- the barriers checked *after*
      the branch test that swallows them, and a dead duplicate of
      MSR/MRS. **An unreachable handler reads exactly like a missing
      one**, and only the fault report tells them apart.

      Only MOVW/MOVT was caught by the compiler
      (`-Wtautological-compare`). The other four needed a guest.

      And the instruments were wrong more often than the code:

      - an A/B harness that grepped the guest for `FAIL` scored a
        *crashed* guest as a pass, so three real catches read as
        misses. Detection is `ctest` now, which asserts a line only a
        completed run prints.
      - the exception-frame check was inline asm and passed with the
        restore of r0 and of r12 broken, because gcc needs registers
        for the operands and chose the ones under test. It is
        `frame.S` now, where nothing is allocated, and it checks `sp`
        too -- the 8-byte alignment bit is otherwise invisible until a
        handler takes a *second* exception.
      - three tests used the one input where correct and broken agree:
        `ITTTT EQ` cannot see an ITAdvance that shifts the whole byte
        (EQ is condition 0b0000, so the top three bits are already
        zero), and an IT block with a *true* condition cannot see a
        handler that inherits ITSTATE. The awkward input is the whole
        test.

      The question the frontend was also meant to answer -- whether a
      Thumb-2 guest on a Thumb-2 host could copy a block rather than
      translate it -- is untouched and still a measurement rather than
      a plan. The register file is in memory and the guest's pc is not
      the host's, so "copy it" is unlikely to survive contact.

- [x] **rv32: Zihpm, Zihintntl, Zihintpause, Zicond, Zawrs, Zacas,
      Zalasr.** All seven, in the four places this entry said each had
      to land -- and the entry was wrong about the starting point: it
      called Zacas "written and disabled" with `amocas.d` *wrong*,
      under two comments in `rv_config.h` saying the same, above a
      define that had been 1 and passing both architecture tests since
      the Sail configuration was corrected. A claim about a default is
      checked by reading the default.

      | | interpreter | JIT | UDB + Sail | suite |
      |---|---|---|---|---|
      | Zicond | one line | compare, negate, AND; branch-free on both hosts | both | arch-test, 2 |
      | Zihintpause | nothing to add | lowered with FENCE, as nothing | both | arch-test, 1 |
      | Zihintntl | nothing to add | an ADD into x0 | both | arch-test, 4 + 4 compressed |
      | Zihpm | 29 real counters, events are traps | CSR access declines, as all of it does | both, with `HPM_EVENTS` | arch-test, 2 |
      | Zacas | was already there | helper | both | arch-test, 2 |
      | Zalasr | `rv_hart_atomic` | the same function, as a helper | UDB; **not in Sail 0.13.1** | none |
      | Zawrs | parks as WFI while reserved | SYSTEM declines | both | none; Sail probed directly |

      At that point arch-test was 391/391 on both backends (378 + 13),
      riscv-tests 77/77 on both, and `isatest` 454 checks on both and
      on the F746 under the Thumb-2 JIT. The next entry is why none of
      those three numbers lasted the day. The ISA string names all of them
      and a test holds the configure summary to it. Details in
      [frontend/rv32.md](frontend/rv32.md).

      What doing it found, none of which was in the seven:

      - **`rv_csr_read` had no `default:` without PMP.** The arm was
        inside `#if RV_EXT_PMP` because the PMP address registers were
        the only range it recognised, so a `-DRV32_EXT_PMP=OFF` build
        returned *success* for a CSR that does not exist, with the
        value whatever the caller's stack held. `test_csr_absent`: 4
        failures with the guard put back, in a PMP-off tree.
      - **Every RV32 run was reported as a fault.** `emu_cpu_status_t`
        gained `faulted`, the callers pass a stack variable, and RV32
        and G4MH never cleared it: "core 0 stopped on an instruction it
        could not execute", at a pc that changed from run to run, under
        tests that look only for PASS. Cleared in `emu_core_status`
        now; `test_cpu.c`.
      - **The disassembler ran every long mnemonic into its operands**
        -- `amoadd.wa0, a2, (a1)` -- had `clmul` at a reserved funct3,
        and printed any funct7 it did not know as the base operation,
        so `bclr` read as `sll`. `test_disasm` checks 71 words against
        what binutils calls them.
      - **Sail's counter-enable masks said `0x0`** under "No S or U
        mode", for as long as the emulator has had both. Nothing read
        the registers back.

      And three about the testing itself:

      - **The first mutation run was worthless from the fifth mutant
        on.** The driver restored each file with `shutil.copy2`, which
        preserves the modification time -- so the restored source was
        *older* than the object built from the mutant, `make` rebuilt
        nothing, and every later mutant inherited the earlier ones. It
        showed as an interpreter-only test failing under a
        translator-only mutation. 38 mutants, all detected once it was
        fixed; the driver now asserts the binary was relinked.
      - **A check named for a bug passed against it.**
        `hpm-write-high-keeps-low` wrote the high half while the low
        half was zero, and zero is what a write that clears its
        neighbour leaves. Found by the mutant that should have failed
        it failing two other checks instead.
      - **The counters were first put beside the other counters**,
        which moved every field after them by 352 bytes. Moved to the
        end of the struct before anything was measured against it.

- [x] **rv32: fifteen arch-test suites nobody had named, and thirteen
      defects in them.** ACT builds a suite only if its *directory* is
      named, and the default list in `scripts/run-arch-test.sh` named
      thirty. Listing the checkout to find `ZihintntlZca` turned up
      fifteen more this core is eligible for: `Zcf`, `Zcd`, `ZcbM`,
      `ZcbZbb`, seven PMP directories, `SvZicbo`, `SvPMPZicbo` and two
      `ExceptionsSv*`. Offered, they built **99 tests that had never
      run. Thirteen failed, and all thirteen were the emulator.**

      | defect | tests |
      |---|---|
      | **the JIT's dead-value pass deleted a load nothing read** -- no fault, and on a device no read | `pmpsm_cfg_A_tor_zero`, `--jit` only |
      | `pmpaddr` stored 32 bits where a 32-bit bus allows 30 | `pmpsm_grain`, `pmpsm_grain_check`, `pmpsm_csr_walk-1`, `-5` |
      | `fld`/`fsd` at 4 mod 8 never raised address-misaligned | `pmpf_cfg_wr`, `pmpzcd_legal_lxwr` |
      | a failing `sc.w` never asked whether memory was there | `sv32_exceptions_Zalrsc_{M,S,U}mode` |
      | cbo.clean/flush/inval took a virtual address to the bus | `sv32_zicbom_exceptions_{S,U}mode` |
      | ...and were not checked against PMP | `pmpzicbo_cbo_wr_01` |

      And three more found on the way that no architecture test holds
      at all: `menvcfg`/`senvcfg` were consulted by nothing, so every
      cache-block operation ran at every privilege; FIOM was writable
      and CBIE held its reserved value, against this core's own
      description; and the PMP check matched an access by its first
      byte, which a 32-byte block can straddle. `isatest` holds those.

      **490/490 on both backends** now, all fifteen in the default
      list, and `isatest` is 491 checks -- on the F746 too, where the
      first row was reproduced with the fix reverted: the APLIC claim
      was not acknowledged. Which test holds which fix was measured by
      reverting each alone; the table is in CLAUDE.md.

      What is left of it:

      - [x] **Zicbop**, which the script's comment had as "not
            claimed": claimed. Six tests, not three -- declaring it to
            UDB released three more that were already in `SvZicbo` and
            `PMPZicbo`, named and silently filtered.
      - [x] **`fld`/`fsd` were still two word accesses**, and the same
            mistake was live at another width: a misaligned word across
            a page boundary was translated at its first byte and taken
            physically contiguous. Both are one checked access now, as
            is `cbo.zero`. See [frontend/rv32.md](frontend/rv32.md).
      - [x] **The four `Misalign*` suites.** `run-arch-test.sh
            --misaligned` builds the default `RV32_MISALIGNED=ON`
            configuration and runs it against a description derived
            from the ordinary one: **509/509 on both backends**, and the
            thirteen Misalign tests fail all thirteen against the OFF
            build. See [validation.md](validation.md).
      - [x] **`ctest`'s `arch-test-I` was misnamed.** The runner
            executed every ELF in the work directory whatever
            `--extensions` named -- 39 from a fresh checkout, 496 after
            one full run. It now runs a directory of links to the named
            suites' ELFs alone, says how many the work directory holds
            besides, warns on a named suite that built nothing, and
            fails when none of them did. `--extensions I` is 39 with
            496 on disk.

- [ ] **rv32: Zalasr and Zawrs have no second opinion.** No arch-test
      suite for either, and no Sail model for Zalasr at 0.13.1.
      `isatest` covers both on both backends, with every check
      confirmed by a mutant, and the Linux test init executes both
      from user mode -- which makes three consistent readings of the
      specification and not a checked one. Revisit when the Sail pin
      moves: Zalasr is in later models.

- [x] **rv32: `-DRV32_EXT_PMP=OFF` is a configuration the matrix
      checks** (`host-nopmp`). `isatest` and `sv32bench` hung there:
      `test_jit_trap_context` relied on a PMP fetch fault to leave
      S-mode, and both wrote PMP CSRs that do not exist. The guests now
      take the option as they take `RV32_MISALIGNED`; without PMP,
      `isatest` drops what needs it and instead checks that reading and
      writing a PMP CSR is an illegal instruction -- 711 checks on both
      backends, against 748.

- [x] **rv32: the new extensions and the rest of the tree.** Both
      halves measured.

      *In a guest's `-march`*: only Zicond changes what the compiler
      emits, and it emits little -- four `czero` in CoreMark, executed
      15 times in 518,236 instructions; none in Dhrystone or the crypto
      guest; one in `bench`, which retires 2.6% fewer instructions and
      runs 20% slower under the JIT for a reason that is not Zicond's
      (the B-extension entry at the top of this file). The march stays
      as it is.

      *Under Linux*: `boot/rv32-emu.dts` declares them, the kernel
      boots, and the test init passes 17 checks from user mode. The
      kernel itself executes Zbb, `cbo.zero`, the cache-block
      maintenance operations and `clmul`, counted; it never executes
      `wrs.nto`, because one hart has nobody to wait for.

- [ ] **rv32: what the RVA profiles would still need.** RVI20U32 is
      met. The RVA families are 64-bit only, so there is nothing to
      claim, but read for RV32 the RVA22 mandatory list is short by
      two things now ([frontend/rv32.md](frontend/rv32.md#profiles)):
      **Zic64b** (a 64-byte cache block; `RV_CACHE_BLOCK_SIZE` is 32 to
      match the M7's line, and the device tree and both suite configs
      would follow it) and **Zkt**, which is a claim about timing nobody
      has examined -- the interpreter's `clmul` loops over set bits.
      **Zfhmin and Svinval are done**: 14 more architecture tests, 510
      and 523 (`--misaligned`) on both backends, and `isatest` holding
      FCVT.D.H and FCVT.H.D, which no suite test covers. FP loads and
      stores the translator does not lower -- FLD, FSD, FLH, FSH -- go
      to the helper now instead of ending the block (D-fld: 514
      interpreted to 265). RVA20's list is met but for Zicclsm being a build
      option, and Sstvala and Ssccptr being true without a test that
      says so.

- [x] **gdb: what the four targets did not describe.** Registers have
      their own widths now (`reg_width`, 64-bit accessors), and ARMv7-M
      serves gdb's VFP feature -- d0..d15 and fpscr. PowerPC serves
      SPEFSCR in a feature of its own rather than gdb's SPE feature,
      whose upper halves and `acc` this model does not have. The memory
      map is built from the bus and the flash ops for every frontend;
      the two hard-coded ones named flash where the flash ops could not
      program -- RV32's at 0x8000_0000, stale since execute-in-place,
      and G4MH's at 0 ([gdb.md](gdb.md#the-guest-stub)).

- [ ] **rv32: Zihpm counts traps only.** No overflow interrupt
      (Sscofpmf), no privilege filter bits in `mhpmevent`. Linux finds
      the 29 counters through OpenSBI ("16 firmware and 31 hardware
      counters") and says sampling is unsupported for want of sscof.

- [ ] **JIT** - Autovectorisation of the IR pipeline. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] Add simple drivers for the rh850u2b6.based on their specification in the reference manual.
  - [ ] Option bytes: impact on clocks and startup.
  - [ ] Clock tree
  - [x] ltsc -- modelled in `g4mh_ltsc.c`, driven by the runner's
        `set_time`; DOOM's clock runs off it.
  - [x] ostm -- OSTM0 is in `g4mh_intc.c` with its compare interrupt.
        One channel; the part has more.
  - [ ] gpio/on emulated on top of stm32 gpio hal, emualated to output the binary state of output on a udp/ip connexion.
  - [ ] kcrc
  - [ ] wdtb
- [ ] Architecture a pcie emulated peripheral over USB for the emulator and a pcie over USB slave firmware to deploy on the various boards to exercise the available peripherals.
- [ ] Implement an emulated GTM device for the stm32f746zg/stm32n657 to demonstrate the emulator's capabilities.
- [ ] Implement TAUD peripheral in rh850u2b6 with the remaining stm32 timer availble.
- [x] **PowerPC: the e200z7 MMU.** Built as designed here, from
      `docs/ppc/e200z759CRM.pdf` chapter 10 and section 7.7 and nothing
      else -- [frontend/ppc.md](frontend/ppc.md#the-mmu) has what is
      modelled, the four places the model is not the manual, and the
      figures. `ppc-mmu` is 150 checks against the manual's tables, and
      37 single-point defects each fail the check named for them. The
      identity map costs nothing measurable interpreted -- -0.73% by
      callgrind, 224-231 against 224-231 host cycles an instruction on
      the F746 -- and +0.26% under `--jit`. Getting there found that
      moving the interpreter's loop into a function cost the F746 10%
      with x86-64 seeing +0.1%. Two things the design note did not foresee:
      `dcbz` had been ignoring rB since it was written, and `check.py`'s
      list of instructions "not on this core" hid a `tlbld`/`tlbwe`
      naming clash in objdump.
- [x] **PowerPC: run the JIT under a programmed TLB.** It did not:
      `blocked` was `mmu_on`. Now the translator fetches through a
      side-effect-free probe, the context carries `mmu_on`, MSR[IS],
      MSR[PR] and PID0, a TLB write moves the generation, and accesses
      take the helper under the MMU. `coremark-tlb` (CoreMark with a
      start-up that programs the TLB): x86-64 JIT 0.596 s → **0.112 s**,
      interpreted 12,170,424 → 2,480; `ppc-mmu --jit` 4,496 → 734.
      `ppc-mmu` gained the `jit-` cases, which run code, change one thing
      it was built under, and run it again -- six of eight defects in
      the machinery passed the old checks, and all ten now fail on the
      one named for them ([frontend/ppc.md](frontend/ppc.md#the-mmu)).
- [ ] **PowerPC: a window per TLB entry.** Under the MMU every access is
      a helper call, so `coremark-tlb` runs at half the identity map's
      speed under the JIT (0.112 s against 0.052). The RAM window could
      be offered per block when one entry maps the whole of it with
      read and write for the block's mode, with the TLB generation
      already guarding it. And a guest refilling the TLB from a miss
      handler flushes every block per refill; per-entry invalidation is
      what that would need.
- [ ] **PowerPC: little-endian pages.** MAS2[E] reads 0 today. Doing it
      means byte-reversed data access per page, ISI[BO] for a VLE page
      marked E, and DSI[BO] for an access crossing pages of different
      endianness (11.16.5) -- and a data fault path that can carry
      ESR[BO].

- [~] Finish the ppc emualtor with dual core support and implement a simple driver for the e200z7.
      **The single core is finished and the other two halves are not
      started.** Decoder, disassembler, the embedded FP unit, an IR
      translator and a JIT on both hosts are in, each checked against
      something that is not this code, and the frontend runs on the
      F746 ([frontend/ppc.md](frontend/ppc.md)). Still to do: a second
      core — `ncores` is 1, and the reservation is a flag with no
      address, which is the manual's single-core behaviour and needs
      revisiting for two — and a driver, which wants a peripheral model
      or the passthrough window to drive.
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
