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

- [ ] **The F446 has built and not run** under FreeRTOS.
- [ ] **The F446's code cache, and G4MH builds'.** At the 32 KB default
      the JIT used to lose to the interpreter. Decided for the F746
      (96 KB) and the N6 (128 KB in AXISRAM2), each except with G4MH
      compiled in, whose RAM leaves no room; on the F746 at 32 KB the
      smaller Thumb-2 code has since made the JIT level with the
      interpreter (109.2 against 110.46). The F446 has 128 KiB of RAM in
      all and has not been measured since. The ways out -- a larger
      default and what it costs the guest, the interpreter as the
      firmware default, or smaller code -- are in
      [jit/tuning.md](jit/tuning.md#emu_jit_code_bytes--the-dominant-term)
      and [backend/thumb2.md](backend/thumb2.md#where-the-bytes-go).
- [ ] **Board figures that predate `emu_cycles.h`.** Any of them from a
      run longer than 19.9 s at 216 MHz (23.9 s on the F446) is a whole
      wrap short, and the documents do not record run lengths. The ones
      in `jit/tuning.md`, `backend/thumb2.md` and the first section of
      `performance.md` are after the fix. The second half of
      `performance.md` and both `stm32f446/` pages are not, and are
      marked as history.
- [ ] **ARMv7-M's interpreter costs 2.7x RV32's per instruction** on the
      F746, and nothing has profiled it there
      ([frontend/armv7m.md](frontend/armv7m.md#not-done)).

## JIT

- [ ] **The peripheral window for the other frontends.** It is inlined
      on Thumb-2 for RV32 only, where it is worth 2.5-7x on driver code
      ([performance.md](performance.md#driver-performance-the-passthrough-window)).
      G4MH, PowerPC and ARMv7-M would each set `has_pt` beside
      `has_fast` and arm it in their `bind` as RV32 does -- PowerPC only
      for a little-endian window it does not have.
- [ ] **Investigate G4MH's generation under one block table.** `jit_gen`
      is per PE, and where the PEs share a table (`--serial`, a board
      with `G4MH_PE_COUNT > 1`) two PEs whose MPU generations differ
      would discard every block at every switch between them. PowerPC
      made its counter one for all cores for exactly that reason.
      Unmeasured.
- [ ] **JIT** - Autovectorisation of the IR pipeline.
- [ ] **PowerPC's FP unit is a call per instruction.** Every EFPU2
      instruction under the JIT is a helper call into `ppc_fpu_exec`,
      whose bookkeeping -- SPEFSCR status, sticky bits, FG and FX --
      was 14.7% of host instructions on Quake after the integer fast
      path ([frontend/ppc.md](frontend/ppc.md#what-uses-it-and-what-it-costs)).
      A lowering for normal operands under round-to-nearest with no
      enables would still have to produce FG and FX exactly; measure
      what the call costs before writing one.

## Linux

From the last measurements in [linux.md](linux.md).

- [ ] **FENCE.I flushes the whole cache**: ~3,770 a boot, nearly all
      before udev. A store into a page holding translated code is what
      it has to catch; tracking that per page would make it local.
- [ ] **Every process translates its own copy of libc.** The key is
      satp, so identical physical pages in two address spaces are two
      sets of blocks. Keying on the physical page would share them, at
      the cost of a translation on every lookup.
- [ ] **Benchmarks under Linux**, still not run.
- [ ] **The run ends in a kernel panic**, because the test init exits
      and there is nothing in the device tree to power off through. It is
      read as the terminator, so every run goes to the instruction cap or
      a timeout to print its figures.
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

- [ ] Implement a simple driver for the e200z7, which wants a
      peripheral model or the passthrough window to drive. The SoC is
      the MPC5675K (`docs/nxp/`), whose SSCM already starts core 1:
  - [ ] Implement driver STM System Timer Module (Chapter 51)
  - [ ] Implement SWT Software Watchdog Timer (Chapter 52)
- [ ] **Anything to compare it with.** No e200 is attached and no
      other emulator is consulted, so a shared misreading of the manual
      is invisible to every check there is.

## RH850 / G4MH

- [ ] Add simple drivers for the rh850u2b6.based on their specification in the reference manual.
  - [ ] Option bytes: impact on clocks and startup. Located in flash at a defined address.
  - [ ] Clock tree
  - [x] ltsc (Section 44) -- modelled in `g4mh_ltsc.c`, driven by the runner's
        `set_time`; DOOM's clock runs off it.
  - [~] ostm (Section 34) -- OSTM0 is in `g4mh_intc.c` with its compare interrupt.
        One channel; the part has more: missing one channel per core.
  - [ ] gpio/on (Section 2) emulated on top of stm32 gpio hal, emualated to output the binary state of output on a udp/ip connexion.
  - [ ] kcrc (Section 57) peripheral
  - [ ] wdtb (Section 33): one channel per core
- [ ] Implement TAUD peripheral in rh850u2b6 with the remaining stm32 timer availble qnd plug it into the pcie framework
- [ ] Finish implementing g4mh and rh850 cpu system (Section 3)

## Platforms

- [ ] A tinyusb network device over USB (https://github.com/hathach/tinyusb).
      What it was waiting for is done: lwIP runs on a FreeRTOS task of
      its own, given a turn by the run loop when it has work
      ([network.md](network.md#on-a-task-of-its-own)). A USB device's
      events would wake that task, which today only the run loop does.
- [ ] Architecture a pcie emulated peripheral over USB for the emulator and a pcie over USB slave firmware to deploy on the various boards to exercise the available peripherals.
- [ ] Implement an emulated GTM device for the stm32f746zg/stm32n657 and plug it in the pcie framework.
- [ ] Add support for armv6-m in frontend armv7-m (rename it armvX-m)
- [ ] Add support for armv8-m into frontend armvX-m
