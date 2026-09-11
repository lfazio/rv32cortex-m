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
     * One byte, because the caller asks for a *pointer* and then reads
     * or writes a length this function is never told. That is TinyEMU's
     * interface and it is the weak point of it: the bounds check here
     * can only say the address is in RAM, not that the access fits.
     *
     * What makes that tolerable is the other end -- virtio.c walks a
     * descriptor at a time and each is bounded by the ring -- and what
     * would make it wrong is a guest that lies about a descriptor
     * length. A malicious guest is not in this emulator's threat model;
     * a *buggy* one gets a NULL here the moment it points outside RAM,
     * which is the common case and the one worth catching.
     */
    return (uint8_t *)emu_bus_host_ptr(map->bus, (uint32_t)paddr, 1u);
}

void irq_init(IRQSignal *irq, SetIRQFunc *set_irq_fn, void *opaque,
              int irq_num)
{
    irq->set_irq = set_irq_fn;
    irq->opaque = opaque;
    irq->irq_num = irq_num;
}
