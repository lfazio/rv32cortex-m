# rv32cortex-m

A retargetable 32-bit ISA emulator whose host is an **ARM Cortex-M
microcontroller**, with the emulated guest driving the host's **real
peripherals** through an identity-mapped passthrough window.

Four guest architectures (RISC-V RV32, Renesas RH850 G4MH, NXP PowerPC
e200z7 and ARM Cortex-M), each with an interpreter and a JIT; two JIT
backends (Thumb-2 and x86-64) behind one shared IR; and four platforms
(a native host runner and STM32F4/F7/N6 firmware).
Validated against the official
[RISC-V Architecture Test Suite](https://github.com/riscv/riscv-arch-test)
at **391/391** and the Berkeley `riscv-tests` at **77/77**, on hardware
as well as on a host.

The ARMv7-M frontend began as a way to make the **Thumb-2 backend
testable without hardware** — `tests/guest/armv7m/t2exec.c` compiles the
real encoder into a Cortex-M guest, emits instructions into guest RAM
and executes them — and is now a complete ARMv7E-M + FPv5-SP core with a
JIT, **checked against a real Cortex-M7** rather than another emulator:
the same test programs run as guests and as native Nucleo-F746ZG
firmware, and every difference is the emulator's. See
[docs/frontend/armv7m.md](docs/frontend/armv7m.md).

---

**Every build gate, and which combinations are checked, is in [BUILD.md](BUILD.md)** — with `scripts/build-matrix.sh` to build
all of them. Two configurations had stopped compiling before it
existed.

## The idea

The emulator turns an ARM Cortex-M part into a RISC-V machine. The
interesting part is not the interpreter — it is the memory map:

```
guest 0x4000_0000..0x5FFF_FFFF   ─── identity map ───▶   STM32 APB1/APB2/AHB1/AHB2
```

That range is where a RISC-V platform leaves room for memory-mapped I/O,
and also exactly where the STM32 puts its peripherals. Mapping it
one-to-one means **a guest driver uses the addresses printed in the
vendor reference manual**, with no translation to reason about.

The consequence is the design's main idea:

> **Peripheral drivers live in the guest, not in the emulator.**

The emulator contains no GPIO driver, no UART driver, no SPI driver. It
forwards loads and stores. Porting to a new microcontroller means writing
a clock setup, a linker script and a region table — not a new driver
stack. [`tests/guest/stm32drv.c`](tests/guest/stm32drv.c) is a working
demonstration: GPIO and USART drivers written entirely as RISC-V guest
code, driving real silicon.

Three axes, independent of each other:

| axis | what it decides | selected by |
|---|---|---|
| platform | where it runs | `EMU_PLATFORM=host\|stm32f446\|stm32f746\|stm32n6` |
| frontend | what it emulates | `EMU_GUEST_ARCH_RV32`, `EMU_GUEST_ARCH_G4MH`, `EMU_GUEST_ARCH_PPC`, `EMU_GUEST_ARCH_ARMV7M` |
| backend | how it executes | `EMU_JIT=ON\|OFF`, `--jit` on the host runner |

---

## Building

### Host — development and both test suites

```sh
cmake -B build/host -DEMU_PLATFORM=host -DCMAKE_BUILD_TYPE=Release
cmake --build build/host
ctest --test-dir build/host -L fast
```

RV32 is on by default. Add `-DEMU_GUEST_ARCH_G4MH=ON`,
`-DEMU_GUEST_ARCH_PPC=ON` or `-DEMU_GUEST_ARCH_ARMV7M=ON` to compile
more frontends into one runner, which then picks with `--frontend` or
from the image's ELF header; `-DEMU_GUEST_ARCH_RV32=OFF` builds one
alone. Each has its own page under [docs/frontend/](docs/frontend/).

### Firmware — Nucleo-F746ZG

```sh
cmake -B build/f746 -DEMU_PLATFORM=stm32f746 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release -DRV32_GUEST=isatest
cmake --build build/f746 --target flash        # probe-rs over ST-LINK
```

The older Nucleo-F446RE is still supported: `-DEMU_PLATFORM=stm32f446`,
`--chip STM32F446RETx` on `/dev/ttyACM0`. `EMU_PLATFORM` picks the CPU,
the FPU and the vendor pack.

Console: the ST-LINK virtual COM port at **921600 8N1** — USART3 on the
Nucleo-F746ZG (a Nucleo-144 wires the VCP to PD8/PD9), USART2 on the
F446RE.

```sh
picocom -b 921600 /dev/ttyACM1
```

On the F746 the network is **on by default**, so that port carries IP
rather than text once the banner is out: the console becomes telnet, gdb
listens on 1234 and guest images go up by TFTP, all over PPP on the same
wire (SLIP with `-DEMU_NET_LINK=slip`). See
[docs/network.md](docs/network.md); build `-DEMU_NET=OFF` for a plain
serial console, which is what the board figures in
[docs/jit/tuning.md](docs/jit/tuning.md) were measured with.

```sh
./scripts/ppp-up.sh              # after the board says "net ... on this port"
telnet 192.168.7.2
```

The `flash` targets pass `--connect-under-reset` always, because this
firmware never idles and a plain attach races it. **A failed flash leaves
the previous firmware running**, which reports a plausible result for a
change that was never loaded — read the flash log before believing a
board result.

### Options

`-DRV32_GUEST=` selects the embedded guest image: `isatest`, `hello`,
`bench`, `stm32drv`, `irqtest`, `cmsistest`, `coremark`, `dhrystone`,
`whetstone`, `fptest` or `mmiobench`. A PowerPC firmware takes
`-DPPC_GUEST=` instead: `alu`, `sys`, `coremark` or `crypto`.

| Option | Default | Effect |
|---|---|---|
| `EMU_JIT` | `ON` | The JIT. `OFF` is smaller, and is how a suspected JIT bug is isolated. |
| `EMU_JIT_CODE_BYTES` | `32768` | Code cache, on a microcontroller. **The dominant performance term, and a threshold rather than a dial**: below a guest's translated working set the JIT is *slower* than the interpreter, and at the default that is true of CoreMark — see [docs/jit/tuning.md](docs/jit/tuning.md). |
| `EMU_JIT_LOOP_CAP` | `128` | Guest instructions per block entry: an interrupt-latency knob, not a throughput one. |
| `EMU_NET` | `ON` (F746) | lwIP over PPP (or SLIP, `EMU_NET_LINK`) on the console UART: telnet, gdb and TFTP. **The UART stops being a console** -- `OFF` gets it back. |
| `EMU_ENABLE_TRACE` | `OFF` | Per-instruction trace hook. Slow, and the fastest way to find where execution diverges. |
| `RV32_EXT_PMP` / `RV32_EXT_SDTRIG` | `ON` | Each costs a little even unused; `OFF` removes it. |
| `EMU_NATIVE_COREMARK` | `OFF` | Run CoreMark natively on the ARM instead of the emulator, for the baseline. |
| `EMU_SDL` | `OFF` | Show the guest framebuffer in an SDL3 window, and feed the keyboard and mouse devices from it. Needs `sdl3` via pkg-config; off by default so a host without it still builds. |
| `EMU_SDL_SCALE` | auto | Window scale, stepped down until the window fits a conventional screen -- 3x suits 320x200 and 1x suits the 1024x768 default. Set it to override either way. |
| `EMU_DOOM` | `OFF` | Fetch and build DOOM as a guest. The fetch is large -- the WAD is compiled in as a C array -- so a checkout that does not ask for it does not pay. |
| `EMU_GUEST_RAM_KIB` | `48` | Guest RAM region the images are linked against. DOOM needs `8192`; below that its rv32 image is skipped with a message naming this flag. |
| `RV_GUEST_MARCH` | see below | Guest ISA. A **cache variable**, so pass it explicitly when changing it. |

Guest images are built
`rv32imafc_zicsr_zifencei_zicbom_zicboz_zba_zbb_zbc_zbs_zacas`. `Zcb` is
supported by the emulator but deliberately absent there — compressed
guest code is *slower* to interpret, not faster. So are Zicond, Zihpm,
Zalasr, Zawrs and the two hint extensions, which the core implements and
no guest but the self-test uses: the march decides what the compiler
emits for *every* guest, and every published retired count with it. See
[docs/frontend/rv32.md](docs/frontend/rv32.md).

**`EMU_JIT_CODE_BYTES`, `RV_GUEST_MARCH` and `COREMARK_ITERATIONS` are
cache variables and silently outlive the build directory they were set
in.** Check `CMakeCache.txt` before quoting any measurement;
`scripts/report-figures.sh` prints them beside the numbers.

### Vendor driver pack

The firmware uses ST's own code wherever it exists — CMSIS-Core, ST's CMSIS
device layer (register definitions, `startup_stm32f446xx.s`,
`system_stm32f4xx.c`) and the STM32Cube HAL for clock, GPIO and USART bring-up.
Nothing reimplements a peripheral the vendor already supports.

These are fetched at configure time from ST's modular repositories. To build
offline, point at local checkouts named `cmsis_core`, `cmsis_device_f4` and
`stm32f4xx_hal_driver`:

```sh
cmake -B build/stm32f446 ... -DSTM32CUBE_LOCAL_DIR=/path/to/checkouts
```

---

## Running

```sh
./build/host/emu-host build/host/guest/isatest.bin
./build/host/emu-host --jit --quiet build/host/guest/isatest.bin
./build/host/emu-host --frontend g4mh guest.bin
./build/ppc/emu-host --jit build/ppc/guest/ppc-coremark.elf
```

`emu-host` picks a frontend from `--frontend`, else from the image's ELF
`e_machine`, else the first compiled in. A flat binary says nothing about
its architecture, so it gets the default. `--jit` selects the translating
backend for any frontend; without it, the interpreter.

Read the report at the end of a run before believing it. It says how
many instructions were translated, how many were interpreted, and how
many translation attempts were declined or overflowed — and a backend
that declines everything passes every test while proving nothing.

Useful flags: `--dump` (register file on exit), `--max-insn N`,
`--trace-skip`/`--trace-count` (with `-DEMU_ENABLE_TRACE=ON`), `--gdb`
(RSP stub on :1234, waits for a client).

### Games

DOOM runs as a guest on both frontends. It is **fetched, never
vendored**: DOOM is GPL-2.0 and this tree is Apache-2.0, so nothing of
it lives here and `tests/guest/doom.cmake` is a recipe rather than a
copy. The platform layer it needs lives in the DOOM port beside the
video driver, for the same reason.

**Two stages, and the first is not optional.** The port bakes its
texture and map tables and shrinks the WAD on the *host* before any
guest image can be built, so run the generator once:

```sh
cmake -S . -B build/doom -DEMU_PLATFORM=host -DEMU_SDL=ON \
      -DEMU_DOOM=ON -DEMU_GUEST_RAM_KIB=8192
scripts/doom-gentables.sh            # host stage: needs gcc-multilib
```

Then the image for whichever frontend, and run it:

```sh
# RISC-V
cmake --build build/doom --target guest-doom
./build/doom/emu-host --jit --ram 0x2000000 --timer-hz 6 \
                      build/doom/guest/doom.bin

# RH850 G4MH -- needs Renesas CC-RH, the only compiler that emits G4MH
cmake --build build/doom --target guest-doom-g4mh
./build/doom/emu-host --frontend g4mh --load 0x80000000 --ram 0x4000000 \
                      --jit --timer-hz 6 build/doom/guest/doom-g4mh.bin
```

`--timer-hz` is the dial worth knowing. The guest renders a few frames a
second while `mtime` and the LTSC run at wall-clock, so DOOM advances
many world-tics per drawn frame and plays absurdly fast; dividing its
clock trades simulated time for playability.

`-DDOOM_DIR=<path>` points at an existing checkout instead of fetching.

**Status.** Both frontends run it: start-up, level load and rendering,
with the framebuffer's own frame counter agreeing with the guest's.

Getting G4MH there needed four fixes in the *generation* stage rather
than the emulator, and all four were invisible on x86 -- the baked
texture arrays were two bytes short of what the struct's padding
requires, the shrunken WAD renumbered lumps that the baked tables index
by number, packed them at odd offsets that a target trapping on
misaligned access cannot read, and stripped 99 of the 138 sprites
`R_InitSpriteDefs` insists on. A second frontend is what found them.

Quake runs the same way, with the game data supplied rather than
fetched: id Software made the shareware `pak0.pak` freely
redistributable and it is still not this repository's to ship.

```sh
cmake -S . -B build/quake -DEMU_PLATFORM=host -DEMU_SDL=ON \
      -DEMU_QUAKE=ON -DQUAKE_PAK=/path/to/pak0.pak \
      -DEMU_GUEST_ROM_MIB=32 -DEMU_GUEST_RAM_KIB=32768
cmake --build build/quake --target guest-quake
./build/quake/emu-host --jit --ram 0x4000000 build/quake/guest/quake.bin
```

Neither size is optional. The PAK is 18.7 MiB and lands in `.rodata`,
so the default 16 MiB flash window cannot hold the image; and Quake's
heap is a 16 MiB static array, so `.bss` comes to about 17.5 MiB. The
build checks both and skips with a message naming the flag, rather than
failing at the link on `region overflowed`.

Both frontends run it, at 800x600:

```sh
# RH850 G4MH -- needs Renesas CC-RH
scripts/g4mh-build-quake.sh <quake-src> /path/to/pak0.pak
./build/quake/emu-host --frontend g4mh --load 0x80000000 --ram 0x4000000 \
                       --jit build/quake-g4mh.bin
```

**Status.** rv32 plays: 339 files from the PAK, console, surface cache,
`demo1.dem` loaded and running, frames the framebuffer counts, no trap
across four billion instructions. G4MH reaches the same point -- it
initialises, loads the demo and names the level -- and its build takes
the PAK in through the linker rather than the compiler, because 18.7 MB
as a C array is 93 MB of source and CC-RH runs out of memory on it.

There is no sound on either: the port's audio path is a syscall
belonging to another board, and this one has no audio device.

### Linux

Linux 6.12 boots on the RV32 frontend, through OpenSBI, with Sv32
paging and userspace in U-mode. Two targets, because there are two
different guests behind the same kernel:

```sh
cmake --build build/host --target linux         # built-in initramfs
cmake --build build/host --target linux-shell   # a root filesystem, to a prompt
```

`linux` runs a static `-nostdlib` init that prints and exits. It is
self-contained -- no root filesystem to supply -- and it is what
exercises Sv32 and the U-mode boundary.

`linux-shell` boots a real root filesystem on virtio-blk to a login
prompt you can type at (`root`, no password). Point it at one first;
this build cannot produce a root filesystem and does not try:

```sh
cmake -B build/host -DEMU_LINUX_ROOTFS=/path/to/core-image-minimal.ext4
```

A Yocto `core-image-minimal` for `MACHINE=qemuriscv32` (poky
**scarthgap**) is what this was tested against. Yocto's *own* kernel is
6.6 and does not work here -- the APLIC driver landed in 6.10 -- so it
is Yocto's root filesystem on the 6.12 kernel `scripts/run-linux.sh`
builds, and that pairing is the one that reaches a shell.

Both targets go through [`scripts/run-linux.sh`](scripts/run-linux.sh),
which builds init, the kernel and OpenSBI before it runs anything;
`LINUX_SRC`, `LINUX_BOOTARGS`, `LINUX_DISK`, `LINUX_MAXINSN`,
`LINUX_JIT` and `LINUX_ZBB` override the pieces. Ctrl-C ends the emulator; it does not
reach the guest.

Expect about two minutes: **109 s to the login prompt**, translated,
on an x86-64 host. udev is about half of that.

It used to be over fifteen minutes, and this paragraph used to call the
udev stall "an open question, not a speed problem, and no amount of
translator work will move it". It was three problems, two of them the
translator's: the JIT flushed its whole cache every time Linux turned
the FP unit off and on around a trap; its block table filled at 8192
blocks while the code buffer was a fifth used; and virtio-blk never
answered the GET_ID request udev sends to read the disk's serial. The
first two are fixed in the JIT and the third in
`src/emu/virtio/virtio_tinyemu.c`; the measurements are in
[docs/TODO.md](docs/TODO.md).

`LINUX_JIT=0` interprets instead of translating, which is what to do
when a boot misbehaves and the question is whether the translator is
why.

The machine those targets describe can also be driven by hand:

```sh
dtc -I dts -O dtb -o rv32-emu.dtb boot/rv32-emu.dts
./build/host/emu-host --supervisor --dtb rv32-emu.dtb \
                      --virtio-input --9p share:/path/to/dir <image>
```

| flag | what it does |
|---|---|
| `--supervisor` | delivers external interrupts to S-mode, where an OS under OpenSBI runs. Without it every register reads correctly and the interrupt arrives where nothing is listening. |
| `--virtio-input` | a virtio keyboard and mouse, for a driver to bind to. The simple polled devices stay; both see the same events. |
| `--9p [TAG:]DIR` | a host directory as a filesystem: `mount -t 9p -o trans=virtio,version=9p2000.L TAG /mnt` |

Devices land at `0x1000_1000` upwards, `0x1000` apart, on interrupts
from 1, **in the order the options ask for them**. The device tree has
to agree and nothing checks that it does -- they are two descriptions
of one machine, and disagreeing is silent.

The virtio devices themselves are TinyEMU's, vendored under
[third_party/tinyemu/](third_party/tinyemu/README.md) and unmodified;
the porting layer is four functions.

### The G4MH toolchain

CC-RH is the only compiler that emits G4MH and a checkout cannot assume
it. Two scripts drive it, and both take the shared start-up in
`tests/guest/g4mh/crt0.asm` -- a vector table, `gp`/`ep`, zeroed `.bss`
and `PSW.CU0`, every line of which is there because something failed
without it:

```sh
scripts/g4mh-build-guest.sh tests/guest/g4mh/barrier3.c   # one C guest
scripts/g4mh-build-doom.sh  <doom-src> [out.bin]          # what the target runs
scripts/g4mh-check-encodings.sh                           # assemble, print fields
```

`g4mh-check-encodings.sh` is the second encoder, and **the only thing
here that can say a hand-written opcode constant is wrong**. Run it
before writing one.

`g4mh-sweep` is the other direction -- what the frontend cannot *name*,
counted over a real image and ordered by how often it occurs:

```sh
cmake --build build/doom --target g4mh-sweep
./build/doom/g4mh-sweep build/doom/guest/doom-g4mh.bin 0x80000000
```

Read the slots and not the total: a flat image is code and data
together, so a real gap recurs at one sub-opcode and noise is scattered
singletons. And a `.short` is a *candidate*, not a defect -- confirm it
by executing the encoding, because this disassembler has printed
`.short` for instructions the interpreter handles correctly. The
reverse error is the dangerous one and the sweep cannot see it: a slot
it names can still be decoded wrongly.

### Watching it run

```
guest  143.5  host 6681.9  ratio 46.58  cyc 2766.2  c/g 19.28  br 1243.0  miss 17.19 M/s
```

A performance line on stderr, rewritten in place twice a second, and a
whole-run average at exit. Shown automatically on a terminal, and with
`--rate` when output is redirected -- no special build, and no pty
trick.

`ratio` is host instructions per guest instruction, which is what a
translation change moves; `c/g` is the same in cycles. The host columns
come from `perf_event_open` and need `kernel.perf_event_paranoid` at 2
or lower; anything derived from a counter that is not there shows a
dash rather than a number, because zero is a measurement and "the host
executed nothing" cannot be true.

Figures and what they have already disproved are in
[docs/performance.md](docs/performance.md).

### Validation

```sh
./scripts/run-arch-test.sh      # official riscv-arch-test, 391/391
./scripts/run-riscv-tests.sh    # Berkeley suite, 77/77
./scripts/build-matrix.sh --test # every configuration that should build, and its ctest
./scripts/report-figures.sh     # every quoted figure, regenerated
./scripts/check-doc-flags.sh    # every build flag named in the docs exists
./scripts/t2-check-encodings.sh # Thumb-2 emitters against arm-none-eabi-as
```

Both RISC-V suites also run against translated code:
`EMU_EXTRA_ARGS=--jit` for the first, and `EMU_HOST` naming a wrapper
that adds `--jit` for the second.

The other frontends have no reference model, so each is held against
something that is not this code. G4MH: Renesas' own assembler as a
second encoder, and compiled guests. ARMv7-M: **a real Cortex-M7** — the
same programs run on the Nucleo-F746ZG and on the emulator
(`tests/armv7m-diff/`). PowerPC: binutils for every encoding, a Python
model on unbounded integers, and exact rational arithmetic for the FP
unit (`tests/ppc-check/`), all three in `ctest`.

**Run both suites.** They cover different things, and a regression that
only the Berkeley suite catches will sit unnoticed if only arch-test is
run — which is exactly what happened to `rv32mi/csr` when F was added.
It runs the other way too: four `ExceptionsSv` tests failed for six days
while riscv-tests stayed at 77/77, because its guests never make a
misaligned access.

`run-arch-test.sh` **builds its own runner**, into `build/arch-test-host`
and with `-DRV32_MISALIGNED=OFF`. That is not a spare copy: the suite
validates the core against `tests/arch-test/*/{*.yaml,sail.json}`, which
declare that this core reports misaligned accesses rather than splitting
them, and the emulator's own default is the opposite because picolibc
needs it. Set `EMU_HOST` to test a binary built elsewhere, and match the
config yourself if you do.

### Debugging

```sh
cmake --build build/f746 --target gdbserver   # OpenOCD on :3333
gdb-multiarch build/f746/src/platform/stm32f746/emu-stm32f746.elf \
  -ex 'target extended-remote :3333'
```

That debugs the *emulator*. To debug the **guest**, use the built-in RSP
stub: `emu-host --gdb`, then `target remote :1234`. Two connections,
two different programs -- [docs/gdb.md](docs/gdb.md) has the rest,
including the three RSP mistakes that fail by hanging rather than
erroring.

The host `gdb` on Debian is x86-only; use `gdb-multiarch`, or
`probe-rs gdb`.

- `emu-host --dump` prints the full guest register file on exit.
- A `HardFault` on the ARM side usually means the passthrough window let
  a guest access reach an address the ARM bus rejects — the region table
  is where to look. An unimplemented address in that window makes the AHB
  signal an error, which kills the *emulator*, not the guest.

---

## Repository layout

```
include/emu/      the frontend contract, and the ISA-agnostic runtime's API
include/<isa>/    one frontend's headers: rv32, g4mh, ppc, armv7m
src/emu/          bus, passthrough, console, ELF loader, gdb stub, registry;
                  the IR, the JIT framework and the glue between them
src/backend/      IR -> host code: thumb2/, x86_64/, and an IR interpreter
src/frontend/
  rv32/           hart, CSRs, PMP, Sv32, decoder, interpreter, translator,
                  CLINT, APLIC
  g4mh/           core, decoder, interpreter, translator, MPU, INTC, timers
  ppc/            core, decoder, disassembler, interpreter, translator,
                  the embedded FP unit
  armv7m/         core, decoder, disassembler, interpreter, translator,
                  exceptions, NVIC
  softfloat/      Berkeley SoftFloat: every frontend's FP unit
src/platform/
  common/         the session and run loop every platform shares
  host/           native runner (frontend-neutral)
  stm32/          what the boards share
  stm32f446/ stm32f746/ stm32n6/   firmware: clocks, linker scripts, vendor glue
src/net/          lwIP over SLIP or PPP: telnet, gdb, TFTP
tests/
  unit/           host unit tests, per layer and per frontend
  guest/          programs that run inside the emulator
  arch-test/      DUT description for the official RISC-V suite
  armv7m-diff/    one program, on a Cortex-M7 and on the emulator
  ppc-check/      the PowerPC decoder, semantics and FP unit, each against
                  something that is not this code
scripts/          validation runners, the build matrix, encoder checks
docs/<vendor>/    reference documentation
```

Guest images (`tests/guest/`):

| Image | Purpose |
|---|---|
| `isatest` | RV32 self-test, including traps and CBO |
| `hello`   | smallest useful guest; confirms the console path |
| `bench`   | compute-bound workload for throughput measurement |
| `stm32drv`| GPIO and USART2 drivers written as guest code |
| `irqtest` | a real peripheral interrupt taken by guest code |
| `cmsistest`| the same interrupt through the CMSIS-Core shim |
| `fptest`  | floating point, and deliberately no PMP |
| `mmiobench`| driver-shaped workload for the passthrough window |
| `coremark`| CoreMark, fetched from upstream and built for RV32 |
| `dhrystone`| netlib's Dhrystone 2.1. On the host its clock is derived from the instruction count, so it compares *frontends* and not backends — see [docs/performance.md](docs/performance.md) |
| `whetstone`| Whetstone 1.2, single precision — the floating-point counterpart. Same clock caveat, and its rate depends on `WHET_LOOPS` |
| `atomics` | the A extension, LR/SC and the AMOs |
| `crypto`  | TinyCrypt AES-128 and SHA-256 against their published vectors — a real library, checked against FIPS rather than against itself |
| `fbtest`  | the framebuffer device |
| `virtiotest`| drives a virtio queue to completion and waits for the interrupt; needs `--virtio-console`, `--disk` or `--net` |
| `sv32bench-bare` / `sv32bench-paged` | one source built twice, differing in one `-D`: the same kernel in M-mode under Bare and in S-mode under Sv32 with PMP armed. What the inlined memory path is worth, and the only coverage of a *translated* block below M-mode |

ARMv7-M guests are separate, in `tests/guest/armv7m/`, because they are
built with the ARM toolchain rather than the RISC-V one and run on the
`armv7m` frontend:

| Image | Purpose |
|---|---|
| `hello` | hand-written assembly, 16-bit encodings only; the first thing that ran |
| `alu` | compiled C; found three decode defects the assembly could not |
| `itblock` | IT, ITT, ITE, ITTTT and the in-block flag rule |
| `nvic` | SysTick, exception entry and return, PRIMASK, an external source |
| `t2exec` | **the real Thumb-2 encoder, emitting into RAM and executing it** |
| `coremark`, `dhrystone`, `crypto` | the shared benchmarks, timed by SysTick -- which lands an interrupt in compiled code thousands of times a run |

Instruction-level conformance is not a guest: it is
`tests/armv7m-diff/run.py`, which runs generated instructions, raw
encodings and a system-level suite on the board and on the emulator and
compares them.

PowerPC guests are in `tests/guest/ppc/`, run on the `ppc` frontend, and
are loaded as ELF — the image's segment flag is what says whether it is
VLE or Book E:

| Image | Purpose |
|---|---|
| `isatest.bin` | hand-written VLE: the 16-bit forms and what only VLE has. A flat image, so it needs `--load 0x80000000` |
| `ppc-alu` | compiled C against values computed in Python: the forms a compiler reaches for unasked |
| `ppc-sys` | the system level, 183 checks with the manual's section beside each: interrupts, privilege, SPRs, reservations, timers, the FP interrupts |
| `ppc-park` | a `wait` that nothing may end |
| `ppc-coremark`, `ppc-crypto` | the shared benchmarks, with a decrementer interrupt landing in compiled code throughout |

---

## Documentation

| | |
|---|---|
| [docs/Architecture.md](docs/Architecture.md) | the three axes and the frontend contract |
| [docs/frontend/rv32.md](docs/frontend/rv32.md) | RV32 scope, memory map, floating point |
| [docs/frontend/g4mh.md](docs/frontend/g4mh.md) | G4MH scope, and what is *not* verified |
| [docs/frontend/ppc.md](docs/frontend/ppc.md) | e200z7 scope, the decoder, FP unit and JIT, and how each is checked without an e200 |
| [docs/frontend/armv7m.md](docs/frontend/armv7m.md) | ARMv7-M scope, the board-differential suites, the JIT |
| [docs/backend/thumb2.md](docs/backend/thumb2.md) | the ARMv7E-M emitter, and what it measures on the board |
| [docs/backend/x86_64.md](docs/backend/x86_64.md) | the x86-64 emitter, which exists for coverage |
| [docs/jit/README.md](docs/jit/README.md) | the IR pipeline, block model, FP policy |
| [docs/jit/staleness.md](docs/jit/staleness.md) | what a translated block bakes in |
| [docs/jit/tuning.md](docs/jit/tuning.md) | every knob, what it is worth, what can see it |
| [docs/jit/floating-point.md](docs/jit/floating-point.md) | host FPU or SoftFloat, and the NaN and flag rules that decide |
| [docs/validation.md](docs/validation.md) | the suites, and the bugs they caught |
| [docs/performance.md](docs/performance.md) | the measured figures |
| [docs/memory.md](docs/memory.md) | ROM, RAM and flash: who backs what, and why the platform does |
| [docs/gdb.md](docs/gdb.md) | the guest stub and the emulator stub -- two connections, different programs |
| [docs/network.md](docs/network.md) | the board over IP: telnet, gdb and TFTP, and what the handover costs |
| [docs/porting.md](docs/porting.md) | porting to another target |
| [docs/TODO.md](docs/TODO.md) | open work |
| [docs/host/](docs/host/README.md), [docs/stm32f446/](docs/stm32f446/README.md), [docs/stm32n6/](docs/stm32n6/itcm.md) | per-platform and per-platform/frontend notes |
| [BUILD.md](BUILD.md) | every build gate, and which combinations are checked |

Working notes — what has bitten, and will again — are in
[CLAUDE.md](CLAUDE.md).

---

## Licence

Apache-2.0. Vendor code fetched at build time keeps its own licences
(ST: BSD-3-Clause; ARM CMSIS: Apache-2.0).
