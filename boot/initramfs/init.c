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

void _start(void)
{
    console_init();

    put("\n");
    put("RV32-LINUX-INIT: userspace is running\n");
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
