/* SPDX-License-Identifier: Apache-2.0 */
/*
 * fuzz_elf.c - the ELF loader against a reference, on a board's bus.
 *
 * An image arrives from outside the guest: a file named on the host
 * runner's command line, and on a board an upload over TFTP or gdb's
 * `load` that becomes the next guest unseen. emu_elf_load (the host:
 * every segment copied) and emu_elf_map (a board: segments in guest RAM
 * copied, the rest mapped read-only out of the image) are both run on
 * each input, each on a fresh bus, and held to a reference written here
 * from the ELF32 specification and the loader's documented rules --
 * sharing no code with emu_elf.c -- on three things:
 *
 *   - whether the image is accepted;
 *   - if it is, every byte of guest memory afterwards: each PT_LOAD's
 *     file bytes at its physical address and zeroes to its memsz,
 *     applied in order to a shadow of every RAM and ROM buffer, and a
 *     mapped segment readable back through the bus as the file's bytes;
 *   - the bus afterwards: no region wrapping past 4 GB, no two
 *     overlapping, and no device written to.
 *
 * The bus has RAM at 0 and at the top of the address space, which no
 * platform here maps, so that an address computed past 0xFFFFFFFF lands
 * somewhere a comparison can see instead of failing quietly.
 *
 * An input is either the image itself (first byte odd) or a description
 * of one -- header fields and up to eight program headers with addresses
 * chosen near the bus's edges -- serialised here, which reaches the
 * boundaries in seconds where raw bytes would take hours. Both go
 * through a heap copy of exactly the image's length, so a read past its
 * end is ASan's to report.
 *
 * FUZZ_STATS=1 prints what the run covered at exit; FUZZ_SHOW=1 prints
 * each input's verdict.
 */

#include "emu/emu_bus.h"
#include "emu/emu_elf.h"
#include "emu/emu_memmap.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* The bus                                                             */
/* ------------------------------------------------------------------ */

#define RAM_BASE EMU_GUEST_RAM_BASE
#define RAM_SIZE 0x10000u
#define ROM_BASE EMU_GUEST_ROM_BASE
#define ROM_SIZE 0x10000u
#define LOW_BASE 0x00000000u
#define LOW_SIZE 0x1000u
#define TOP_BASE 0xFFFF0000u
#define TOP_SIZE 0x10000u

/* One shadow per buffer the loader may write. */
typedef struct {
    uint32_t base, size;
    uint8_t *mem;    /* what the bus points at */
    uint8_t *shadow; /* what the reference says it should hold */
} buf_t;

static uint8_t g_ram[RAM_SIZE], g_ram_s[RAM_SIZE];
static uint8_t g_rom[ROM_SIZE], g_rom_s[ROM_SIZE];
static uint8_t g_low[LOW_SIZE], g_low_s[LOW_SIZE];
static uint8_t g_top[TOP_SIZE], g_top_s[TOP_SIZE];

static buf_t g_bufs[4];
static unsigned g_nbufs;

static uint32_t g_dev_writes;

static emu_fault_t dev_read(void *ctx, uint32_t off, uint32_t size,
                            uint32_t *out)
{
    (void)ctx;
    (void)off;
    (void)size;
    *out = 0u;
    return EMU_FAULT_NONE;
}

static emu_fault_t dev_write(void *ctx, uint32_t off, uint32_t size,
                             uint32_t val)
{
    (void)ctx;
    (void)off;
    (void)size;
    (void)val;
    g_dev_writes++;
    return EMU_FAULT_NONE;
}

static const emu_dev_ops_t k_dev = {dev_read, dev_write, NULL};

/* The regions the reference knows about, in the bus's terms. */
typedef struct {
    uint32_t base, size;
    bool writable_by_load; /* RAM or ROM: emu_bus_load copies into it */
} ref_region_t;

static ref_region_t g_rr[EMU_MAX_REGIONS + 8];
static unsigned g_nrr;

static void add_buf(uint32_t base, uint32_t size, uint8_t *mem, uint8_t *sh)
{
    memset(mem, 0xA5, size);
    memset(sh, 0xA5, size);
    g_bufs[g_nbufs++] = (buf_t){base, size, mem, sh};
}

static bool setup(emu_bus_t *bus, bool with_rom)
{
    g_nbufs = 0u;
    g_nrr = 0u;
    g_dev_writes = 0u;
    emu_bus_init(bus);

#define RR(b, s, w) (g_rr[g_nrr++] = (ref_region_t){(b), (s), (w)})
    if (!emu_bus_add_ram(bus, "low", LOW_BASE, g_low, LOW_SIZE) ||
        !emu_bus_add_mmio(bus, "uart", EMU_GUEST_UART_BASE, 0x1000u, &k_dev,
                          NULL) ||
        !emu_bus_add_mmio(bus, "periph", EMU_GUEST_PERIPH_BASE,
                          EMU_GUEST_PERIPH_SIZE, &k_dev, NULL) ||
        !emu_bus_add_ram(bus, "ram", RAM_BASE, g_ram, RAM_SIZE) ||
        !emu_bus_add_ram(bus, "top", TOP_BASE, g_top, TOP_SIZE)) {
        return false;
    }
    RR(LOW_BASE, LOW_SIZE, true);
    RR(EMU_GUEST_UART_BASE, 0x1000u, false);
    RR(EMU_GUEST_PERIPH_BASE, EMU_GUEST_PERIPH_SIZE, false);
    RR(RAM_BASE, RAM_SIZE, true);
    RR(TOP_BASE, TOP_SIZE, true);
    add_buf(LOW_BASE, LOW_SIZE, g_low, g_low_s);
    add_buf(RAM_BASE, RAM_SIZE, g_ram, g_ram_s);
    add_buf(TOP_BASE, TOP_SIZE, g_top, g_top_s);
    if (with_rom) {
        if (!emu_bus_add_rom(bus, "rom", ROM_BASE, g_rom, ROM_SIZE)) {
            return false;
        }
        RR(ROM_BASE, ROM_SIZE, true);
        add_buf(ROM_BASE, ROM_SIZE, g_rom, g_rom_s);
    }
#undef RR
    return true;
}

/* ------------------------------------------------------------------ */
/* The reference                                                       */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t type, offset, vaddr, paddr, filesz, memsz;
} seg_t;

#define MAX_SEGS 64u

static uint32_t rd(const uint8_t *p, unsigned n, bool be)
{
    uint32_t v = 0u;

    for (unsigned i = 0u; i < n; i++) {
        v |= (uint32_t)p[be ? (n - 1u - i) : i] << (8u * i);
    }
    return v;
}

/*
 * The header, by the ELF32 specification, and the loader's documented
 * strictness: a static executable, entries of at least the standard 32
 * bytes, a table inside the file, and each loadable segment's file
 * bytes inside the file with filesz no more than memsz. Program
 * headers past MAX_SEGS are not modelled: the input generator never
 * makes them, and a raw image with more is skipped.
 */
static bool ref_parse(const uint8_t *img, size_t len, seg_t *segs,
                      unsigned *nseg, uint32_t *entry, bool *modelled)
{
    *modelled = true;
    *nseg = 0u;
    if (len < 52u || img[0] != 0x7Fu || img[1] != 'E' || img[2] != 'L' ||
        img[3] != 'F' || img[4] != 1u || (img[5] != 1u && img[5] != 2u)) {
        return false;
    }
    const bool be = img[5] == 2u;
    const uint32_t phoff = rd(img + 28, 4u, be);
    const uint32_t phentsize = rd(img + 42, 2u, be);
    const uint32_t phnum = rd(img + 44, 2u, be);

    if (rd(img + 16, 2u, be) != 2u || phentsize < 32u || phnum == 0u) {
        return false;
    }
    if ((uint64_t)phoff + (uint64_t)phentsize * phnum > len) {
        return false;
    }
    *entry = rd(img + 24, 4u, be);
    for (uint32_t i = 0u; i < phnum; i++) {
        const uint8_t *ph = img + phoff + (uint64_t)i * phentsize;
        seg_t s = {rd(ph, 4u, be),      rd(ph + 4, 4u, be),
                   rd(ph + 8, 4u, be),  rd(ph + 12, 4u, be),
                   rd(ph + 16, 4u, be), rd(ph + 20, 4u, be)};

        if (s.type != 1u) {
            continue;
        }
        if (s.filesz > s.memsz || (uint64_t)s.offset + s.filesz > len) {
            return false;
        }
        if (*nseg == MAX_SEGS) {
            *modelled = false;
            return false;
        }
        segs[(*nseg)++] = s;
    }
    return *nseg != 0u;
}

/* The shadow byte for a guest address, or NULL if no buffer holds it. */
static uint8_t *shadow_at(uint64_t a)
{
    for (unsigned i = 0u; i < g_nbufs; i++) {
        if (a >= g_bufs[i].base && a < (uint64_t)g_bufs[i].base + g_bufs[i].size) {
            return &g_bufs[i].shadow[a - g_bufs[i].base];
        }
    }
    return NULL;
}

/* Bytes [a, a+n) into the shadow, all or the segment is refused: every
 * byte must be in RAM or ROM, below 4 GB. src NULL means zeroes. */
static bool shadow_put(uint64_t a, const uint8_t *src, uint64_t n)
{
    for (uint64_t i = 0u; i < n; i++) {
        if (shadow_at(a + i) == NULL) {
            return false;
        }
    }
    for (uint64_t i = 0u; i < n; i++) {
        *shadow_at(a + i) = (src != NULL) ? src[i] : 0u;
    }
    return true;
}

typedef struct {
    uint32_t base, size, offset; /* a segment mapped read-only */
} mapped_t;

/*
 * The outcome emu_elf_map should have: a segment wholly inside guest RAM
 * (by its physical address and memsz) is copied as emu_elf_load would;
 * any other has its file bytes mapped as a region of its own, which the
 * bus accepts only at four bytes or more, inside 4 GB, overlapping
 * nothing, while there is room, and its .bss tail zeroed at its *run*
 * address, which must be inside guest RAM.
 */
static bool ref_apply(const uint8_t *img, const seg_t *segs, unsigned n,
                      bool map, mapped_t *mapped, unsigned *nmapped)
{
    *nmapped = 0u;
    for (unsigned i = 0u; i < n; i++) {
        const seg_t *s = &segs[i];
        const bool in_ram = map && s->paddr >= RAM_BASE &&
                            (uint64_t)s->paddr + s->memsz <=
                                (uint64_t)RAM_BASE + RAM_SIZE;

        if (!map || in_ram) {
            if (!shadow_put(s->paddr, img + s->offset, s->filesz) ||
                !shadow_put((uint64_t)s->paddr + s->filesz, NULL,
                            s->memsz - s->filesz)) {
                return false;
            }
            continue;
        }
        if (s->filesz != 0u) {
            const uint64_t a0 = s->paddr, a1 = a0 + s->filesz;

            if (s->filesz < 4u || a1 > 0x100000000ull ||
                g_nrr >= EMU_MAX_REGIONS) {
                return false;
            }
            for (unsigned r = 0u; r < g_nrr; r++) {
                if (a0 < (uint64_t)g_rr[r].base + g_rr[r].size &&
                    g_rr[r].base < a1) {
                    return false;
                }
            }
            g_rr[g_nrr++] = (ref_region_t){s->paddr, s->filesz, false};
            mapped[(*nmapped)++] = (mapped_t){s->paddr, s->filesz, s->offset};
        }
        if (s->memsz > s->filesz) {
            const uint64_t b0 = (uint64_t)s->vaddr + s->filesz;

            if (b0 < RAM_BASE ||
                b0 + (s->memsz - s->filesz) > (uint64_t)RAM_BASE + RAM_SIZE ||
                !shadow_put(b0, NULL, s->memsz - s->filesz)) {
                return false;
            }
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Checks                                                              */
/* ------------------------------------------------------------------ */

static struct {
    uint64_t inputs, raw, accepted[2], refused[2], mapped, bss, wraps;
    uint64_t skipped;
} g_st;

static void stats_at_exit(void)
{
    fprintf(stderr,
            "fuzz-elf: %llu inputs (%llu raw, %llu not modelled); load "
            "%llu accepted, %llu refused; map %llu accepted, %llu refused; "
            "%llu segments mapped, %llu .bss tails, %llu near 4 GB\n",
            (unsigned long long)g_st.inputs, (unsigned long long)g_st.raw,
            (unsigned long long)g_st.skipped,
            (unsigned long long)g_st.accepted[0],
            (unsigned long long)g_st.refused[0],
            (unsigned long long)g_st.accepted[1],
            (unsigned long long)g_st.refused[1],
            (unsigned long long)g_st.mapped, (unsigned long long)g_st.bss,
            (unsigned long long)g_st.wraps);
}

static void fail(const char *what, const char *mode, const char *err)
{
    fprintf(stderr, "fuzz-elf: %s (%s; loader said: %s)\n", what, mode,
            err != NULL ? err : "accepted");
    abort();
}

/* The bus as the loader left it: inside 4 GB, nothing overlapping. */
static void check_bus(const emu_bus_t *bus, const char *mode)
{
    for (uint32_t i = 0u; i < bus->count; i++) {
        const emu_region_t *a = &bus->regions[i];

        if ((uint64_t)a->base + a->size > 0x100000000ull) {
            fprintf(stderr, "  region %s at %08x, %08x bytes\n", a->name,
                    (unsigned)a->base, (unsigned)a->size);
            fail("a region wraps past 4 GB", mode, NULL);
        }
        for (uint32_t j = i + 1u; j < bus->count; j++) {
            const emu_region_t *b = &bus->regions[j];

            if ((uint64_t)a->base < (uint64_t)b->base + b->size &&
                (uint64_t)b->base < (uint64_t)a->base + a->size) {
                fprintf(stderr, "  %s at %08x+%x and %s at %08x+%x\n",
                        a->name, (unsigned)a->base, (unsigned)a->size,
                        b->name, (unsigned)b->base, (unsigned)b->size);
                fail("two regions overlap", mode, NULL);
            }
        }
    }
    if (g_dev_writes != 0u) {
        fail("the loader wrote to a device", mode, NULL);
    }
}

static void run_one(const uint8_t *img, size_t len, bool map)
{
    static emu_bus_t bus;
    const char *const mode = map ? "emu_elf_map" : "emu_elf_load";
    seg_t segs[MAX_SEGS];
    mapped_t mapped[MAX_SEGS];
    unsigned nseg = 0u, nmapped = 0u;
    uint32_t want_entry = 0u, entry = 0xDEADBEEFu;
    bool modelled = true;
    const char *err;

    if (!setup(&bus, !map)) {
        abort();
    }
    const bool ok_hdr =
        ref_parse(img, len, segs, &nseg, &want_entry, &modelled);
    if (!modelled) {
        g_st.skipped++;
        return;
    }
    const bool want =
        ok_hdr && ref_apply(img, segs, nseg, map, mapped, &nmapped);

    err = map ? emu_elf_map(&bus, img, len, EMU_ELF_ANY_MACHINE, 0u, RAM_BASE,
                            RAM_SIZE, &entry, NULL)
              : emu_elf_load(&bus, img, len, EMU_ELF_ANY_MACHINE, 0u, &entry,
                             NULL);
    if (getenv("FUZZ_SHOW") != NULL) {
        fprintf(stderr, "%s: %s, reference %s, %u segments\n", mode,
                err != NULL ? err : "accepted", want ? "accepts" : "refuses",
                nseg);
    }
    check_bus(&bus, mode);
    if ((err == NULL) != want) {
        for (unsigned i = 0u; i < nseg; i++) {
            fprintf(stderr,
                    "  PT_LOAD off %08x vaddr %08x paddr %08x filesz %08x "
                    "memsz %08x\n",
                    (unsigned)segs[i].offset, (unsigned)segs[i].vaddr,
                    (unsigned)segs[i].paddr, (unsigned)segs[i].filesz,
                    (unsigned)segs[i].memsz);
        }
        fail(want ? "refused an image the reference accepts"
                  : "accepted an image the reference refuses",
             mode, err);
    }
    if (err != NULL) {
        g_st.refused[map]++;
        return;
    }
    g_st.accepted[map]++;
    if (entry != want_entry) {
        fail("wrong entry point", mode, NULL);
    }
    for (unsigned i = 0u; i < g_nbufs; i++) {
        if (memcmp(g_bufs[i].mem, g_bufs[i].shadow, g_bufs[i].size) != 0) {
            for (uint32_t b = 0u; b < g_bufs[i].size; b++) {
                if (g_bufs[i].mem[b] != g_bufs[i].shadow[b]) {
                    fprintf(stderr, "  at %08x: %02x, reference %02x\n",
                            (unsigned)(g_bufs[i].base + b),
                            g_bufs[i].mem[b], g_bufs[i].shadow[b]);
                    break;
                }
            }
            fail("guest memory differs from the reference", mode, NULL);
        }
    }
    for (unsigned i = 0u; i < nmapped; i++) {
        uint8_t *const got = malloc(mapped[i].size);

        if (got == NULL) {
            abort();
        }
        if (!emu_bus_dump(&bus, mapped[i].base, got, mapped[i].size) ||
            memcmp(got, img + mapped[i].offset, mapped[i].size) != 0) {
            fail("a mapped segment does not read back as the file", mode,
                 NULL);
        }
        free(got);
        g_st.mapped++;
    }
    for (unsigned i = 0u; i < nseg; i++) {
        g_st.bss += segs[i].memsz > segs[i].filesz;
        g_st.wraps += (uint64_t)segs[i].paddr + segs[i].memsz > 0xFFFF0000ull;
    }
}

/* ------------------------------------------------------------------ */
/* Inputs                                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    const uint8_t *p;
    size_t n;
} in_t;

static uint32_t take(in_t *in, unsigned bytes)
{
    uint32_t v = 0u;

    for (unsigned i = 0u; i < bytes; i++) {
        v |= (uint32_t)(in->n != 0u ? (in->n--, *in->p++) : 0u) << (8u * i);
    }
    return v;
}

/* An address near something worth being near. */
static uint32_t pick_addr(in_t *in)
{
    static const uint32_t bases[] = {
        RAM_BASE,           RAM_BASE + RAM_SIZE, ROM_BASE,
        ROM_BASE + ROM_SIZE, LOW_BASE,           TOP_BASE,
        0u - 0x100u,        EMU_GUEST_PERIPH_BASE,
        EMU_GUEST_UART_BASE, 0x30000000u,        0x7FFFFF00u,
    };
    const uint32_t sel = take(in, 1u);
    const int32_t delta = (int16_t)take(in, 2u);

    if (sel == 0xFFu) {
        return take(in, 4u);
    }
    return bases[sel % (sizeof(bases) / sizeof(bases[0]))] + (uint32_t)delta;
}

static void put(uint8_t *p, unsigned n, uint32_t v, bool be)
{
    for (unsigned i = 0u; i < n; i++) {
        p[be ? (n - 1u - i) : i] = (uint8_t)(v >> (8u * i));
    }
}

/*
 * A description: flags, then the program headers, then the bytes the
 * segments' contents come from. Offsets point into those bytes, mostly.
 */
static uint8_t *build(in_t *in, size_t *out_len)
{
    const uint32_t flags = take(in, 1u);
    const uint32_t odd = take(in, 1u); /* header fields at their edges */
    const bool be = (flags & 2u) != 0u;
    const unsigned nph = 1u + (take(in, 1u) % 8u);
    const unsigned phent = 32u + ((flags & 4u) ? (take(in, 1u) % 16u) : 0u);
    const uint32_t phoff_w = (odd & 1u) ? take(in, 4u) : 52u;
    const uint32_t phnum_w = (odd & 2u) ? take(in, 2u) : nph;
    const uint32_t phent_w = (odd & 4u) ? take(in, 2u) : phent;
    seg_t s[8];

    for (unsigned i = 0u; i < nph; i++) {
        const uint32_t k = take(in, 1u);

        s[i].type = (k % 8u == 7u) ? take(in, 1u) % 4u : 1u;
        s[i].paddr = pick_addr(in);
        s[i].vaddr = (k & 0x10u) ? pick_addr(in) : s[i].paddr;
        s[i].filesz = take(in, 2u) % 0x2000u;
        s[i].memsz = s[i].filesz +
                     ((k & 0x20u) ? take(in, 2u) % 0x2000u : 0u) -
                     ((k & 0x40u) ? 1u : 0u);
        s[i].offset = (k & 0x80u) ? take(in, 4u) : take(in, 2u) % 0x400u;
    }
    const size_t data = in->n;
    const size_t hdr = 52u + (size_t)nph * phent;
    const size_t len = hdr + data;
    uint8_t *const img = calloc(1u, len);

    if (img == NULL) {
        abort();
    }
    img[0] = 0x7Fu;
    img[1] = 'E';
    img[2] = 'L';
    img[3] = 'F';
    img[4] = (flags & 8u) ? 2u : 1u;
    img[5] = be ? 2u : 1u;
    img[6] = 1u;
    put(img + 16, 2u, (flags & 0x10u) ? 3u : 2u, be);
    put(img + 18, 2u, 0xF3u, be);
    put(img + 20, 4u, 1u, be);
    put(img + 24, 4u, RAM_BASE + (flags & 0xE0u), be);
    put(img + 28, 4u, phoff_w, be);
    put(img + 40, 2u, 52u, be);
    put(img + 42, 2u, phent_w, be);
    put(img + 44, 2u, phnum_w, be);
    for (unsigned i = 0u; i < nph; i++) {
        uint8_t *const ph = img + 52u + (size_t)i * phent;

        put(ph, 4u, s[i].type, be);
        /* Contents start after the headers unless the input says not. */
        put(ph + 4, 4u,
            (s[i].offset < 0x400u) ? (uint32_t)hdr + s[i].offset : s[i].offset,
            be);
        put(ph + 8, 4u, s[i].vaddr, be);
        put(ph + 12, 4u, s[i].paddr, be);
        put(ph + 16, 4u, s[i].filesz, be);
        put(ph + 20, 4u, s[i].memsz, be);
    }
    memcpy(img + hdr, in->p, data);
    *out_len = len;
    return img;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static bool once;
    uint8_t *img;
    size_t len;

    if (!once) {
        once = true;
        if (getenv("FUZZ_STATS") != NULL) {
            atexit(stats_at_exit);
        }
    }
    if (size == 0u) {
        return 0;
    }
    g_st.inputs++;
    if ((data[0] & 1u) != 0u) {
        g_st.raw++;
        len = size - 1u;
        img = malloc(len != 0u ? len : 1u);
        if (img == NULL) {
            abort();
        }
        memcpy(img, data + 1, len);
    } else {
        in_t in = {data + 1, size - 1u};

        img = build(&in, &len);
    }
    run_one(img, len, false);
    run_one(img, len, true);
    free(img);
    return 0;
}
