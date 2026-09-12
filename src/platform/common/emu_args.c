/* SPDX-License-Identifier: Apache-2.0 */
/*
 * host_args.c - see emu_args.h. The command line, and the file it names.
 */

#include "emu_args.h"
#include "emu_console.h"

#include "emu/emu_cpu.h"
#include "emu/emu_jit.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

/* Defaults that only this file and main() need agree on. */
#define DEFAULT_RAM_SIZE (1u << 20) /* 1 MiB */

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

void emu_args_list_frontends(void)
{
    for (const emu_cpu_ops_t *const *p = emu_frontends; *p != NULL; p++) {
        emu_console_printf("%s%s", (p == emu_frontends) ? "" : ", ",
                           (*p)->name);
    }
}

/*
 * **One call per line, because emu_console_printf truncates at 192
 * bytes.**
 *
 * It formats into a fixed buffer -- a target has no heap and no stdio --
 * and vsnprintf then silently drops the rest. This usage text was a
 * single call of about 1400 bytes, so everything after `--load` was
 * invisible: `--ram`, `--jit`, `--gdb`, `--dump` all existed, all
 * worked, and none of them was ever printed. The tell was in the visible
 * part, reading "else the frv32" -- the sentence cut mid-word and the
 * frontend list from the *next* call landing against it.
 *
 * A table makes that structurally impossible rather than merely fixed:
 * no line here is close to the limit, and adding one cannot push another
 * out. Anything needing a run-time value is formatted on its own line.
 */
void emu_args_usage(void)
{
    static const char *const k_head[] = {
        "usage: emu [options] <image>",
        "",
        "  <image>              flat binary (preferred) or static ELF32",
        "",
        "  --frontend NAME      guest ISA (default: from the ELF header,",
        "                       else the first compiled in).",
    };
    static const char *const k_tail[] = {
        "  --entry ADDR         reset pc (default: load address, or the",
        "                       ELF entry point)",
        "  --dtb FILE           flattened device tree, placed at the top of",
        "  --rate               show the performance trace even when stderr",
        "                       is not a terminal. It is shown anyway on a",
        "                       terminal; a whole-run average is printed at",
        "                       exit either way.",
        "  --virtio-console     a virtio console beside the NS16550.",
        "  --virtio-input       present a virtio keyboard and mouse, for an",
        "                       OS driver to bind to. The simple polled",
        "                       devices stay; both see the same events.",
        "  --supervisor         the guest's OS runs in S-mode (Linux under",
        "                       OpenSBI). Routes external interrupts there",
        "                       instead of to M-mode; without it a driver",
        "                       waits for ever on a completed queue.",
        "  --9p [TAG:]DIR       share a host directory over virtio-9p; the",
        "                       guest mounts it with",
        "                         mount -t 9p -o trans=virtio,version=9p2000.L",
        "                                     TAG /mnt   (TAG defaults to host)",
        "                       RAM with its address in a1 (RISC-V) -- what",
        "                       OpenSBI and Linux read the machine out of",
        "  --max-insn N         stop after N instructions (0 = unlimited)",
        "  --timer-hz N         timer ticks per second of guest time",
        "  --quantum N          instructions per core per round;",
        "                       1 is instruction-interleaved lockstep",
#if EMU_JIT_SELECTABLE
        "  --jit                use the JIT backend, not the interpreter",
#endif
        "  --gdb [port]         serve a gdb stub on localhost (default 1234)",
        "  --quiet              suppress the exit summary",
        "  --dump               dump register state on exit",
    };

    for (unsigned i = 0; i < sizeof(k_head) / sizeof(k_head[0]); i++) {
        emu_console_printf("%s\n", k_head[i]);
    }
    emu_console_printf("                       This build has: ");
    emu_args_list_frontends();
    emu_console_printf("\n");

    emu_console_printf("  --load ADDR          load address for a flat "
                       "binary (default 0x%08x,\n",
                       (unsigned)EMU_GUEST_ROM_BASE);
    emu_console_printf("                       the flash window); one linked "
                       "for RAM wants 0x%08x\n",
                       (unsigned)EMU_GUEST_RAM_BASE);
    emu_console_printf("  --ram BYTES          guest RAM size (default %u)\n",
                       (unsigned)DEFAULT_RAM_SIZE);

    for (unsigned i = 0; i < sizeof(k_tail) / sizeof(k_tail[0]); i++) {
        emu_console_printf("%s\n", k_tail[i]);
    }
    emu_console_printf("  (--quantum default %u)\n",
                       (unsigned)EMU_DEFAULT_BUDGET);
}

bool emu_args_parse(int argc, char **argv, emu_args_t *opt, int *status)
{
    *status = 0;
    memset(opt, 0, sizeof(*opt));
    opt->load_addr = EMU_GUEST_ROM_BASE;
    opt->ram_size = DEFAULT_RAM_SIZE;
    opt->quantum = EMU_DEFAULT_BUDGET;
    opt->timer_div = 1u;
    opt->trace_count = 64u;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];

        if (strcmp(a, "--help") == 0 || strcmp(a, "-h") == 0) {
            emu_args_usage();
            return false;
        }
        if (strcmp(a, "--quiet") == 0) {
            opt->quiet = true;
            continue;
        }
        if (strcmp(a, "--dump") == 0) {
            opt->dump = true;
            continue;
        }

        if (i + 1 < argc) {
            if (strcmp(a, "--frontend") == 0) {
                opt->frontend = argv[++i];
                continue;
            }
            if (strcmp(a, "--load") == 0) {
                if (!parse_u32(argv[++i], &opt->load_addr)) {
                    emu_args_usage();
                    *status = 2;
                    return false;
                }
                continue;
            }
            if (strcmp(a, "--entry") == 0) {
                if (!parse_u32(argv[++i], &opt->entry)) {
                    emu_args_usage();
                    *status = 2;
                    return false;
                }
                continue;
            }
            if (strcmp(a, "--dtb") == 0) {
                if (argv[++i] == NULL) {
                    emu_args_usage();
                    *status = 2;
                    return false;
                }
                opt->dtb_path = argv[i];
                continue;
            }
            if (strcmp(a, "--rate") == 0) {
                opt->rate = true;
                continue;
            }
            if (strcmp(a, "--virtio-console") == 0) {
                opt->virtio_console = true;
                continue;
            }
            if (strcmp(a, "--virtio-input") == 0) {
                opt->virtio_input = true;
                continue;
            }
            if (strcmp(a, "--supervisor") == 0) {
                opt->supervisor = true;
                continue;
            }
            if (strcmp(a, "--9p") == 0) {
                if (argv[++i] == NULL) {
                    emu_args_usage();
                    *status = 2;
                    return false;
                }
                /*
                 * `tag:dir`, or just `dir` with a default tag. The tag
                 * is what `mount -t 9p` names, so it has to be
                 * settable -- a device tree written elsewhere will have
                 * picked one.
                 */
                {
                    const char *const colon = strchr(argv[i], ':');

                    if (colon != NULL && colon != argv[i]) {
                        static char tagbuf[64];
                        const size_t n = (size_t)(colon - argv[i]);

                        if (n >= sizeof(tagbuf)) {
                            emu_args_usage();
                            *status = 2;
                            return false;
                        }
                        memcpy(tagbuf, argv[i], n);
                        tagbuf[n] = '\0';
                        opt->p9_tag = tagbuf;
                        opt->p9_root = colon + 1;
                    } else {
                        opt->p9_tag = "host";
                        opt->p9_root = argv[i];
                    }
                }
                continue;
            }
            if (strcmp(a, "--ram") == 0) {
                if (!parse_u32(argv[++i], &opt->ram_size)) {
                    emu_args_usage();
                    *status = 2;
                    return false;
                }
                continue;
            }
            if (strcmp(a, "--max-insn") == 0) {
                uint32_t v;
                if (!parse_u32(argv[++i], &v)) {
                    emu_args_usage();
                    *status = 2;
                    return false;
                }
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
                    if (!parse_u32(argv[++i], &v)) {
                        emu_args_usage();
                        *status = 2;
                        return false;
                    }
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
                uint32_t v;
                if (!parse_u32(argv[++i], &v)) {
                    emu_args_usage();
                    *status = 2;
                    return false;
                }
                opt->trace_skip = v;
                continue;
            }
            if (strcmp(a, "--trace-count") == 0) {
                uint32_t v;
                if (!parse_u32(argv[++i], &v)) {
                    emu_args_usage();
                    *status = 2;
                    return false;
                }
                opt->trace_count = v;
                continue;
            }
#endif
            if (strcmp(a, "--timer-hz") == 0) {
                if (!parse_u32(argv[++i], &opt->timer_div)) {
                    emu_args_usage();
                    *status = 2;
                    return false;
                }
                continue;
            }
            if (strcmp(a, "--quantum") == 0) {
                if (!parse_u32(argv[++i], &opt->quantum) ||
                    opt->quantum == 0u) {
                    emu_args_usage();
                    *status = 2;
                    return false;
                }
                continue;
            }
        }

        if (a[0] == '-') {
            emu_console_printf("emu: unknown option %s\n", a);
            emu_args_usage();
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

    /*
     * **No "an image is required" check here**, though the host needs
     * one. A board's image is linked in, so its equivalent command line
     * names none, and a parser shared with it cannot insist. The host
     * enforces it where it reads the file -- the one caller that cannot
     * proceed without a path.
     */
    return true;
}
