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

/*
 * Kept by pointer rather than by index, because posting an event has to
 * find the right device and there is at most one of each. A machine
 * with two keyboards is not a thing this emulator presents.
 */
static VIRTIODevice *g_kbd;
static VIRTIODevice *g_mouse;

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
    g_kbd = NULL;
    g_mouse = NULL;
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
/* Block                                                               */
/* ------------------------------------------------------------------ */

/*
 * A disk, backed by a host file.
 *
 * **512-byte sectors, which is not a choice.** virtio-blk defines the
 * unit of every request as 512 bytes regardless of what the underlying
 * storage does, so a file whose length is not a multiple of that has a
 * partial last sector the guest cannot address -- reported at start-up
 * rather than rounded, because a rounded-up image reads whatever
 * follows the file and a rounded-down one silently loses the tail.
 */
#define EMU_VIRTIO_SECTOR 512

typedef struct {
    BlockDevice dev;
    FILE *fp;
    int64_t sectors;
    bool writable;
} emu_block_t;

static int64_t block_sector_count(BlockDevice *bs)
{
    const emu_block_t *const b = (const emu_block_t *)bs->opaque;

    return b->sectors;
}

/*
 * Synchronous, and reporting so by returning 0.
 *
 * TinyEMU's interface allows a device to return 1 and call the
 * completion later, which is what a real asynchronous backend wants.
 * A host file read is a memcpy from the page cache -- making it
 * asynchronous would mean a thread, a queue and an ordering question,
 * to hide a latency that is not there.
 */
static int block_read(BlockDevice *bs, uint64_t sector, uint8_t *buf, int n,
                      BlockDeviceCompletionFunc *cb, void *opaque)
{
    emu_block_t *const b = (emu_block_t *)bs->opaque;
    const size_t want = (size_t)n * EMU_VIRTIO_SECTOR;

    (void)cb;
    (void)opaque;

    if ((int64_t)sector + n > b->sectors) {
        return -1;
    }

    if (fseek(b->fp, (long)(sector * EMU_VIRTIO_SECTOR), SEEK_SET) != 0 ||
        fread(buf, 1u, want, b->fp) != want) {
        return -1;
    }
    return 0;
}

static int block_write(BlockDevice *bs, uint64_t sector, const uint8_t *buf,
                       int n, BlockDeviceCompletionFunc *cb, void *opaque)
{
    emu_block_t *const b = (emu_block_t *)bs->opaque;
    const size_t want = (size_t)n * EMU_VIRTIO_SECTOR;

    (void)cb;
    (void)opaque;

    /*
     * A read-only disk refuses rather than pretending. virtio-blk has a
     * status byte for exactly this, and a write that silently vanished
     * would give a guest a filesystem that appears to work and loses
     * everything at the next mount.
     */
    if (!b->writable || (int64_t)sector + n > b->sectors) {
        return -1;
    }

    if (fseek(b->fp, (long)(sector * EMU_VIRTIO_SECTOR), SEEK_SET) != 0 ||
        fwrite(buf, 1u, want, b->fp) != want) {
        return -1;
    }
    (void)fflush(b->fp);
    return 0;
}

bool emu_virtio_add_block(uint32_t base, int irq_num, const char *path,
                          bool writable)
{
    static emu_block_t blk;
    VIRTIOBusDef def;
    long size;

    if (path == NULL) {
        return false;
    }

    /*
     * Opened read-write first when asked, and the fallback is reported
     * rather than silent: a disk that turns out to be read-only is
     * something a guest will discover much later, as a filesystem that
     * will not mount.
     */
    blk.fp = writable ? fopen(path, "r+b") : NULL;
    blk.writable = (blk.fp != NULL);
    if (blk.fp == NULL) {
        blk.fp = fopen(path, "rb");
        if (blk.fp == NULL) {
            fprintf(stderr, "virtio: cannot open disk '%s'\n", path);
            return false;
        }
        if (writable) {
            fprintf(stderr, "virtio: '%s' is not writable; attached read-only\n",
                    path);
        }
    }

    if (fseek(blk.fp, 0, SEEK_END) != 0) {
        fprintf(stderr, "virtio: cannot size disk '%s'\n", path);
        return false;
    }
    size = ftell(blk.fp);
    if (size <= 0) {
        fprintf(stderr, "virtio: disk '%s' is empty\n", path);
        return false;
    }
    if ((size % EMU_VIRTIO_SECTOR) != 0) {
        fprintf(stderr,
                "virtio: disk '%s' is %ld bytes, not a multiple of %d -- "
                "the last partial sector is unreachable\n",
                path, size, EMU_VIRTIO_SECTOR);
    }
    blk.sectors = size / EMU_VIRTIO_SECTOR;

    if (bus_def_for(base, irq_num, &def) == NULL) {
        return false;
    }

    blk.dev.get_sector_count = block_sector_count;
    blk.dev.read_async = block_read;
    blk.dev.write_async = block_write;
    blk.dev.opaque = &blk;

    return remember(virtio_block_init(&def, &blk.dev), "block", base);
}

/* ------------------------------------------------------------------ */
/* Input                                                               */
/* ------------------------------------------------------------------ */

bool emu_virtio_add_keyboard(uint32_t base, int irq_num)
{
    VIRTIOBusDef def;
    VIRTIODevice *dev;

    if (bus_def_for(base, irq_num, &def) == NULL) {
        return false;
    }

    dev = virtio_input_init(&def, VIRTIO_INPUT_TYPE_KEYBOARD);
    if (!remember(dev, "keyboard", base)) {
        return false;
    }
    g_kbd = dev;
    return true;
}

bool emu_virtio_add_mouse(uint32_t base, int irq_num)
{
    VIRTIOBusDef def;
    VIRTIODevice *dev;

    if (bus_def_for(base, irq_num, &def) == NULL) {
        return false;
    }

    /*
     * MOUSE rather than TABLET: relative motion, which is what a mouse
     * is and what a guest expects to be able to turn past the edge of
     * the screen with. TABLET reports an absolute position, which suits
     * a pointer that must track the host's cursor exactly and is the
     * wrong shape for a game.
     */
    dev = virtio_input_init(&def, VIRTIO_INPUT_TYPE_MOUSE);
    if (!remember(dev, "mouse", base)) {
        return false;
    }
    g_mouse = dev;
    return true;
}

void emu_virtio_key_event(bool down, uint16_t evdev_code)
{
    if (g_kbd != NULL) {
        /*
         * The return value says the queue was full -- the guest is not
         * draining. Dropped rather than retried: input is only
         * interesting while it is recent, and a queue that has backed
         * up is one whose events are already stale.
         */
        (void)virtio_input_send_key_event(g_kbd, down ? TRUE : FALSE,
                                          evdev_code);
    }
}

void emu_virtio_mouse_event(int dx, int dy, int dz, unsigned int buttons)
{
    if (g_mouse != NULL) {
        (void)virtio_input_send_mouse_event(g_mouse, dx, dy, dz, buttons);
    }
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
