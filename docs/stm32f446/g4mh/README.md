# stm32f446 / g4mh

**It runs**, since 2026-10, and it had not before: this board staged its
guest image by a copy of its own that always took `RV32_GUEST`, so a
G4MH firmware embedded RV32's `isatest`, ran it as RH850 code from
0x2000_0000 and faulted at its first fetch. It stages through
`cmake/guest-image.cmake` now, as the F746 and the N6 do, with G4MH's
load address. The small guest passes (15 checks), and G4MH CoreMark x40
gives CRC 0x65c5 -- "Correct operation validated", 17.2 s by the host's
clock, at 143.65 host cycles an instruction interpreted (150.42 before
the unread `cycles` counter went, 2026-10-26).

```sh
cmake -B build/g4mh -DEMU_PLATFORM=stm32f446 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON
cmake --build build/g4mh
```

**The defaults are this part's**: 32 KiB of cluster RAM and 16 of local
RAM, as on every board, and **the interpreter** -- the one pairing whose
JIT is off by default. A JIT build spends some 77 KiB before the guest
gets a byte, and the most that fits beside G4MH is a 12 KB cache, which
loses: CoreMark at 204.4 host cycles an instruction with 65,142
evictions and 28 KiB left to the guest, against 143.6 and 62 KiB
interpreted ([../../jit/tuning.md](../../jit/tuning.md)). With the JIT
on, the old defaults failed to link with `cannot move location counter
backwards`.

This page used to say the pair "links, never run" for want of an RH850
toolchain, and that G4MH had no JIT. Neither is true any more:

- **Renesas CC-RH builds the guests**, driven by
  `scripts/g4mh-build-guest.sh`; the firmware embeds the image named by
  `G4MH_GUEST` from `tests/guest/g4mh/`, or one given by absolute path
  -- CoreMark, from `scripts/g4mh-build-coremark.sh`, which is not
  committed. (`EMU_GUEST_BIN` would run it in place at 0x2000_0000;
  `G4MH_GUEST` keeps G4MH's load address.)
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
