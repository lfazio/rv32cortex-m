/* SPDX-License-Identifier: Apache-2.0 */
/*
 * rv32_frontend.c - The RISC-V frontend's emu_cpu_ops_t.
 *
 * Everything a platform is allowed to know about this frontend. The hart,
 * the CLINT, the APLIC and the backend selection all live behind it, so
 * neither the host runner nor the firmware names rv_hart_t.
 *
 * Nothing here is on a per-instruction path: `run` hands a whole budget to
 * the selected backend and every other entry point is setup or fires on a
 * trap. See the header note in emu/emu_cpu.h -- that is the property that
 * lets this table be indirect at all.
 */

#include "emu/emu_cpu.h"
#include "emu/emu_memmap.h"

#include "rv32/rv_aplic.h"
#include "rv32/rv_backend.h"
#include "rv32/rv_clint.h"
#include "rv32/rv_disasm.h"
#include "rv32/rv_hart.h"
#include "rv32/rv_jit.h"
#include "rv32/rv_memmap.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* Identification                                                      */
/* ------------------------------------------------------------------ */

/*
 * The ISA string, assembled from the extensions actually compiled in, so
 * the banner a platform prints cannot drift from what the core implements.
 * B is exactly Zba+Zbb+Zbs, which is what misa reports; Zbc is separate.
 */
#if RV_EXT_M
#  define RV32_S_M "M"
#else
#  define RV32_S_M ""
#endif
#if RV_EXT_A
#  define RV32_S_A "A"
#else
#  define RV32_S_A ""
#endif
#if RV_EXT_F
#  define RV32_S_F "F"
#else
#  define RV32_S_F ""
#endif
#if RV_EXT_C
#  define RV32_S_C "C"
#else
#  define RV32_S_C ""
#endif
#if RV_EXT_ZBA && RV_EXT_ZBB && RV_EXT_ZBS
#  define RV32_S_B "B"
#else
#  define RV32_S_B ""
#endif
#if RV_EXT_ZBC
#  define RV32_S_ZBC "_zbc"
#else
#  define RV32_S_ZBC ""
#endif

#define RV32_ISA_STRING \
    "RV32I" RV32_S_M RV32_S_A RV32_S_F RV32_S_C RV32_S_B RV32_S_ZBC

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/*
 * One core, statically allocated. Both platforms are single-core, and a
 * static instance is what keeps the firmware's footprint known at link
 * time -- there is no allocator on the target and adding one to place a
 * struct whose size is a compile-time constant would be a step backwards.
 *
 * The CLINT and APLIC live here too rather than in the platform: they are
 * part of the RISC-V privileged platform, not of the board, and what they
 * do is write this hart's mip.
 */
static rv_hart_t  g_hart;
static rv_clint_t g_clint;
static rv_aplic_t g_aplic;

static emu_cpu_t *rv32_instance(unsigned index)
{
    /* Single-core. A multi-core frontend would index an array here and
     * the rest of this file would take the hart from the emu_cpu_t. */
    return (index == 0u) ? (emu_cpu_t *)&g_hart : NULL;
}

static EMU_ALWAYS_INLINE rv_hart_t *hart_of(const emu_cpu_t *cpu)
{
    return (rv_hart_t *)(uintptr_t)(const void *)cpu;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void rv32_init(emu_cpu_t *cpu, emu_bus_t *bus, uint32_t coreid)
{
    rv_hart_t *h = hart_of(cpu);

    rv_hart_init(h, bus, coreid);
    rv_clint_init(&g_clint, h);
    rv_aplic_init(&g_aplic, h);

#if RV_ENABLE_JIT
    /*
     * Prefer the JIT when it is compiled in. It declines what it cannot
     * translate and falls back to the interpreter per instruction, so this
     * is a speed choice, not a coverage one.
     */
    rv_backend = &rv_backend_jit;
#endif
    if (rv_backend->init != NULL && !rv_backend->init(cpu)) {
        /* A backend that cannot start (no code buffer, say) is not fatal:
         * the interpreter runs the same guest. */
        rv_backend = &rv_backend_interp;
    }
}

static void rv32_reset(emu_cpu_t *cpu, uint32_t reset_pc)
{
    rv_hart_t *h = hart_of(cpu);
    rv_hart_reset(h, reset_pc);
    if (rv_backend->reset != NULL) {
        rv_backend->reset(cpu);
    }
}

static void rv32_boot(emu_cpu_t *cpu, uint32_t ram_base, uint32_t ram_size)
{
    rv_hart_boot(hart_of(cpu), ram_base, ram_size);
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

static emu_run_reason_t rv32_run(emu_cpu_t *cpu, uint32_t budget,
                                 uint32_t *retired)
{
    return rv_backend->run(cpu, budget, retired);
}

static void rv32_invalidate(emu_cpu_t *cpu, uint32_t addr, uint32_t len)
{
    if (rv_backend->invalidate != NULL) {
        rv_backend->invalidate(cpu, addr, len);
    }
}

static emu_run_reason_t rv32_step(emu_cpu_t *cpu)
{
    return rv_step(hart_of(cpu));
}

/* ------------------------------------------------------------------ */
/* Platform devices                                                    */
/* ------------------------------------------------------------------ */

/*
 * The CLINT and the APLIC: the two devices a RISC-V guest expects to find
 * because the privileged architecture defines them, as opposed to the ones
 * the board happens to have. The console UART is neither, so the platform
 * places that itself.
 *
 * ACLINT rather than legacy CLINT, in the two pieces a platform may place
 * independently; at these addresses they occupy exactly the legacy window,
 * so guests written for either layout work.
 */
/*
 * The APLIC is the shared one: external interrupts are a system resource
 * and its sources are not per hart.
 */
static bool rv32_add_shared_devices(emu_bus_t *bus)
{
    return emu_bus_add_mmio(bus, "aplic", RV_GUEST_APLIC_BASE,
                            RV_APLIC_SIZE, &rv_aplic_ops, &g_aplic);
}

/*
 * The ACLINT is hart-local: msip and mtimecmp exist per hart, and what
 * they drive is that hart's mip. Single-hart here, so the distinction
 * makes no difference today -- it is drawn because it is the architecture's
 * and a second hart would need it.
 */
static bool rv32_add_core_devices(emu_cpu_t *cpu, emu_bus_t *bus,
                                  unsigned index)
{
    (void)cpu; (void)index;
    return emu_bus_add_mmio(bus, "aclint-mswi", RV_GUEST_ACLINT_MSWI_BASE,
                            RV_ACLINT_MSWI_SIZE, &rv_aclint_mswi_ops,
                            &g_clint) &&
           emu_bus_add_mmio(bus, "aclint-mtimer", RV_GUEST_ACLINT_MTIMER_BASE,
                            RV_ACLINT_MTIMER_SIZE, &rv_aclint_mtimer_ops,
                            &g_clint);
}

static void rv32_set_irq(emu_cpu_t *cpu, uint32_t source, bool level)
{
    (void)cpu;
    /*
     * The APLIC has no "lower" operation: a source is cleared by the guest
     * writing CLRIP, or implicitly when it claims the interrupt. Dropping
     * a line is therefore a no-op rather than an error -- a host handler
     * that raises and then clears is describing an edge.
     */
    if (level) {
        rv_aplic_raise(&g_aplic, source);
    }
}

static void rv32_set_unmask_hook(emu_cpu_t *cpu, emu_unmask_fn fn, void *ctx)
{
    (void)cpu;
    rv_aplic_set_eoi(&g_aplic, fn, ctx);
}

static void rv32_advance_time(emu_cpu_t *cpu, uint32_t ticks)
{
    (void)cpu;
    rv_clint_advance(&g_clint, ticks);
}

static void rv32_set_time(emu_cpu_t *cpu, uint64_t now)
{
    (void)cpu;
    rv_clint_set_time(&g_clint, now);
}

/* ------------------------------------------------------------------ */
/* Platform services                                                   */
/* ------------------------------------------------------------------ */

static void rv32_set_syscall(emu_cpu_t *cpu, emu_syscall_fn fn, void *user)
{
#if RV_ENABLE_ECALL_HOOK
    rv_hart_t *h = hart_of(cpu);
    h->ecall = fn;
    h->ecall_user = user;
#else
    (void)cpu; (void)fn; (void)user;
#endif
}

static void rv32_set_trace(emu_cpu_t *cpu, emu_trace_fn fn, void *user)
{
#if EMU_ENABLE_TRACE
    rv_hart_t *h = hart_of(cpu);
    h->trace = fn;
    h->trace_user = user;
#else
    (void)cpu; (void)fn; (void)user;
#endif
}

static void rv32_set_cache(emu_cpu_t *cpu, const emu_cache_ops_t *ops)
{
#if RV_EXT_ZICBOM
    hart_of(cpu)->cache = ops;
#else
    (void)cpu; (void)ops;
#endif
}

static void rv32_halt(emu_cpu_t *cpu)
{
    hart_of(cpu)->state = EMU_STATE_HALTED;
}

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

static void rv32_status(const emu_cpu_t *cpu, emu_cpu_status_t *out)
{
    const rv_hart_t *h = hart_of(cpu);

    out->backend = rv_backend->name;
    out->pc      = h->pc;
    out->retired = h->minstret;
    out->state   = (emu_state_t)h->state;
#if EMU_ENABLE_STATS
    out->traps   = h->trap_count;
#else
    out->traps   = 0u;
#endif
    /*
     * Parked in WFI with mie empty means nothing can ever wake this hart:
     * the interrupt-enable stack is irrelevant, because WFI resumes on a
     * pending-and-enabled interrupt whether or not MIE is set, but a
     * source that is not enabled at all can never become one.
     */
    out->wakeable = (h->mie != 0u);
}

static uint32_t rv32_reg_read(const emu_cpu_t *cpu, unsigned r)
{
    return (r < 32u) ? hart_of(cpu)->x[r] : 0u;
}

static void rv32_reg_write(emu_cpu_t *cpu, unsigned r, uint32_t v)
{
    if (r < 32u) {
        rv_hart_t *h = hart_of(cpu);
        h->x[r] = v;
        h->x[0] = 0u;      /* x0 stays hardwired */
    }
}

/* ------------------------------------------------------------------ */
/* State dump                                                          */
/* ------------------------------------------------------------------ */

/*
 * Formatted here rather than by the platform because only the frontend
 * knows what its registers are called and which status registers matter
 * after a fault. Written without stdio: the firmware has none, and the two
 * platforms would otherwise need two copies of this.
 */

static const char *cause_name(uint32_t mcause)
{
    if (mcause & RV_CAUSE_INTERRUPT) {
        switch (mcause & ~RV_CAUSE_INTERRUPT) {
        case RV_INT_M_SOFT:  return "machine software interrupt";
        case RV_INT_M_TIMER: return "machine timer interrupt";
        case RV_INT_M_EXT:   return "machine external interrupt";
        default:             return "unknown interrupt";
        }
    }
    switch (mcause) {
    case RV_EXC_INSN_MISALIGNED:    return "instruction address misaligned";
    case RV_EXC_INSN_ACCESS_FAULT:  return "instruction access fault";
    case RV_EXC_ILLEGAL_INSN:       return "illegal instruction";
    case RV_EXC_BREAKPOINT:         return "breakpoint";
    case RV_EXC_LOAD_MISALIGNED:    return "load address misaligned";
    case RV_EXC_LOAD_ACCESS_FAULT:  return "load access fault";
    case RV_EXC_STORE_MISALIGNED:   return "store address misaligned";
    case RV_EXC_STORE_ACCESS_FAULT: return "store access fault";
    case RV_EXC_ECALL_U:            return "environment call from U-mode";
    case RV_EXC_ECALL_S:            return "environment call from S-mode";
    case RV_EXC_ECALL_M:            return "environment call from M-mode";
    default:                        return "unknown exception";
    }
}

/* 8 hex digits into a caller-supplied 9-byte buffer. */
static const char *hex32(char buf[9], uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < 8u; i++) {
        buf[7u - i] = digits[v & 0xFu];
        v >>= 4;
    }
    buf[8] = '\0';
    return buf;
}

static const char *dec64(char buf[21], uint64_t v)
{
    char *p = buf + 20;
    *p = '\0';
    do {
        *--p = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    return p;
}

static void rv32_dump(const emu_cpu_t *cpu, emu_print_fn out, void *ctx)
{
    const rv_hart_t *h = hart_of(cpu);
    char hb[9];
    char db[21];

    out(ctx, "\n  pc      ");  out(ctx, hex32(hb, h->pc));
    out(ctx, "\n  mcause  ");  out(ctx, hex32(hb, h->mcause));
    out(ctx, "  (");           out(ctx, cause_name(h->mcause));
    out(ctx, ")\n  mepc    ");  out(ctx, hex32(hb, h->mepc));
    out(ctx, "   mtval ");     out(ctx, hex32(hb, h->mtval));
    out(ctx, "\n  mstatus ");  out(ctx, hex32(hb, h->mstatus));
    out(ctx, "   mtvec ");     out(ctx, hex32(hb, h->mtvec));
    out(ctx, "\n  priv    ");
    out(ctx, (h->priv == RV_PRIV_M) ? "M"
           : (h->priv == RV_PRIV_S) ? "S" : "U");
#if RV_EXT_S
    /*
     * The S bank as well as the M one. Which of the two holds the live
     * trap state depends on delegation, and reading the wrong one is the
     * standard way to misdiagnose a fault on a core with two levels.
     */
    out(ctx, "\n  scause  ");  out(ctx, hex32(hb, h->scause));
    out(ctx, "   sepc  ");     out(ctx, hex32(hb, h->sepc));
    out(ctx, "  stval ");      out(ctx, hex32(hb, h->stval));
    out(ctx, "\n  stvec   ");  out(ctx, hex32(hb, h->stvec));
    out(ctx, "  medeleg ");    out(ctx, hex32(hb, h->medeleg));
    out(ctx, "  mideleg ");    out(ctx, hex32(hb, h->mideleg));
#endif
    out(ctx, "\n");

    for (unsigned i = 0; i < 32u; i++) {
        const char *n = rv_reg_name(i);
        out(ctx, (i % 4u == 0u) ? "  " : "  ");
        out(ctx, n);
        /* Pad the ABI name out to four columns. */
        for (size_t k = strlen(n); k < 4u; k++) {
            out(ctx, " ");
        }
        out(ctx, " ");
        out(ctx, hex32(hb, h->x[i]));
        if (i % 4u == 3u) {
            out(ctx, "\n");
        }
    }

    out(ctx, "  retired ");
    out(ctx, dec64(db, h->minstret));
#if EMU_ENABLE_STATS
    out(ctx, "  traps ");
    out(ctx, dec64(db, h->trap_count));
#endif
    out(ctx, "\n");
}

/* ------------------------------------------------------------------ */
/* The table                                                           */
/* ------------------------------------------------------------------ */

const emu_cpu_ops_t rv32_frontend = {
    .name        = "rv32",
    .desc        = RV32_ISA_STRING,
    .elf_machine = EMU_EM_RISCV,

    .instance    = rv32_instance,
    .init        = rv32_init,
    .reset       = rv32_reset,
    .boot        = rv32_boot,

    .run         = rv32_run,
    .invalidate  = rv32_invalidate,
    .step        = rv32_step,

    .ncores          = 1u,
    .add_shared_devices = rv32_add_shared_devices,
    .add_core_devices   = rv32_add_core_devices,
    .set_irq         = rv32_set_irq,
    .set_unmask_hook = rv32_set_unmask_hook,
    .advance_time    = rv32_advance_time,
    .set_time        = rv32_set_time,

    .set_syscall = rv32_set_syscall,
    .set_trace   = rv32_set_trace,
    .set_cache   = rv32_set_cache,
    .halt        = rv32_halt,

    .status      = rv32_status,
    .nregs       = 32u,
    .reg_name    = rv_reg_name,
    .reg_read    = rv32_reg_read,
    .reg_write   = rv32_reg_write,
    .dump        = rv32_dump,
#if RV_ENABLE_DISASM
    .disasm      = rv_disasm,
#else
    .disasm      = NULL,
#endif
};
