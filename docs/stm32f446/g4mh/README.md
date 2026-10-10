# stm32f446 / g4mh

**Not known to have run on this board.** The frontend swap itself is
checked on the other two: `scripts/build-matrix.sh` builds a G4MH-only
firmware for the F746 (one PE and three) and for the N6, which is the
check that the frontend seam holds through the platform layer. There is
no F446 row for it.

```sh
cmake -B build/g4mh -DEMU_PLATFORM=stm32f446 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON \
      -DG4MH_CRAM_KIB=32 -DG4MH_LRAM_KIB=16 -DEMU_JIT_CODE_BYTES=12288
cmake --build build/g4mh
```

**The three sizes are not optional on this part.** With the defaults —
128 KiB of cluster RAM, 64 of local RAM and a 32 KB code cache — the
link fails with `cannot move location counter backwards`, 159 KiB over a
board that has 128. Shrinking the two RAMs alone still leaves it 12 KiB
over, because a JIT build spends 77 KiB before the guest gets a byte;
the recipe above, or `-DEMU_JIT=OFF` in place of the smaller cache,
links.

This page used to say the pair "links, never run" for want of an RH850
toolchain, and that G4MH had no JIT. Neither is true any more:

- **Renesas CC-RH builds the guests**, driven by
  `scripts/g4mh-build-guest.sh`; the firmware embeds the image named by
  `G4MH_GUEST` from `tests/guest/g4mh/`.
- **G4MH has a translator** (`g4mh_ir.c`) and so a JIT on both hosts
  through the shared IR. Its small guest has run on a board with the
  host agreeing to the digit ([../../performance.md](../../performance.md)).
- **Memory is the thing to check on a 128 KiB part**, as above: the
  U2B6's map is far larger than this board, and what a build backs with
  `.bss` is a CMake option. See [../../memory.md](../../memory.md).

## Devices

INTC1 at `0xFFFC_0000` (SELF alias) and `0xFFFC_4000` (PE0), INTC2 at
`0xFFF8_0000`, OSTM0 at `0xFFEC_0000`. Real RH850/U2B addresses — see
[`../../host/g4mh/README.md`](../../host/g4mh/README.md) for the layout and
what is and is not modelled.

These sit well above the passthrough window at `0x4000_0000`, so there is
no collision with the STM32 peripheral space. That was not luck: RH850 puts
its on-chip control registers at the top of the address space.

## Investigate

- **Whether a G4MH guest can drive STM32 peripherals through the
  passthrough window.** It should — the window is architecture neutral
  and RH850 has no conflicting use for `0x4000_0000` — but a real driver
  would be the proof.
- **The NVIC bridge wired to the INTC.** `set_irq` and `set_unmask_hook`
  are implemented on the G4MH side and the platform calls them; it has
  not been observed to deliver a real peripheral interrupt.
