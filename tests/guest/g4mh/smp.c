/* SPDX-License-Identifier: Apache-2.0 */
/*
 * smp.c - three PEs that contend, then three that do not.
 *
 * What a host running the cores in parallel has to get right, written so
 * that every number it prints is the same at any interleaving -- so one
 * expected line holds the serial round-robin and an SMP host alike:
 *
 *   lock   a CAXI spinlock around a deliberately non-atomic increment.
 *          Two PEs inside at once lose an increment, and the total
 *          comes out short.
 *   llsc   LDL.W / STC.W increments of one word. A conditional store
 *          that succeeds after another PE's store loses one.
 *   bits   every PE toggles its own bit of one shared byte with NOT1,
 *          an odd number of times. A read-modify-write that is not one
 *          writes a stale byte back and flips another PE's bit. Twice:
 *          once in the register form and once in the immediate one,
 *          because they are different paths through a translator -- the
 *          JIT lowers only the second, and a guest using only the first
 *          left a mutant in it standing.
 *   sum    each PE's own xorshift loop, touching nothing shared: the
 *          part a multicore host runs in parallel, and what the speedup
 *          is measured on. The values are tests/guest/g4mh/smp_want.py's.
 *
 * Shared state is at a fixed address rather than in .bss, because every
 * PE runs crt0 and crt0 zeroes .bss -- a PE released late would wipe
 * what the others had done.
 *
 * Needs a three-PE build (-DG4MH_PE_COUNT=3) and --load 0x80000000, the
 * address CC-RH links at; scripts/g4mh-build-guest.sh builds it.
 */

#define NPE 3u

#ifndef N_LOCK
#define N_LOCK 20000u
#endif
#ifndef N_LLSC
#define N_LLSC 20000u
#endif
#ifndef N_BITS
#define N_BITS 200001u /* odd: every bit ends set */
#endif
#ifndef N_WORK
#define N_WORK 1000000u
#endif

#define BOOTCTRL (*(volatile unsigned *)0xFFFB2000u)
#define UART_THR (*(volatile unsigned char *)0x10000000u)

/* 16 KiB into RAM: past the image, below every PE's stack, and inside
 * the smallest RAM a board gives a G4MH guest. */
#define SHARED ((volatile unsigned *)0x80004000u)
#define LOCK SHARED[0]
#define COUNT_LOCK SHARED[1]
#define COUNT_LLSC SHARED[2]
#define BITS (*(volatile unsigned char *)&SHARED[3])
#define BITS2 (*(volatile unsigned char *)&SHARED[5])
#define DONE SHARED[4]
#define SUM(pe) SHARED[8u + (pe)]

/* See guest.c: clang-format breaks inline_asm comments, and only the
 * bare form of this fence is recognised -- text after "off" disables it. */
/* clang-format off */
#pragma inline_asm read_htcfg0
static unsigned read_htcfg0(void)
{
    stsr 0, r10, 2      ; HTCFG0 is selID 2, register 0
}

#pragma inline_asm halt_
static void halt_(void)
{
    halt
}

#pragma inline_asm cas_
static unsigned cas_(volatile unsigned *p, unsigned expect, unsigned want)
{
    caxi [r6], r7, r8   ; r8 <- the word as it was
    mov  r8, r10
}

#pragma inline_asm ldl_
static unsigned ldl_(volatile unsigned *p)
{
    ldl.w [r6], r10
}

#pragma inline_asm stc_
static unsigned stc_(volatile unsigned *p, unsigned v)
{
    stc.w r7, [r6]      ; r7 <- 1 stored, 0 lost the reservation
    mov   r7, r10
}

#pragma inline_asm not1_
static void not1_(volatile unsigned char *p, unsigned bit)
{
    not1 r7, [r6]
}

/* Format VIII: the bit number is in the encoding, so one per PE. */
#pragma inline_asm not1_b0
static void not1_b0(volatile unsigned char *p)
{
    not1 0, 0[r6]
}

#pragma inline_asm not1_b1
static void not1_b1(volatile unsigned char *p)
{
    not1 1, 0[r6]
}

#pragma inline_asm not1_b2
static void not1_b2(volatile unsigned char *p)
{
    not1 2, 0[r6]
}

#pragma inline_asm syncm_
static void syncm_(void)
{
    syncm
}
/* clang-format on */

static void putc_(char c)
{
    UART_THR = (unsigned char)c;
}

static void puts_(const char *s)
{
    while (*s != '\0') {
        putc_(*s++);
    }
}

static void putu(unsigned v)
{
    unsigned scale = 1u;

    while (v / scale >= 10u) {
        scale *= 10u;
    }
    while (scale != 0u) {
        putc_((char)('0' + ((v / scale) % 10u)));
        scale /= 10u;
    }
}

static void puthex(unsigned v)
{
    int i;

    for (i = 28; i >= 0; i -= 4) {
        putc_("0123456789abcdef"[(v >> i) & 0xFu]);
    }
}

static void lock(void)
{
    while (cas_(&LOCK, 0u, 1u) != 0u) {
        /* CAXI comparing unequal yields the quantum */
    }
    syncm_();
}

static void unlock(void)
{
    syncm_();
    LOCK = 0u;
}

static void llsc_inc(volatile unsigned *p)
{
    unsigned v;

    do {
        v = ldl_(p);
    } while (stc_(p, v + 1u) == 0u);
}

static unsigned work(unsigned x, unsigned n)
{
    unsigned sum = 0u;

    while (n-- != 0u) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        sum += x;
    }
    return sum;
}

int main(void)
{
    const unsigned pe = read_htcfg0() & 0x7u;
    unsigned i;

    if (pe == 0u) {
        for (i = 0u; i < 16u; i++) {
            SHARED[i] = 0u;
        }
        syncm_();
        BOOTCTRL = (1u << NPE) - 1u; /* the others start here */
    }

    for (i = 0u; i < N_LOCK; i++) {
        lock();
        COUNT_LOCK = COUNT_LOCK + 1u; /* not atomic, on purpose */
        unlock();
    }
    for (i = 0u; i < N_LLSC; i++) {
        llsc_inc(&COUNT_LLSC);
    }
    for (i = 0u; i < N_BITS; i++) {
        not1_(&BITS, pe);
    }
    for (i = 0u; i < N_BITS; i++) {
        if (pe == 0u) {
            not1_b0(&BITS2);
        } else if (pe == 1u) {
            not1_b1(&BITS2);
        } else {
            not1_b2(&BITS2);
        }
    }
    SUM(pe) = work(0x9E3779B9u * (pe + 1u), N_WORK);
    syncm_();
    llsc_inc(&DONE);

    if (pe == 0u) {
        while (DONE != NPE) {
            /* the others' results are in once they have counted */
        }
        syncm_();
        puts_("g4mh smp: lock=");
        putu(COUNT_LOCK);
        puts_(" llsc=");
        putu(COUNT_LLSC);
        puts_(" bits=");
        putu(BITS);
        puts_(",");
        putu(BITS2);
        puts_(" sum=");
        for (i = 0u; i < NPE; i++) {
            puthex(SUM(i));
            putc_(i + 1u < NPE ? ',' : '\n');
        }
        puts_("G4MH-SMP-END\n");
    }
    halt_();
    return 0;
}
