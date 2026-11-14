/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv_crypto.c - RISC-V scalar cryptography, RV32 (rv_crypto.h).
 *
 * Each instruction as the Scalar Cryptography ISA v1.0.1 defines it, and
 * as Spike and Sail compute it: the architecture suites for each
 * extension and tests/arch-test/probes/kcrypto.S, which runs every S-box
 * entry through the four instructions that read one, hold it to both.
 *
 * The AES S-box is emu_crypto.c's, which the IR shares; the SM4 table
 * is the published one (GB/T 32907-2016), which has no shorter
 * definition to compute from.
 */
#include "rv32/rv_crypto.h"

#include "emu/emu_crypto.h"
#include "emu/emu_types.h"

#if RV_EXT_ZBKB || RV_EXT_ZBKX || RV_EXT_ZKND || RV_EXT_ZKNE || \
    RV_EXT_ZKNH || RV_EXT_ZKSED || RV_EXT_ZKSH
#define RV_CRYPTO_ANY 1
#else
#define RV_CRYPTO_ANY 0
#endif


static EMU_ALWAYS_INLINE uint32_t rol32(uint32_t x, uint32_t n)
{
    n &= 31u;
    return (x << n) | (x >> ((32u - n) & 31u));
}

#if RV_EXT_ZKNE || RV_EXT_ZKND
/* x * 2 in GF(2^8), without a test on x's top bit. */
static EMU_ALWAYS_INLINE uint32_t xt2(uint32_t x)
{
    return ((x << 1) ^ (0x1Bu & (0u - (x >> 7)))) & 0xFFu;
}
#endif

/* The instruction's byte select, bs, as a shift: bits 31:30. */
static EMU_ALWAYS_INLINE uint32_t bs_shift(uint32_t insn)
{
    return (insn >> 30) * 8u;
}

#if RV_EXT_ZBKB
/* Reverse the bits of each byte. */
static uint32_t brev8(uint32_t x)
{
    x = ((x & 0x55555555u) << 1) | ((x >> 1) & 0x55555555u);
    x = ((x & 0x33333333u) << 2) | ((x >> 2) & 0x33333333u);
    return ((x & 0x0F0F0F0Fu) << 4) | ((x >> 4) & 0x0F0F0F0Fu);
}

/* The low half's bits to the even positions, the high half's to the odd. */
static uint32_t zip32(uint32_t x)
{
    uint32_t r = 0u;

    for (uint32_t i = 0u; i < 16u; i++) {
        r |= ((x >> i) & 1u) << (2u * i);
        r |= ((x >> (i + 16u)) & 1u) << (2u * i + 1u);
    }
    return r;
}

static uint32_t unzip32(uint32_t x)
{
    uint32_t r = 0u;

    for (uint32_t i = 0u; i < 16u; i++) {
        r |= ((x >> (2u * i)) & 1u) << i;
        r |= ((x >> (2u * i + 1u)) & 1u) << (i + 16u);
    }
    return r;
}
#endif

#if RV_EXT_ZBKX
/*
 * Each `bits`-wide element of b names one of a's, or zero when it names
 * none: 8 nibbles for xperm4, 4 bytes for xperm8. The out-of-range case
 * is a mask, not a test, and the variable shift is the barrel shifter's.
 */
static uint32_t xperm(uint32_t a, uint32_t b, uint32_t bits)
{
    const uint32_t n = 32u / bits;
    const uint32_t mask = (1u << bits) - 1u;
    uint32_t r = 0u;

    for (uint32_t i = 0u; i < n; i++) {
        const uint32_t idx = (b >> (i * bits)) & mask;
        const uint32_t ok = 0u - (uint32_t)(idx < n); /* ~0 in range */
        const uint32_t v = (a >> ((idx * bits) & 31u)) & mask;

        r |= (v & ok) << (i * bits);
    }
    return r;
}
#endif

bool rv_crypto_is(uint32_t insn)
{
    uint32_t v;

    return rv_crypto_exec(insn, 0u, 0u, &v);
}

bool rv_crypto_exec(uint32_t insn, uint32_t a, uint32_t b, uint32_t *out)
{
#if RV_CRYPTO_ANY
    const uint32_t op = insn & 0x7Fu;
    const uint32_t f3 = (insn >> 12) & 7u;
    const uint32_t f7 = insn >> 25;
    const uint32_t rs2 = (insn >> 20) & 31u;
    uint32_t v;

    if (op == 0x13u) { /* OP-IMM: rs2 is part of the encoding */
        switch ((f7 << 8) | (rs2 << 3) | f3) {
#if RV_EXT_ZBKB
        case (0x34u << 8) | (7u << 3) | 5u: /* brev8 */
            v = brev8(a);
            break;
        case (0x04u << 8) | (15u << 3) | 1u: /* zip */
            v = zip32(a);
            break;
        case (0x04u << 8) | (15u << 3) | 5u: /* unzip */
            v = unzip32(a);
            break;
#endif
#if RV_EXT_ZKNH
        case (0x08u << 8) | (0u << 3) | 1u: /* sha256sum0 */
            v = rol32(a, 30u) ^ rol32(a, 19u) ^ rol32(a, 10u);
            break;
        case (0x08u << 8) | (1u << 3) | 1u: /* sha256sum1 */
            v = rol32(a, 26u) ^ rol32(a, 21u) ^ rol32(a, 7u);
            break;
        case (0x08u << 8) | (2u << 3) | 1u: /* sha256sig0 */
            v = rol32(a, 25u) ^ rol32(a, 14u) ^ (a >> 3);
            break;
        case (0x08u << 8) | (3u << 3) | 1u: /* sha256sig1 */
            v = rol32(a, 15u) ^ rol32(a, 13u) ^ (a >> 10);
            break;
#endif
#if RV_EXT_ZKSH
        case (0x08u << 8) | (8u << 3) | 1u: /* sm3p0 */
            v = a ^ rol32(a, 9u) ^ rol32(a, 17u);
            break;
        case (0x08u << 8) | (9u << 3) | 1u: /* sm3p1 */
            v = a ^ rol32(a, 15u) ^ rol32(a, 23u);
            break;
#endif
        default:
            return false;
        }
        *out = v;
        return true;
    }
    if (op != 0x33u) {
        return false;
    }
#if RV_EXT_ZBKB
    if (f7 == 0x04u && f3 == 4u) { /* pack; rs2 0 is also zext.h */
        *out = (b << 16) | (a & 0xFFFFu);
        return true;
    }
    if (f7 == 0x04u && f3 == 7u) { /* packh */
        *out = ((b & 0xFFu) << 8) | (a & 0xFFu);
        return true;
    }
#endif
#if RV_EXT_ZBKX
    if (f7 == 0x14u && (f3 == 2u || f3 == 4u)) { /* xperm4, xperm8 */
        *out = xperm(a, b, (f3 == 2u) ? 4u : 8u);
        return true;
    }
#endif
    if (f3 != 0u) {
        return false;
    }
    switch (f7) {
#if RV_EXT_ZKNH
    case 0x28u: /* sha512sum0r */
        v = (a << 25) ^ (a << 30) ^ (a >> 28) ^ (b >> 7) ^ (b >> 2) ^ (b << 4);
        break;
    case 0x29u: /* sha512sum1r */
        v = (a << 23) ^ (a >> 14) ^ (a >> 18) ^ (b >> 9) ^ (b << 18) ^
            (b << 14);
        break;
    case 0x2Au: /* sha512sig0l */
        v = (a >> 1) ^ (a >> 7) ^ (a >> 8) ^ (b << 31) ^ (b << 25) ^ (b << 24);
        break;
    case 0x2Bu: /* sha512sig1l */
        v = (a << 3) ^ (a >> 6) ^ (a >> 19) ^ (b >> 29) ^ (b << 26) ^
            (b << 13);
        break;
    case 0x2Eu: /* sha512sig0h */
        v = (a >> 1) ^ (a >> 7) ^ (a >> 8) ^ (b << 31) ^ (b << 24);
        break;
    case 0x2Fu: /* sha512sig1h */
        v = (a << 3) ^ (a >> 6) ^ (a >> 19) ^ (b >> 29) ^ (b << 13);
        break;
#endif
    default: {
        /* The byte-select forms: bs in bits 31:30, the operation below. */
        const uint32_t sh = bs_shift(insn);
        const uint32_t x = (b >> sh) & 0xFFu;
        uint32_t u;

        switch (f7 & 0x1Fu) {
#if RV_EXT_ZKNE
        case 0x11u: /* aes32esi */
            u = emu_aes_sbox(x, false);
            break;
        case 0x13u: { /* aes32esmi: the S-box byte times {3, 1, 1, 2} */
            const uint32_t s = emu_aes_sbox(x, false);
            const uint32_t s2 = xt2(s);

            u = ((s ^ s2) << 24) | (s << 16) | (s << 8) | s2;
            break;
        }
#endif
#if RV_EXT_ZKND
        case 0x15u: /* aes32dsi */
            u = emu_aes_sbox(x, true);
            break;
        case 0x17u: { /* aes32dsmi: times {11, 13, 9, 14} */
            const uint32_t s = emu_aes_sbox(x, true);
            const uint32_t s2 = xt2(s);
            const uint32_t s4 = xt2(s2);
            const uint32_t s8 = xt2(s4);

            u = ((s8 ^ s2 ^ s) << 24) | ((s8 ^ s4 ^ s) << 16) |
                ((s8 ^ s) << 8) | (s8 ^ s4 ^ s2);
            break;
        }
#endif
#if RV_EXT_ZKSED
        case 0x18u: { /* sm4ed */
            const uint32_t s = emu_sm4_sbox(x);

            u = s ^ (s << 8) ^ (s << 2) ^ (s << 18) ^ ((s & 0x3Fu) << 26) ^
                ((s & 0xC0u) << 10);
            break;
        }
        case 0x1Au: { /* sm4ks */
            const uint32_t s = emu_sm4_sbox(x);

            u = s ^ ((s & 0x07u) << 29) ^ ((s & 0xFEu) << 7) ^
                ((s & 0x01u) << 23) ^ ((s & 0xF8u) << 13);
            break;
        }
#endif
        default:
            (void)x;
            (void)sh;
            return false;
        }
        v = a ^ rol32(u, sh);
        break;
    }
    }
    *out = v;
    return true;
#else
    (void)insn;
    (void)a;
    (void)b;
    (void)out;
    return false;
#endif
}
