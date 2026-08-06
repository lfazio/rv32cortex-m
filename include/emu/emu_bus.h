/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_bus.h - Guest physical address space.
 *
 * The bus is a small sorted-by-nothing table of regions. Each access walks
 * the table, but a one-entry "last hit" cache makes the common case (the
 * region you touched last time) a single compare, which is what matters in
 * a tight interpreter loop.
 *
 * Four region kinds:
 *
 *   EMU_MEM_RAM       backed by a host buffer, read/write
 *   EMU_MEM_ROM       backed by a host buffer, writes fault
 *   EMU_MEM_MMIO      handled by a device callback (virtual devices)
 *   EMU_MEM_PASSTHRU  translated onto real ARM addresses and accessed
 *                     directly, subject to the region's perm/width mask
 *
 * PASSTHRU is what lets the guest drive the STM32's own peripherals.
 * Because guest 0x40000000..0x5FFFFFFF happens to be exactly where the
 * STM32 puts its APB/AHB peripherals, that window is an identity map.
 *
 * Nothing here knows what instruction set the guest runs. Accesses report
 * an emu_fault_t -- which *kind* of access was refused -- and the frontend
 * turns that into a trap cause of its own; see rv_exc_from_fault.
 */
#ifndef EMU_BUS_H
#define EMU_BUS_H

#include "emu_types.h"
#include "emu_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The core an MMIO device may need to poke (to raise an interrupt line, or
 * read the time). Opaque here: devices that need frontend state cast it,
 * and the ISA-agnostic ones never look.
 */
struct emu_cpu;

/* ------------------------------------------------------------------ */
/* Region description                                                  */
/* ------------------------------------------------------------------ */

typedef enum {
    EMU_MEM_RAM = 0,
    EMU_MEM_ROM,
    EMU_MEM_MMIO,
    EMU_MEM_PASSTHRU,
} emu_region_kind_t;

/* Access permissions. */
#define EMU_PERM_R   0x1u
#define EMU_PERM_W   0x2u
#define EMU_PERM_X   0x4u
#define EMU_PERM_RW  (EMU_PERM_R | EMU_PERM_W)
#define EMU_PERM_RX  (EMU_PERM_R | EMU_PERM_X)
#define EMU_PERM_RWX (EMU_PERM_R | EMU_PERM_W | EMU_PERM_X)

/* Permitted access widths. Some ARM peripherals fault on the wrong width. */
#define EMU_W8       0x1u
#define EMU_W16      0x2u
#define EMU_W32      0x4u
#define EMU_WANY     (EMU_W8 | EMU_W16 | EMU_W32)

/*
 * Access kind, for the frontend's own protection checks (RISC-V PMP and
 * Sdtrig, RH850's MPU). Distinct from emu_fault_t, which reports what the
 * *bus* refused; this says what was being attempted.
 */
typedef enum {
    EMU_ACC_FETCH = 0,
    EMU_ACC_LOAD,
    EMU_ACC_STORE,
} emu_access_t;

typedef struct emu_dev_ops {
    /* off is the byte offset within the region; size is 1, 2 or 4. */
    emu_fault_t (*read)(void *ctx, uint32_t off, uint32_t size, uint32_t *out);
    emu_fault_t (*write)(void *ctx, uint32_t off, uint32_t size, uint32_t val);
    /* Optional: advance device time / raise interrupts. May be NULL. */
    void        (*tick)(void *ctx, struct emu_cpu *cpu);
} emu_dev_ops_t;

typedef struct emu_region {
    uint32_t    base;      /* guest base address                        */
    uint32_t    size;      /* bytes; 0 disables the entry               */
    void       *host;      /* RAM/ROM: backing buffer                   */
    uintptr_t   host_base; /* PASSTHRU: host address of guest `base`    */
    const emu_dev_ops_t *ops;   /* MMIO only                             */
    void       *ctx;           /* MMIO only                             */
    const char *name;
    uint8_t     kind;      /* emu_region_kind_t                          */
    uint8_t     perm;      /* EMU_PERM_*                                 */
    uint8_t     widths;    /* EMU_W*                                     */
    uint8_t     flags;
} emu_region_t;

/* ------------------------------------------------------------------ */
/* Bus                                                                 */
/* ------------------------------------------------------------------ */

typedef struct emu_bus {
    /*
     * Fast-path caches come first so they sit at small offsets from the
     * bus pointer, which keeps the inline paths below to single-instruction
     * loads on Thumb-2.
     *
     * Each caches the last plain-memory region used for a given access
     * kind. They are the difference between a region walk plus permission
     * and width checks on every access, and a compare-and-load: at one
     * instruction fetch and up to one data access per emulated
     * instruction, that walk was the single largest cost in the
     * interpreter.
     *
     * A span of 0 means "empty", so the unsigned compare fails and control
     * falls into the slow path. Anything that can invalidate the mapping
     * (adding a region, loading an image, a debugger write) calls
     * emu_bus_flush().
     */
    uint32_t       fetch_base;
    uint32_t       fetch_span;   /* bytes where a 16-bit fetch fits entirely */
    const uint8_t *fetch_host;

    uint32_t       data_base;
    uint32_t       data_span;    /* region size; callers guarantee alignment */
    uint8_t       *data_host;

    emu_region_t regions[EMU_MAX_REGIONS];
    uint32_t    count;
    uint32_t    last;      /* index of the last region that matched */
#if EMU_ENABLE_STATS
    uint32_t    fault_count;
#endif
} emu_bus_t;

/* Drop the fast-path caches. Cheap; call whenever the map may have moved. */
void emu_bus_flush(emu_bus_t *bus);

void emu_bus_init(emu_bus_t *bus);

/*
 * Append a region. Returns false if the table is full or the region
 * overlaps one already present (overlaps are a configuration bug, not a
 * runtime condition, so they are rejected up front rather than resolved by
 * priority).
 */
bool emu_bus_add(emu_bus_t *bus, const emu_region_t *r);

/* Convenience constructors. */
bool emu_bus_add_ram(emu_bus_t *bus, const char *name,
                    uint32_t base, void *buf, uint32_t size);
bool emu_bus_add_rom(emu_bus_t *bus, const char *name,
                    uint32_t base, const void *buf, uint32_t size);
bool emu_bus_add_mmio(emu_bus_t *bus, const char *name,
                     uint32_t base, uint32_t size,
                     const emu_dev_ops_t *ops, void *ctx);
/*
 * host_base is a host address, so it is uintptr_t rather than uint32_t:
 * on the target the two are the same width, but the host build needs the
 * full pointer to reach a simulated peripheral buffer.
 */
bool emu_bus_add_passthru(emu_bus_t *bus, const char *name,
                         uint32_t base, uint32_t size, uintptr_t host_base,
                         uint8_t perm, uint8_t widths);

/* Look up the region containing addr, or NULL. Updates the hit cache. */
emu_region_t *emu_bus_find(emu_bus_t *bus, uint32_t addr);

/*
 * Slow paths. These do the full region walk with permission and width
 * checks, and refill the caches above when the target turns out to be
 * plain memory. Call the inline wrappers below instead.
 */
emu_fault_t emu_bus_read_slow(emu_bus_t *bus, uint32_t addr, uint32_t size, uint32_t *out);
emu_fault_t emu_bus_write_slow(emu_bus_t *bus, uint32_t addr, uint32_t size, uint32_t val);
emu_fault_t emu_bus_fetch16_slow(emu_bus_t *bus, uint32_t addr, uint16_t *out);

/*
 * Data accesses. size is 1, 2 or 4, and the address must already be
 * naturally aligned -- the frontend's load/store helpers enforce that, so
 * a misalignment can be reported with the cause its architecture defines
 * for it and the fast paths below need no alignment test. On success
 * returns EMU_FAULT_NONE and stores to *out; on failure returns the fault
 * kind and leaves *out untouched. Loads are zero-extended; sign extension
 * is the caller's job.
 */
static EMU_ALWAYS_INLINE emu_fault_t emu_bus_read(emu_bus_t *bus, uint32_t addr,
                                             uint32_t size, uint32_t *out)
{
    /*
     * Both halves of the test matter. `off < span` rejects the empty cache
     * (span 0) and anything below the region; `span - off >= size` keeps
     * the access from running off the end. Writing it as
     * `off <= span - size` would underflow when span is 0 and wave every
     * address through to a NULL host pointer.
     */
    const uint32_t off = addr - bus->data_base;
    if (EMU_LIKELY(off < bus->data_span && (bus->data_span - off) >= size)) {
        const uint8_t *p = bus->data_host + off;
        switch (size) {
        case 1:  *out = *p; break;
        case 2:  *out = *(const uint16_t *)(const void *)p; break;
        default: *out = *(const uint32_t *)(const void *)p; break;
        }
        return EMU_FAULT_NONE;
    }
    return emu_bus_read_slow(bus, addr, size, out);
}

static EMU_ALWAYS_INLINE emu_fault_t emu_bus_write(emu_bus_t *bus, uint32_t addr,
                                              uint32_t size, uint32_t val)
{
    /*
     * Both halves of the test matter. `off < span` rejects the empty cache
     * (span 0) and anything below the region; `span - off >= size` keeps
     * the access from running off the end. Writing it as
     * `off <= span - size` would underflow when span is 0 and wave every
     * address through to a NULL host pointer.
     */
    const uint32_t off = addr - bus->data_base;
    if (EMU_LIKELY(off < bus->data_span && (bus->data_span - off) >= size)) {
        uint8_t *p = bus->data_host + off;
        switch (size) {
        case 1:  *p = (uint8_t)val; break;
        case 2:  *(uint16_t *)(void *)p = (uint16_t)val; break;
        default: *(uint32_t *)(void *)p = val; break;
        }
        return EMU_FAULT_NONE;
    }
    return emu_bus_write_slow(bus, addr, size, val);
}

/* Instruction fetch of a 16-bit parcel. Requires EMU_PERM_X. */
static EMU_ALWAYS_INLINE emu_fault_t emu_bus_fetch16(emu_bus_t *bus, uint32_t addr,
                                                uint16_t *out)
{
    const uint32_t off = addr - bus->fetch_base;
    if (EMU_LIKELY(off < bus->fetch_span)) {
        *out = *(const uint16_t *)(const void *)(bus->fetch_host + off);
        return EMU_FAULT_NONE;
    }
    return emu_bus_fetch16_slow(bus, addr, out);
}

/*
 * Translate a guest address to the host address backing it, for regions
 * where that is meaningful (RAM, ROM and passthrough). Returns NULL for
 * unmapped addresses and for virtual devices, which have no backing store.
 * Used by the cache-block operations so ARM cache maintenance is applied
 * to the memory that actually holds the guest's data.
 */
void *emu_bus_host_ptr(emu_bus_t *bus, uint32_t addr, uint32_t len);

/*
 * Host-side bulk access, used to load guest images and to implement the
 * debug monitor. These bypass permission checks by design.
 */
bool emu_bus_load(emu_bus_t *bus, uint32_t addr, const void *src, uint32_t len);
bool emu_bus_dump(emu_bus_t *bus, uint32_t addr, void *dst, uint32_t len);

#ifdef __cplusplus
}
#endif

#endif /* EMU_BUS_H */
