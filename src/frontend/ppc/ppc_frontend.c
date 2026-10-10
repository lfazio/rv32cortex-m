/* SPDX-License-Identifier: Apache-2.0 */
/*
 * ppc_frontend.c - the e200z7 frontend's emu_cpu_ops_t.
 *
 * Shaped like rv32_frontend.c and g4mh_frontend.c, which is the point:
 * if a third frontend needs something neither of the first two has, it
 * belongs in the contract rather than in a platform #ifdef. This one is
 * the test of that claim, because it is the first big-endian guest --
 * and the only thing it needed beyond the existing contract was
 * emu_bus_set_big_endian().
 *
 * Note what is *not* here: no memory. The platform provides the backing
 * store, which is the rule docs/memory.md states and which the G4MH
 * frontend had to be rescued from.
 */

#include "emu/emu_cpu.h"
#include "emu/emu_memmap.h"

#include "ppc/ppc_cpu.h"
#include "ppc/ppc_decode.h"
#include "ppc/ppc_disasm.h"
#include "ppc/ppc_ir.h"

#include <string.h>

static ppc_cpu_t g_cpu;

/*
 * Which encoding the image's entry point is in.
 *
 * On the part this is a pin sampled at reset (p_rst_vlemode) and then a
 * bit of each TLB entry. This model has no MMU, so it is one answer per
 * image, and the image says it itself: a PowerPC ELF marks a segment
 * holding VLE code with PF_PPC_VLE. A raw binary cannot say, and is
 * taken as VLE -- what an MPC57xx runs, and what every assembled guest
 * here is.
 */
#define PF_PPC_VLE 0x10000000u

static uint32_t be32(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) |
           p[3];
}

static uint32_t be16(const uint8_t *p)
{
    return ((uint32_t)p[0] << 8) | p[1];
}

static bool image_is_vle(const void *base, uint32_t size)
{
    const uint8_t *const d = (const uint8_t *)base;

    /* ELF32, big-endian, with program headers of the size we read. */
    if (d == NULL || size < 52u || d[0] != 0x7Fu || d[1] != 'E' || d[2] != 'L' ||
        d[3] != 'F' || d[4] != 1u || d[5] != 2u) {
        return true;
    }
    {
        const uint32_t entry = be32(d + 24);
        const uint32_t phoff = be32(d + 28);
        const uint32_t phentsize = be16(d + 42);
        const uint32_t phnum = be16(d + 44);

        if (phentsize < 32u) {
            return true;
        }
        for (uint32_t i = 0u; i < phnum; i++) {
            const uint64_t at = (uint64_t)phoff + (uint64_t)i * phentsize;
            const uint8_t *ph;

            if (at + 32u > size) {
                break;
            }
            ph = d + at;
            /* PT_LOAD holding the entry point. */
            if (be32(ph) == 1u && entry >= be32(ph + 8) &&
                entry - be32(ph + 8) < be32(ph + 20)) {
                return (be32(ph + 24) & PF_PPC_VLE) != 0u;
            }
        }
    }
    return true;
}

static EMU_ALWAYS_INLINE ppc_cpu_t *cpu_of(const emu_cpu_t *cpu)
{
    return (ppc_cpu_t *)(uintptr_t)(const void *)cpu;
}

static emu_cpu_t *ppc_instance(unsigned index)
{
    return (index == 0u) ? (emu_cpu_t *)&g_cpu : NULL;
}

static void ppc_ops_init(emu_cpu_t *cpu, emu_bus_t *bus, uint32_t coreid)
{
    ppc_cpu_init(cpu_of(cpu), bus, coreid);
}

static void ppc_ops_reset(emu_cpu_t *cpu, uint32_t reset_pc)
{
    ppc_cpu_t *const c = cpu_of(cpu);

    ppc_cpu_reset(c, reset_pc);
    if (ppc_backend->reset != NULL) {
        ppc_backend->reset(cpu);
    }
}

/*
 * The runner says which backend it wants. A frontend that picked for
 * itself could not be asked to run the same guest both ways, and here
 * the interpreter is the only statement of what an answer should be.
 */
static bool ppc_select_backend(emu_cpu_t *cpu, bool want_jit)
{
#if EMU_HAVE_JIT
    ppc_backend = want_jit ? &ppc_backend_jit : &ppc_backend_interp;
#else
    (void)want_jit;
    ppc_backend = &ppc_backend_interp;
#endif
    return ppc_backend->init == NULL || ppc_backend->init(cpu);
}

static void ppc_ops_invalidate(emu_cpu_t *cpu, uint32_t addr, uint32_t len)
{
    if (ppc_backend->invalidate != NULL) {
        ppc_backend->invalidate(cpu, addr, len);
    }
}

static void ppc_ops_boot(emu_cpu_t *cpu, const emu_boot_info_t *info)
{
    /*
     * The stack pointer convention is the ABI's, not the architecture's:
     * r1 is the stack pointer on every PowerPC ABI, and a guest linked
     * by gcc expects it set. Pointed at the top of RAM, 8-aligned as the
     * EABI requires.
     */
    ppc_cpu_t *c = cpu_of(cpu);
    c->r[1] = (info->ram_base + info->ram_size) & ~7u;

    /*
     * And the encoding, which is the image's to say. Here rather than
     * at reset because this is where the image is in hand: boot runs on
     * every start and every reload, after the reset that preceded it.
     */
    c->vle = image_is_vle(info->image, info->image_size);
    c->jit_ctx = ppc_cpu_ctx(c);
}

static emu_run_reason_t ppc_ops_run(emu_cpu_t *cpu, uint32_t budget,
                                    uint32_t *retired)
{
    return ppc_backend->run(cpu, budget, retired);
}

static emu_run_reason_t ppc_ops_step(emu_cpu_t *cpu)
{
    return ppc_step(cpu_of(cpu));
}

static void ppc_ops_halt(emu_cpu_t *cpu)
{
    cpu_of(cpu)->state = EMU_STATE_HALTED;
}

static void ppc_ops_status(const emu_cpu_t *cpu, emu_cpu_status_t *out)
{
    const ppc_cpu_t *c = cpu_of(cpu);

    memset(out, 0, sizeof(*out));
    out->pc = c->pc;
    out->retired = c->retired;
    out->state = c->state;
    out->backend = ppc_backend->name;
}

/*
 * Register names, in the ABI's terms where it has them. r1 is sp and r2
 * is the small-data pointer on the EABI; the rest are plain numbers,
 * which is what every PowerPC disassembler prints.
 */
static const char *ppc_reg_name(unsigned r)
{
    static const char *const k[PPC_NGPR] = {
        "r0",  "sp",  "r2",  "r3",  "r4",  "r5",  "r6",  "r7",
        "r8",  "r9",  "r10", "r11", "r12", "r13", "r14", "r15",
        "r16", "r17", "r18", "r19", "r20", "r21", "r22", "r23",
        "r24", "r25", "r26", "r27", "r28", "r29", "r30", "r31",
    };
    return (r < PPC_NGPR) ? k[r] : "?";
}

static uint32_t ppc_reg_read(const emu_cpu_t *cpu, unsigned r)
{
    return (r < PPC_NGPR) ? cpu_of(cpu)->r[r] : 0u;
}

static void ppc_reg_write(emu_cpu_t *cpu, unsigned r, uint32_t v)
{
    if (r < PPC_NGPR) {
        cpu_of(cpu)->r[r] = v;
    }
}

/*
 * Guest time. The time base counts up and the decrementer down, both
 * from this one call -- the platform's tick is the guest's clock, the
 * same arrangement the other two frontends use.
 */
static void ppc_ops_advance_time(emu_cpu_t *cpu, uint32_t ticks)
{
    ppc_cpu_advance(cpu_of(cpu), ticks);
}

static void ppc_ops_set_time(emu_cpu_t *cpu, uint64_t now)
{
    ppc_cpu_set_time(cpu_of(cpu), now);
}

/*
 * The external input. `level` false is honoured rather than dropped --
 * unlike the RV32 side, where the APLIC has no lower operation, this
 * core has no interrupt controller at all, so the line *is* the state.
 */
static void ppc_ops_set_irq(emu_cpu_t *cpu, uint32_t source, bool level)
{
    (void)source;
    ppc_cpu_set_ext(cpu_of(cpu), level);
}

/* ------------------------------------------------------------------ */
/* Post-mortem                                                         */
/* ------------------------------------------------------------------ */

static const char *hex8(char b[9], uint32_t v)
{
    for (unsigned i = 0u; i < 8u; i++) {
        b[i] = "0123456789abcdef"[(v >> (28u - 4u * i)) & 15u];
    }
    b[8] = '\0';
    return b;
}

static void ppc_dump(const emu_cpu_t *cpu, emu_print_fn out, void *ctx)
{
    const ppc_cpu_t *const c = cpu_of(cpu);
    char b[9];
    const struct {
        const char *name;
        uint32_t v;
    } k[] = {
        {"pc   ", c->pc},   {"msr  ", c->msr},   {"cr   ", c->cr},
        {"xer  ", c->xer},  {"lr   ", c->lr},    {"ctr  ", c->ctr},
        {"srr0 ", c->srr0}, {"srr1 ", c->srr1},  {"esr  ", c->esr},
        {"dear ", c->dear}, {"csrr0", c->csrr0}, {"spefs", c->spefscr},
    };

    out(ctx, c->vle ? "\n  VLE" : "\n  Book E");
    /*
     * The syndrome in words: ESR is the only thing that says which of
     * the conditions sharing IVOR6 was raised.
     */
    out(ctx, (c->esr & PPC_ESR_PIL) != 0u   ? "  (illegal instruction)"
             : (c->esr & PPC_ESR_PPR) != 0u ? "  (privileged instruction)"
             : (c->esr & PPC_ESR_PTR) != 0u ? "  (trap)"
             : (c->esr & PPC_ESR_SPE) != 0u ? "  (SPE/EFPU)"
                                            : "");
    for (unsigned i = 0u; i < sizeof(k) / sizeof(k[0]); i++) {
        out(ctx, (i % 4u) == 0u ? "\n  " : "  ");
        out(ctx, k[i].name);
        out(ctx, " ");
        out(ctx, hex8(b, k[i].v));
    }
    for (unsigned i = 0u; i < PPC_NGPR; i++) {
        char n[6] = {'r', (char)('0' + i / 10u), (char)('0' + i % 10u), ' ', ' ', '\0'};

        if (i < 10u) {
            n[1] = (char)('0' + i);
            n[2] = ' ';
        }
        out(ctx, (i % 4u) == 0u ? "\n  " : "  ");
        out(ctx, n);
        out(ctx, " ");
        out(ctx, hex8(b, c->r[i]));
    }
    out(ctx, "\n");
}

#if PPC_ENABLE_DISASM
/*
 * The trace hook's disassembler. The contract hands over the encoding
 * and not the core, and which of this core's two encodings those bytes
 * are in is not in the bytes -- so it is the image's, the same answer
 * the fetch that produced them used.
 */
static size_t ppc_ops_disasm(char *buf, size_t buflen, uint32_t pc, uint64_t insn,
                             unsigned len)
{
    return ppc_disasm_mode(buf, buflen, pc, (uint32_t)insn, len, g_cpu.vle);
}
#endif

#if EMU_HAVE_JIT
static void put_dec(emu_print_fn out, void *ctx, uint64_t v)
{
    char buf[24];
    unsigned i = sizeof(buf);

    buf[--i] = '\0';
    do {
        buf[--i] = (char)('0' + (unsigned)(v % 10u));
        v /= 10u;
    } while (v != 0u && i > 0u);
    out(ctx, &buf[i]);
}

static void put_top(emu_print_fn out, void *ctx, const char *title,
                    const uint32_t *by_sem)
{
    bool used[PPC_S_COUNT] = {false};

    out(ctx, title);
    for (unsigned k = 0u; k < 12u; k++) {
        uint32_t best = 0u;
        uint32_t at = 0u;

        for (uint32_t s = 0u; s < (uint32_t)PPC_S_COUNT; s++) {
            if (!used[s] && by_sem[s] > best) {
                best = by_sem[s];
                at = s;
            }
        }
        if (best == 0u) {
            break;
        }
        used[at] = true;
        out(ctx, " ");
        out(ctx, ppc_sem_name(at));
        out(ctx, ":");
        put_dec(out, ctx, best);
    }
    out(ctx, "\n");
}

/*
 * What the translator did with each instruction it was shown. The
 * framework's own figures cannot say: a call to the interpreter from
 * inside a block counts as translated there.
 */
static void ppc_report(emu_print_fn out, void *ctx)
{
    const ppc_ir_stats_t *const s = ppc_ir_get_stats();

    if (s->native + s->helper + s->declined == 0u) {
        return;
    }
    out(ctx, "\n-- ppc translator (per instruction translated) --\n"
             "  lowered  ");
    put_dec(out, ctx, s->native);
    out(ctx, "  helper ");
    put_dec(out, ctx, s->helper);
    out(ctx, "  declined ");
    put_dec(out, ctx, s->declined);
    out(ctx, "\n");
    put_top(out, ctx, "  helper  ", s->helper_by_sem);
    put_top(out, ctx, "  declined", s->declined_by_sem);
}
#endif

static void ppc_set_syscall(emu_cpu_t *cpu, emu_syscall_fn fn, void *user)
{
    ppc_cpu_t *c = cpu_of(cpu);
    c->syscall = fn;
    c->syscall_user = user;
}

#if EMU_ENABLE_TRACE
static void ppc_set_trace(emu_cpu_t *cpu, emu_trace_fn fn, void *user)
{
    ppc_cpu_t *c = cpu_of(cpu);
    c->trace = fn;
    c->trace_user = user;
}
#endif

const emu_cpu_ops_t ppc_frontend = {
    .name = "ppc",
    /*
     * The banner's line, and it was missing: `emu_main` prints
     * `ops->desc` unconditionally, so a PowerPC run opened with
     * "emu: (null) on x86-64". The other two frontends both set it.
     */
    .desc = "NXP PowerPC e200z7 (Book E, VLE)",
    .nregs = PPC_NGPR,
    .ncores = 1u,
    /*
     * EM_PPC. The 64-bit variant has its own number and is a different
     * architecture as far as the loader is concerned, so it is not
     * listed as an alternate.
     */
    .elf_machine = 20u,

    .instance = ppc_instance,
    .init = ppc_ops_init,
    .reset = ppc_ops_reset,
    .boot = ppc_ops_boot,
    .run = ppc_ops_run,
    .step = ppc_ops_step,
    .halt = ppc_ops_halt,
    .status = ppc_ops_status,
    .select_backend = ppc_select_backend,
    .invalidate = ppc_ops_invalidate,
    .dump = ppc_dump,
#if PPC_ENABLE_DISASM
    .disasm = ppc_ops_disasm,
#endif
#if EMU_HAVE_JIT
    .report = ppc_report,
#endif

    .reg_name = ppc_reg_name,
    .reg_read = ppc_reg_read,
    .reg_write = ppc_reg_write,

    .advance_time = ppc_ops_advance_time,
    .set_time = ppc_ops_set_time,
    .set_irq = ppc_ops_set_irq,

    .set_syscall = ppc_set_syscall,
#if EMU_ENABLE_TRACE
    .set_trace = ppc_set_trace,
#endif
};
