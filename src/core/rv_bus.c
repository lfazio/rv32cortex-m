/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_bus.c - Guest physical address space.
 */

#include "rv32/rv_bus.h"

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
static RV_ALWAYS_INLINE bool in_region(const rv_region_t *r, uint32_t addr)
{
    return (uint32_t)(addr - r->base) < r->size;
}

rv_region_t *rv_bus_find(rv_bus_t *bus, uint32_t addr)
{
    /* Guest code has strong spatial locality; check last hit first. */
    rv_region_t *r = &bus->regions[bus->last];
    if (RV_LIKELY(bus->count != 0u && in_region(r, addr))) {
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

/* Map an access size to its RV_W* bit. */
static RV_ALWAYS_INLINE uint32_t width_bit(uint32_t size)
{
    /* 1 -> RV_W8, 2 -> RV_W16, 4 -> RV_W32 */
    return (size == 4u) ? RV_W32 : ((size == 2u) ? RV_W16 : RV_W8);
}

/*
 * Common prologue: resolve the region, verify the whole access fits inside
 * it, and check permission plus width. Returns NULL and leaves *off
 * untouched on failure.
 */
static rv_region_t *check(rv_bus_t *bus, uint32_t addr, uint32_t size,
                          uint32_t perm, uint32_t *off)
{
    rv_region_t *r = rv_bus_find(bus, addr);
    if (RV_UNLIKELY(r == NULL)) {
        return NULL;
    }
    const uint32_t o = addr - r->base;
    /* r->size >= 4 is enforced by rv_bus_add, so this cannot underflow. */
    if (RV_UNLIKELY(o > r->size - size)) {
        return NULL;    /* access straddles the end of the region */
    }
    if (RV_UNLIKELY((r->perm & perm) == 0u)) {
        return NULL;
    }
    if (RV_UNLIKELY((r->widths & width_bit(size)) == 0u)) {
        return NULL;
    }
    *off = o;
    return r;
}

/* ------------------------------------------------------------------ */
/* Setup                                                               */
/* ------------------------------------------------------------------ */

void rv_bus_init(rv_bus_t *bus)
{
    memset(bus, 0, sizeof(*bus));
}

bool rv_bus_add(rv_bus_t *bus, const rv_region_t *r)
{
    if (bus->count >= RV_MAX_REGIONS || r->size < 4u) {
        return false;
    }
    /* Overlaps are a configuration bug: reject rather than pick a winner. */
    for (uint32_t i = 0; i < bus->count; i++) {
        const rv_region_t *e = &bus->regions[i];
        const uint32_t a0 = r->base, a1 = r->base + r->size - 1u;
        const uint32_t b0 = e->base, b1 = e->base + e->size - 1u;
        if (a0 <= b1 && b0 <= a1) {
            return false;
        }
    }
    bus->regions[bus->count++] = *r;
    /* The new region may shadow whatever the caches point at. */
    rv_bus_flush(bus);
    return true;
}

bool rv_bus_add_ram(rv_bus_t *bus, const char *name,
                    uint32_t base, void *buf, uint32_t size)
{
    const rv_region_t r = {
        .base = base, .size = size, .host = buf, .host_base = 0,
        .ops = NULL, .ctx = NULL, .name = name,
        .kind = RV_MEM_RAM, .perm = RV_PERM_RWX, .widths = RV_WANY, .flags = 0,
    };
    return rv_bus_add(bus, &r);
}

bool rv_bus_add_rom(rv_bus_t *bus, const char *name,
                    uint32_t base, const void *buf, uint32_t size)
{
    const rv_region_t r = {
        .base = base, .size = size,
        /* The cast drops const; the ROM kind is what actually blocks writes. */
        .host = (void *)(uintptr_t)buf, .host_base = 0,
        .ops = NULL, .ctx = NULL, .name = name,
        .kind = RV_MEM_ROM, .perm = RV_PERM_RX, .widths = RV_WANY, .flags = 0,
    };
    return rv_bus_add(bus, &r);
}

bool rv_bus_add_mmio(rv_bus_t *bus, const char *name,
                     uint32_t base, uint32_t size,
                     const rv_dev_ops_t *ops, void *ctx)
{
    const rv_region_t r = {
        .base = base, .size = size, .host = NULL, .host_base = 0,
        .ops = ops, .ctx = ctx, .name = name,
        .kind = RV_MEM_MMIO, .perm = RV_PERM_RW, .widths = RV_WANY, .flags = 0,
    };
    return rv_bus_add(bus, &r);
}

bool rv_bus_add_passthru(rv_bus_t *bus, const char *name,
                         uint32_t base, uint32_t size, uintptr_t host_base,
                         uint8_t perm, uint8_t widths)
{
    const rv_region_t r = {
        .base = base, .size = size, .host = NULL, .host_base = host_base,
        .ops = NULL, .ctx = NULL, .name = name,
        .kind = RV_MEM_PASSTHRU, .perm = perm, .widths = widths, .flags = 0,
    };
    return rv_bus_add(bus, &r);
}

/* ------------------------------------------------------------------ */
/* Data access                                                         */
/* ------------------------------------------------------------------ */

void rv_bus_flush(rv_bus_t *bus)
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
 * slow path installs it knowing rv_bus_write_slow will re-resolve.
 */
static void arm_data_cache(rv_bus_t *bus, const rv_region_t *r)
{
    bus->data_base = r->base;
    bus->data_span = r->size;
    bus->data_host = (uint8_t *)r->host;
}

rv_exc_t rv_bus_read_slow(rv_bus_t *bus, uint32_t addr, uint32_t size,
                          uint32_t *out)
{
    uint32_t off;
    rv_region_t *r = check(bus, addr, size, RV_PERM_R, &off);
    if (RV_UNLIKELY(r == NULL)) {
#if RV_ENABLE_STATS
        bus->fault_count++;
#endif
        return RV_EXC_LOAD_ACCESS_FAULT;
    }

    switch (r->kind) {
    case RV_MEM_RAM:
    case RV_MEM_ROM: {
        const uint8_t *p = (const uint8_t *)r->host + off;
        /* Callers guarantee natural alignment, so these loads are aligned. */
        switch (size) {
        case 1:  *out = *p; break;
        case 2:  *out = *(const uint16_t *)(const void *)p; break;
        default: *out = *(const uint32_t *)(const void *)p; break;
        }
        /* Only RAM is safe to cache: see arm_data_cache. */
        if (r->kind == RV_MEM_RAM && r->perm == RV_PERM_RWX) {
            arm_data_cache(bus, r);
        }
        return RV_EXC_NONE;
    }

    case RV_MEM_PASSTHRU: {
        volatile const uint8_t *p =
            (volatile const uint8_t *)(r->host_base + off);
        switch (size) {
        case 1:  *out = *p; break;
        case 2:  *out = *(volatile const uint16_t *)(volatile const void *)p; break;
        default: *out = *(volatile const uint32_t *)(volatile const void *)p; break;
        }
        return RV_EXC_NONE;
    }

    default:  /* RV_MEM_MMIO */
        return r->ops->read(r->ctx, off, size, out);
    }
}

rv_exc_t rv_bus_write_slow(rv_bus_t *bus, uint32_t addr, uint32_t size,
                           uint32_t val)
{
    uint32_t off;
    rv_region_t *r = check(bus, addr, size, RV_PERM_W, &off);
    if (RV_UNLIKELY(r == NULL)) {
#if RV_ENABLE_STATS
        bus->fault_count++;
#endif
        return RV_EXC_STORE_ACCESS_FAULT;
    }

    switch (r->kind) {
    case RV_MEM_RAM: {
        uint8_t *p = (uint8_t *)r->host + off;
        switch (size) {
        case 1:  *p = (uint8_t)val; break;
        case 2:  *(uint16_t *)(void *)p = (uint16_t)val; break;
        default: *(uint32_t *)(void *)p = val; break;
        }
        if (r->perm == RV_PERM_RWX) {
            arm_data_cache(bus, r);
        }
        return RV_EXC_NONE;
    }

    case RV_MEM_PASSTHRU: {
        volatile uint8_t *p = (volatile uint8_t *)(r->host_base + off);
        switch (size) {
        case 1:  *p = (uint8_t)val; break;
        case 2:  *(volatile uint16_t *)(volatile void *)p = (uint16_t)val; break;
        default: *(volatile uint32_t *)(volatile void *)p = val; break;
        }
        return RV_EXC_NONE;
    }

    case RV_MEM_MMIO:
        return r->ops->write(r->ctx, off, size, val);

    default:  /* RV_MEM_ROM: perm check above should have caught this */
        return RV_EXC_STORE_ACCESS_FAULT;
    }
}

rv_exc_t rv_bus_fetch16_slow(rv_bus_t *bus, uint32_t addr, uint16_t *out)
{
    uint32_t off;
    rv_region_t *r = check(bus, addr, 2u, RV_PERM_X, &off);
    if (RV_UNLIKELY(r == NULL)) {
#if RV_ENABLE_STATS
        bus->fault_count++;
#endif
        return RV_EXC_INSN_ACCESS_FAULT;
    }

    switch (r->kind) {
    case RV_MEM_RAM:
    case RV_MEM_ROM:
        *out = *(const uint16_t *)(const void *)((const uint8_t *)r->host + off);
        /*
         * span is size-1, not size: a 16-bit fetch at the final byte of the
         * region would read one byte past its end.
         */
        bus->fetch_base = r->base;
        bus->fetch_span = r->size - 1u;
        bus->fetch_host = (const uint8_t *)r->host;
        return RV_EXC_NONE;

    case RV_MEM_PASSTHRU:
        *out = *(volatile const uint16_t *)(volatile const void *)
               (r->host_base + off);
        return RV_EXC_NONE;

    default:
        /* Executing from a virtual device is never legitimate. */
        return RV_EXC_INSN_ACCESS_FAULT;
    }
}

void *rv_bus_host_ptr(rv_bus_t *bus, uint32_t addr, uint32_t len)
{
    rv_region_t *r = rv_bus_find(bus, addr);
    if (r == NULL) {
        return NULL;
    }
    const uint32_t off = addr - r->base;
    if (len > r->size - off) {
        return NULL;
    }

    switch (r->kind) {
    case RV_MEM_RAM:
    case RV_MEM_ROM:
        return (uint8_t *)r->host + off;
    case RV_MEM_PASSTHRU:
        return (void *)(r->host_base + off);
    default:
        /* A virtual device has no backing memory to maintain. */
        return NULL;
    }
}

/* ------------------------------------------------------------------ */
/* Host-side bulk access (image loading, debug monitor)                */
/* ------------------------------------------------------------------ */

bool rv_bus_load(rv_bus_t *bus, uint32_t addr, const void *src, uint32_t len)
{
    const uint8_t *s = (const uint8_t *)src;

    /* Writing guest memory behind the emulator's back invalidates any
     * cached instruction mapping. */
    rv_bus_flush(bus);

    while (len != 0u) {
        rv_region_t *r = rv_bus_find(bus, addr);
        if (r == NULL || (r->kind != RV_MEM_RAM && r->kind != RV_MEM_ROM)) {
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

bool rv_bus_dump(rv_bus_t *bus, uint32_t addr, void *dst, uint32_t len)
{
    uint8_t *d = (uint8_t *)dst;

    while (len != 0u) {
        rv_region_t *r = rv_bus_find(bus, addr);
        if (r == NULL || (r->kind != RV_MEM_RAM && r->kind != RV_MEM_ROM)) {
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
