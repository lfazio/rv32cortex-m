/* SPDX-License-Identifier: Apache-2.0 */
/*
 * The smallest userspace that proves Linux reached it.
 *
 * There is no rv32 libc in the cross toolchain this repo uses -- the
 * riscv64-linux-gnu compiler can *generate* rv32 code but ships no rv32
 * runtime to link against -- so this is -nostdlib with the syscalls
 * written out. That is a feature for a first boot rather than a
 * limitation: a static binary with no interpreter and no relocations
 * removes every failure mode between "the kernel mounted the initramfs"
 * and "userspace ran", so if this prints, the kernel got the whole way.
 *
 * The RISC-V Linux syscall convention: a7 is the number, a0-a5 the
 * arguments, `ecall` the trap, a0 the result. rv32 uses the generic
 * numbering, so write is 64 and exit_group is 94.
 */

static long sys(long n, long a, long b, long c)
{
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;

    __asm__ volatile("ecall" : "+r"(a0) : "r"(a7), "r"(a1), "r"(a2) : "memory");
    return a0;
}

/* The five-argument form, for mount and openat. */
static long sys5(long n, long a, long b, long c, long d, long e)
{
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a4 __asm__("a4") = e;

    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4)
                     : "memory");
    return a0;
}

static void put(const char *s)
{
    long n = 0;

    while (s[n] != '\0') {
        n++;
    }
    (void)sys(64, 1, (long)s, n); /* write(1, s, n) */
}

/*
 * Give ourselves a console.
 *
 * The kernel opens /dev/console as fd 0, 1 and 2 for init when it can,
 * and it could not: an initramfs holding one file has no /dev, so the
 * boot said "Warning: unable to open an initial console" and every
 * write(1, ...) below went to a closed descriptor. Userspace *ran* --
 * the exit_group at the end reached the kernel and panicked it with the
 * right exit code -- and produced not one visible byte, which is the
 * shape of failure this tree keeps recording: a run that cannot report
 * itself reads exactly like a run that did not happen.
 *
 * devtmpfs, because the kernel has already populated it with the console
 * node by the time init runs; mounting it is cheaper than persuading
 * gen_init_cpio to carry a device node.
 */
static void console_init(void)
{
    /* mount("devtmpfs", "/dev", "devtmpfs", 0, NULL) */
    (void)sys5(40, (long)"devtmpfs", (long)"/dev", (long)"devtmpfs", 0, 0);

    /* openat(AT_FDCWD, "/dev/console", O_RDWR) */
    const long fd = sys5(56, -100, (long)"/dev/console", 2, 0, 0);

    if (fd >= 0) {
        (void)sys5(24, fd, 0, 0, 0, 0); /* dup3(fd, 0, 0) */
        (void)sys5(24, fd, 1, 0, 0, 0);
        (void)sys5(24, fd, 2, 0, 0, 0);
    }
}

/* ------------------------------------------------------------------ */
/* What the device tree promises, asked from user mode                 */
/* ------------------------------------------------------------------ */

/*
 * boot/rv32-emu.dts names a dozen extensions, and a kernel that boots
 * with them named has shown only that it did not die of them. These are
 * the ones a user program can reach, executed here: under a real kernel,
 * in U-mode, behind its page tables and its interrupt handling -- none
 * of which the bare-metal self-test has.
 *
 * Each is assembled with `.option arch`, because this file is built
 * -march=rv32ima and has to keep running on a kernel told less.
 *
 * A failure here is rarely a wrong value. It is a signal: an instruction
 * the emulator refuses kills init, and the kernel's panic names the
 * signal in `exitcode` -- 4 for SIGILL, 0xb for SIGSEGV.
 */
#define WITH_EXT(ext, body)                                                    \
    ".option push\n.option arch, +" ext "\n" body "\n.option pop\n"

static int g_ext_checks;
static int g_ext_fails;

static void put_hex(unsigned long v)
{
    char b[11];

    b[0] = '0';
    b[1] = 'x';
    for (int i = 0; i < 8; i++) {
        const unsigned d = (unsigned)(v >> (28 - 4 * i)) & 0xFu;

        b[2 + i] = (char)((d < 10u) ? ('0' + d) : ('a' + d - 10u));
    }
    b[10] = '\0';
    put(b);
}

static void expect(const char *name, unsigned long got, unsigned long want)
{
    g_ext_checks++;
    if (got != want) {
        g_ext_fails++;
        put("RV32-LINUX-EXT: FAIL ");
        put(name);
        put(" got ");
        put_hex(got);
        put(" want ");
        put_hex(want);
        put("\n");
    }
}

static long sys6(long n, long a, long b, long c, long d, long e, long f)
{
    register long a7 __asm__("a7") = n;
    register long a0 __asm__("a0") = a;
    register long a1 __asm__("a1") = b;
    register long a2 __asm__("a2") = c;
    register long a3 __asm__("a3") = d;
    register long a4 __asm__("a4") = e;
    register long a5 __asm__("a5") = f;

    __asm__ volatile("ecall"
                     : "+r"(a0)
                     : "r"(a7), "r"(a1), "r"(a2), "r"(a3), "r"(a4), "r"(a5)
                     : "memory");
    return a0;
}

static unsigned char g_block[64] __attribute__((aligned(64)));
static volatile unsigned long g_word;

static void test_extensions(void)
{
    volatile unsigned long one = 1, zero = 0, val = 0x12345678ul;
    unsigned long r;

    /*
     * A line as each group is reached, before it runs: when one of these
     * kills the process there is no later line to say which, and the
     * panic that follows names only the signal.
     */
    put("RV32-LINUX-EXT: zicond\n");
    __asm__ volatile(WITH_EXT("zicond", "czero.eqz %0, %1, %2")
                     : "=&r"(r)
                     : "r"((unsigned long)val), "r"((unsigned long)zero));
    expect("czero.eqz-0", r, 0ul);
    __asm__ volatile(WITH_EXT("zicond", "czero.nez %0, %1, %2")
                     : "=&r"(r)
                     : "r"((unsigned long)val), "r"((unsigned long)zero));
    expect("czero.nez-0", r, 0x12345678ul);
    __asm__ volatile(WITH_EXT("zicond", "czero.eqz %0, %1, %2")
                     : "=&r"(r)
                     : "r"((unsigned long)val), "r"((unsigned long)one));
    expect("czero.eqz-1", r, 0x12345678ul);

    /*
     * The hints, and the one that matters under an operating system: a
     * prefetch of address zero. Nothing is mapped there in any process,
     * so an emulator that "performs" the prefetch takes a page fault the
     * kernel turns into SIGSEGV.
     */
    put("RV32-LINUX-EXT: hints\n");
    __asm__ volatile(WITH_EXT("zihintpause, +zihintntl, +zicbop",
                              "pause\n"
                              "ntl.all\n"
                              "prefetch.r 0(%0)\n"
                              "prefetch.w 32(%0)") ::"r"(0ul));
    expect("hints-survived", 1ul, 1ul);

    /* Zalasr and Zacas, on ordinary process memory. */
    put("RV32-LINUX-EXT: zalasr, zacas\n");
    g_word = 0xCAFEF00Dul;
    __asm__ volatile(WITH_EXT("zalasr", "lw.aq %0, (%1)")
                     : "=&r"(r)
                     : "r"(&g_word)
                     : "memory");
    expect("lw.aq", r, 0xCAFEF00Dul);
    __asm__ volatile(WITH_EXT("zalasr", "sw.rl %0, (%1)") ::"r"(0x0BADF00Dul),
                     "r"(&g_word)
                     : "memory");
    expect("sw.rl", g_word, 0x0BADF00Dul);

    r = 0x0BADF00Dul;
    __asm__ volatile(WITH_EXT("zacas", "amocas.w %0, %2, (%1)")
                     : "+r"(r)
                     : "r"(&g_word), "r"(0x600DCAFEul)
                     : "memory");
    expect("amocas.w-old", r, 0x0BADF00Dul);
    expect("amocas.w-new", g_word, 0x600DCAFEul);

    /*
     * Zawrs. With nothing reserved it returns at once; with a
     * reservation it waits for an interrupt, and under a kernel there is
     * always one coming -- the timer tick -- so it returns then. What
     * would make this hang or die is the firmware having left
     * mstatus.TW set, which makes the wait an illegal instruction below
     * M-mode.
     */
    put("RV32-LINUX-EXT: zawrs\n");
    __asm__ volatile(WITH_EXT("zawrs", "wrs.sto\n"
                                       "wrs.nto\n"
                                       "lr.w t0, (%0)\n"
                                       "wrs.nto") ::"r"(&g_word)
                     : "t0", "memory");
    expect("wrs-returned", 1ul, 1ul);

    /*
     * Zicboz, **from user mode**, which needs two things the bare-metal
     * tests cannot arrange: the firmware granting it to the supervisor
     * in menvcfg, and the supervisor granting it to its processes in
     * senvcfg. Either one missing is SIGILL. The second half of the
     * buffer says the block really is 32 bytes, as the device tree
     * tells the kernel.
     */
    put("RV32-LINUX-EXT: zicboz\n");
    for (unsigned i = 0; i < sizeof(g_block); i++) {
        g_block[i] = 0xFFu;
    }
    __asm__ volatile(WITH_EXT("zicboz", "cbo.zero (%0)") ::"r"(g_block)
                     : "memory");
    {
        unsigned long lo = 0, hi = 0;

        for (unsigned i = 0; i < 32u; i++) {
            lo |= g_block[i];
            hi += (g_block[32u + i] == 0xFFu) ? 1u : 0u;
        }
        expect("cbo.zero-zeroed", lo, 0ul);
        expect("cbo.zero-one-block", hi, 32ul);
    }

    /*
     * A misaligned store across a page boundary, into a page the kernel
     * has not yet given us.
     *
     * Two anonymous pages, of which only the first has been touched. The
     * store's last two bytes fall in the second: the emulator has to
     * fault *naming the second page* so the kernel maps it, and then the
     * retried store has to put two bytes in each. It used to translate
     * the first byte only and write four physically contiguous bytes --
     * no fault, and half the store in whatever frame followed the first
     * page's. Read back through the mapping, those two bytes were simply
     * not there.
     */
    put("RV32-LINUX-EXT: split access\n");
    {
        /* mmap2(NULL, 8192, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0) */
        unsigned char *const p =
            (unsigned char *)sys6(222, 0, 8192, 3, 0x22, -1, 0);

        expect("mmap", ((unsigned long)p & 0xFFFul), 0ul);
        if (((unsigned long)p & 0xFFFul) == 0ul && p != (unsigned char *)0) {
            p[0] = 1; /* the first page exists; the second does not yet */
            __asm__ volatile("sw %0, 0(%1)" ::"r"(0xA1B2C3D4ul),
                             "r"(p + 4094)
                             : "memory");
            expect("split-store-b0", p[4094], 0xD4ul);
            expect("split-store-b1", p[4095], 0xC3ul);
            expect("split-store-b2", p[4096], 0xB2ul);
            expect("split-store-b3", p[4097], 0xA1ul);

            __asm__ volatile("lw %0, 0(%1)"
                             : "=&r"(r)
                             : "r"(p + 4095)
                             : "memory");
            expect("split-load", r, 0x00A1B2C3ul);
        }
    }

    put("RV32-LINUX-EXT: ");
    put_hex((unsigned long)g_ext_checks);
    put(" checks, ");
    put_hex((unsigned long)g_ext_fails);
    put((g_ext_fails == 0) ? " failures -- PASS\n" : " failures -- FAIL\n");
}

void _start(void)
{
    /*
     * The global pointer, which is the one thing a C runtime does before
     * main that this file cannot do without.
     *
     * The linker relaxes accesses to small writable data into offsets
     * from gp, on the assumption that start-up code pointed it at
     * `__global_pointer$`. Nothing here did, and nothing needed to while
     * this program had no writable globals: gp was zero, and the first
     * one it gained was addressed at 0xFFFFF840 and killed init with
     * SIGSEGV -- on a plain byte store, two lines above the instruction
     * the test was actually about. `norelax`, or the linker turns this
     * very `la` into a move from gp.
     */
    __asm__ volatile(".option push\n"
                     ".option norelax\n"
                     "la gp, __global_pointer$\n"
                     ".option pop");

    console_init();

    put("\n");
    put("RV32-LINUX-INIT: userspace is running\n");
    test_extensions();
    put("RV32-LINUX-INIT: done\n");

    /*
     * exit_group, not exit. Leaving init is a kernel panic either way,
     * and the panic message is the terminator this run is read by -- a
     * test whose pass condition is "nothing failed" also passes when
     * nothing ran, so something only a completed boot can produce has to
     * be printed above.
     */
    (void)sys(94, 0, 0, 0);
    for (;;) {
    }
}
