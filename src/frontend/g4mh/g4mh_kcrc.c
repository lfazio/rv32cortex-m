/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_kcrc.c - Data CRC Function K: see g4mh_kcrc.h.
 *
 * Bit at a time, which is slow and needs no table: this is a device a
 * guest feeds a word at a time through the bus, and the bus access
 * costs more than the 32 iterations.
 */

#include "g4mh/g4mh_kcrc.h"

void g4mh_kcrc_init(g4mh_kcrc_t *k)
{
    /*
     * The CRC32 row of table 57.15, as reset leaves it: polynomial
     * 04C11DB7H, mask FFFF_FFFFH, and a register of FFFF_FFFFH, which
     * is why DOUT0 reads "0000 0000H after reset".
     */
    k->state = 0xFFFFFFFFull;
    k->poly = 0x04C11DB7ull;
    k->mask = 0xFFFFFFFFull;
    k->ctl = G4MH_KCRC_CTL_RESET;
    k->din = 0u;
}

/* PSIZE is the width less one: 07H for 8 bits up to 3FH for 64. */
static unsigned width(const g4mh_kcrc_t *k)
{
    return ((k->ctl & G4MH_KCRC_CTL_PSIZE_MASK) >> G4MH_KCRC_CTL_PSIZE_SHIFT) + 1u;
}

static uint64_t width_mask(unsigned w)
{
    return (w >= 64u) ? ~0ull : ((1ull << w) - 1u);
}

static uint64_t reflect(uint64_t v, unsigned w)
{
    uint64_t r = 0u;

    for (unsigned i = 0u; i < w; i++) {
        r = (r << 1) | ((v >> i) & 1u);
    }
    return r;
}

static bool lsb_first(const g4mh_kcrc_t *k)
{
    return (k->ctl & G4MH_KCRC_CTL_CMD2) != 0u;
}

static bool refin(const g4mh_kcrc_t *k)
{
    return ((k->ctl & G4MH_KCRC_CTL_CMD1) != 0u) != lsb_first(k);
}

static bool refout(const g4mh_kcrc_t *k)
{
    return ((k->ctl & G4MH_KCRC_CTL_CMD0) != 0u) != lsb_first(k);
}

static void feed_byte(g4mh_kcrc_t *k, uint32_t byte)
{
    const unsigned w = width(k);
    const uint64_t wm = width_mask(w);
    const uint64_t top = 1ull << (w - 1u);
    const uint64_t poly = (lsb_first(k) ? reflect(k->poly, w) : k->poly) & wm;
    const uint32_t b = refin(k) ? (uint32_t)reflect(byte & 0xFFu, 8u) : (byte & 0xFFu);
    uint64_t s = k->state & wm;

    for (int bit = 7; bit >= 0; bit--) {
        const bool fb = ((s & top) != 0u) != (((b >> bit) & 1u) != 0u);

        s = (s << 1) & wm;
        if (fb) {
            s ^= poly;
        }
    }
    k->state = (k->state & ~wm) | s;
}

/* The value DOUT1:DOUT0 reads: reflected if asked, then masked. */
static uint64_t result(const g4mh_kcrc_t *k)
{
    const unsigned w = width(k);
    const uint64_t s = k->state & width_mask(w);

    return ((refout(k) ? reflect(s, w) : s) ^ k->mask) & width_mask(w);
}

static emu_fault_t kcrc_read(void *ctx, uint32_t off, uint32_t size,
                             uint32_t *out)
{
    const g4mh_kcrc_t *k = (const g4mh_kcrc_t *)ctx;

    if (off == G4MH_KCRC_DIN) {
        /* 32, 16 or 8 bits; the bus extracts the bytes asked for. */
        *out = k->din;
        return EMU_FAULT_NONE;
    }
    /* Everything else is 32-bit only (table 57.6). */
    if (size != 4u || (off & 3u) != 0u) {
        return EMU_FAULT_LOAD;
    }
    switch (off) {
    case G4MH_KCRC_DOUT0:
        *out = (uint32_t)result(k);
        break;
    case G4MH_KCRC_DOUT1:
        *out = (uint32_t)(result(k) >> 32);
        break;
    case G4MH_KCRC_CTL:
        *out = k->ctl;
        break;
    case G4MH_KCRC_POLY0:
        *out = (uint32_t)k->poly;
        break;
    case G4MH_KCRC_POLY1:
        *out = (uint32_t)(k->poly >> 32);
        break;
    case G4MH_KCRC_XOR0:
        *out = (uint32_t)k->mask;
        break;
    case G4MH_KCRC_XOR1:
        *out = (uint32_t)(k->mask >> 32);
        break;
    default:
        *out = 0u;
        break;
    }
    return EMU_FAULT_NONE;
}

static emu_fault_t kcrc_write(void *ctx, uint32_t off, uint32_t size,
                              uint32_t val)
{
    g4mh_kcrc_t *k = (g4mh_kcrc_t *)ctx;

    if (off == G4MH_KCRC_DIN) {
        /*
         * DW decides how many bytes take part, whatever the width of
         * the write: the manual's own sequence switches DW to 8 bits
         * before its final 8-bit write. Least significant byte first.
         */
        const uint32_t dw = k->ctl & G4MH_KCRC_CTL_DW_MASK;
        const unsigned n = (dw == 3u) ? 1u : ((dw == 1u) ? 2u : 4u);

        (void)size;
        k->din = val;
        for (unsigned i = 0u; i < n; i++) {
            feed_byte(k, val >> (8u * i));
        }
        return EMU_FAULT_NONE;
    }
    if (size != 4u || (off & 3u) != 0u) {
        return EMU_FAULT_STORE;
    }
    switch (off) {
    case G4MH_KCRC_DOUT0:
        k->state = (k->state & 0xFFFFFFFF00000000ull) | val;
        break;
    case G4MH_KCRC_DOUT1:
        k->state = (k->state & 0xFFFFFFFFull) | ((uint64_t)val << 32);
        break;
    case G4MH_KCRC_CTL:
        /* "Write the value after reset" to the reserved bits: they stay. */
        k->ctl = (val & G4MH_KCRC_CTL_WRITABLE) |
                 (G4MH_KCRC_CTL_RESET & ~G4MH_KCRC_CTL_WRITABLE);
        break;
    case G4MH_KCRC_POLY0:
        k->poly = (k->poly & 0xFFFFFFFF00000000ull) | val;
        break;
    case G4MH_KCRC_POLY1:
        k->poly = (k->poly & 0xFFFFFFFFull) | ((uint64_t)val << 32);
        break;
    case G4MH_KCRC_XOR0:
        k->mask = (k->mask & 0xFFFFFFFF00000000ull) | val;
        break;
    case G4MH_KCRC_XOR1:
        k->mask = (k->mask & 0xFFFFFFFFull) | ((uint64_t)val << 32);
        break;
    default:
        break;
    }
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t g4mh_kcrc_ops = {
    .read = kcrc_read,
    .write = kcrc_write,
    .tick = NULL,
};
