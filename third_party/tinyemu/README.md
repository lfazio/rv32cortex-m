# TinyEMU, vendored

`virtio.c`, `virtio.h` and `list.h` from
[fernandotcl/TinyEMU](https://github.com/fernandotcl/TinyEMU), which is
Fabrice Bellard's TinyEMU with that fork's changes. **MIT**, and the
licence text is at the top of each file where Bellard put it.

## Why vendored, when everything else here is fetched

CoreMark, TinyCrypt, DOOM and Quake are fetched by the build and never
copied in. This is not, for two reasons that do not apply to those.

It is **three files and no build system** -- 2650 lines of `virtio.c`
plus two headers -- against a repository with its own CMake, its own
CPU emulators and its own SDL frontend, none of which this tree wants.
Fetching would mean cloning all of that to take three files out of it.

And it is **MIT**, so it can live beside Apache-2.0 code with its
copyright intact, which is exactly what a vendored copy preserves and
what the DOOM and Quake ports could not do.

## What is modified

**Nothing.** The files are byte-identical to upstream, so a future
update is a copy rather than a merge. Everything this emulator needs to
supply -- a bus, a memory map, an interrupt line -- is in
`src/emu/virtio/virtio_glue.[ch]`, which implements the four functions
`virtio.c` calls into its host:

| TinyEMU | this tree |
|---|---|
| `cpu_register_device` | `emu_bus_add_mmio` |
| `phys_mem_get_ram_ptr` | `emu_bus_host_ptr` |
| `set_irq` | the frontend's interrupt hook |
| `mallocz`, `get_le32`, ... | a few lines of `cutils` |

That table is the whole of the porting layer, which is why this was
worth importing rather than writing.

## What is wrapped

One behaviour, without touching the file. `virtio.c` is not compiled on
its own: `src/emu/virtio/virtio_tinyemu.c` `#include`s it, which puts its
`static` device struct and queue helpers in reach, and replaces the
block device's request handler with one that delegates IN and OUT and
**completes everything else**. Upstream lets any other request type fall
through a `default: break;` -- no status, descriptor never returned --
and Linux sends one on every boot: `VIRTIO_BLK_T_GET_ID`, to read
`/sys/block/vda/serial` for udev. The worker that asked blocked in the
kernel and `udevadm settle` waited out its 120 seconds. GET_ID now
answers with an empty serial, as QEMU does for a disk configured without
one; anything else unknown gets `VIRTIO_BLK_S_UNSUPP`.

An update is still a copy. If upstream reshapes what the wrapper uses,
it stops compiling -- the same failure the glue relies on.

## Updating

Copy the three files from upstream and rebuild. If `virtio.c` gains a
call into its host, the build breaks at the link with the name of it --
which is the right failure, and the reason the glue defines exactly
what is used and nothing more.
