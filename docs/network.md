# The board over IP

**On by default** on the F746 (`EMU_NET=ON`). The board takes a guest
image over TFTP, reports over telnet and serves gdb — so running the
274-test architecture suite on hardware costs seconds per test instead of
a reflash.

There is no second wire. IP runs over the **ST-LINK's virtual COM
port**, the same one the serial console used, rather than bringing up the
on-board LAN8742A — which would mean an ETH driver, DMA descriptors
maintained by hand on a part with caches, and PHY bring-up.

Two link layers, chosen at build time with `EMU_NET_LINK`:

| | host end | why |
|---|---|---|
| `ppp` (default) | `scripts/ppp-up.sh`, which runs `pppd` and so needs root | the two ends *negotiate*: addresses come from IPCP rather than being compiled into the firmware and configured again on the host, and a link that goes down is detected |
| `slip` | `scripts/slip-up.sh`, or `scripts/slip-tun.py` without root | a framing rule in thirty lines, kept buildable because it is what there is when `pppd` is not |

> **The UART stops being a console.** `emu_net_init()` is a one-way
> handover: after it the serial line carries nothing but IP. Text and
> a framed link cannot share a wire, so this cannot be a fallback. Build
> with `-DEMU_NET=OFF` to get the serial console back.

### On a task of its own

lwIP runs on a FreeRTOS task of its own, and every call into it is made
there: starting the stack, starting gdb's listener, and each poll are
jobs `emu_os_net_call()` hands to the task. The run loop **waits** for
each job to finish. The stack still never runs beside a guest
instruction: it reads and writes guest memory and registers for gdb, and
programs the flash an image executes from, which is safe only between
slices -- where the run loop hands over. What the task buys is lwIP's
own stack (2,444 bytes of 4,096 at peak on the F746, under telnet, gdb
and TFTP) and a task a future device can wake; it is still `NO_SYS`,
with no locking in lwIP.

**The hand-off is the cost, so the run loop asks first.** Two switches,
**1,742 to 1,886 cycles** on the F746 by the board's own count, against
a slice of 60,000 to 210,000 -- 1 to 3% if it happened every slice,
which is what the inline poll did. `emu_net_wants_poll()` reads plain
state on the run loop's task and says yes only for a received byte
(`board_console_rx_ready()`, which every board now has), console or gdb
output queued for a client, or a timer due, from lwIP's own
`sys_timeouts_sleeptime()` and at least once a second. The stack's turn
comes once there is something to do. In RV32 CoreMark x120, 2.07 s on
the F746 with telnet connected, that was **12 to 14 hand-offs and
31,000-36,000 cycles**, 0.008% of the run, at 14.71-14.85 host cycles an
instruction. The run summary prints both: `os net on its own task: N
hand-offs, M cycles in switches`.

The gdb stub's replies went to `tcp_write` from wherever the stub was:
from lwIP's receive path when answering a packet, and from the run loop
when reporting a stop. They go into a 4 KiB ring now, which the poll
drains after its input, so a reply to a packet still leaves in the poll
that brought the packet.

Checked with `scripts/check-host-net.sh` -- the host on the POSIX port
and on the SMP port, and the F746 over SLIP -- and RV32 CoreMark's CRC on
the board. PPP differs only in how bytes reach the stack, which this did
not touch, and was not run: `pppd` wants the real root.

---

## Bringing it up

```sh
cmake -B build/f746 -DEMU_PLATFORM=stm32f746 \
      -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DCMAKE_BUILD_TYPE=Release
cmake --build build/f746 --target flash

./scripts/ppp-up.sh              # *after* the board prints "net ... on this port"
ping 192.168.7.2
```

(`-DEMU_NET_LINK=slip` and `sudo ./scripts/slip-up.sh` for the other
link.) Order matters: the board prints its banner on the serial line and
*then* hands the port over, so attaching first loses the banner and
attaching late is harmless — everything printed before a client
connects is held in a 4 KiB ring and delivered on connect.

| | default | option |
|---|---|---|
| board | `192.168.7.2` | `EMU_NET_ADDR` |
| host | `192.168.7.1` | `EMU_NET_PEER` |
| mask | `255.255.255.0` | `EMU_NET_MASK` |

### The same stack on the host runner

`emu-host --ppp` (in a `-DEMU_NET=ON` build) opens a pty, and `pppd`
attaches to it exactly as it attaches to `/dev/ttyACM0` -- so the lwIP,
telnet, TFTP and gdb stub a board runs become debuggable with the tools
a development machine already has:

```sh
./build/hnet/emu-host --ppp <guest>     # prints a /dev/pts/N
scripts/ppp-host.sh /dev/pts/N          # pppd, which needs root
```

`--ppp=DEV` uses an existing device instead of a new pty. It took the
device as the next argument until 2026-10, so the line above took the
guest for a device and printed the usage -- for as long as nothing ran
it, which the next paragraph is about.

The runner's default subnet is 192.168.8.x, so it can be up beside a
board on 192.168.7.x.

**And it is tested without root.** `scripts/check-host-net.sh` runs
`scripts/check-board-net.sh --no-flash` -- ping, a guest uploaded over
TFTP and graded from the telnet console, and gdb reading the pc --
against a `-DEMU_NET=ON -DEMU_NET_LINK=slip` runner, from inside
`unshare -rn`: a network namespace of its own, where a TUN device needs
no privilege and `scripts/slip-tun.py` bridges it to the runner's pty
as it does to a board's UART. PPP would need `pppd`, which wants the
real root; the stack, the console, TFTP and the stub are the same code
over either link. The matrix's `host-slip` row runs it under `--test`
(7 s), and `host-net` still only builds the PPP form.

```sh
cmake -S . -B build/hnet -DEMU_NET=ON -DEMU_NET_LINK=slip
cmake --build build/hnet
scripts/check-host-net.sh build/hnet
```

**The same, against a board.** `--board DEV` flashes a SLIP firmware
and bridges the namespace's TUN to the board's UART instead -- the port
opens inside the namespace like any other file -- and `-- CMD` runs a
command there in place of the checks, with `EMU_NET_ADDR` set. Which
is how a suite runs on hardware with no root:

```sh
cmake -S . -B build/f746slip2m -DCMAKE_TOOLCHAIN_FILE=cmake/arm-none-eabi.cmake \
      -DEMU_PLATFORM=stm32f746 -DEMU_NET=ON -DEMU_NET_LINK=slip \
      -DEMU_MAX_INSN=2000000 -DEMU_PROBE=<vid:pid:serial>
cmake --build build/f746slip2m --target flash
scripts/check-host-net.sh --board /dev/serial/by-id/<the board's> build/f746slip2m -- \
    env EMU_HOST=$PWD/scripts/emu-board scripts/run-riscv-tests.sh
```

`EMU_MAX_INSN=2000000` because the shim cannot pass `--max-insn`, and
the Berkeley tests end by looping (docs/validation.md): at the default
100 M each one would spend twenty seconds parked. `scripts/emu-board`
uploads one file -- an ELF stripped, or a flat image given `--load`
wrapped in a one-segment ELF at that address, so the board copies it to
RAM. It sent two halves, `rom` and `ram`, from before the board took one
file, so each half had been arriving as an image of its own.

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
tftp 192.168.7.2 -c put guest.bin       # or an ELF; the name is not read
```

One file, into the flash arena, then the guest restarts on it. An ELF
is placed by its program headers -- text mapped from flash where it
lies, writable segments copied to RAM -- and a flat image runs in place.
It used to be two pseudo-files, `rom` and `ram`, while the board had to
be told where the read-only part ended; the guest copies its own `.data`
now, so there is nothing to tell it.

What makes it fragile, and what each costs:

- **There is no length in a TFTP request**, so running out of arena is
  how its end is discovered: the write fails, the board erases, and the
  client's retry succeeds. One transfer per erase cycle is meant to
  fail.
- **lwIP's TFTP server keeps one session and never reclaims it**, so a
  client killed mid-transfer would wedge every later upload behind "Only
  one connection at a time is supported". `emu_net_tftp_poll()` is a
  watchdog that rebuilds the server after 8 s of silence. It triggers on
  idleness rather than on the server's own state because lwIP refuses a
  request before `ctx->open()` is ever reached -- a watchdog gated on
  the wrong observable is a watchdog that never fires.
- **lwIP closes a finished transfer and an abandoned one the same way**:
  `close()` on the last block, on its own timeout, and from the
  watchdog's `tftp_cleanup()`, with nothing to say which. `write()` sees
  every block first, the empty one ending a multiple of 512 bytes
  included, so a transfer is complete exactly when its last block was
  short. Anything else is skipped -- the bytes are passed over in the
  arena, not erased -- and the console says `upload abandoned after N
  bytes; nothing run`.

  The board used to commit an abandoned transfer and run it, and every
  first upload after a flash was one: that upload erased the arena
  first, three 256 KiB sectors, during which the board hears nothing;
  the client retransmitted its request, was refused as a second
  session, and stopped after one block; and the watchdog committed the
  512 bytes and ran them. The F746 no longer erases there if the
  arena's blank tail is at least 64 KiB, which after a reflash it
  usually is. `check-board-net.sh` sends one block and goes silent, and
  requires the board to say it dropped it; with the commit put back that
  check fails, on the host and on the board.

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
