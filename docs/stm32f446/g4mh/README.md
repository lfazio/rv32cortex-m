# stm32f446 / g4mh

**Links, never run.** A G4MH-only firmware builds and links at 39,676 bytes
of text against the RV32 build's 70,040 — which is the check that the
frontend seam holds through the platform layer, and the only thing this
pair has been shown to do.

```sh
cmake -B build/g4mh -DRV32_PLATFORM=stm32f446 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release \
      -DEMU_FRONTEND_RV32=OFF -DEMU_FRONTEND_G4MH=ON
cmake --build build/g4mh
```

Nothing has executed on it because there is no RH850 toolchain here, so
there is no guest image to embed. `RV32_GUEST` still names a RISC-V binary,
which a G4MH core would decode as nonsense.

## What would need to happen

1. **A G4MH guest toolchain** — `rh850-elf-gcc`, or Renesas CCRH. This is
   the blocker; everything else is downstream of it.
2. **A guest image and link script.** `tests/guest/guest.ld.in` is written
   for RISC-V; the G4MH equivalent needs the reset vector at `RBASE` and
   the exception table laid out to match `handler_address()`.
3. **`RV32_GUEST` split per frontend**, or renamed — one variable currently
   names both the image and, implicitly, its architecture.
4. **The NVIC bridge wired to the INTC.** `emu_cpu_ops_t::set_irq` and
   `set_unmask_hook` are implemented on the G4MH side and the platform
   calls them, so this should already work; it has never been observed to.

## Devices

INTC1 at `0xFFFC_0000` (SELF alias) and `0xFFFC_4000` (PE0), INTC2 at
`0xFFF8_0000`, OSTM0 at `0xFFEC_0000`. Real RH850/U2B addresses — see
[`../../host/g4mh/README.md`](../../host/g4mh/README.md) for the layout and
what is and is not modelled.

These sit well above the passthrough window at `0x4000_0000`, so there is
no collision with the STM32 peripheral space. That was not luck: RH850 puts
its on-chip control registers at the top of the address space.

## To do

- Everything above.
- **A G4MH Thumb-2 JIT.** There is none; the frontend runs on the
  interpreter. See [`docs/Architecture.md`](../../Architecture.md).

## Investigate

- **Whether a G4MH guest can drive STM32 peripherals through the
  passthrough window at all.** It should — the window is architecture
  neutral and RH850 has no conflicting use for `0x4000_0000` — but a real
  driver would be the proof.

## Discarded

- Nothing yet. Nothing has been tried.
