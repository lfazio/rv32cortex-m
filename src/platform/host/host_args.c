/* SPDX-License-Identifier: Apache-2.0 */
/*
 * host_args.c - see host_args.h. The command line, and the file it names.
 */

#include "host_args.h"
#include "emu_console.h"

#include "emu/emu_cpu.h"
#include "emu/emu_jit.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Defaults that only this file and main() need agree on. */
#define DEFAULT_RAM_SIZE  (1u << 20)   /* 1 MiB */

/*
 * Whether `--jit` means anything in this build.
 *
 * The host half of the question, and now the only half: whether the
 * *frontend* has a second backend is `ops->select_backend != NULL`, a
 * property answered at run time rather than a macro naming the frontends
 * that exist.
 */
#define EMU_JIT_SELECTABLE EMU_HAVE_JIT

static bool parse_u32(const char *s, uint32_t *out)
{
    char *end = NULL;
    errno = 0;
    const unsigned long long v = strtoull(s, &end, 0);
    if (errno != 0 || end == s || *end != '\0' || v > 0xFFFFFFFFull) {
        return false;
    }
    *out = (uint32_t)v;
    return true;
}

void host_list_frontends(FILE *f)
{
    for (const emu_cpu_ops_t *const *p = emu_frontends; *p != NULL; p++) {
        fprintf(f, "%s%s", (p == emu_frontends) ? "" : ", ", (*p)->name);
    }
}

static void usage(void)
{
    fprintf(stderr,
        "usage: emu-host [options] <image>\n"
        "\n"
        "  <image>              flat binary (preferred) or static ELF32\n"
        "\n"
        "  --frontend NAME      guest ISA (default: from the ELF header,\n"
        "                       else the first compiled in). This build has: ");
    host_list_frontends(stderr);
    fprintf(stderr,
        "\n"
        "  --load ADDR          load address for a flat binary\n"
        "                       (default 0x%08x, the flash window; a binary\n"
        "                        linked to run from RAM wants 0x%08x)\n"
        "  --entry ADDR         reset pc (default: load address, or the\n"
        "                       ELF entry point)\n"
        "  --ram BYTES          guest RAM size (default %u)\n"
        "  --max-insn N         stop after N instructions (0 = unlimited)\n"
        "  --timer-hz N         timer ticks per second of guest time\n"
        "  --quantum N          instructions per core per round (default %u).\n"
        "                       1 is instruction-interleaved lockstep\n"
#if EMU_JIT_SELECTABLE
        "  --jit                use the JIT backend instead of the interpreter\n"
#endif
        "  --gdb [port]         serve a gdb stub on localhost (default 1234)\n"
        "  --quiet              suppress the exit summary\n"
        "  --dump               dump register state on exit\n",
        EMU_GUEST_ROM_BASE, EMU_GUEST_RAM_BASE, DEFAULT_RAM_SIZE,
        (unsigned)EMU_DEFAULT_BUDGET);
}

uint8_t *host_read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        emu_console_printf("emu: %s: %s\n", path, strerror(errno));
        return NULL;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        emu_console_printf("emu: %s: not seekable\n", path);
        fclose(f);
        return NULL;
    }
    const long n = ftell(f);
    if (n < 0) {
        emu_console_printf("emu: %s: %s\n", path, strerror(errno));
        fclose(f);
        return NULL;
    }
    rewind(f);

    uint8_t *buf = malloc((size_t)n ? (size_t)n : 1u);
    if (buf == NULL) {
        fclose(f);
        emu_console_printf("emu: out of memory\n");
        return NULL;
    }
    if (fread(buf, 1u, (size_t)n, f) != (size_t)n) {
        emu_console_printf("emu: %s: short read\n", path);
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *out_len = (size_t)n;
    return buf;
}

bool host_args_parse(int argc, char **argv, host_args_t *opt, int *status)
{
    *status = 0;
    memset(opt, 0, sizeof(*opt));
    opt->load_addr   = EMU_GUEST_ROM_BASE;
    opt->ram_size    = DEFAULT_RAM_SIZE;
    opt->quantum     = EMU_DEFAULT_BUDGET;
    opt->timer_div   = 1u;
    opt->trace_count = 64u;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            usage();
            return false;
        }
        if (strcmp(a, "--quiet") == 0) { opt->quiet = true; continue; }
        if (strcmp(a, "--dump") == 0)  { opt->dump = true;  continue; }

        if (i + 1 < argc) {
            if (strcmp(a, "--frontend") == 0) {
                opt->frontend = argv[++i];
                continue;
            }
            if (strcmp(a, "--load") == 0) {
                if (!parse_u32(argv[++i], &opt->load_addr)) { usage(); *status = 2; return false; }
                continue;
            }
            if (strcmp(a, "--entry") == 0) {
                if (!parse_u32(argv[++i], &opt->entry)) { usage(); *status = 2; return false; }
                continue;
            }
            if (strcmp(a, "--ram") == 0) {
                if (!parse_u32(argv[++i], &opt->ram_size)) { usage(); *status = 2; return false; }
                continue;
            }
            if (strcmp(a, "--max-insn") == 0) {
                uint32_t v;
                if (!parse_u32(argv[++i], &v)) { usage(); *status = 2; return false; }
                opt->max_insn = v;
                continue;
            }
#if EMU_NET
            if (strcmp(a, "--ppp") == 0) {
                /*
                 * The same IP stack the board runs, over a pty instead of
                 * a UART. An optional argument names an existing device
                 * for a caller that has already arranged one end.
                 */
                opt->ppp = true;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    opt->ppp_dev = argv[++i];
                }
                continue;
            }
#endif
            /*
             * Not inside an RV32 block, for the same reason --jit is not:
             * the stub is in emucore, the register layout comes from
             * ops->gdb_target, and every frontend that has one gets it.
             * Guarded on RV32 this option was *rejected outright* by a
             * G4MH-only build, whose gdb target description existed and
             * was unreachable -- the identical defect --jit had, in the
             * same file, left behind when that one was fixed.
             *
             * A frontend with no description is caught where the stub is
             * started, by name, rather than by the option not existing.
             */
            if (strcmp(a, "--gdb") == 0) {
                /* Port only; the stub listens on loopback. Waits for a
                 * client before the first instruction, because the whole
                 * guest is over in milliseconds otherwise. */
                opt->gdb_port = 1234;
                if (i + 1 < argc && argv[i + 1][0] != '-') {
                    uint32_t v;
                    if (!parse_u32(argv[++i], &v)) { usage(); *status = 2; return false; }
                    opt->gdb_port = (int)v;
                }
                continue;
            }
            /*
             * Not inside the RV32 block: both frontends have an x86-64
             * backend, and while this option was guarded on RV32 a
             * G4MH-only build rejected it outright -- so the only reachable
             * backend was whichever one the frontend happened to prefer.
             */
            if (strcmp(a, "--jit") == 0) {
#if EMU_JIT_SELECTABLE
                /*
                 * The whole reason the x86-64 backends exist: with this,
                 * the architecture suite and riscv-tests run against
                 * *translated* code -- coverage the Thumb-2 backend can
                 * only get by flashing a board.
                 */
                opt->want_jit = true;
#else
                emu_console_printf("emu: no JIT backend for this host\n");
                *status = 2;
            return false;
#endif
                continue;
            }
#if EMU_ENABLE_TRACE
            if (strcmp(a, "--trace-skip") == 0) {
                uint32_t v; if (!parse_u32(argv[++i], &v)) { usage(); *status = 2; return false; }
                opt->trace_skip = v; continue;
            }
            if (strcmp(a, "--trace-count") == 0) {
                uint32_t v; if (!parse_u32(argv[++i], &v)) { usage(); *status = 2; return false; }
                opt->trace_count = v; continue;
            }
#endif
            if (strcmp(a, "--timer-hz") == 0) {
                if (!parse_u32(argv[++i], &opt->timer_div)) { usage(); *status = 2; return false; }
                continue;
            }
            if (strcmp(a, "--quantum") == 0) {
                if (!parse_u32(argv[++i], &opt->quantum) || opt->quantum == 0u) {
                    usage(); *status = 2; return false;
                }
                continue;
            }
        }

        if (a[0] == '-') {
            emu_console_printf("emu: unknown option %s\n", a);
            usage();
            *status = 2;
            return false;
        }
        if (opt->path != NULL) {
            emu_console_printf("emu: more than one image given\n");
            *status = 2;
            return false;
        }
        opt->path = a;
    }

    if (opt->path == NULL) {
        usage();
        *status = 2;
        return false;
    }
    return true;
}
