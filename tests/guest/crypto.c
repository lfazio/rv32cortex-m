/* SPDX-License-Identifier: Apache-2.0 */
/*
 * crypto.c - AES-128 and SHA-256 on TinyCrypt, as a real workload.
 *
 * Why this guest exists
 * ---------------------
 * The other guests answer "is it correct" and "how fast", and between
 * them they execute almost no bit manipulation: a trace histogram of
 * CoreMark, Dhrystone and bench sampled 400,000 instructions each and
 * found **zero** rotations, bit-extracts or count-leading-zeros. That is
 * not a property of the emulator, it is a property of the workloads --
 * compilers emit shifts and masks for ordinary C.
 *
 * Cryptography is where those instructions live. SHA-256's compression
 * function is built from rotations; AES's key schedule and MixColumns
 * are byte-permutes and shifts. So this guest is what makes the Zbb
 * lowerings measurable at all, and what would notice if one of them were
 * wrong.
 *
 * **It is a correctness test first.** Both algorithms are checked against
 * their published vectors -- FIPS-197 appendix B for AES, FIPS-180-2 for
 * SHA-256 -- so a wrong answer names itself rather than showing up as a
 * changed checksum with nothing to compare against. A crypto primitive
 * has the useful property that almost any error is catastrophic: a
 * single wrong bit in a rotation changes every subsequent byte.
 *
 * TinyCrypt is Intel's, BSD-3-Clause, fetched rather than vendored --
 * same arrangement as CoreMark and for the same reason.
 */

#include <stdbool.h>
#include <stdint.h>

#include "tinycrypt/aes.h"
#include "tinycrypt/constants.h"
#include "tinycrypt/sha256.h"

/*
 * The console, as every guest here does it: there is no libc, so the
 * NS16550 transmit register is written directly.
 */
#define UART_THR (*(volatile uint8_t *)0x10000000u)

static void puts_(const char *s)
{
    while (*s != '\0') {
        UART_THR = (uint8_t)*s++;
    }
}

static void puthex(uint32_t v)
{
    for (int i = 28; i >= 0; i -= 4) {
        const uint32_t d = (v >> i) & 0xFu;

        UART_THR = (uint8_t)(d < 10u ? ('0' + d) : ('a' + d - 10u));
    }
}

/* A byte, for dumping a digest without 32 leading zeros each time. */
static void puthex8(uint8_t v)
{
    for (int i = 4; i >= 0; i -= 4) {
        const uint32_t d = ((uint32_t)v >> i) & 0xFu;

        UART_THR = (uint8_t)(d < 10u ? ('0' + d) : ('a' + d - 10u));
    }
}

/*
 * memcpy and memset, because TinyCrypt's utils.c calls them and guest
 * images link -nostdlib. The first guest here to need either -- everything
 * before this was self-contained or used the compiler's builtins on
 * objects small enough to inline.
 *
 * Deliberately dull: byte at a time, no word-at-a-time fast path. They
 * are not what this guest measures, and a clever one would be a second
 * implementation to get wrong. GCC recognises these shapes and will still
 * call them rather than expand them, which is what the linker wants.
 */
void *memcpy(void *dst, const void *src, unsigned long n);
void *memset(void *dst, int c, unsigned long n);

void *memcpy(void *dst, const void *src, unsigned long n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n-- != 0ul) {
        *d++ = *s++;
    }
    return dst;
}

void *memset(void *dst, int c, unsigned long n)
{
    uint8_t *d = dst;

    while (n-- != 0ul) {
        *d++ = (uint8_t)c;
    }
    return dst;
}

static uint32_t g_fail;

static void check(const char *name, const uint8_t *got, const uint8_t *want,
                  unsigned n)
{
    bool bad = false;

    /* No libc here, so the comparison is written out. */
    for (unsigned i = 0; i < n; i++) {
        if (got[i] != want[i]) {
            bad = true;
        }
    }

    if (bad) {
        g_fail++;
        puts_("  FAIL ");
        puts_(name);
        puts_("\n    got  ");
        for (unsigned i = 0; i < n; i++) {
            puthex8(got[i]);
        }
        puts_("\n    want ");
        for (unsigned i = 0; i < n; i++) {
            puthex8(want[i]);
        }
        puts_("\n");
    } else {
        puts_("  ok   ");
        puts_(name);
        puts_("\n");
    }
}

/*
 * FIPS-197 appendix B: the worked example, which is the one vector every
 * AES implementation is checked against and the one whose intermediate
 * values are published if it fails.
 */
static void test_aes(void)
{
    static const uint8_t key[16] = {0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae,
                                    0xd2, 0xa6, 0xab, 0xf7, 0x15, 0x88,
                                    0x09, 0xcf, 0x4f, 0x3c};
    static const uint8_t in[16] = {0x32, 0x43, 0xf6, 0xa8, 0x88, 0x5a,
                                   0x30, 0x8d, 0x31, 0x31, 0x98, 0xa2,
                                   0xe0, 0x37, 0x07, 0x34};
    static const uint8_t want[16] = {0x39, 0x25, 0x84, 0x1d, 0x02, 0xdc,
                                     0x09, 0xfb, 0xdc, 0x11, 0x85, 0x97,
                                     0x19, 0x6a, 0x0b, 0x32};
    struct tc_aes_key_sched_struct s;
    uint8_t out[16];

    (void)tc_aes128_set_encrypt_key(&s, key);
    (void)tc_aes_encrypt(out, in, &s);
    check("aes128-encrypt", out, want, 16u);

    (void)tc_aes128_set_decrypt_key(&s, key);
    (void)tc_aes_decrypt(out, want, &s);
    check("aes128-decrypt", out, in, 16u);
}

/*
 * FIPS-180-2: "abc", the shortest vector, and the million-'a' one reduced
 * to a length this guest can afford. The first catches a broken round
 * function; the second catches a broken length encoding or a padding
 * error, which the short one cannot.
 */
static void test_sha256(void)
{
    static const uint8_t want_abc[32] = {
        0xba, 0x78, 0x16, 0xbf, 0x8f, 0x01, 0xcf, 0xea, 0x41, 0x41, 0x40,
        0xde, 0x5d, 0xae, 0x22, 0x23, 0xb0, 0x03, 0x61, 0xa3, 0x96, 0x17,
        0x7a, 0x9c, 0xb4, 0x10, 0xff, 0x61, 0xf2, 0x00, 0x15, 0xad};
    struct tc_sha256_state_struct s;
    uint8_t digest[32];

    (void)tc_sha256_init(&s);
    (void)tc_sha256_update(&s, (const uint8_t *)"abc", 3u);
    (void)tc_sha256_final(digest, &s);
    check("sha256-abc", digest, want_abc, 32u);

    /*
     * A longer message, hashed in pieces, so the buffering path runs --
     * the one that has to carry a partial block across calls. A digest
     * of the whole thing in one update would never exercise it.
     */
    static const uint8_t want_split[32] = {
        0x24, 0x8d, 0x6a, 0x61, 0xd2, 0x06, 0x38, 0xb8, 0xe5, 0xc0, 0x26,
        0x93, 0x0c, 0x3e, 0x60, 0x39, 0xa3, 0x3c, 0xe4, 0x59, 0x64, 0xff,
        0x21, 0x67, 0xf6, 0xec, 0xed, 0xd4, 0x19, 0xdb, 0x06, 0xc1};
    static const char msg[] =
        "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";

    (void)tc_sha256_init(&s);
    for (unsigned i = 0; i < sizeof(msg) - 1u; i += 7u) {
        unsigned n = sizeof(msg) - 1u - i;

        (void)tc_sha256_update(&s, (const uint8_t *)msg + i,
                               (n > 7u) ? 7u : n);
    }
    (void)tc_sha256_final(digest, &s);
    check("sha256-split", digest, want_split, 32u);
}

/*
 * The measured part: enough rounds that the figure means something, with
 * the result folded into a checksum so the compiler cannot delete the
 * work and the emulator cannot skip it unnoticed.
 */
#ifndef CRYPTO_ROUNDS
#define CRYPTO_ROUNDS 200u
#endif

static uint32_t bench(void)
{
    static const uint8_t key[16] = {0x00, 0x01, 0x02, 0x03, 0x04, 0x05,
                                    0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b,
                                    0x0c, 0x0d, 0x0e, 0x0f};
    struct tc_aes_key_sched_struct ks;
    struct tc_sha256_state_struct hs;
    uint8_t block[16] = {0};
    uint8_t digest[32];
    uint32_t sum = 0u;

    (void)tc_aes128_set_encrypt_key(&ks, key);

    for (unsigned r = 0; r < CRYPTO_ROUNDS; r++) {
        (void)tc_aes_encrypt(block, block, &ks);

        (void)tc_sha256_init(&hs);
        (void)tc_sha256_update(&hs, block, sizeof(block));
        (void)tc_sha256_final(digest, &hs);

        /* Feed the digest back in, so no round can be hoisted out. */
        for (unsigned i = 0; i < 16u; i++) {
            block[i] ^= digest[i];
        }
        sum = (sum << 1) ^ digest[0] ^ ((uint32_t)digest[31] << 8);
    }
    return sum;
}

int main(void)
{
    puts_("\nCRYPTO-START\n");

    test_aes();
    test_sha256();

    const uint32_t sum = bench();

    puts_("  rounds   0x");
    puthex(CRYPTO_ROUNDS);
    puts_("\n  checksum 0x");
    puthex(sum);
    puts_("\n  failures 0x");
    puthex(g_fail);
    puts_("\nCRYPTO-END\n");

    return (int)g_fail;
}
