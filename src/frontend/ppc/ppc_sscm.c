/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_sscm.c - SSCM_DPMBOOT and SSCM_DPMKEY, which start core 1.
 *
 * MPC5675K Reference Manual, Rev. 10, section 5.6.2.2: "Write the reset
 * vector into DPMBOOT[P2BOOT]. Set DPMBOOT[DVLE] to indicate that the
 * second core will be executing in VLE mode. Write 0x5AF0 to
 * DPMKEY[KEY]. Write 0xA50F to DPMKEY[KEY]. After the second write to
 * DPMKEY[KEY], Core_1 jumps to its reset vector."
 *
 * Three choices where the manual says nothing, each the conservative
 * reading:
 *
 *   - **any other key ends the sequence.** A write that is neither the
 *     key expected next nor 0x5AF0 -- which always begins one -- leaves
 *     core 1 where it is;
 *   - **the sequence releases a core in reset, and nothing else.** The
 *     manual describes it as releasing core 1 from reset, not as a way
 *     to restart one that runs, so a second sequence while core 1 runs
 *     does nothing;
 *   - **DPMBOOT is read when the second key lands**, so a guest that
 *     rewrites it between the keys starts core 1 at the later address.
 */

#include "ppc/ppc_sscm.h"
#include "ppc/ppc_cpu.h"

void ppc_sscm_reset(ppc_sscm_t *s)
{
    s->dpmboot = 0u;
    s->error = 0u;
    s->debugport = 0u;
    s->key_armed = false;
}

/*
 * The register an access falls in, as a 32-bit word at `*base`, and the
 * shift that brings the accessed bytes to the bottom -- big-endian, so
 * the lowest address is the most significant byte. False for an access
 * that is misaligned (49.3) or reaches past the module.
 */
static bool lane(uint32_t off, uint32_t size, uint32_t *base, uint32_t *shift)
{
    if ((size != 1u && size != 2u && size != 4u) || (off & (size - 1u)) != 0u ||
        off + size > PPC_SSCM_SIZE) {
        return false;
    }
    *base = off & ~3u;
    *shift = (4u - (off - *base) - size) * 8u;
    return true;
}

static uint32_t mask_of(uint32_t size)
{
    return (size == 4u) ? 0xFFFFFFFFu : ((1u << (8u * size)) - 1u);
}

/* The word at `base` as the registers in it read. */
static uint32_t word_read(const ppc_sscm_t *s, uint32_t base)
{
    switch (base) {
    case 0x00u: /* STATUS: LSM 0, the device is in DPM | MEMCONFIG */
        return PPC_SSCM_MEMCONFIG_RESET;
    case 0x04u: /* reserved | ERROR */
        return s->error;
    case 0x08u: /* DEBUGPORT | reserved */
        return (uint32_t)s->debugport << 16;
    case PPC_SSCM_DPMBOOT:
        return s->dpmboot;
    default: /* the password words, DPMKEY: write-only; UOPS: all zero */
        return 0u;
    }
}

static emu_fault_t sscm_read(void *ctx, uint32_t off, uint32_t size,
                             uint32_t *out)
{
    const ppc_sscm_t *const s = (const ppc_sscm_t *)ctx;
    uint32_t base;
    uint32_t shift;

    if (!lane(off, size, &base, &shift) || base == 0x14u) {
        return EMU_FAULT_LOAD;
    }
    *out = (word_read(s, base) >> shift) & mask_of(size);
    return EMU_FAULT_NONE;
}

static void key(ppc_sscm_t *s, uint32_t k)
{
    if (k == PPC_SSCM_KEY2 && s->key_armed) {
        s->key_armed = false;
        if (s->core1 != NULL && ppc_cpu_held(s->core1)) {
            ppc_cpu_start(s->core1, s->dpmboot & PPC_SSCM_DPMBOOT_P2BOOT,
                          (s->dpmboot & PPC_SSCM_DPMBOOT_DVLE) != 0u);
        }
        return;
    }
    s->key_armed = (k == PPC_SSCM_KEY1);
}

static emu_fault_t sscm_write(void *ctx, uint32_t off, uint32_t size,
                              uint32_t val)
{
    ppc_sscm_t *const s = (ppc_sscm_t *)ctx;
    uint32_t base;
    uint32_t shift;

    if (!lane(off, size, &base, &shift) || base == 0x14u ||
        base == PPC_SSCM_UOPS) {
        return EMU_FAULT_STORE;
    }
    const uint32_t m = mask_of(size) << shift;
    const uint32_t v = (val << shift) & m;

    switch (base) {
    case 0x04u:
        s->error = (uint16_t)((s->error & ~m) | v);
        break;
    case 0x08u:
        s->debugport =
            (uint16_t)(((((uint32_t)s->debugport << 16) & ~m) | v) >> 16);
        break;
    case PPC_SSCM_DPMBOOT:
        s->dpmboot = ((s->dpmboot & ~m) | v) &
                     (PPC_SSCM_DPMBOOT_P2BOOT | PPC_SSCM_DPMBOOT_DVLE);
        break;
    case PPC_SSCM_DPMKEY:
        /* KEY is the low half; a write that does not reach it is none. */
        if ((m & 0xFFFFu) != 0u) {
            key(s, v & 0xFFFFu);
        }
        break;
    default: /* STATUS's w1c bit and the password words: nothing to do */
        break;
    }
    return EMU_FAULT_NONE;
}

const emu_dev_ops_t ppc_sscm_ops = {
    .read = sscm_read,
    .write = sscm_write,
    .tick = NULL,
};
