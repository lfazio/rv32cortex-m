/* SPDX-License-Identifier: Apache-2.0 */
/*
 * virtio_glue.h - what TinyEMU's virtio.c expects of its host.
 *
 * `third_party/tinyemu/virtio.c` is vendored byte-identical, so a future
 * update is a copy rather than a merge. It reaches into its host through
 * a handful of names that TinyEMU declares in `iomem.h`, `cutils.h` and
 * `list.h` -- headers that also carry a CPU emulator, a PCI bus and an
 * SDL frontend this tree does not want. **This file is those names and
 * nothing else**, which is why the import was three files rather than a
 * repository.
 *
 * The whole porting layer:
 *
 *   cpu_register_device      -> emu_bus_add_mmio
 *   phys_mem_get_ram_ptr     -> emu_bus_host_ptr
 *   set_irq                  -> a callback the platform supplies
 *   mallocz, get_le32, ...   -> a few lines below
 *
 * If a future virtio.c calls something new, the build breaks at the
 * link with its name. That is the right failure, and the reason this
 * declares exactly what is used rather than a plausible superset.
 */

#ifndef EMU_VIRTIO_GLUE_H
#define EMU_VIRTIO_GLUE_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "emu/emu_bus.h"

/* ------------------------------------------------------------------ */
/* cutils.h                                                            */
/* ------------------------------------------------------------------ */

/*
 * TinyEMU's BOOL is an int-sized tri-state in places (it returns -1 from
 * some callbacks), so it is `int` here rather than `bool`. Copying the
 * spelling matters more than improving it: virtio.c is not edited, so
 * anything it assigns to a BOOL has to fit.
 */
typedef int BOOL;
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif

static inline void *mallocz(size_t size)
{
    void *const p = malloc(size);

    if (p != NULL) {
        memset(p, 0, size);
    }
    return p;
}

static inline int min_int(int a, int b)
{
    return (a < b) ? a : b;
}

#ifndef countof
#define countof(x) (sizeof(x) / sizeof((x)[0]))
#endif

static inline int max_int(int a, int b)
{
    return (a > b) ? a : b;
}

/*
 * The little-endian accessors, byte at a time.
 *
 * **Not a cast to a wider type**, which is what the obvious version
 * does: virtio descriptors are laid out by the guest at whatever
 * alignment it chose, and this tree already has one recorded case of a
 * word read off an odd address being fatal on a target that traps. Byte
 * assembly costs a few instructions on the host and cannot be wrong.
 */
static inline uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | ((uint32_t)p[1] << 8));
}

static inline uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static inline uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)get_le32(p) | ((uint64_t)get_le32(p + 4) << 32);
}

static inline void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

/* ------------------------------------------------------------------ */
/* iomem.h                                                             */
/* ------------------------------------------------------------------ */

/*
 * The memory map, which here is just the bus.
 *
 * TinyEMU's PhysMemoryMap owns its regions and can move them
 * (phys_mem_set_addr, for PCI BARs). This one does not: virtio-mmio
 * devices sit at fixed addresses chosen by the device tree, so the only
 * operation needed is "register a device" and the only lookup is "where
 * is this guest address in host memory".
 */
typedef struct PhysMemoryMap {
    emu_bus_t *bus;
} PhysMemoryMap;

/*
 * A registered range. Returned by cpu_register_device and, for MMIO
 * virtio, never used again -- only the PCI transport calls
 * phys_mem_set_addr on it.
 */
typedef struct PhysMemoryRange {
    PhysMemoryMap *map;
    uint32_t addr;
    uint32_t size;
} PhysMemoryRange;

/*
 * `size_log2` rather than a byte count, which is TinyEMU's spelling:
 * 0, 1, 2 for 8, 16 and 32 bits.
 */
typedef void DeviceWriteFunc(void *opaque, uint32_t offset, uint32_t val,
                             int size_log2);
typedef uint32_t DeviceReadFunc(void *opaque, uint32_t offset, int size_log2);

#define DEVIO_SIZE8 (1 << 0)
#define DEVIO_SIZE16 (1 << 1)
#define DEVIO_SIZE32 (1 << 2)
#define DEVIO_DISABLED (1 << 4)

PhysMemoryRange *cpu_register_device(PhysMemoryMap *s, uint64_t addr,
                                     uint64_t size, void *opaque,
                                     DeviceReadFunc *read_func,
                                     DeviceWriteFunc *write_func,
                                     int devio_flags);

void phys_mem_set_addr(PhysMemoryRange *pr, uint64_t addr, BOOL enabled);

/*
 * A host pointer to guest memory.
 *
 * This is how the virtqueue is reached: the guest publishes descriptor
 * rings in its own RAM and the device walks them directly. It returns
 * NULL for an address that is not RAM, and virtio.c checks -- so a
 * guest that points a descriptor at a device register gets a refused
 * transfer rather than a wild write.
 */
uint8_t *phys_mem_get_ram_ptr(PhysMemoryMap *map, uint64_t paddr, BOOL is_rw);

typedef void SetIRQFunc(void *opaque, int irq_num, int level);

typedef struct {
    SetIRQFunc *set_irq;
    void *opaque;
    int irq_num;
} IRQSignal;

void irq_init(IRQSignal *irq, SetIRQFunc *set_irq, void *opaque, int irq_num);

static inline void set_irq(IRQSignal *irq, int level)
{
    irq->set_irq(irq->opaque, irq->irq_num, level);
}

#endif /* EMU_VIRTIO_GLUE_H */
