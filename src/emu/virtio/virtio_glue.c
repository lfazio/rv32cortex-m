/* SPDX-License-Identifier: Apache-2.0 */
/*
 * virtio_glue.c - the three functions TinyEMU's virtio.c calls out to.
 *
 * See virtio_glue.h for why this exists and what it replaces. The whole
 * of it is an adapter between two shapes of the same idea:
 *
 *   TinyEMU                        this tree
 *   -------                        ---------
 *   read(opaque, off, size_log2)   read(ctx, off, size, &out) -> fault
 *   write(opaque, off, val, log2)  write(ctx, off, size, val) -> fault
 *
 * The differences are that TinyEMU passes a log2 size and cannot report
 * a fault, and this bus passes a byte count and can. Neither direction
 * loses anything: a virtio register access is always 1, 2 or 4 bytes,
 * and virtio.c has no failure to report -- an out-of-range offset reads
 * zero by its own choice, which is what the specification says a
 * reserved register does.
 */

#include "virtio_glue.h"
#include "virtio.h" /* VIRTIO_PAGE_SIZE: the bound its accesses keep */

#include <stdio.h>

/* ------------------------------------------------------------------ */
/* MMIO                                                                */
/* ------------------------------------------------------------------ */

/*
 * One of these per registered device, because the bus holds a single
 * context pointer and this adapter needs three things: the device's own
 * opaque and its two entry points.
 *
 * Statically allocated. A run has a handful of virtio devices at fixed
 * addresses -- the device tree names them and they never move -- so a
 * fixed table is simpler than an allocation whose lifetime nothing
 * tracks, and running out is a start-up failure rather than a silent
 * one.
 */
#ifndef EMU_VIRTIO_MAX_DEVICES
#define EMU_VIRTIO_MAX_DEVICES 8
#endif

typedef struct {
    void *opaque;
    DeviceReadFunc *read;
    DeviceWriteFunc *write;
    bool used;
} virtio_port_t;

static virtio_port_t g_ports[EMU_VIRTIO_MAX_DEVICES];
static PhysMemoryRange g_ranges[EMU_VIRTIO_MAX_DEVICES];
static unsigned g_nports;

/*
 * `size` is a byte count and TinyEMU wants its log2. Only 1, 2 and 4 can
 * arrive -- the bus rejects anything else before a device sees it -- so
 * this is a lookup rather than a loop, and an unexpected width is
 * refused rather than rounded to something plausible.
 */
static bool size_log2_of(uint32_t size, int *out)
{
    switch (size) {
    case 1u:
        *out = 0;
        return true;
    case 2u:
        *out = 1;
        return true;
    case 4u:
        *out = 2;
        return true;
    default:
        return false;
    }
}

static emu_fault_t virtio_port_read(void *ctx, uint32_t off, uint32_t size,
                                    uint32_t *out)
{
    virtio_port_t *const p = (virtio_port_t *)ctx;
    int log2;

    if (!size_log2_of(size, &log2)) {
        return EMU_FAULT_LOAD;
    }

    *out = p->read(p->opaque, off, log2);
    return EMU_FAULT_NONE;
}

static emu_fault_t virtio_port_write(void *ctx, uint32_t off, uint32_t size,
                                     uint32_t val)
{
    virtio_port_t *const p = (virtio_port_t *)ctx;
    int log2;

    if (!size_log2_of(size, &log2)) {
        return EMU_FAULT_STORE;
    }

    p->write(p->opaque, off, val, log2);
    return EMU_FAULT_NONE;
}

static const emu_dev_ops_t k_virtio_port_ops = {
    .read = virtio_port_read,
    .write = virtio_port_write,
    .tick = NULL,
};

PhysMemoryRange *cpu_register_device(PhysMemoryMap *s, uint64_t addr,
                                     uint64_t size, void *opaque,
                                     DeviceReadFunc *read_func,
                                     DeviceWriteFunc *write_func,
                                     int devio_flags)
{
    virtio_port_t *port;
    PhysMemoryRange *range;

    /*
     * The flags say which access widths the device accepts. virtio-mmio
     * asks for all three and the adapter above honours whatever arrives,
     * so nothing here needs them -- but ignoring a *restriction*
     * silently would be wrong, so a device that asked for a subset is
     * refused rather than given more than it wanted.
     */
    if ((devio_flags & (DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32)) !=
        (DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32)) {
        fprintf(stderr, "virtio: device at 0x%08x wants a width subset "
                        "(flags 0x%x), which this adapter does not model\n",
                (unsigned)addr, (unsigned)devio_flags);
        return NULL;
    }

    if (g_nports >= EMU_VIRTIO_MAX_DEVICES) {
        fprintf(stderr, "virtio: more than %d devices\n",
                EMU_VIRTIO_MAX_DEVICES);
        return NULL;
    }

    port = &g_ports[g_nports];
    port->opaque = opaque;
    port->read = read_func;
    port->write = write_func;
    port->used = true;

    range = &g_ranges[g_nports];
    range->map = s;
    range->addr = (uint32_t)addr;
    range->size = (uint32_t)size;

    if (!emu_bus_add_mmio(s->bus, "virtio", (uint32_t)addr, (uint32_t)size,
                          &k_virtio_port_ops, port)) {
        fprintf(stderr, "virtio: could not map a device at 0x%08x\n",
                (unsigned)addr);
        return NULL;
    }

    g_nports++;
    return range;
}

/*
 * PCI only, and this tree has no PCI bus: virtio-mmio devices sit where
 * the device tree says and never move. Reached only through the PCI
 * transport, which nothing here constructs -- so it says so rather than
 * pretending to work.
 */
void phys_mem_set_addr(PhysMemoryRange *pr, uint64_t addr, BOOL enabled)
{
    (void)pr;
    (void)addr;
    (void)enabled;
    fprintf(stderr, "virtio: phys_mem_set_addr is PCI-only and unsupported\n");
}

uint8_t *phys_mem_get_ram_ptr(PhysMemoryMap *map, uint64_t paddr, BOOL is_rw)
{
    (void)is_rw;

    /*
     * The caller asks for a *pointer* and then reads or writes a length
     * this function is never told -- TinyEMU's interface. What it does
     * promise is the bound: every caller in virtio.c stays inside the
     * page the address is in (its copies stop at the page's end and ask
     * again; its 16- and 32-bit ring accesses are aligned). So the rest
     * of the page must be in the region, and a pointer is given only
     * then.
     *
     * It checked one byte. A guest whose RAM ends mid-page -- `--ram`
     * takes any size -- put a descriptor table at its last bytes, and
     * get_desc copied 16 bytes from past the end of the host's buffer
     * (fuzz-virtio). The cost is that a partial last page is not
     * reachable by a device, which no driver places rings in.
     *
     * And an address past 4 GB is not one: a guest that set a queue's
     * high word had it truncated onto its own RAM.
     */
    const uint32_t rest =
        VIRTIO_PAGE_SIZE - (uint32_t)(paddr & (VIRTIO_PAGE_SIZE - 1u));

    if ((paddr >> 32) != 0u) {
        return NULL;
    }
    return (uint8_t *)emu_bus_host_ptr(map->bus, (uint32_t)paddr, rest);
}

void irq_init(IRQSignal *irq, SetIRQFunc *set_irq_fn, void *opaque,
              int irq_num)
{
    irq->set_irq = set_irq_fn;
    irq->opaque = opaque;
    irq->irq_num = irq_num;
}
