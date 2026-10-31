# Open work

Only what is still to do. Finished work is described in the document it
changed -- a frontend's in [frontend/](frontend/), the JIT's in
[jit/](jit/README.md), the scheduler's in [os.md](os.md), Linux's in
[linux.md](linux.md), the games' in [games.md](games.md), the N6 port's
in [stm32n6/](stm32n6/README.md) -- and what was learned the hard way
is in CLAUDE.md, as rules, and [lessons.md](lessons.md) at length.
Status lines here are claims about the code, so they go stale: `scripts/report-figures.sh` regenerates the ones a host can
check, and anything measured on hardware carries the commit it was
measured at.

## Needs a board

- [ ] **Board figures that predate `emu_cycles.h`.** Any of them from a
      run longer than 19.9 s at 216 MHz (23.9 s on the F446) is a whole
      wrap short, and the documents do not record run lengths. The ones
      in `jit/tuning.md`, `backend/thumb2.md`, `frontend/ppc.md` and the
      first section of `performance.md` were measured again at ce02bd0. The second half of
      `performance.md` and both `stm32f446/` pages are not, and are
      marked as history.

## Testing

- [ ] **Fuzz testing.** The first target exists for every frontend
      and has paid: `fuzz-<isa>-jit`, the interpreter against the
      x86-64 JIT, and `fuzz-ir` for the IR alone ([fuzzing.md](fuzzing.md)):
      eighteen defects, eight of them in an interpreter, one a whole
      class of ARMv7-M encodings the board then answered. The rest of this item is still open. Before
      it, the tree had generation, not fuzzing:
      `tests/armv7m-diff/rawgen.py` (raw encodings against the F746),
      the property tests over whole encoding spaces (ARMv7-M's decoder
      over 402,712,576 encodings, G4MH's length over 65,536 halfwords),
      `tests/ppc-check/` against binutils and a Python model, the
      Spike/Sail probes, and `ir_diff_ref`. None is coverage-guided, and
      none reaches what a guest controls beyond the instruction word.
      clang 21 links `-fsanitize=fuzzer,address` on the host, so each of
      these is a libFuzzer target behind an option, smallest oracle
      first:
  - **Instruction streams, interpreter against JIT**, per frontend:
    the same bytes run both ways and compared on the register file,
    memory, trap state and the counters. **Done for all four**
    (`tests/fuzz/fuzz_harness.h` and a target each). What is left:
    Spike as a third side for RV32 (BSD-3, CLAUDE.md) -- **set aside
    until late in the project**, by decision (2026-10-26); timers, and
    whether an ISB or an
    interrupt raised by a store is honoured *in time* -- both set aside
    today ([fuzzing.md](fuzzing.md#devices-and-interrupts)); and
    self-modifying code, which ARMv7-M detects exactly and nothing has
    fuzzed. Interrupt controllers are mapped for every frontend, and
    G4MH and PowerPC run two cores ([fuzzing.md](fuzzing.md#more-than-one-core)).
    No GPL emulator, not even as a reference.
  - **The IR**: done for the passes and the x86-64 lowering
    (`fuzz-ir`). Not reached: the Thumb-2 lowering, which runs only on
    a board -- its corpus replayed there would be the way; `pass_mac`,
    which forms a multiply-accumulate only where the backend lowers
    one, and x86-64 does not; and the FP exception flags, which the
    reference does not compute.
  - **Translation and protection**: done for RV32 -- Sv32, PMP and
    the data TLB against a walk written from the spec
    (`fuzz-rv32-mmu`), which found a PPN past the bus read as an alias
    and an M-mode access half in an unlocked PMP entry let through.
    G4MH's MPU (`fuzz-g4mh-mpu`, from the U2B manual: seven defects,
    all found writing the reference), ARMv7-M's (`fuzz-armv7m-mpu`,
    from the ARM ARM and the F746: none), PowerPC's TLB with the JIT's
    blocks kept across TLB, MSR and PID changes (`fuzz-ppc-tlb`: none),
    and RV32's fetch side and the JIT's inline data TLB probe, code
    running in S and U (`fuzz-rv32-vm`: none). Not reached by any of
    them: PowerPC VLE pages, `lwarx`/`stwcx.`, `dcbz` and the cache
    instructions under the MMU, and a TLB change on one core retiring
    another's blocks; RV32 compressed code and anything but loads and
    stores, which `fuzz-rv32-vm` compares between the backends only;
    G4MH's and ARMv7-M's MPUs on a second core; and ARMv7-M's exception
    frames, whose stack `fuzz-armv7m-mpu` keeps open to every access --
    the board's answers on stacking faults (`stk-*`, `unstk-*`) are
    held by `test_stacking_board_answers` instead.
  - **What arrives from outside the guest**: done for the ELF loader,
    the gdb stub, the upload paths and the virtio rings (`fuzz-elf`,
    `fuzz-gdb`, `fuzz-upload`, `fuzz-virtio`: sixteen defects,
    [fuzzing.md](fuzzing.md#what-arrives-from-outside-the-guest)). Not
    reached: lwIP's own TFTP packet parsing, the telnet console and
    SLIP's framing, and virtio-9p, whose fuzzing would be the host's
    filesystem's -- a scratch directory in a mount namespace would make
    it safe to try.
  - **ARMv7-M on the board**: feed a fuzzer's corpus to rawgen's
    harness, so the F746 stays the oracle for what it alone can
    answer.

      Every target needs a mutant it finds before it counts, as every
      test here does: a fuzzer that runs for an hour and reports nothing
      can mean the code is right or that the target never reaches it.

## JIT

- [ ] **The peripheral window for G4MH and PowerPC.** It is inlined on
      Thumb-2 for RV32, where it is worth 2.5-7x on driver code
      ([performance.md](performance.md#driver-performance-the-passthrough-window)),
      and for ARMv7-M, 5.5-13x
      ([frontend/armv7m.md](frontend/armv7m.md#the-peripheral-window)).
      G4MH would set `has_pt` beside `has_fast` and arm it in its `bind`
      as those two do, and needs a driver-shaped guest to measure it
      with -- its own clock or a board timer, as ARMv7-M's `mmiobench`
      runs TIM2; PowerPC only for a little-endian window it does not
      have.
- [ ] **G4MH's generation under one block table.** `jit_gen` is per PE,
      and where the PEs share a table (`--serial`, a board with
      `G4MH_PE_COUNT > 1`) two PEs whose MPU generations differ would
      discard every block at every switch between them. PowerPC made
      its counter one for all cores for exactly that reason. Measured at
      8979eec: none today. The three-PE `smp.bin` under `--serial --jit`
      flushes 7 times and none of them is the generation's (a print at
      the generation flush, confirmed in the binary, never fired) --
      no G4MH guest writes the MPU, so every PE's count stays at its
      reset's. It bites the first multi-PE guest that programs its PEs'
      MPUs differently. The PE is part of a block's context now (each
      PE its own blocks, since a block bakes in its PE's local RAM), so
      the generation could be per PE too, in the context or the key.
- [ ] **Autovectorisation of the IR pipeline.**
- [ ] **A flush after start-up is worth 17% on the F746, by itself.**
      RV32 Dhrystone runs at 19.19 host cycles an instruction with the
      image window from the start, and at 15.96 with one flush forced
      at the 20th slice and nothing else changed: the steady-state loop
      is retranslated into an empty buffer, in order, instead of among
      start-up's blocks. Arming the image window gets this by accident
      ([backend/thumb2.md](backend/thumb2.md#the-guest-ram-window)).
      What it says about where hot blocks should go -- and whether
      CoreMark, which never flushes, has the same to gain -- is
      unmeasured. The `.bss` half of this row's layout sensitivity
      (16.42 or 19.30 by where `.bss` starts) was the hart in SRAM1,
      and the core state is pinned in the DTCM now
      ([performance.md](performance.md#one-lto-partition-for-abs));
      the 19.19 against 15.96 above was measured before that, so
      measure the flush again against the pinned layout first.
- [ ] **Below 32 KB the JIT is still slower than the interpreter.**
      Giving up on pcs that keep being evicted
      ([jit/tuning.md](jit/tuning.md#emu_jit_thrash_evictions--giving-up-on-what-does-not-fit))
      took Whetstone on the F746 from not finishing to 4.06 s at 32 KB,
      ahead of the interpreter's 6.29 s, but 16 KB is 8.30 s and 8 KB
      17.87 s. There the retries are what is left: every 65,536
      interpreted dispatches all the marks are forgotten and the cycle
      thrashes again until re-marked (6 and 11 retries). A retry that
      grows each time it is undone, or that forgets only marks older
      than the last, is the next thing to try -- measured on the board,
      where a translation costs tens of thousands of cycles.
- [ ] **ARMv7-M's sp/pc and IT rules cost its interpreter 7.1% on the
      F746** (143.44 to 153.69 host cycles an instruction on CoreMark;
      [frontend/armv7m.md](frontend/armv7m.md#what-the-board-corrected)),
      about 2% in the predicates by a PC profile and the rest code shape.
      The JIT, the default, pays nothing. Six variants were measured; a
      decode cache is the one lever not tried again, and it lost before
      ([frontend/armv7m.md](frontend/armv7m.md#the-run-loop-in-itcm-and-the-decode-cache-that-lost)).
- [ ] **PowerPC's FP unit is a call per instruction.** Every EFPU2
      instruction under the JIT is a helper call into `ppc_fpu_exec`,
      whose bookkeeping -- SPEFSCR status, sticky bits, FG and FX -- was
      14.7% of host instructions on Quake after the integer fast path
      ([frontend/ppc.md](frontend/ppc.md#what-uses-it-and-what-it-costs)).
      A lowering for normal operands under round-to-nearest with no
      enables would still have to produce FG and FX exactly; measure
      what the call costs before writing one.

## Linux

From the last measurements in [linux.md](linux.md).

- [ ] **FENCE.I flushes the whole cache**, and measured, it is not
      worth making local yet ([linux.md](linux.md#fencei-what-it-costs-and-where-it-comes-from)):
      at most 0.3 s of a 3.15 s initramfs boot, and the flushes are the
      kernel patching its own, already translated, text -- so it would
      take invalidation by physical page and every write path tracked,
      DMA and inlined M-mode stores included, for that.
- [ ] **Every process translates its own copy of libc.** The key is
      satp, so identical physical pages in two address spaces are two
      sets of blocks. Keying on the physical page would share them, at
      the cost of a translation on every lookup.
- [ ] **The slow path of a probed access is a 40-byte call.** The
      probe itself is 34 bytes before the access now
      ([backend/x86_64.md](backend/x86_64.md#the-data-tlb-probe)), and
      the call it falls back to -- the out-pointer, the cpu, the spec,
      `movabs` and `call rax`, the trap test -- is most of what is left.
      A shared stub per access kind, as Thumb-2 has, would make it a
      `call rel32` -- kept at the head of the code buffer, which a
      flush would then have to leave alone.
      Yocto at 32 MB is still 198 compactions, so this is the next
      lever if 64 MB is ever too much.
- [ ] **A reload loses the virtio devices.** `emu_main_reload`, which
      runs a guest uploaded over TFTP or gdb, rebuilds the bus and never
      adds them back (`virtio_attach` runs once, in `emu_main`); the
      glue's port table cannot be refilled either, having no reset, and
      TinyEMU has no way to free a device. Found writing `fuzz-virtio`;
      host network builds only.
- [ ] **A display under Linux.** The framebuffer device exists
      ([games.md](games.md)); nothing in the device tree describes it to
      a kernel.

## Platforms

- [ ] **A tinyusb network device over USB**
      (https://github.com/hathach/tinyusb). What it was waiting for is
      done: lwIP runs on a FreeRTOS task of its own, given a turn by the
      run loop when it has work
      ([network.md](network.md#on-a-task-of-its-own)). A USB device's
      events would wake that task, which today only the run loop does.
- [ ] **PCIe over USB**: the design of an emulated PCIe peripheral
      reached over USB, and of a PCIe-over-USB device firmware deployed
      on each board to exercise the peripherals it has. The use case is:
      develop firmware on pc while using real peripherals on boards.

## Guests

- [ ] **ARMv7-M**, from [frontend/armv7m.md](frontend/armv7m.md#not-done):
      the whole F746 firmware as a guest (it needs an STM32F746 machine
      model, which is a direction change), the IR and register allocator
      executed in a guest, the JIT's remaining items, and the board's
      answers kept as fixtures.

## RV32

- [ ] Implement V extensions
- [ ] Implement Crypto extensions: Zbk[bcx], Zknd, Zkne, Zknh, Zksed, Zkr, Zkn, Zks, Zk, Zkt

## RV64

- [ ] Propose a plan to extend rv32 to handle rv64, and implement it. The plan should include a way to
      handle the 64-bit registers in the IR and the JIT, and a way to
      handle the 64-bit memory accesses in the frontend and the JIT.
- [ ] Implement the profiles: RVI20, RVA20 (in riscv-spec.pdf)

## PowerPC

- [ ] Implement vectorial extensions
- [ ] **Anything to compare it with.** No e200 is attached and no
      other emulator is consulted, so a shared misreading of the manual
      is invisible to every check there is. The only second document,
      the MPC5675K reference manual, agrees on the MMU except where it
      describes a different core: its e200z7 has 64 TLB entries and
      SPE, this frontend's e200z759n3 32 and EFPU2
      ([frontend/ppc.md](frontend/ppc.md#the-mmu)). Whether to model the
      MPC5675K's core, whose peripherals are already here, is open.
- [ ] **An emulated GTM** for the STM32F746ZG and the STM32N657, plugged
      into the PCIe framework.

## ArmvX-m

- [ ] **The lazy FP save is neither checked nor asked.** The deferred
      half of an FP frame is written by the first FP instruction in the
      handler (`armv7m_fpu.c`) straight to the bus: no MPU check
      (MLSPERR) and a bus error ignored (LSPERR). The board has answered
      every other frame question (`stk-*`): ask it this one the same way,
      a thread with FPCA set on a PSP whose FP half is refused or past
      the end of RAM.

- [ ] Implement neon extensions if available
- [ ] **The ARMv7-M frontend is a model of the F746's ITCM and DTCM**,
      and the ARMv8-M frontend is a model of the N657's TCMs. The
      frontends' `bind` functions are the only place where a guest can
      see what the board has, so they must be extended to describe the
      TCMs to their guests.
- [ ] **ARMv6-M in the ARMv7-M frontend**, which becomes armvX-m.
- [ ] **ARMv8-M in the armvX-m frontend.**

## RH850 / G4MH

- [ ] **Simple drivers for the RH850/U2B6**, from its reference manual:
  - [ ] Option bytes, and their effect on the clocks and start-up. They
        sit in flash at a defined address.
  - [ ] The clock tree.
  - [x] LTSC (section 44) -- modelled in `g4mh_ltsc.c`, driven by the
        runner's `set_time`; DOOM's clock runs off it.
  - [x] OSTM (section 34) -- `g4mh_ostm.c`, the U2B6's three units at
        their own addresses and channels, replacing the stand-in.
  - [ ] GPIO/ON (section 2), emulated on top of the STM32 GPIO HAL, the
        state of each output sent over a UDP/IP connection.
  - [x] KCRC (section 57) -- `g4mh_kcrc.c`, the four units, held to
        every row of the manual's setting table.
  - [x] WDTB (section 33) -- `g4mh_wdtb.c`, one per PE, with the
        ECM's reset configuration deciding what an error does.
  - [ ] The ECM (section 56), so a guest can configure what each error
        does; and FENC, the external NMI, so FENMI has a source.
- [ ] **TAUD on the RH850/U2B6**, built on the STM32 timers that remain
      free and plugged into the PCIe framework.
- [ ] **The G4MH and RH850 CPU system** (section 3), finished. Done:
      register banks and `RESBANK`, the system registers' writable
      bits, `CFALI` and `CHBII` against the MPU, FENMI and FENC
      ([frontend/g4mh.md](frontend/g4mh.md#register-banks)). Left: the
      FXU's instructions, on PE0 (in progress); a cache for `CFALI` to
      lock; something to drive the NMI pin; and the debug level --
      `DBTRAP`, `DBRET`, `DBPC`, `DBPSW` -- which no manual in
      `docs/renesas/` describes: the Emulation manual is needed.
- [ ] **The readings only an RH850 can settle.** Every G4MH result rests
      on Renesas' documents, cross-checked where a second one exists
      ([frontend/g4mh.md](frontend/g4mh.md#what-rests-on-the-manuals-alone));
      what none settles: a read-modify-write refused for both read and
      write setting both detail bits, DISPOSE's order, LD.DW's odd
      register in MEI, `SVLOCK` binding `LDM.MP`, and the reserved
      encodings. A U2B board, asked the way `tests/armv7m-diff/` asks
      the F746, is the way to close it.
