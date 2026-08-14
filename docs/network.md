# The board over IP

**On by default** on the F746 (`EMU_NET=ON`). The board takes a guest
image over TFTP, reports over telnet and serves gdb — so running the
274-test architecture suite on hardware costs seconds per test instead of
a reflash.

There is no second wire. SLIP runs over the **ST-LINK's virtual COM
port**, the same one the serial console used, rather than bringing up the
on-board LAN8742A — which would mean an ETH driver, DMA descriptors
maintained by hand on a part with caches, and PHY bring-up.

> **The UART stops being a console.** `emu_net_init()` is a one-way
> handover: after it the serial line carries nothing but IP. Text and
> SLIP cannot share a wire, so this cannot be a fallback. Build with
> `-DEMU_NET=OFF` to get the serial console back.

---

## Bringing it up

```sh
cmake -B build/f746 -DEMU_PLATFORM=stm32f746 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build/f746 --target flash

sudo ./scripts/slip-up.sh        # *after* the board prints "net SLIP on this port"
ping 192.168.7.2
```

Order matters: the board prints its banner on the serial line and *then*
hands the port over, so attaching SLIP first loses the banner and
attaching it late is harmless — everything printed before a client
connects is held in a 4 KiB ring and delivered on connect.

| | default | option |
|---|---|---|
| board | `192.168.7.2` | `EMU_NET_ADDR` |
| host | `192.168.7.1` | `EMU_NET_PEER` |
| mask | `255.255.255.0` | `EMU_NET_MASK` |

## What is served

| port | service | use |
|---|---|---|
| 23 | telnet | the console: banner, guest output, traces, results |
| 1234 | gdb RSP | `target remote 192.168.7.2:1234` — debugs the **guest** |
| 69/udp | TFTP | upload a guest image and run it without reflashing |

### Traces and console — telnet

```sh
telnet 192.168.7.2
```

Raw TCP carrying exactly what the serial console carried. Everything
printed before you connect is buffered, so the boot banner survives —
which is what makes it usable at all, since the banner is where the IP
address and the guest RAM figure are printed.

With `-DEMU_ENABLE_TRACE=ON` the per-instruction trace comes out here
too. It is slow and it is the fastest way to find where execution
diverges: read the **pc deltas**, because an instruction that changes the
pc without retiring is a trap.

### gdb — port 1234

```sh
gdb-multiarch build/f746/guest/isatest.elf \
  -ex 'target remote 192.168.7.2:1234'
```

This debugs the **guest**, not the firmware — breakpoints are guest pcs.
To debug the emulator itself, use OpenOCD on the SWD port instead; see
[gdb.md](gdb.md), which also covers why breakpoints are a pc list rather
than a patched instruction.

The stub is started after the guest exists and before it runs, so a
debugger that connects immediately finds the guest at its reset vector.
Failure to start is not fatal: a board that cannot serve gdb still runs
guests, and saying so beats halting.

The register layout follows the frontend built in, not the platform —
gdb's `g` packet is a fixed per-architecture concatenation and gdb does
not ask.

### Guest images — TFTP

```sh
tftp 192.168.7.2 -c put guest.ro rom
tftp 192.168.7.2 -c put guest.rw ram
```

Two pseudo-files, `rom` and `ram`, matching the split the baked-in image
already has. Both halves go to the flash arena back to back, which makes
an uploaded image identical in shape to a baked-in one — an earlier
version wrote the writable half straight into guest RAM, where
`start_guest()` promptly zeroed it.

Two things that make this fragile in ways worth knowing:

- **There is no length in a TFTP request**, so running out of arena is
  how the end of the arena is discovered. Recovery is to erase and let
  the client retry — and it has to be wired for *both* halves, because
  the client sends rom then ram and the transfer that overruns is almost
  always the second.
- **lwIP's TFTP server keeps one session and never reclaims it**, so a
  client killed mid-transfer would wedge every later upload behind "Only
  one connection at a time is supported". `emu_net_tftp_poll()` is a
  watchdog that rebuilds the server on idleness. It triggers on idleness
  rather than on the server's own state because lwIP refuses a request
  before `ctx->open()` is ever reached — a watchdog gated on the wrong
  observable is a watchdog that never fires.

## Costs, measured

| | guest RAM |
|---|---|
| `EMU_NET=OFF` | 282 KiB |
| `EMU_NET=ON` | **265 KiB** |

Against the worst architecture test's 222 KiB, so the suite still fits.
`rx drops 0` across a full `isatest` run.

## Two traps

**`__WFI` stops the clock lwIP tells the time by.** `sys_now()` comes
from `board_cycles()`, which is DWT CYCCNT — a counter of *processor*
cycles, gated by `__WFI`. Parking in it slowed the stack's notion of time
to **6% of real time**: 29 seconds of wall clock advanced lwIP's clock by
1.74. Every timeout in the stack is slowed by that factor, which became
fatal in TFTP, whose 10-second session timeout then needs ~3 minutes. The
park loop no longer sleeps while the stack is up.

**The host end of the wire has two settings that fail silently.**
`slattach -s 921600` does not work — net-tools maps `-s` through a fixed
table of `Bxxx` constants that stops short of it, and fails the open
outright. That one at least says so. The other does not: **slattach
clears `CSIZE` without setting it**, so the line comes up at **cs5**, and
five data bits against the board's eight means every frame misassembles
and SLIP discards all of it. `stty raw` does not touch `CSIZE` either.
`scripts/slip-up.sh` sets the line with `stty` before and after slattach
and prints what it ended up with, because a mis-framed line is otherwise
silent — the board looks broken and every check on the board passes.
