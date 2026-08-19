# Open work

Moved out of README.md. Status lines here are claims about the code, so
they go stale: `scripts/report-figures.sh` regenerates the ones a host can
check, and anything measured on hardware carries the commit it was
measured at.

## Roadmap

- [ ] **Linux** - Prepare a Linux guest rv32g with mmu and run it on the emulator (x86_64 only). This is a big task, but it would be a good demonstration of the emulator's capabilities. Implement minimal virtio devices to get a shell and run some benchmarks. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] **JIT** - Autovectorisation of the IR pipeline. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] Run doom in Linux so it can be used as a benchmark for the emulator. This is a big task, but it would be a good demonstration of the emulator's capabilities implement a sdl backend for the emulator to run doom in Linux. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] **TFTP upload fails on the F746** — `Error code 2: error writing
      file` on the `rom` half, so `HAL_FLASH_Program` is refusing at
      0x08040000 (sector 5). Not caused by the PPP work: nothing in the
      flash path changed and `.itcm` is byte-identical between the SLIP
      and PPP builds (1048 bytes, same contents). **Not proven to
      pre-date it either** — that needs one run of the same upload
      against the SLIP firmware, which is the first thing to do. Note
      `hello.bin.rw` is 0 bytes in the current tree, so the empty-`.rw`
      case CLAUDE.md warns about is what an upload test would exercise;
      pick a guest with real `.data`.
- [ ] Add simple drivers for the rh850u2b6.
- [ ] Architecture a serial protocol over TCP similar to PCIe so a PC host running the emualtor can access the rh850u2b6's peripherals. This is a big task, but it would be a good demonstration of the emulator's capabilities. First over serial, then maybe over USB or rela ethernet device. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] Implement an emulated GTM device for the stm32f746zg to demonstrate the emulator's capabilities.
- [ ] Finish the ppc emualtor with dual core support and implement a simple driver for the e200z7. This is a big task, but it would be a good demonstration of the emulator's capabilities.
- [ ] **Port to the Nucleo-N667X0 (STM32N6).** A third platform, and the
      first one that is not ARMv7E-M: the N6 is **Cortex-M55**, i.e.
      Armv8.1-M with Helium. What that changes, in the order it will
      bite:
      - `EMU_HOST_JIT_THUMB2` currently tests
        `__ARM_ARCH >= 7 && __thumb2__`, which an M55 satisfies — so the
        existing emitter will be *selected* and every encoding it emits
        is still valid. The port is therefore expected to run before it
        is tuned, and the risk is that this hides the work rather than
        that it fails loudly.
      - Caches again, and differently: the M55 has L1 I- and D-cache
        like the M7, so `board_sync_icache` needs the same override —
        that hook now exists and is weak, so forgetting it is silent.
        See the entry in CLAUDE.md; this is exactly the shape that has
        already cost one session.
      - `src/emu/` claims to build for ARMv6-M through ARMv8.1-M. The N6
        is the first thing that would *check* that claim.
      - Helium (MVE) is the interesting part and is **not** the port:
        it is a second, wider emitter, and the pair-statistics histogram
        should say whether anything in a guest can use it before a line
        of it is written. Run `-DRV32_PAIR_STATS=ON` first.
      Add a `f746`-style row to `scripts/build-matrix.sh` with the port,
      not after it.

**Measured and rejected**, kept here so they are not retried blind:

| Idea | Result |
|---|---|
| Interpreter loop in SRAM | **slower** — 162 vs 122 cycles, and 8 KiB off the guest |
| PMP mapped onto the ARM MPU | **not possible** — the MPU cannot distinguish a guest access from an emulator access, because the JIT's inlined load *is* both |

