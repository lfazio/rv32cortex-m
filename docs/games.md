# DOOM and Quake

Benchmarks with a real frame rate rather than a checksum, and the first
guests big enough to make the JIT's figures mean something. Both run on
RV32 and on G4MH at 800x600, and DOOM on PowerPC too; recipes are in
the README, and the ports --
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
| ppc  | yes | **yes, as Book E** | `powerpc-linux-gnu-gcc` is not built with VLE (`-mvle` is rejected), but the core runs classic Book E too and GCC generates that: CoreMark and crypto run on both backends. What was missing for a game was a 32-bit `libgcc` and a C library for that build -- [built since](#doom-on-powerpc) |

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
was the far one, and what it lacked was never the emulator: a runtime
library and a C library, which `scripts/ppc-build-runtime.sh` now
builds -- see [DOOM on PowerPC](#doom-on-powerpc).

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

## DOOM on PowerPC

**It plays** -- start-up, E1M1, the demo, frames through the same
framebuffer -- on the e200z7 as compiled Book E, under the JIT. Built
by a script rather than a CMake target, for G4MH's reason: the guest
links a runtime this tree's CMake does not configure.

```sh
scripts/ppc-build-runtime.sh                 # once: picolibc + compiler-rt
scripts/ppc-build-doom.sh <doom-src>         # after scripts/doom-gentables.sh
./build/host/emu-host --jit --ram 0x2000000 --timer-hz 6 \
                      build/guest/doom-ppc.elf
```

**The runtime is what was missing, and neither half is GPL.** Debian's
powerpc64 cross compiler in `-m32` mode ships no 32-bit `libgcc` and no
C library, so there was nowhere for a 64-bit division, a soft-float
`double` or `printf` to come from. `ppc-build-runtime.sh` fetches and
builds both into `build/ppc-runtime/`, never vendored:

| library | licence | what it supplies |
|---|---|---|
| picolibc 1.8.10 | BSD | the C library the RV32 guests already use |
| compiler-rt's builtins, LLVM 19.1.7 | Apache-2.0 WITH LLVM-exception | the generic C builtins GCC calls where `libgcc` would be: 64-bit division and shifts, soft-float `double` and `float`, the conversions |

Both are built with the guests' flags plus `-mlong-double-64`, because
neither builds IBM's 128-bit `long double` for this target. picolibc
goes without thread-local storage, so `errno` is a global: nothing sets
`r2` as a thread pointer, and the F746 has already shown what a
`__tls` errno does when nobody does (CLAUDE.md, "a C library's thread
pointer is start-up code"). `fp_mode.c` is in the builtins list because the soft-float
arithmetic asks it for the rounding mode, and the link says so if it is
missing.

The image is an ELF linked at 0x80000000 by `tests/guest/ppc/bigguest.ld`
-- `cguest.ld`'s layout, in 32 MiB -- and runs from guest RAM. The ELF
loader places it, so it needs no `--load`.

**The first big-endian target the port has had, and three things only
it could find:**

| what was wrong | why the little-endian targets did not care |
|---|---|
| `m_swap.c` carried id's inverted `#ifndef __BIG_ENDIAN__`, and its definitions disagreed with `m_swap.h`'s declarations | `SHORT()` and `LONG()` are the identity there, so the functions were never compiled where they are used |
| `W_CheckNumForName` read the wanted name as a native `int` and the lump's through `NAME4`, which is little-endian by construction | the two orders agree; here every lookup failed, and the first, `PNAMES`, is fatal |
| the baked tables hold raw `texture_t` and `node_t` bytes, dumped on a little-endian host | -- |

The first two are fixed in the port. The comment above `NAME4` said
"both sides of every comparison go through it", and one side did not:
a comment asserting an invariant is where to look first, again. The
third is fixed in the build: there is no big-endian host to regenerate
on, so `ppc-build-doom.sh` swaps each `texture_t`'s three shorts and
its patches' three ints, and each `node_t`'s twelve ints and two
shorts, into copies -- 125 texture arrays and 3,414 nodes. Everything
else baked is C integers and means the same either way, and the 8-bit
indexed framebuffer has no byte order.

`R_Init` warns about 197 missing patches. RV32 prints the same 197:
they are what the WAD shrinker removed, not something PowerPC lost.

**What it runs at is DOOM's ceiling, not the emulator's.** On the
x86-64 JIT at `--timer-hz 1` -- the guest's clock at wall speed, so the
game is never ahead of it -- 3e9 instructions, twice each:

| frontend | frames a second |
|---|---|
| PowerPC | 34.5, 34.4 |
| RV32 | 34.3, 34.3 |

Both are DOOM's 35 Hz tic rate. The game waits for its clock on either
frontend, so this host cannot tell them apart, and the figure is a
ceiling rather than a comparison.

**What it still interprets is the time base, deliberately.** `mfspr`
of TBU or TBL ends a block (`must_decline` in `ppc_ir.c`): on the
processor clock the time base counts retired instructions, and a block
reports how many it ran only when it leaves, so a read from inside one
would be short by the block so far. DOOM's clock reads it three times a
call -- 15.2M interpreted instructions and 10.1M declined block entries
in a 3e9 run, 0.5%. On the platform clock, which DOOM selects
(`HID0[SEL_TBCLK]`), the read would be exact; but whether a block may
lower it would then depend on HID0, a translate-time read of mutable
state, and with the frame rate pinned at 35 on both frontends there is
nothing to pay for that. Worth revisiting for a guest that is
CPU-bound *and* polls the time base.

**`--fb-dump FILE`** writes the last frame the guest presented as a PPM
when the runner exits, which is how this was checked with no window:
the picture, not just the console. `guest-fbtest-dump` holds it to the
framebuffer test's own frame count and geometry.

The port layer, `i_platform_ppc.c`, lives in the DOOM port beside the
other two, for the licence reason the rest of it does.

## Quake

**Quake on rv32 -- it runs.** Initialises, loads `demo1.dem`
and plays it, on rv32 and on G4MH, at 800x600. It was the
harder target and it answered the question it was set: the JIT
holds up under floating point at scale, and the host/guest
ratio on it (46.6) is *better* than Dhrystone's (52.6), which
has no floating point at all -- which is what refuted the
theory that SoftFloat was the cost.
https://github.com/lfazio/quake-embedded
