/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_virtio.c - the platform's view of virtio.
 *
 * TinyEMU's virtio.c wants a VIRTIOBusDef (a memory map, an address and
 * an IRQSignal) and a backend per device. This turns "an address, an
 * interrupt number and a host directory" into those, so nothing outside
 * this directory sees TinyEMU's types.
 */

#include "emu/emu_virtio.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "virtio_glue.h"
/*
 * virtio.h only. It includes fs.h itself, and **fs.h has no include
 * guard**, so naming it here as well redefines every type in it.
 */
#include "virtio.h"

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/*
 * One memory map for the whole machine, and one IRQ signal per device.
 *
 * Static, and bounded by the same constant the glue uses for its port
 * table: a machine has a handful of virtio devices at addresses its
 * device tree names, so running out is a start-up failure rather than
 * something to allocate around.
 */
#ifndef EMU_VIRTIO_MAX_DEVICES
#define EMU_VIRTIO_MAX_DEVICES 8
#endif

static PhysMemoryMap g_map;
static IRQSignal g_irqs[EMU_VIRTIO_MAX_DEVICES];
static VIRTIODevice *g_devs[EMU_VIRTIO_MAX_DEVICES];
static unsigned g_ndevs;
static bool g_ready;

static emu_virtio_irq_fn g_irq_fn;
static void *g_irq_ctx;

/*
 * TinyEMU's IRQSignal calls a SetIRQFunc; the platform supplies an
 * emu_virtio_irq_fn. The two differ only in the opaque, so this is the
 * adapter and there is nothing else to it.
 */
static void virtio_raise(void *opaque, int irq_num, int level)
{
    (void)opaque;

    if (g_irq_fn != NULL) {
        g_irq_fn(g_irq_ctx, irq_num, level);
    }
}

bool emu_virtio_init(emu_bus_t *bus, emu_virtio_irq_fn irq, void *irq_ctx)
{
    if (bus == NULL) {
        return false;
    }

    g_map.bus = bus;
    g_irq_fn = irq;
    g_irq_ctx = irq_ctx;
    g_ndevs = 0u;
    g_ready = true;
    return true;
}

unsigned emu_virtio_count(void)
{
    return g_ndevs;
}

/*
 * Fill in the bus description for one device.
 *
 * `pci_bus` stays NULL, and that is what selects the MMIO transport
 * inside virtio.c -- it branches on the pointer rather than on a flag.
 * Worth stating because it is the one place the transport is chosen and
 * it does not look like a choice.
 */
static VIRTIOBusDef *bus_def_for(uint32_t base, int irq_num,
                                 VIRTIOBusDef *out)
{
    if (!g_ready || g_ndevs >= EMU_VIRTIO_MAX_DEVICES) {
        fprintf(stderr, "virtio: not initialised, or more than %d devices\n",
                EMU_VIRTIO_MAX_DEVICES);
        return NULL;
    }

    irq_init(&g_irqs[g_ndevs], virtio_raise, NULL, irq_num);

    memset(out, 0, sizeof(*out));
    out->pci_bus = NULL;
    out->mem_map = &g_map;
    out->addr = base;
    out->irq = &g_irqs[g_ndevs];
    return out;
}

static bool remember(VIRTIODevice *dev, const char *what, uint32_t base)
{
    if (dev == NULL) {
        fprintf(stderr, "virtio: could not create %s at 0x%08x\n", what,
                (unsigned)base);
        return false;
    }

    g_devs[g_ndevs] = dev;
    g_ndevs++;
    return true;
}

/* ------------------------------------------------------------------ */
/* Console                                                             */
/* ------------------------------------------------------------------ */

/*
 * The guest's console, which on this emulator is the host's terminal --
 * the same place the NS16550 goes.
 *
 * Output is unbuffered on purpose. A guest that stops mid-line has
 * usually just told you why, and buffering is what loses that last
 * line; this tree has already recorded one case of a firmware halting
 * with its reason sitting in a ring nobody could reach.
 */
static void console_write(void *opaque, const uint8_t *buf, int len)
{
    (void)opaque;
    (void)fwrite(buf, 1u, (size_t)len, stdout);
    (void)fflush(stdout);
}

/*
 * No input. Returning 0 means "nothing available", which the driver
 * handles; the alternative -- reading the host's stdin here -- would
 * fight the NS16550 for the same terminal, and two consoles sharing one
 * wire is a thing this tree has already learned not to do.
 */
static int console_read(void *opaque, uint8_t *buf, int len)
{
    (void)opaque;
    (void)buf;
    (void)len;
    return 0;
}

bool emu_virtio_add_console(uint32_t base, int irq_num)
{
    static CharacterDevice cdev;
    VIRTIOBusDef def;

    if (bus_def_for(base, irq_num, &def) == NULL) {
        return false;
    }

    cdev.opaque = NULL;
    cdev.write_data = console_write;
    cdev.read_data = console_read;

    return remember(virtio_console_init(&def, &cdev), "console", base);
}

/* ------------------------------------------------------------------ */
/* 9p                                                                  */
/* ------------------------------------------------------------------ */

bool emu_virtio_add_9p(uint32_t base, int irq_num, const char *tag,
                       const char *root)
{
    VIRTIOBusDef def;
    FSDevice *fs;

    if (tag == NULL || root == NULL) {
        return false;
    }

    /*
     * Checked here rather than left to the backend, because a missing
     * directory is the likely mistake and fs_disk_init reports it as a
     * failure with no name attached -- which reads as "virtio is
     * broken" rather than "that path does not exist".
     */
    if (access(root, R_OK) != 0) {
        fprintf(stderr, "virtio: 9p root '%s' is not readable\n", root);
        return false;
    }

    if (bus_def_for(base, irq_num, &def) == NULL) {
        return false;
    }

    fs = fs_disk_init(root);
    if (fs == NULL) {
        fprintf(stderr, "virtio: could not open 9p root '%s'\n", root);
        return false;
    }

    return remember(virtio_9p_init(&def, fs, tag), "9p", base);
}
