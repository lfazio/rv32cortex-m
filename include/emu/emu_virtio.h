/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_virtio.h - virtio-mmio devices, over TinyEMU's implementation.
 *
 * The devices themselves are `third_party/tinyemu/virtio.c`, vendored
 * unmodified; `src/emu/virtio/virtio_glue.c` is the four functions it
 * calls into its host. This header is what the *platform* uses, and it
 * exists so nothing outside src/emu/virtio/ has to know TinyEMU's types
 * -- a platform names an address, an interrupt and a backing file.
 *
 * **Everything here is virtio-mmio.** The PCI transport is refused at
 * run time with a message; see virtio_nopci.c for why that is better
 * than a stub.
 *
 * The guest finds these through its device tree, which must agree with
 * the addresses and interrupt numbers passed here. Nothing checks that
 * agreement -- it is two descriptions of one machine, and the usual
 * failure is a driver that probes and finds nothing.
 */

#ifndef EMU_VIRTIO_H
#define EMU_VIRTIO_H

#include <stdbool.h>
#include <stdint.h>

#include "emu/emu_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How a device raises its interrupt line.
 *
 * The platform supplies this because only it knows what is listening:
 * on the RISC-V host that is the APLIC, and on a machine with no
 * interrupt controller there is nothing to call. `level` is 0 or 1 --
 * virtio-mmio is level triggered, and the device lowers the line when
 * the guest acknowledges by writing InterruptACK.
 */
typedef void (*emu_virtio_irq_fn)(void *ctx, int irq_num, int level);

/*
 * Bring up the subsystem. Called once, before any device is added.
 *
 * The bus is where devices are mapped and where their queues are read
 * from: virtio walks descriptor rings in guest RAM directly, through
 * emu_bus_host_ptr, so a bus whose RAM is not host-backed cannot carry
 * these devices at all.
 */
bool emu_virtio_init(emu_bus_t *bus, emu_virtio_irq_fn irq, void *irq_ctx);

/*
 * A console on virtio-mmio.
 *
 * Separate from the NS16550 the platform already provides, and not a
 * replacement for it: a Linux guest uses the NS16550 or the SBI console
 * for early output and can move to this one once drivers are up. Both
 * can exist, and on this emulator both end up at the same terminal.
 */
bool emu_virtio_add_console(uint32_t base, int irq_num);

/*
 * A filesystem, as virtio-9p.
 *
 * `root` is a host directory the guest sees; `tag` is the name it
 * mounts by:
 *
 *     mount -t 9p -o trans=virtio,version=9p2000.L <tag> /mnt
 *
 * 9p rather than virtio-blk for the first filesystem because it needs
 * no image to build and no partition table to get right -- the host
 * directory *is* the filesystem, so there is nothing between changing a
 * file and the guest seeing it.
 */
bool emu_virtio_add_9p(uint32_t base, int irq_num, const char *tag,
                       const char *root);

/*
 * How many devices have been added, which is what a device-tree builder
 * needs to know and what a start-up banner should report.
 */
unsigned emu_virtio_count(void);

#ifdef __cplusplus
}
#endif

#endif /* EMU_VIRTIO_H */
