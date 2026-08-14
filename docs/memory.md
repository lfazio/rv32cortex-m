# ROM, RAM and flash

Who owns the backing store for guest memory, and why the answer is "the
platform" rather than "the frontend".

The short version: **a frontend describes an address map; a platform
provides the memory behind it.** Flash in particular is where code is
*executed from*, so on a microcontroller it is the host part's own flash
holding the guest image — served read-only, costing no SRAM at all.
Backing an architectural flash size with `.bss` is how the G4MH frontend
came to need 3.44 MiB on a part with 320 KiB.

---

## The three kinds of region

`emu_bus` knows three, and the difference is enforced on every access:

| | added by | writable | backed by |
|---|---|---|---|
| ROM | `emu_bus_add_rom` | no — a store faults | host flash, or any const buffer |
| RAM | `emu_bus_add_ram` | yes | host SRAM |
| MMIO | `emu_bus_add_mmio` | via callbacks | a device |

A zero-length region is rejected, which matters more than it sounds:
several bugs here have been "the size was computed as zero and the region
silently did not exist".

## RV32

Three regions, and the split between them is what buys the guest its
address space.

```
0x2000_0000  ROM   the guest image, executed in place from ARM flash
0x8000_0000  ROM   the image's read-only half   (guest-ro)
     + ro    RAM   guest RAM, the rest of SRAM  (ram)
0x4000_0000  pass  the ARM's own peripherals, identity-mapped
```

The guest is linked contiguously from `EMU_GUEST_RAM_BASE` and still
resets to offset 0; only *which backing store answers the low addresses*
changes. The read-only half is served straight out of flash where
`guest_image.S` put it, so the SRAM buffer covers
`[ro, ro + GUEST_RAM_SIZE)` instead of `[0, GUEST_RAM_SIZE)`. On the
largest architecture tests that is **140 KiB of the 345 they need**,
gained for nothing.

RAM comes from the linker script (`__guest_ram_start` / `__guest_ram_end`)
— whatever the firmware does not use — not from a `.bss` array. That is
the property worth copying: the size is a link-time fact about the part,
not a number compiled into the emulator.

Two things that have bitten here, both recorded in CLAUDE.md:

- **`build_address_space()` reads `g_img_*`, and `main()` was setting
  them afterwards.** With them zero the ROM region is zero-length,
  `emu_bus_add` rejects it, and the firmware halts before its first guest
  instruction. Moving a value from link time to run time moves every
  reader of it into an ordering that nothing checks.
- **Rebuilding the bus drops the frontend's devices.** `start_guest()`
  begins with `emu_bus_init()`, which clears the region table, so the
  frontend's `add_shared_devices` / `add_core_devices` have to run again
  every time.

## G4MH

The U2B6's map is much larger than any part this runs on:

| region | on the part | backed here | how |
|---|---|---|---|
| code flash `0x0000_0000` | 3 MiB | **nothing** | the platform supplies it |
| cluster RAM `0xFE00_0000` | 384 KiB | `G4MH_CRAM_KIB`, default 128 | `.bss` |
| local RAM `0xFDE0_0000` | 64 KiB per PE | `G4MH_LRAM_KIB`, default 64 | `.bss` per PE |

The architectural sizes stay in `g4mh_memmap.h` as `*_SIZE`, because they
describe the part. What a build allocates is `*_BACKED`, from the CMake
options. The two are deliberately different numbers, and a `#error`
catches a configuration asking for more than the part has.

**Flash is not a trade-off, it is a different thing.** A platform says
where it lives:

```c
void g4mh_set_flash(const void *base, uint32_t size, bool writable);
```

- The **firmware** passes the guest image in its own flash, read-only.
  That costs no SRAM, and read-only is the model rather than a limitation:
  there is no flash sequencer here, and a guest writing its own code is
  doing something a real part refuses. Passing `writable = false` makes it
  fault instead of silently succeeding.
- The **host runner** does not call it and gets a writable arena of
  `G4MH_FLASH_KIB` (256 KiB), because its ELF loader writes the image
  straight into guest memory.

Firmware builds set `G4MH_FLASH_KIB=0`, so the arena does not exist at
all — `#if G4MH_FLASH_BACKED > 0u` compiles it out rather than allocating
zero bytes and hoping.

Measured, F746, `-DEMU_FRONTEND_RV32=OFF -DEMU_FRONTEND_G4MH=ON`:

| | `.bss` | links |
|---|---|---|
| before | 3.44 MiB + firmware | no — `cannot move location counter backwards` |
| after | 320 KiB total, 192 of it CRAM + LRAM | yes |

## The rule

When adding a frontend, ask what its regions are *backed by* before
writing the sizes down. A frontend that allocates its own memory map
works on a host and cannot be ported, and nothing will tell you until
someone tries the firmware build — which is why
`-DEMU_FRONTEND_RV32=OFF -DEMU_FRONTEND_G4MH=ON` is the contract check.

---

## Byte order

Both existing frontends are little-endian and every host is
little-endian, so the bus composed bytes in host order and was never
wrong. A big-endian guest (PowerPC e200z7, task #36) makes that a real
question, and the answer is a split rather than a switch:

| region kind | swapped? | why |
|---|---|---|
| RAM, ROM | **yes** | the guest's image is in the guest's byte order, and these compose bytes into a value |
| MMIO | no | a device callback hands back a *value*; no bytes were composed |
| PASSTHRU | no | a real peripheral register holds a value, read natively |
| instruction fetch | **yes** | instructions are in the image like anything else |

Getting the split wrong is silent either way. Swap the whole bus and
every timer and UART register reads byte-reversed; swap nothing and a
big-endian guest's own data is garbage. `test_bus_big_endian` checks both
directions, and both were confirmed by breaking them — 6 failures for no
swap, 1 for swapping MMIO too.

`emu_bus_set_big_endian()` is how a frontend declares it, once, at init.
It is a property of the guest architecture, not of a region or a
platform.

**It costs a little-endian build nothing**, which matters because
CLAUDE.md's standing rule is that anything on the access path is paid by
every guest whether it uses the feature or not. `EMU_BUS_ANY_BE` is
derived from the frontends selected, so the test compiles away entirely.
Verified by inspection rather than asserted: `emu_bus.c` compiled with
`-DEMU_FRONTEND_PPC=0` contains **zero** byte-swap instructions, and two
with it set.
