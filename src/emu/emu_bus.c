/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_bus.c - Guest physical address space.
 */

#include "emu/emu_bus.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Region lookup                                                       */
/* ------------------------------------------------------------------ */

/*
 * The unsigned subtraction below is the standard range test: if addr is
 * below base it wraps to a huge value and fails the compare, so one
 * comparison covers both ends. A disabled region has size 0 and can never
 * match.
 */
static EMU_ALWAYS_INLINE bool in_region(const emu_region_t *r, uint32_t addr)
{
    return (uint32_t)(addr - r->base) < r->size;
}

emu_region_t *emu_bus_find(emu_bus_t *bus, uint32_t addr)
{
    /* Guest code has strong spatial locality; check last hit first. */
    emu_region_t *r = &bus->regions[bus->last];
    if (EMU_LIKELY(bus->count != 0u && in_region(r, addr))) {
        return r;
    }

    for (uint32_t i = 0; i < bus->count; i++) {
        r = &bus->regions[i];
        if (in_region(r, addr)) {
            bus->last = i;
            return r;
        }
    }
    return NULL;
}

/* Map an access size to its EMU_W* bit. */
static EMU_ALWAYS_INLINE uint32_t width_bit(uint32_t size)
{
    /* 1 -> EMU_W8, 2 -> EMU_W16, 4 -> EMU_W32 */
    return (size == 4u) ? EMU_W32 : ((size == 2u) ? EMU_W16 : EMU_W8);
}

/*
 * Common prologue: resolve the region, verify the whole access fits inside
 * it, and check permission plus width. Returns NULL and leaves *off
 * untouched on failure.
 */
static emu_region_t *check(emu_bus_t *bus, uint32_t addr, uint32_t size,
                          uint32_t perm, uint32_t *off)
{
    emu_region_t *r = emu_bus_find(bus, addr);
    if (EMU_UNLIKELY(r == NULL)) {
        return NULL;
    }
    const uint32_t o = addr - r->base;
    /* r->size >= 4 is enforced by emu_bus_add, so this cannot underflow. */
    if (EMU_UNLIKELY(o > r->size - size)) {
        return NULL;    /* access straddles the end of the region */
    }
    if (EMU_UNLIKELY((r->perm & perm) == 0u)) {
        return NULL;
    }
    if (EMU_UNLIKELY((r->widths & width_bit(size)) == 0u)) {
        return NULL;
    }
    *off = o;
    return r;
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

void emu_bus_init(emu_bus_t *bus)
{
    memset(bus, 0, sizeof(*bus));
}

bool emu_bus_add(emu_bus_t *bus, const emu_region_t *r)
{
    if (bus->count >= EMU_MAX_REGIONS || r->size < 4u) {
        return false;
    }
    /* Overlaps are a configuration bug: reject rather than pick a winner. */
    for (uint32_t i = 0; i < bus->count; i++) {
        const emu_region_t *e = &bus->regions[i];
        const uint32_t a0 = r->base, a1 = r->base + r->size - 1u;
        const uint32_t b0 = e->base, b1 = e->base + e->size - 1u;
        if (a0 <= b1 && b0 <= a1) {
            return false;
        }
    }
    bus->regions[bus->count++] = *r;
    /* The new region may shadow whatever the caches point at. */
    emu_bus_flush(bus);
    return true;
}

bool emu_bus_add_ram(emu_bus_t *bus, const char *name,
                    uint32_t base, void *buf, uint32_t size)
{
    const emu_region_t r = {
        .base = base, .size = size, .host = buf, .host_base = 0,
        .ops = NULL, .ctx = NULL, .name = name,
        .kind = EMU_MEM_RAM, .perm = EMU_PERM_RWX, .widths = EMU_WANY, .flags = 0,
    };
    return emu_bus_add(bus, &r);
}

bool emu_bus_add_rom(emu_bus_t *bus, const char *name,
                    uint32_t base, const void *buf, uint32_t size)
{
    const emu_region_t r = {
        .base = base, .size = size,
        /* The cast drops const; the ROM kind is what actually blocks writes. */
        .host = (void *)(uintptr_t)buf, .host_base = 0,
        .ops = NULL, .ctx = NULL, .name = name,
        .kind = EMU_MEM_ROM, .perm = EMU_PERM_RX, .widths = EMU_WANY, .flags = 0,
    };
    return emu_bus_add(bus, &r);
}

bool emu_bus_add_mmio(emu_bus_t *bus, const char *name,
                     uint32_t base, uint32_t size,
                     const emu_dev_ops_t *ops, void *ctx)
{
    const emu_region_t r = {
        .base = base, .size = size, .host = NULL, .host_base = 0,
        .ops = ops, .ctx = ctx, .name = name,
        .kind = EMU_MEM_MMIO, .perm = EMU_PERM_RW, .widths = EMU_WANY, .flags = 0,
    };
    return emu_bus_add(bus, &r);
}

bool emu_bus_add_passthru(emu_bus_t *bus, const char *name,
                         uint32_t base, uint32_t size, uintptr_t host_base,
                         uint8_t perm, uint8_t widths)
{
    const emu_region_t r = {
        .base = base, .size = size, .host = NULL, .host_base = host_base,
        .ops = NULL, .ctx = NULL, .name = name,
        .kind = EMU_MEM_PASSTHRU, .perm = perm, .widths = widths, .flags = 0,
    };
    return emu_bus_add(bus, &r);
}

/* ------------------------------------------------------------------ */
/* Data access                                                         */
/* ------------------------------------------------------------------ */

/*
 * Declare the guest big-endian.
 *
 * Set once, by the frontend, before anything runs. It is a property of
 * the guest architecture, not of a region or a platform.
 */
void emu_bus_set_big_endian(emu_bus_t *bus, bool on)
{
#if EMU_BUS_ANY_BE
    bus->big_endian = on;
#else
    /*
     * No big-endian frontend in this build, so there is no flag and
     * nothing to do. This is not a silent failure waiting to happen:
     * EMU_BUS_ANY_BE is derived from the same EMU_FRONTEND_PPC that
     * compiles the only caller that would pass true, so `on` is
     * structurally false here. If a second big-endian frontend is ever
     * added, it goes in that derivation too -- and forgetting would show
     * up immediately as a guest whose every word is byte-reversed.
     */
    (void)bus;
    (void)on;
#endif
    emu_bus_flush(bus);
}

void emu_bus_flush(emu_bus_t *bus)
{
    bus->fetch_span = 0u;
    bus->data_span = 0u;
    bus->fetch_host = NULL;
    bus->data_host = NULL;
}

/*
 * Arm the data fast path for a plain-memory region.
 *
 * ROM is cached for reads only: caching it for writes would let the
 * inline store path bypass the permission check that makes it read-only.
 * The write slow path therefore never installs a ROM region, and the read
 * slow path installs it knowing emu_bus_write_slow will re-resolve.
 */
static void arm_data_cache(emu_bus_t *bus, const emu_region_t *r)
{
    bus->data_base = r->base;
    bus->data_span = r->size;
    bus->data_host = (uint8_t *)r->host;
}

emu_fault_t emu_bus_read_slow(emu_bus_t *bus, uint32_t addr, uint32_t size,
                          uint32_t *out)
{
    uint32_t off;
    emu_region_t *r = check(bus, addr, size, EMU_PERM_R, &off);
    if (EMU_UNLIKELY(r == NULL)) {
#if EMU_ENABLE_STATS
        bus->fault_count++;
#endif
        return EMU_FAULT_LOAD;
    }

    switch (r->kind) {
    case EMU_MEM_RAM:
    case EMU_MEM_ROM: {
        const uint8_t *p = (const uint8_t *)r->host + off;
        /* Callers guarantee natural alignment, so these loads are aligned. */
        switch (size) {
        case 1:  *out = *p; break;
        case 2:  *out = *(const uint16_t *)(const void *)p; break;
        default: *out = *(const uint32_t *)(const void *)p; break;
        }
        /*
         * RAM and ROM compose bytes, so they are the byte-order case. The
         * PASSTHRU and MMIO arms below deliberately do not: see the note
         * by EMU_BUS_ORDER in emu_bus.h.
         */
        *out = EMU_BUS_ORDER(bus, *out, size);
        /* Only RAM is safe to cache: see arm_data_cache. */
        if (r->kind == EMU_MEM_RAM && r->perm == EMU_PERM_RWX) {
            arm_data_cache(bus, r);
        }
        return EMU_FAULT_NONE;
    }

    case EMU_MEM_PASSTHRU: {
        volatile const uint8_t *p =
            (volatile const uint8_t *)(r->host_base + off);
        switch (size) {
        case 1:  *out = *p; break;
        case 2:  *out = *(volatile const uint16_t *)(volatile const void *)p; break;
        default: *out = *(volatile const uint32_t *)(volatile const void *)p; break;
        }
        return EMU_FAULT_NONE;
    }

    default:  /* EMU_MEM_MMIO */
        return r->ops->read(r->ctx, off, size, out);
    }
}

emu_fault_t emu_bus_write_slow(emu_bus_t *bus, uint32_t addr, uint32_t size,
                           uint32_t val)
{
    uint32_t off;
    emu_region_t *r = check(bus, addr, size, EMU_PERM_W, &off);
    if (EMU_UNLIKELY(r == NULL)) {
#if EMU_ENABLE_STATS
        bus->fault_count++;
#endif
        return EMU_FAULT_STORE;
    }

    switch (r->kind) {
    case EMU_MEM_RAM: {
        uint8_t *p = (uint8_t *)r->host + off;
        /* RAM composes bytes; PASSTHRU and MMIO below take a value. */
        val = EMU_BUS_ORDER(bus, val, size);
        switch (size) {
        case 1:  *p = (uint8_t)val; break;
        case 2:  *(uint16_t *)(void *)p = (uint16_t)val; break;
        default: *(uint32_t *)(void *)p = val; break;
        }
        if (r->perm == EMU_PERM_RWX) {
            arm_data_cache(bus, r);
        }
        return EMU_FAULT_NONE;
    }

    case EMU_MEM_PASSTHRU: {
        volatile uint8_t *p = (volatile uint8_t *)(r->host_base + off);
        switch (size) {
        case 1:  *p = (uint8_t)val; break;
        case 2:  *(volatile uint16_t *)(volatile void *)p = (uint16_t)val; break;
        default: *(volatile uint32_t *)(volatile void *)p = val; break;
        }
        return EMU_FAULT_NONE;
    }

    case EMU_MEM_MMIO:
        return r->ops->write(r->ctx, off, size, val);

    default:  /* EMU_MEM_ROM: perm check above should have caught this */
        return EMU_FAULT_STORE;
    }
}

emu_fault_t emu_bus_fetch16_slow(emu_bus_t *bus, uint32_t addr, uint16_t *out)
{
    uint32_t off;
    emu_region_t *r = check(bus, addr, 2u, EMU_PERM_X, &off);
    if (EMU_UNLIKELY(r == NULL)) {
#if EMU_ENABLE_STATS
        bus->fault_count++;
#endif
        return EMU_FAULT_FETCH;
    }

    switch (r->kind) {
    case EMU_MEM_RAM:
    case EMU_MEM_ROM:
        *out = *(const uint16_t *)(const void *)((const uint8_t *)r->host + off);
        *out = (uint16_t)EMU_BUS_ORDER(bus, *out, 2u);
        /*
         * span is size-1, not size: a 16-bit fetch at the final byte of the
         * region would read one byte past its end.
         */
        bus->fetch_base = r->base;
        bus->fetch_span = r->size - 1u;
        bus->fetch_host = (const uint8_t *)r->host;
        return EMU_FAULT_NONE;

    case EMU_MEM_PASSTHRU:
        *out = *(volatile const uint16_t *)(volatile const void *)
               (r->host_base + off);
        return EMU_FAULT_NONE;

    default:
        /* Executing from a virtual device is never legitimate. */
        return EMU_FAULT_FETCH;
    }
}

void *emu_bus_host_ptr(emu_bus_t *bus, uint32_t addr, uint32_t len)
{
    emu_region_t *r = emu_bus_find(bus, addr);
    if (r == NULL) {
        return NULL;
    }
    const uint32_t off = addr - r->base;
    if (len > r->size - off) {
        return NULL;
    }

    switch (r->kind) {
    case EMU_MEM_RAM:
    case EMU_MEM_ROM:
        return (uint8_t *)r->host + off;
    case EMU_MEM_PASSTHRU:
        return (void *)(r->host_base + off);
    default:
        /* A virtual device has no backing memory to maintain. */
        return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Host-side bulk access (image loading, debug monitor)                */
/* ------------------------------------------------------------------ */

bool emu_bus_load(emu_bus_t *bus, uint32_t addr, const void *src, uint32_t len)
{
    const uint8_t *s = (const uint8_t *)src;

    /* Writing guest memory behind the emulator's back invalidates any
     * cached instruction mapping. */
    emu_bus_flush(bus);

    while (len != 0u) {
        emu_region_t *r = emu_bus_find(bus, addr);
        if (r == NULL || (r->kind != EMU_MEM_RAM && r->kind != EMU_MEM_ROM)) {
            return false;
        }
        const uint32_t off = addr - r->base;
        uint32_t n = r->size - off;
        if (n > len) {
            n = len;
        }
        memcpy((uint8_t *)r->host + off, s, n);
        addr += n;
        s += n;
        len -= n;
    }
    return true;
}

bool emu_bus_dump(emu_bus_t *bus, uint32_t addr, void *dst, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;

    while (len != 0u) {
        emu_region_t *r = emu_bus_find(bus, addr);
        if (r == NULL || (r->kind != EMU_MEM_RAM && r->kind != EMU_MEM_ROM)) {
            return false;
        }
        const uint32_t off = addr - r->base;
        uint32_t n = r->size - off;
        if (n > len) {
            n = len;
        }
        memcpy(d, (const uint8_t *)r->host + off, n);
        addr += n;
        d += n;
        len -= n;
    }
    return true;
}
