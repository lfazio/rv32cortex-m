/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_session.c - see emu_session.h for why the middle of the two runners
 * is one file and their two ends are not.
 */

#include "emu_session.h"
#include "emu_console.h"

#include "board.h"

#if EMU_NET
#include "emu_net.h"
#endif

#include "emu/emu_elf.h"
#include "emu/emu_ir.h"
#include "emu/emu_jit.h"
#include "emu/emu_memmap.h"

#if EMU_PAIR_STATS
#include "emu/emu_pairstats.h"
#endif

/*
 * Its own conditional, not the one above. Tucking this inside
 * EMU_PAIR_STATS made the declaration depend on an unrelated option
 * being on, so the build failed on an implicit declaration in exactly
 * the configuration the header was added for.
 */
#if EMU_JIT_HOT_REG_STATS
#include "emu/emu_regstats.h"
#endif

#include <string.h>

static void fail(const emu_session_cfg_t *cfg, const char *msg,
                 const char *detail)
{
    if (cfg->fail != NULL) {
        cfg->fail(msg, detail);
    }
}

/*
 * Place the image, and say where the guest starts.
 *
 * Three cases, and the third is the one that reads as doing nothing:
 *
 *   ELF            placed by its program headers, entry from e_entry.
 *                  emu_elf_map rather than emu_elf_load when the caller
 *                  gave a RAM window, so a board maps its flash-resident
 *                  text instead of copying 345 KiB into 243 KiB.
 *
 *   flat, moved    written into the bus. A guest linked to run from RAM
 *                  needs this, and riscv-tests are exactly that -- they
 *                  also write to their own image, which the read-only
 *                  window would fault.
 *
 *   flat, in place nothing to do: the window at EMU_GUEST_ROM_BASE *is*
 *                  this buffer, so the bytes are already where the guest
 *                  will fetch them.
 */
static bool place_image(emu_system_t *sys, const emu_session_cfg_t *cfg,
                        uint32_t *entry_out)
{
    const uint32_t load =
        (cfg->load_addr != 0u) ? cfg->load_addr : EMU_GUEST_ROM_BASE;
    uint32_t entry = cfg->entry;
    emu_bus_t *const bus = sys->core[0].bus;

    if (emu_elf_is_elf(cfg->image, cfg->image_size)) {
        uint32_t e = 0u;
        const char *err;

        if (cfg->ram_size != 0u && load == EMU_GUEST_ROM_BASE) {
            err = emu_elf_map(bus, cfg->image, cfg->image_size,
                              cfg->ops->elf_machine, cfg->ops->elf_machine_alt,
                              cfg->ram_base, cfg->ram_size, &e, NULL);
        } else {
            err = emu_elf_load(bus, cfg->image, cfg->image_size,
                               cfg->ops->elf_machine, cfg->ops->elf_machine_alt,
                               &e, NULL);
        }
        if (err != NULL) {
            fail(cfg, "elf", err);
            return false;
        }
        if (entry == 0u) {
            entry = e;
        }
    } else if (load != EMU_GUEST_ROM_BASE) {
        if (!emu_bus_load(bus, load, cfg->image, cfg->image_size)) {
            fail(cfg, "image does not fit at the load address", NULL);
            return false;
        }
        if (entry == 0u) {
            entry = load;
        }
    } else if (entry == 0u) {
        entry = load;
    }

    *entry_out = entry;
    return true;
}

/*
 * The frontend's own devices, onto every core's bus.
 *
 * Not emu_system_open, which also *opens* the cores: a reload replaces
 * the guest and not the machine, and re-opening would take the cores out
 * from under the gdb stub that is pointed at them.
 */
static bool add_devices(emu_system_t *sys, const emu_session_cfg_t *cfg)
{
    const emu_cpu_ops_t *const ops = sys->ops;

    for (unsigned i = 0; i < sys->ncores; i++) {
        emu_bus_t *const bus = &cfg->buses[i];

        if (ops->set_image != NULL) {
            ops->set_image(cfg->image, cfg->image_size);
        }
        if ((ops->add_shared_devices != NULL &&
             !ops->add_shared_devices(bus)) ||
            (ops->add_core_devices != NULL &&
             !ops->add_core_devices(sys->core[i].cpu, bus, i))) {
            fail(cfg, "the frontend's devices do not fit on the bus", NULL);
            return false;
        }
    }
    return true;
}

/*
 * Clear RAM, place the image, reset and boot: the half of a reload that
 * a first start also does.
 *
 * Split from add_devices because emu_system_open *already* adds them --
 * calling both is how this first went wrong, and it presents as "the
 * frontend's devices do not fit on the bus" with seven of sixteen regions
 * used, which reads as a size problem and is a duplicate one.
 */
static bool finish(emu_system_t *sys, const emu_session_cfg_t *cfg)
{
    uint32_t entry = 0u;

    /* Before the image, because an ELF's segments and its .bss land in
     * here and zeroing afterwards would erase them. */
    if (cfg->ram_host != NULL && cfg->ram_size != 0u) {
        memset(cfg->ram_host, 0, cfg->ram_size);
    }

    if (!place_image(sys, cfg, &entry)) {
        return false;
    }

    /*
     * Cleared *before* the image is placed would be wrong -- an ELF's
     * segments land in it -- so this is here, between the two.
     */
    emu_system_reset(sys, entry);

    /*
     * The device tree goes at the top of RAM, below which the frontend
     * puts the stack. Aligned to 8: the FDT header requires it, and a
     * misaligned blob is rejected by every parser with a message about
     * a bad magic rather than about alignment.
     */
    emu_boot_info_t boot = {
        .ram_base = cfg->ram_base,
        .ram_size = cfg->ram_size,
        .dtb = 0u,
        .supervisor = cfg->supervisor,
    };

    if (cfg->dtb != NULL && cfg->dtb_size != 0u && cfg->ram_size != 0u) {
        emu_bus_t *const bus = sys->core[0].bus;
        const uint32_t at =
            ((cfg->ram_base + cfg->ram_size) - cfg->dtb_size) & ~7u;

        if (at < cfg->ram_base) {
            fail(cfg, "dtb", "does not fit in guest RAM");
            return false;
        }
        for (uint32_t i = 0; i < cfg->dtb_size; i++) {
            if (emu_bus_write(bus, at + i, 1u, cfg->dtb[i]) !=
                EMU_FAULT_NONE) {
                fail(cfg, "dtb", "could not be written to guest RAM");
                return false;
            }
        }
        boot.dtb = at;
    }

    emu_system_boot(sys, &boot);

    /*
     * Every translation is stale: a new image at the same guest
     * addresses is precisely the case a JIT cannot detect for itself.
     * Reset already flushes on both backends, and this covers a frontend
     * whose reset does not.
     */
    emu_system_invalidate(sys, 0u, 0xFFFFFFFFu);
    return true;
}

/*
 * A reload rebuilds the buses, so the frontend's devices have to go back
 * on -- emu_bus_init cleared the table. The cores are *not* re-opened:
 * the guest changes, the machine does not, and re-opening would take them
 * out from under the gdb stub that is pointed at them.
 */
bool emu_session_reload(emu_system_t *sys, const emu_session_cfg_t *cfg)
{
    return add_devices(sys, cfg) && finish(sys, cfg);
}

/*
 * The console, kept so the run loop can poll it for input without every
 * caller having to hold the pointer. See emu_session_poll_uart.
 */
static emu_uart_t *g_uart;

/*
 * Look for an arriving byte, and raise the receive interrupt if one has.
 *
 * The transport is polled, so this is the only thing that ever notices;
 * a guest that enables the receive interrupt and waits is waiting for
 * something nothing else can cause.
 */
void emu_session_poll_uart(void)
{
    if (g_uart != NULL) {
        emu_uart_poll(g_uart);
    }
}

bool emu_session_start(emu_system_t *sys, const emu_session_cfg_t *cfg)
{
    const emu_cpu_ops_t *const ops =
        (cfg->ops != NULL) ? cfg->ops : emu_frontend_default();

    if (cfg->uart != NULL && cfg->uart_tx != NULL) {
        emu_uart_init(cfg->uart, cfg->uart_tx, cfg->uart_rx, NULL);
        /* After init, which clears it. See emu_session_cfg_t::uart_irq. */
        emu_uart_set_irq(cfg->uart, cfg->uart_irq, NULL);
    }
    g_uart = cfg->uart;

    if (!emu_system_open(sys, ops, cfg->buses, cfg->ncores)) {
        fail(cfg, "could not bring the cores up", NULL);
        return false;
    }

    /*
     * Which backend, before anything runs. A frontend prefers its JIT
     * wherever one is compiled in, which is right for firmware and wrong
     * for a runner that has to be able to say "the suite passes
     * interpreted" and "the suite passes translated" as separate claims.
     */
    if (ops->select_backend != NULL &&
        !ops->select_backend(sys->core[0].cpu, cfg->want_jit)) {
        fail(cfg, "backend init failed", NULL);
        return false;
    }

    for (unsigned i = 0; i < sys->ncores; i++) {
        emu_cpu_t *const cpu = sys->core[i].cpu;

        if (ops->set_syscall != NULL && cfg->syscall_fn != NULL) {
            ops->set_syscall(cpu, cfg->syscall_fn, cfg->syscall_ctx);
        }
        if (ops->set_cache != NULL && cfg->cache_ops != NULL) {
            ops->set_cache(cpu, cfg->cache_ops);
        }
        if (ops->set_unmask_hook != NULL && cfg->unmask_fn != NULL) {
            ops->set_unmask_hook(cpu, cfg->unmask_fn, cfg->unmask_ctx);
        }
#if EMU_ENABLE_TRACE
        /*
         * Always, in a build that has tracing -- not "if the platform
         * passed one". A trace is the same question on every platform and
         * the board simply did not have one, which is backwards: it is
         * where a guest is hardest to observe.
         */
        if (ops->set_trace != NULL) {
            ops->set_trace(cpu, emu_trace_insn, (void *)(uintptr_t)ops);
        }
#endif
    }

    /* No add_devices: emu_system_open did it. */
    return finish(sys, cfg);
}

/* ------------------------------------------------------------------ */
/* Reporting                                                           */
/* ------------------------------------------------------------------ */

void emu_session_report(emu_system_t *sys, uint64_t retired,
                        uint32_t host_cycles, bool capped, bool dump_state)
{
    if (capped) {
        /*
         * On its own line and before the numbers, so a harness reading
         * the console can tell "did not terminate" from "ran and failed"
         * without parsing them.
         */
        emu_console_printf("\nemu: instruction cap reached, guest did not "
                           "halt\n");
    }

    emu_print_run_summary(retired, host_cycles);

    if (sys->ncores > 1u) {
        /* Per core, because that is what a determinism check compares
         * between two runs of a multicore guest. */
        for (unsigned i = 0; i < sys->ncores; i++) {
            emu_cpu_status_t st;

            emu_core_status(&sys->core[i], &st);
            emu_console_printf("  core %u  %u retired\n", i,
                               (unsigned)st.retired);
        }
    }

    /*
     * No #if on a JIT macro. emu_print_jit_stats answers "is there one
     * here" from the framework's own code_size, which is the only fact
     * that decides it -- and the capability macro that used to guard this
     * was read in a file that did not include what defines it, so the
     * whole block silently stopped printing.
     */
    (void)emu_print_jit_stats();

    /*
     * What the IR optimiser did. Reported because a pass that never fires
     * and a pass that does not pay are indistinguishable otherwise: these
     * counters had no reader at all until a fusion pass needed proving,
     * and it was a 17% regression twice before it was a 17% win.
     */
    {
        emu_ir_opt_stats_t o;

        emu_ir_opt_totals(&o);
        if (o.blocks != 0u) {
            emu_console_printf(
                "\n-- ir --\n"
                "  blocks   %u optimised\n"
                "  elided   gets %u  puts %u  flags %u  dead %u\n"
                "  fused    const->imm %u  addr %u  identities %u  mac %u\n",
                (unsigned)o.blocks, (unsigned)o.gets_removed,
                (unsigned)o.puts_removed, (unsigned)o.flags_removed,
                (unsigned)o.dead_removed, (unsigned)o.folded,
                (unsigned)o.addr_folded, (unsigned)o.identities,
                (unsigned)o.macs);
        }
    }

#if EMU_PAIR_STATS
    emu_pair_report(40u);
#endif

#if EMU_JIT_HOT_REG_STATS
    emu_reg_report();
#endif

    /*
     * Instructions per second, from the part's own clock.
     *
     * Not a platform's job: it is retired against elapsed against
     * board_clock_hz(), and every platform has all three. A runner
     * reports 0 host cycles and gets no line, which is the same rule that
     * suppresses the ratio.
     */
    if (retired != 0u && host_cycles != 0u && board_clock_hz() != 0u) {
        const uint32_t kips =
            (uint32_t)((uint64_t)retired * (board_clock_hz() / 1000u) /
                       host_cycles);

        emu_console_printf("  speed    %u KIPS\n", (unsigned)kips);
    }

    if (dump_state) {
        emu_report_states(sys);
    }

#if EMU_NET
    /*
     * Only when there is a link. A build that *can* do networking is not
     * a run that did: a host without --ppp would otherwise report "rx
     * drops 0  tftp reclaims 0" about a wire it never opened, which is a
     * measurement of nothing wearing the shape of one.
     *
     * Bytes the wire delivered and nothing collected, next to the guest's
     * own numbers because it is the one failure that makes *those*
     * untrustworthy without looking wrong: a dropped byte is a dropped
     * frame, which is a retransmission at best and a truncated image at
     * worst.
     */
    if (emu_net_active()) {
        emu_console_printf("\n-- net --\n  rx drops %u  tftp reclaims %u\n",
                           (unsigned)board_console_rx_overruns(),
                           (unsigned)emu_net_tftp_reclaims());
    }
#endif
}
