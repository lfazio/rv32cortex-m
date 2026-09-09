# A VGA controller, and why this tree has a framebuffer instead

Asked whether the display device should be an emulated VGA/SVGA rather
than the plain framebuffer in `src/emu/emu_fb.c`. This note records what
that would mean, because the answer is not obvious and the reasoning is
worth having written down before someone re-opens it.

Reference for the register set: the VGADOC collection (`VGAREGS.TXT` and
its companions), which documents the IBM VGA and the common SVGA
extensions. Read it for the register semantics; nothing here is copied
from it.

## What a VGA actually is

Six independent register groups, most reached through an index/data port
pair, plus a memory window with four planes behind it.

| group | ports | what it holds |
|---|---|---|
| Miscellaneous output | `0x3C2` write, `0x3CC` read | clock select, mono/colour I/O mapping, page bit |
| Sequencer | `0x3C4` index, `0x3C5` data | reset, dot clock, **map mask** (per-plane write enable), memory mode including chain-4 |
| CRT controller | `0x3D4` index, `0x3D5` data | ~25 registers of timing, plus **start address**, **offset** (logical line width), max scan line, and a mode control that selects byte/word/doubleword addressing |
| Graphics controller | `0x3CE` index, `0x3CF` data | set/reset and its enable, colour compare, data rotate and logical operation, read map select, **write mode 0-3**, memory-map select, **bit mask** |
| Attribute controller | `0x3C0`, index and data alternating | 16 palette entries, mode control, overscan, pixel panning; its index/data flip-flop is reset by *reading* the input status register at `0x3DA` |
| DAC | `0x3C8` write index, `0x3C7` read index, `0x3C9` data, `0x3C6` pixel mask | 256 entries of 6-bit R, G, B, written as an auto-incrementing byte stream |

The memory window at `0xA0000` is four 64 KB planes, not a linear
buffer, and every access goes through four byte-wide **latches**.

## The part that decides it

VGA's write path is a small dataflow machine that runs **per byte
written**:

- **write mode 0** rotates the data, substitutes the set/reset colour on
  planes where set/reset is enabled, applies a logical operation (AND,
  OR, XOR) against the latches, and then uses the bit mask to choose,
  bit by bit, between the computed value and the latch;
- **write mode 1** writes the latches straight through, which is how a
  fast block copy works;
- **write mode 2** expands the low four bits of the value across the
  planes under the bit mask;
- **write mode 3** ANDs the data with the bit mask and writes the
  set/reset colour through it.

Reads are their own pair of modes, one returning a selected plane and
one returning a colour-compare result across all four.

**That machinery has to live in the store path.** On this bus that means
the `0xA0000` window is MMIO, so every pixel a guest writes becomes a
device dispatch through a bit-mask state machine — 64,000 of them per
frame at 320x200, before anything is drawn twice.

`emu_fb.c` exists to avoid exactly that: its pixels are an ordinary RAM
region, so a guest store is a store, and the device is only the handful
of cold registers that say how big the buffer is and when to show it. A
VGA is the opposite arrangement, and the cost is not a detail of the
implementation — it is what a VGA *is*.

## When it would nonetheless be right

**If the goal is unmodified period software.** A DOS-era binary talks to
these registers because there was nothing else to talk to, and no
framebuffer device will help it. If someone wants to run a real
`i_video.c` against real hardware behaviour, VGA is the honest target and
a custom framebuffer is a port.

That is not this project's goal. Both games named in `TODO.md` are
already ports -- `embeddedDOOM` and `quake-embedded` exist because their
video layers were replaced for bare-metal targets. They want "here is a
buffer, here is a palette, tell me when you have drawn". Implementing
planar write modes so that a port can drive them linearly is work spent
to arrive back where the framebuffer already is.

Worth knowing about Doom specifically: it did not use the linear
mode 13h that the textbooks show. It used the unchained 320x200 planar
arrangement (commonly "Mode X"/"Mode Y"), reached by turning chain-4
*off*, because that gives page flipping through the CRTC start address
and lets a clear write four pixels at once through the map mask. So even
"just support the mode Doom used" means the sequencer, the map mask, and
CRTC start-address flipping — not a linear buffer.

SVGA does offer a way out: VBE 2.0's linear framebuffer removes the
planes and the bank switching, at which point the device is a
framebuffer with a much larger register surface in front of it. If VGA
is ever wanted here, that is the shape to aim for.

## The middle option, if authenticity is the appeal

Keep the RAM-backed buffer exactly as it is and add a small
VGA-*compatible* façade over the parts that are cheap because they are
not in the write path:

- the DAC at `0x3C8`/`0x3C9`, with its auto-incrementing 6-bit RGB byte
  stream, so palette code written the DOS way works unchanged;
- the CRTC start-address pair, so page flipping means changing an offset
  rather than copying a frame.

Both are cold registers touched a few times per frame. Neither requires
planes, latches or write modes. That is perhaps a hundred lines and
costs the store path nothing.

## What is actually decided

Nothing here is built. The framebuffer is, and the games are ports that
want a framebuffer. This note exists so that the next person to ask
"should it be a VGA?" gets the register map, the reason the write path
is the deciding factor, and the middle option — rather than starting the
survey again.
