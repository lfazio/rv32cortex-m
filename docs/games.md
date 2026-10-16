# DOOM and Quake

Benchmarks with a real frame rate rather than a checksum, and the first
guests big enough to make the JIT's figures mean something. Both run on
RV32 and on G4MH at 800x600; recipes are in the README, and the ports --
GPL, like the games -- live in their own repositories, never in this
tree: [embeddedDOOM](https://github.com/lfazio/embeddedDOOM) (branch
`rv32cortex-m`) and [quake-embedded](https://github.com/lfazio/quake-embedded).
What is still open, PowerPC above all, is in [TODO.md](TODO.md#guests).

**DOOM and Quake 1 both run, on rv32 and on G4MH**, at 800x600:
DOOM loads E1M1 and plays, Quake initialises, loads `demo1.dem`
and runs it. Recipes are in the README; the game data is supplied
rather than fetched. What they bought beyond being playable was
four emulator defects neither test suite could see -- a JIT that
read a load's displacement as a floating-point opcode, a guest
runtime with no thread pointer, misaligned access the C library
assumes, and a flash window sized for smaller images.

## Which frontends can run them

The order was rv32, then g4mh, then ppc, and **what set it was
toolchains rather than the emulator** -- the table as it stood when
the work was planned:

| frontend | JIT | C guest possible | blocker |
|---|---|---|---|
| rv32 | yes | yes | none -- CoreMark already runs 1.5G instructions |
| g4mh | yes | **yes** | none known -- CC-RH is installed, see below |
| ppc  | yes | **yes, as Book E** | `powerpc-linux-gnu-gcc` is not built with VLE (`-mvle` is rejected), but the core runs classic Book E too and GCC generates that: CoreMark and crypto run on both backends. What is missing for a game is a 32-bit `libgcc` and a C library for that build |

**CC-RH is installed** -- V2.07.00 and V2.08.00 under
`/usr/local/Renesas/CC-RH/`, with `ccrh`, `asrh` and `rlink`. Both
report *"Paid license of CC-RH V2 is not found, and the evaluation
period has expired"* at link time, and both link anyway: a warning,
not an error. An 8,000-line source compiled to a 927 KB object
without complaint, so there is no obvious size cap -- but nothing
Doom-sized has been linked, and that is the thing to test before
planning around it rather than after.

So rv32 went first because nothing blocked it, and g4mh followed
with CC-RH (`scripts/g4mh-build-doom.sh`, `g4mh-build-quake.sh`). ppc
was the far one; it has a JIT now and runs compiled Book E, so what is
left there is a runtime library and a C library for that build.

## The framebuffer, the window and the input

**A framebuffer device**, portable C in `src/emu/` with no SDL in
it -- the guest writes pixels, the host presents them. Same split
as the NS16550 and the console: the *device* is portable and the
platform owns the window, or it will not build for the F746. A
plain framebuffer rather than `virtio-gpu`, which buys nothing
until something wants 3D acceleration this emulator does not have.
**Done** -- `src/emu/emu_fb.c`. Why it is not an emulated VGA, and
what one would cost, is in [`docs/vga.md`](vga.md): the deciding
factor is that VGA's write modes live in the store path, so every
pixel would become a device dispatch.

`--fb WxH` picks the mode it starts in, 1024x768 by default, and
refuses a geometry the device's table does not hold -- otherwise
the geometry registers would report a size MODE_GET matches
nothing for, and a guest enumerating modes could not find the
one it is already in. It only moves the *starting* mode: the
buffer is sized for the largest either way, so a guest that sets
its own through the mode registers is unaffected.

**An SDL host backend** to present it. `src/platform/host/
sdl_display.c`, behind `-DEMU_SDL=ON`, against **sdl3** rather
than the SDL2 this entry was written expecting. Off by default,
because the device above must build and be testable on a host
that has neither -- which is how the F746 gets a framebuffer at
all.

**Input**, keyboard and mouse, from SDL events. Two devices and
not one: a host binds a separate evdev to each, and a combined
descriptor claiming both is not what any driver expects. There
are now two *pairs* -- these simple polled rings, which DOOM and
Quake use, and the virtio pair an operating system's driver
binds to. Both are fed from the same converted evdev codes, so
they cannot disagree about what a key is.

## DOOM on RV32

**DOOM on rv32 -- it plays.** Full startup, E1M1, frames through
the framebuffer, keyboard and mouse live.

`-DEMU_DOOM=ON`, then `scripts/doom-gentables.sh`, then
`guest-doom`; run with `--jit --ram 0x2000000 --timer-hz 6`.
The port layer is GPL and lives in the port
(https://github.com/lfazio/embeddedDOOM, branch `rv32cortex-m`);
nothing of DOOM's is in this tree.

`--timer-hz` is the dial that matters. The guest renders a few
frames a second while `mtime` runs at wall-clock, so DOOM
advances many world-tics per drawn frame and the game appears to
fast-forward. Dividing guest time back down matches its own
rate -- 6 suits this machine, another will differ.

## Quake

**Quake on rv32 -- it runs.** Initialises, loads `demo1.dem`
and plays it, on rv32 and on G4MH, at 800x600. It was the
harder target and it answered the question it was set: the JIT
holds up under floating point at scale, and the host/guest
ratio on it (46.6) is *better* than Dhrystone's (52.6), which
has no floating point at all -- which is what refuted the
theory that SoftFloat was the cost.
https://github.com/lfazio/quake-embedded
