# Open work

Only what is still to do. Finished work is described in the document it
changed -- a frontend's in [frontend/](frontend/), the JIT's in
[jit/](jit/README.md), the scheduler's in [os.md](os.md), Linux's in
[linux.md](linux.md), the games' in [games.md](games.md), the N6 port's
in [stm32n6/](stm32n6/README.md) -- and what was learned the hard way
is in CLAUDE.md. Status lines here are claims about the code, so they
go stale: `scripts/report-figures.sh` regenerates the ones a host can
check, and anything measured on hardware carries the commit it was
measured at.

## Needs a board

- [ ] **Board figures that predate `emu_cycles.h`.** Any of them from a
      run longer than 19.9 s at 216 MHz (23.9 s on the F446) is a whole
      wrap short, and the documents do not record run lengths. The ones
      in `jit/tuning.md`, `backend/thumb2.md` and the first section of
      `performance.md` are after the fix. The second half of
      `performance.md` and both `stm32f446/` pages are not, and are
      marked as history.

## JIT

- [ ] **The peripheral window for the other frontends.** It is inlined
      on Thumb-2 for RV32 only, where it is worth 2.5-7x on driver code
      ([performance.md](performance.md#driver-performance-the-passthrough-window)).
      G4MH, PowerPC and ARMv7-M would each set `has_pt` beside
      `has_fast` and arm it in their `bind` as RV32 does -- PowerPC only
      for a little-endian window it does not have.
- [ ] **G4MH's generation under one block table.** `jit_gen` is per PE,
      and where the PEs share a table (`--serial`, a board with
      `G4MH_PE_COUNT > 1`) two PEs whose MPU generations differ would
      discard every block at every switch between them. PowerPC made
      its counter one for all cores for exactly that reason. Unmeasured.
- [ ] **Autovectorisation of the IR pipeline.**
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
- [ ] **Loads and stores below M-mode are calls.** CoreMark as a Linux
      process is 6.3x slower than bare metal on the host JIT, with
      block entries as frequent as there
      ([linux.md](linux.md#benchmarks)): paging and the PMP gate turn
      the inlined RAM path off for any OS. An inlined probe has to
      answer both questions -- the translation and the permission --
      and CLAUDE.md's sv32bench entry says 4.0x is the ceiling for the
      first alone.
- [ ] **A display under Linux.** The framebuffer device exists
      ([games.md](games.md)); nothing in the device tree describes it to
      a kernel.

## Guests

- [ ] **ARMv7-M**, from [frontend/armv7m.md](frontend/armv7m.md#not-done):
      the whole F746 firmware as a guest (it needs an STM32F746 machine
      model, which is a direction change), the IR and register allocator
      executed in a guest, the JIT's remaining items, and the board's
      answers kept as fixtures.

## PowerPC

- [ ] **Anything to compare it with.** No e200 is attached and no
      other emulator is consulted, so a shared misreading of the manual
      is invisible to every check there is.

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
- [ ] **The G4MH and RH850 CPU system** (section 3), finished.

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
- [ ] **An emulated GTM** for the STM32F746ZG and the STM32N657, plugged
      into the PCIe framework.
- [ ] **ARMv6-M in the ARMv7-M frontend**, which becomes armvX-m.
- [ ] **ARMv8-M in the armvX-m frontend.**
