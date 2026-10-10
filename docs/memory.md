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

Laid out the way a microcontroller is: code and constants execute from
flash, writable data lives in RAM.

```
0x2000_0000  ROM   the guest image, executed in place from the host's flash
0x4000_0000  pass  the ARM's own peripherals, identity-mapped
0x8000_0000  RAM   .data, .bss and the stack: whatever SRAM the firmware
                   does not use
```

`.text` and `.rodata` are linked at `EMU_GUEST_ROM_BASE`; `.data` has its
run address in RAM and its initialiser at a load address in flash, and
the guest's own `start.S` copies it across — as any crt0 does. The
image is therefore one blob, mapped read-only wherever it already lives,
and it costs the guest no RAM however large it is: that is what lets an
architecture test needing 345 KiB run on a part with 264 KiB of guest
RAM.

**The emulator does not know where the read-only part ends, and no
longer needs to.** It used to: the guest was linked entirely in RAM, the
image was split at `__guest_ro_end`, and the firmware served one half
from flash and installed the other. That put the guest's initialisation
in the loader, and the split was lossy for half the guests — three bytes
wherever `.rodata` ended unaligned — while a comment said the build
checked it. A fragile invariant is a prompt to ask why anything depends
on it.

The host cannot prove the copy loop works, by construction: it loads the
whole blob into RAM, so `.data` is already where it runs. The board is
the test — CoreMark's CRC there would change on zeroed `.data`.

RAM comes from the linker script (`__guest_ram_start` / `__guest_ram_end`)
— whatever the firmware does not use — not from a `.bss` array. That is
the property worth copying: the size is a link-time fact about the part,
not a number compiled into the emulator. A guest is told how much it has
at reset.

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

## PowerPC

One flat image at `0x8000_0000`, in RAM — the 64 KiB-aligned address
IVPR needs, with the vectors first. It is loaded as an **ELF**, on the
host and in the firmware alike, because which instruction encoding it is
in is a flag on its segment ([frontend/ppc.md](frontend/ppc.md)); `.bss`
is not in the file and `crt0.S` zeroes it.

The stack is the loader's to give: `r1` arrives pointing at the top of
RAM. The guests' link script used to name that address, which is right
on one machine — a megabyte on the host, a fifth of that on the F746 —
and the first `stwu` landed outside RAM on the other.

Unlike RV32, the image occupies guest RAM here. Linking it for
execute-in-place from the host's flash is the obvious next step and has
not been done.

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

Measured, F746, `-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON`:

| | `.bss` | links |
|---|---|---|
| before | 3.44 MiB + firmware | no — `cannot move location counter backwards` |
| after | 320 KiB total, 192 of it CRAM + LRAM | yes |

## The rule

When adding a frontend, ask what its regions are *backed by* before
writing the sizes down. A frontend that allocates its own memory map
works on a host and cannot be ported, and nothing will tell you until
someone tries the firmware build — which is why
`-DEMU_GUEST_ARCH_RV32=OFF -DEMU_GUEST_ARCH_G4MH=ON` is the contract check.

---

## Byte order

Three of the four frontends are little-endian and every host is
little-endian, so for a long time the bus composed bytes in host order
and was never wrong. PowerPC is big-endian, which makes it a real
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
`-DEMU_GUEST_ARCH_PPC=0` contains **zero** byte-swap instructions, and two
with it set.

**The JIT has to keep the same split, and it has two paths to keep it
on.** A translated block may read guest RAM directly, through a window
that is bytes on a little-endian host; so for a big-endian guest an IR
`LOAD` means *the bytes as the host reads them*, and the translator
follows it with a byte swap. Outside the window the backend calls the
frontend's accessor, which goes through the bus — and the bus has
already swapped, for RAM, and has not, for a device. So that accessor
swaps what the bus returned, in order that the IR's swap undoes it: one
rule on both paths, and a device register read from translated code
comes out as the value the device holds.
