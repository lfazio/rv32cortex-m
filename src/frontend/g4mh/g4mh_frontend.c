/* SPDX-License-Identifier: Apache-2.0 */
/*
 * g4mh_frontend.c - The RH850 G4MH frontend's emu_cpu_ops_t.
 *
 * The counterpart of rv32_frontend.c, and deliberately shaped the same
 * way: everything a platform is allowed to know about this frontend, with
 * the core, the interrupt controller and the backend selection behind it.
 *
 * Nothing here is on a per-instruction path -- `run` hands a whole budget
 * to the backend and every other entry point is setup or fires on a trap.
 */

#include "emu/emu_cpu.h"
#include "emu/emu_memmap.h"

#include "g4mh/g4mh_cpu.h"
#include "emu/emu_jit.h"
#include "g4mh/g4mh_decode.h"
#include "g4mh/g4mh_disasm.h"
#include "g4mh/g4mh_intc.h"
#include "g4mh/g4mh_intercpu.h"
#include "g4mh/g4mh_memmap.h"

#include <string.h>

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

/*
 * The processing elements, statically allocated -- G4MH_PE_COUNT of them,
 * 3 on a host and 1 on the firmware, where 64 KiB of local RAM per core
 * does not fit in 128 KiB of SRAM.
 *
 * INTC1 is per PE and INTC2 is shared, which the register layout already
 * says: EICn is one 16-bit register at base + 0x02 * n in both units, so
 * channels 0-31 are answered by a core's own INTC1 and the rest by the one
 * INTC2. The state is one g4mh_intc_t per core for the local half plus a
 * shared one for the global half; see docs/host/g4mh/multicore.md.
 */
static g4mh_cpu_t  g_cpu[G4MH_PE_COUNT];
static g4mh_intc_t g_intc[G4MH_PE_COUNT];

/*
 * The inter-CPU peripherals: one state each for the whole system, and
 * one port per core naming which PE is looking, because every one of
 * them has a self region. The ports are what the bus regions bind to.
 */
static g4mh_barrier_t g_barr;
static g4mh_ipir_t    g_ipir;
static g4mh_tptm_t    g_tptm;
static g4mh_intercpu_port_t g_barr_port[G4MH_PE_COUNT];
static g4mh_intercpu_port_t g_ipir_port[G4MH_PE_COUNT];
static g4mh_intercpu_port_t g_tptm_port[G4MH_PE_COUNT];

static emu_cpu_t *g4mh_instance(unsigned index)
{
    return (index < G4MH_PE_COUNT) ? (emu_cpu_t *)&g_cpu[index] : NULL;
}

static EMU_ALWAYS_INLINE g4mh_cpu_t *cpu_of(const emu_cpu_t *cpu)
{
    return (g4mh_cpu_t *)(uintptr_t)(const void *)cpu;
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

static void g4mh_ops_init(emu_cpu_t *cpu, emu_bus_t *bus, uint32_t coreid)
{
    g4mh_cpu_t *c = cpu_of(cpu);

    g4mh_cpu_init(c, bus, coreid);
    /* PE0's instance holds the shared INTC2 half; the others point at it. */
    g4mh_intc_init(&g_intc[coreid], c,
                   (coreid == 0u) ? NULL : &g_intc[0]);
    /* Join the cross-core reservation tracker, so this core's LDL.W can be
     * broken by another core's store. */
    g4mh_ll_register(c);

    /*
     * The inter-CPU peripherals learn where to send an interrupt for
     * this PE. Initialised on PE0's pass rather than at file scope,
     * because a reload rebuilds every core and these hold pending state
     * that must not survive it -- the same reason start_guest() rebuilds
     * the bus.
     */
    if (coreid == 0u) {
        g4mh_barrier_init(&g_barr);
        g4mh_ipir_init(&g_ipir);
        g4mh_tptm_init(&g_tptm);
    }
    g4mh_ipir_bind(&g_ipir, coreid, &g_intc[coreid]);
    g4mh_tptm_bind(&g_tptm, coreid, &g_intc[coreid]);

#if G4MH_HAVE_JIT
    /*
     * Prefer the JIT where it exists. It translates what it can and hands
     * everything else to the interpreter per instruction, so this is a
     * speed choice rather than a coverage one -- but see the host
     * platform, which states which backend it wants rather than
     * inheriting this, because for it the difference *is* coverage.
     */
    g4mh_backend = &g4mh_backend_jit;
#endif
    if (g4mh_backend->init != NULL && !g4mh_backend->init(cpu)) {
        g4mh_backend = &g4mh_backend_interp;
    }
}

static void g4mh_ops_reset(emu_cpu_t *cpu, uint32_t reset_pc)
{
    g4mh_cpu_reset(cpu_of(cpu), reset_pc);
    if (g4mh_backend->reset != NULL) {
        g4mh_backend->reset(cpu);
    }
}

static void g4mh_ops_boot(emu_cpu_t *cpu, uint32_t ram_base, uint32_t ram_size)
{
    g4mh_cpu_boot(cpu_of(cpu), ram_base, ram_size);
}

/* ------------------------------------------------------------------ */
/* Execution                                                           */
/* ------------------------------------------------------------------ */

static emu_run_reason_t g4mh_ops_run(emu_cpu_t *cpu, uint32_t budget,
                                     uint32_t *retired)
{
    g4mh_cpu_t *c = cpu_of(cpu);

    /*
     * The performance counters are advanced here, from the retired
     * delta, rather than in the interpreter's retire path.
     *
     * Ticking per instruction in the interpreter was the obvious place
     * and was wrong: it counts only *interpreted* instructions, and
     * under the JIT that is a small and arbitrary subset. Measured on a
     * seven-instruction program, the interpreter counted 5 and the JIT
     * counted 2 -- and 2 is the worse answer, because it is plausible.
     * This is the failure mode this project keeps recording: a side
     * effect added to the interpreter that the translated path bypasses.
     *
     * `retired` is maintained by both backends because the status line
     * needs it, so the delta is the one quantity that is already correct
     * either way, and taking it here costs nothing per instruction.
     *
     * The price is granularity: the counters advance once per run slice,
     * so a guest reading PMCOUNT with STSR sees the value as of the last
     * slice boundary rather than as of the instruction before it. That is
     * a real limitation and is why nothing here asserts an exact count.
     */
    const uint64_t before = c->retired;
    const emu_run_reason_t why = g4mh_backend->run(cpu, budget, retired);

    if (EMU_UNLIKELY(c->pm_active) && c->retired > before) {
        g4mh_pm_tick(c, (uint32_t)(c->retired - before));
    }
    return why;
}

static void g4mh_ops_invalidate(emu_cpu_t *cpu, uint32_t addr, uint32_t len)
{
    if (g4mh_backend->invalidate != NULL) {
        g4mh_backend->invalidate(cpu, addr, len);
    }
}

static emu_run_reason_t g4mh_ops_step(emu_cpu_t *cpu)
{
    return g4mh_step(cpu_of(cpu));
}

/* ------------------------------------------------------------------ */
/* Platform devices                                                    */
/* ------------------------------------------------------------------ */

/*
 * INTC1 is mapped twice on purpose: once at the SELF alias, which is how
 * core-local code reaches its own controller without knowing its PE
 * number, and once at PE0's absolute base, which is how another core or a
 * debugger reaches it. Both are the same registers, so both get the same
 * device and the same state.
 */
/*
 * INTC2 and the OS timer: one object, mapped into every core's bus at the
 * same address, so all cores see the same global controller.
 */
/*
 * Backing memory for the map above -- and note what is *not* here.
 *
 * Code flash is not allocated. It is where code is executed from, so on a
 * target it is the host part's own flash holding the guest image, exposed
 * read-only and costing no RAM at all; that is exactly what the RV32 side
 * does with its ROM region, and `g4mh_set_flash` is how a platform says
 * so. Backing the architectural 3 MiB with .bss meant this frontend could
 * not link as firmware: 3 MiB of flash plus 384 KiB of cluster RAM plus
 * 64 KiB per PE is 3.44 MiB of .bss on a part with 320 KiB of SRAM.
 *
 * What is left is sized by G4MH_CRAM_KIB and G4MH_LRAM_KIB, because on a
 * microcontroller every byte of it is a byte the guest does not get.
 */
static uint8_t g_cram[G4MH_CRAM_BACKED];
static uint8_t g_lram[G4MH_PE_COUNT][G4MH_LRAM_BACKED];

/* Where code flash is backed, if a platform has said. */
static const void *g_flash_ptr;
static uint32_t    g_flash_len;
static bool        g_flash_rw;

#if G4MH_FLASH_BACKED > 0u
/*
 * The fallback arena, for a platform that does not supply flash -- the
 * host runner, whose ELF loader writes the image straight into it, so it
 * has to be writable. Firmware sets G4MH_FLASH_KIB=0 and this disappears.
 */
static uint8_t g_flash_arena[G4MH_FLASH_BACKED];
#endif

void g4mh_set_flash(const void *base, uint32_t size, bool writable)
{
    g_flash_ptr = base;
    g_flash_len = size;
    g_flash_rw  = writable;
}

static bool g4mh_ops_add_shared_devices(emu_bus_t *bus)
{
    /*
     * Code flash and cluster RAM are one memory the whole cluster shares,
     * so every core's bus points at the same backing store -- which is
     * what makes a store from one PE visible to the next.
     */
    const void *fp = g_flash_ptr;
    uint32_t    fl = g_flash_len;
    bool        rw = g_flash_rw;

#if G4MH_FLASH_BACKED > 0u
    if (fp == NULL) {
        fp = g_flash_arena;
        fl = (uint32_t)sizeof(g_flash_arena);
        rw = true;
    }
#endif

    /*
     * Writable flash is a simplification, not a model: there is no flash
     * sequencer here, and a guest writing its own code is doing something
     * a real part would refuse. A platform serving the image out of its
     * own flash passes writable=false and gets the refusal for free.
     */
    if (fp != NULL && fl != 0u) {
        const bool ok = rw
            ? emu_bus_add_ram(bus, "flash", G4MH_FLASH_BASE,
                              (void *)(uintptr_t)fp, fl)
            : emu_bus_add_rom(bus, "flash", G4MH_FLASH_BASE, fp, fl);
        if (!ok) {
            return false;
        }
    }

    return emu_bus_add_ram(bus, "cram", G4MH_CRAM_BASE,
                           g_cram, (uint32_t)sizeof(g_cram)) &&
           emu_bus_add_mmio(bus, "intc2", G4MH_INTC2_BASE,
                            G4MH_INTC2_SIZE, &g4mh_intc2_ops, &g_intc[0]) &&
           emu_bus_add_mmio(bus, "ostm0", G4MH_OSTM0_BASE,
                            G4MH_OSTM0_SIZE, &g4mh_ostm_ops, &g_intc[0]) &&
           /*
            * INTIF, for TPTMSEL. One register for the whole system, so
            * it binds to the global INTC instance like INTC2 does.
            */
           emu_bus_add_mmio(bus, "intif", G4MH_INTIF_BASE,
                            G4MH_INTIF_SIZE, &g4mh_intif_ops, &g_intc[0]);
}

/*
 * INTC1, twice, and this is the whole reason each core has its own bus:
 *
 *   the SELF alias at 0xFFFC_0000 resolves to *this* core's INTC1, so the
 *   same guest address means different memory on each core;
 *   the absolute window at 0xFFFC_4000 + 0x4000 * n resolves to PE n's,
 *   whichever core is looking.
 *
 * With one shared bus the first of those could not be expressed without an
 * indirection on every access or mutable global state. With a bus per core
 * it is two ordinary regions pointing at different objects, and the access
 * path never learns that multicore exists.
 */
static bool g4mh_ops_add_core_devices(emu_cpu_t *cpu, emu_bus_t *bus,
                                      unsigned index)
{
    (void)cpu;

    if (!emu_bus_add_mmio(bus, "intc1-self", G4MH_INTC1_SELF_BASE,
                          G4MH_INTC1_SIZE, &g4mh_intc1_ops, &g_intc[index])) {
        return false;
    }

    /*
     * Local RAM, mapped twice for the same reason INTC1 is: once at the
     * SELF alias, which on this core means *this* core's RAM, and once at
     * every PE's absolute address, which means the same memory whichever
     * core is looking. One image linked against the SELF window therefore
     * runs on any PE and gets its own copy of everything.
     *
     * The absolute windows descend from PE0, so PE n is at
     * PE0_BASE - n * STRIDE -- see the note in g4mh_memmap.h.
     */
    if (!emu_bus_add_ram(bus, "lram-self", G4MH_LRAM_SELF_BASE,
                         g_lram[index], (uint32_t)sizeof(g_lram[0]))) {
        return false;
    }
    for (unsigned pe = 0; pe < G4MH_PE_COUNT; pe++) {
        static const char *const lnm[3] = { "lram-pe0", "lram-pe1",
                                            "lram-pe2" };
        if (!emu_bus_add_ram(bus, lnm[pe], G4MH_LRAM_PE_BASE(pe),
                             g_lram[pe], (uint32_t)sizeof(g_lram[0]))) {
            return false;
        }
    }
    for (unsigned pe = 0; pe < G4MH_PE_COUNT; pe++) {
        static const char *const nm[3] = { "intc1-pe0", "intc1-pe1",
                                           "intc1-pe2" };
        if (!emu_bus_add_mmio(bus, nm[pe],
                              G4MH_INTC1_PE0_BASE + pe * G4MH_INTC1_PE_STRIDE,
                              G4MH_INTC1_SIZE, &g4mh_intc1_ops,
                              &g_intc[pe])) {
            return false;
        }
    }

    /*
     * BARR, IPIR and TPTM. One region each, per core -- not because the
     * state is per core (it is not; all three are shared) but because
     * each has a *self* region whose meaning is "the PE doing the
     * access". The port carries that PE, so the device never has to ask
     * who is calling and the access path stays a plain region lookup.
     *
     * This is the third use of that arrangement, after INTC1-self and
     * LRAM-self, and it is the whole reason a bus per core pays for
     * itself.
     */
    g_barr_port[index].state = &g_barr;
    g_barr_port[index].pe    = index;
    g_ipir_port[index].state = &g_ipir;
    g_ipir_port[index].pe    = index;
    g_tptm_port[index].state = &g_tptm;
    g_tptm_port[index].pe    = index;

    return emu_bus_add_mmio(bus, "barr", G4MH_BARR_BASE, G4MH_BARR_SIZE,
                            &g4mh_barrier_ops, &g_barr_port[index]) &&
           emu_bus_add_mmio(bus, "ipir", G4MH_IPIR_BASE, G4MH_IPIR_SIZE,
                            &g4mh_ipir_ops, &g_ipir_port[index]) &&
           emu_bus_add_mmio(bus, "tptm", G4MH_TPTM_BASE, G4MH_TPTM_SIZE,
                            &g4mh_tptm_ops, &g_tptm_port[index]);
}

static void g4mh_ops_set_irq(emu_cpu_t *cpu, uint32_t source, bool level)
{
    (void)cpu;
    /*
     * Lowering is a no-op: a channel's request bit is cleared by the guest
     * writing it, or by the core acknowledging the interrupt. A host
     * handler that raises and then clears is describing an edge.
     */
    if (level) {
        g4mh_intc_raise(&g_intc[0], source);
    }
}

static void g4mh_ops_set_unmask_hook(emu_cpu_t *cpu, emu_unmask_fn fn,
                                     void *ctx)
{
    (void)cpu;
    g4mh_intc_set_unmask(&g_intc[0], fn, ctx);
}

static void g4mh_ops_advance_time(emu_cpu_t *cpu, uint32_t ticks)
{
    (void)cpu;
    g4mh_intc_advance(&g_intc[0], ticks);
    /*
     * The TPTM runs off cpu_clk while the OSTM above runs off the
     * platform's 1 MHz tick, and this call hands the same number to
     * both. That is the platform's tick rate standing in for the CPU
     * clock: the emulator has no separate cpu_clk to offer, and the
     * dividers are relative anyway. A guest computing an absolute
     * period from a datasheet frequency will be wrong by that ratio.
     */
    g4mh_tptm_advance(&g_tptm, ticks);
}

static void g4mh_ops_set_time(emu_cpu_t *cpu, uint64_t now)
{
    (void)cpu;
    g4mh_intc_set_time(&g_intc[0], now);
}

/* ------------------------------------------------------------------ */
/* Platform services                                                   */
/* ------------------------------------------------------------------ */

static void g4mh_ops_set_syscall(emu_cpu_t *cpu, emu_syscall_fn fn, void *user)
{
    g4mh_cpu_t *c = cpu_of(cpu);
    c->syscall = fn;
    c->syscall_user = user;
}

static void g4mh_ops_set_trace(emu_cpu_t *cpu, emu_trace_fn fn, void *user)
{
#if EMU_ENABLE_TRACE
    g4mh_cpu_t *c = cpu_of(cpu);
    c->trace = fn;
    c->trace_user = user;
#else
    (void)cpu; (void)fn; (void)user;
#endif
}

static void g4mh_ops_set_cache(emu_cpu_t *cpu, const emu_cache_ops_t *ops)
{
    cpu_of(cpu)->cache = ops;
}

static void g4mh_ops_halt(emu_cpu_t *cpu)
{
    cpu_of(cpu)->state = EMU_STATE_HALTED;
}

/* ------------------------------------------------------------------ */
/* Introspection                                                       */
/* ------------------------------------------------------------------ */

static void g4mh_ops_status(const emu_cpu_t *cpu, emu_cpu_status_t *out)
{
    const g4mh_cpu_t *c = cpu_of(cpu);

    out->backend = g4mh_backend->name;
    out->pc      = c->pc;
    out->retired = c->retired;
    out->state   = (emu_state_t)c->state;
#if EMU_ENABLE_STATS
    out->traps   = c->exc_count;
#else
    out->traps   = 0u;
#endif
    /*
     * Parked in HALT with every channel masked means nothing can ever wake
     * this core. PSW.ID is not part of the test: HALT resumes on a pending
     * request whether or not interrupts are enabled, but a channel that is
     * masked at the controller can never become one.
     */
    out->wakeable = false;
    for (unsigned i = 0; i < G4MH_INT_CHANNELS; i++) {
        if ((g_intc[c->coreid].eic[i] & G4MH_EIC_EIMK) == 0u) {
            out->wakeable = true;
            break;
        }
    }
}

static uint32_t g4mh_ops_reg_read(const emu_cpu_t *cpu, unsigned r)
{
    return (r < 32u) ? cpu_of(cpu)->r[r] : 0u;
}

static void g4mh_ops_reg_write(emu_cpu_t *cpu, unsigned r, uint32_t v)
{
    if (r < 32u) {
        g4mh_cpu_t *c = cpu_of(cpu);
        c->r[r] = v;
        c->r[0] = 0u;      /* r0 stays hardwired */
    }
}

/* ------------------------------------------------------------------ */
/* State dump                                                          */
/* ------------------------------------------------------------------ */

static const char *cause_name(uint32_t cause)
{
    if (cause >= G4MH_EXC_EIINT_BASE && cause < G4MH_EXC_SYSCALL) {
        return "external interrupt";
    }
    if (cause >= G4MH_EXC_TRAP0 && cause < G4MH_EXC_TRAP0 + 0x10u) {
        return "trap 0-15";
    }
    if (cause >= G4MH_EXC_TRAP1 && cause < G4MH_EXC_TRAP1 + 0x10u) {
        return "trap 16-31";
    }
    switch (cause) {
    case G4MH_EXC_SYSERR: return "system error";
    case G4MH_EXC_MIP:    return "instruction-fetch protection";
    case G4MH_EXC_MDP:    return "data protection";
    case G4MH_EXC_RIE:    return "reserved instruction";
    case G4MH_EXC_MAE:    return "misaligned access";
    case G4MH_EXC_FPP:    return "floating-point operation";
    case G4MH_EXC_UCPOP:  return "coprocessor unusable";
    default:              return "none or unknown";
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

static void g4mh_ops_dump(const emu_cpu_t *cpu, emu_print_fn out, void *ctx)
{
    const g4mh_cpu_t *c = cpu_of(cpu);
    char hb[9];
    char db[21];

    /*
     * Both save-register pairs are printed, because which one holds the
     * live return state depends on PSW.NP and reading the wrong one is the
     * classic way to misdiagnose an RH850 fault.
     */
    out(ctx, "\n  pc      ");  out(ctx, hex32(hb, c->pc));
    out(ctx, "\n  psw     ");  out(ctx, hex32(hb, c->psw));
    out(ctx, "\n  eiic    ");  out(ctx, hex32(hb, c->sr[0][G4MH_SR_EIIC]));
    out(ctx, "  (");           out(ctx, cause_name(c->sr[0][G4MH_SR_EIIC]));
    out(ctx, ")\n  eipc    ");  out(ctx, hex32(hb, c->sr[0][G4MH_SR_EIPC]));
    out(ctx, "   eipsw ");     out(ctx, hex32(hb, c->sr[0][G4MH_SR_EIPSW]));
    out(ctx, "\n  feic    ");  out(ctx, hex32(hb, c->sr[0][G4MH_SR_FEIC]));
    out(ctx, "  (");           out(ctx, cause_name(c->sr[0][G4MH_SR_FEIC]));
    out(ctx, ")\n  fepc    ");  out(ctx, hex32(hb, c->sr[0][G4MH_SR_FEPC]));
    out(ctx, "   fepsw ");     out(ctx, hex32(hb, c->sr[0][G4MH_SR_FEPSW]));
    out(ctx, "\n  rbase   ");  out(ctx, hex32(hb, c->sr[1][G4MH_SR_RBASE]));
    out(ctx, "   mea   ");     out(ctx, hex32(hb, c->sr[2][G4MH_SR_MEA]));
    out(ctx, "\n");

    for (unsigned i = 0; i < 32u; i++) {
        const char *n = g4mh_reg_name(i);
        out(ctx, "  ");
        out(ctx, n);
        for (size_t k = strlen(n); k < 4u; k++) {
            out(ctx, " ");
        }
        out(ctx, " ");
        out(ctx, hex32(hb, c->r[i]));
        if (i % 4u == 3u) {
            out(ctx, "\n");
        }
    }

    out(ctx, "  retired ");
    out(ctx, dec64(db, c->retired));
#if EMU_ENABLE_STATS
    out(ctx, "  exceptions ");
    out(ctx, dec64(db, c->exc_count));
#endif
    out(ctx, "\n");
}

/* ------------------------------------------------------------------ */
/* The table                                                           */
/* ------------------------------------------------------------------ */

const emu_cpu_ops_t g4mh_frontend = {
    .name        = "g4mh",
    .desc        = "Renesas RH850 G4MH",
    .elf_machine     = EMU_EM_V850,
    .elf_machine_alt = EMU_EM_V800,

    .instance    = g4mh_instance,
    .init        = g4mh_ops_init,
    .reset       = g4mh_ops_reset,
    .boot        = g4mh_ops_boot,

    .run         = g4mh_ops_run,
    .invalidate  = g4mh_ops_invalidate,
    .step        = g4mh_ops_step,

    .ncores          = G4MH_PE_COUNT,
    .add_shared_devices = g4mh_ops_add_shared_devices,
    .add_core_devices   = g4mh_ops_add_core_devices,
    .set_irq         = g4mh_ops_set_irq,
    .set_unmask_hook = g4mh_ops_set_unmask_hook,
    .advance_time    = g4mh_ops_advance_time,
    .set_time        = g4mh_ops_set_time,

    .set_syscall = g4mh_ops_set_syscall,
    .set_trace   = g4mh_ops_set_trace,
    .set_cache   = g4mh_ops_set_cache,
    .halt        = g4mh_ops_halt,

    .status      = g4mh_ops_status,
    .nregs       = 32u,
    .reg_name    = g4mh_reg_name,
    .reg_read    = g4mh_ops_reg_read,
    .reg_write   = g4mh_ops_reg_write,
    .dump        = g4mh_ops_dump,
#if G4MH_ENABLE_DISASM
    .disasm      = g4mh_disasm,
#else
    .disasm      = NULL,
#endif
};
