/* SPDX-License-Identifier: Apache-2.0 */
/*
 * emu_stats.c - the run summary and the JIT statistics, shared.
 *
 * **Neither of these is a frontend's or a platform's.** The counters
 * come from the dispatch loop in src/emu/emu_jit.c, which every frontend
 * and every host share, so a copy of the printing in each platform was
 * two copies of one thing -- and the F746's was guarded on the RV32
 * frontend, so a G4MH firmware ran a Thumb-2 JIT and reported nothing
 * about it. That is the frontend with no reference model, where "is
 * translation being exercised at all" is the number that matters most.
 *
 * What stays with the platform is the *frontend-specific* half: the
 * RV32 backend's helper-call counts, its passthrough arming, and its
 * hot-register histogram have no framework equivalent, because no other
 * backend has them.
 */

#include "emu_console.h"

#include "emu/emu_jit.h"

#if EMU_GUEST_ARCH_RV32 && EMU_HAVE_JIT
#  include "rv32/rv_backend.h"
#  include "rv32/rv_jit.h"
#endif

void emu_print_run_summary(uint64_t retired, uint32_t host_cycles)
{
    emu_console_printf("\n-- done --\n  retired  %u instructions\n"
                       "  host     %u cycles\n",
                       (unsigned)retired, (unsigned)host_cycles);

    if (retired == 0u) {
        return;
    }

    /*
     * Host cycles per emulated guest instruction, x100 so the fractional
     * part survives integer division -- picolibc is built without float
     * formatting, which is right on a part whose FPU is single precision
     * and whose guest owns it.
     */
    const uint32_t x100 =
        (uint32_t)((uint64_t)host_cycles * 100u / retired);

    emu_console_printf("  ratio    %u.%02u host cycles per guest instruction\n",
                       (unsigned)(x100 / 100u), (unsigned)(x100 % 100u));
}

bool emu_print_jit_stats(void)
{
    emu_jit_stats_t js;

    emu_jit_get_stats(&js);

    /*
     * `code_size` is the test for "there is a JIT here": the framework
     * only has a buffer once a backend initialised one, and it is zero
     * on an interpreter build -- so this needs to name no backend.
     */
    if (js.code_size == 0u) {
        return false;
    }

    emu_console_printf(
        "\n-- jit --\n"
        "  blocks   %u\n"
        "  code     %u/%u bytes\n"
        "  blks/xlat %u\n"
        "  compact  %u (%u evicted)\n"
        "  flushes  %u\n",
        (unsigned)js.blocks, (unsigned)js.code_used, (unsigned)js.code_size,
        (unsigned)js.translations, (unsigned)js.compactions,
        (unsigned)js.evictions, (unsigned)js.flushes);

    /*
     * Read this before believing a passing test. A backend that declines
     * everything and falls back passes every suite while proving nothing
     * about the translator -- `interp` against the retired count is what
     * says whether translation is being exercised.
     *
     * declined and overflowed stay apart: "nothing translatable here" and
     * "the buffer filled" are different outcomes needing different
     * recoveries, and conflating them once cost 65% of all host cycles
     * with every test still passing.
     */
    emu_console_printf(
        "  interp   %u instructions fell back\n"
        "  declined %u overflow %u\n"
        "  blk entr %u\n",
        (unsigned)js.interp_fallbacks, (unsigned)js.declined,
        (unsigned)js.overflowed, (unsigned)js.block_entries);
    return true;
}

/*
 * The RV32 backend's own counters, which have no framework equivalent:
 * which helper calls it emitted, whether the passthrough window was
 * armed, and how often a block reads the registers a per-block cache
 * would hold. Empty for any other frontend, so a caller needs no #if.
 */
void emu_print_backend_stats(void)
{
#if EMU_GUEST_ARCH_RV32 && EMU_HAVE_JIT
    if (rv_backend != &rv_backend_jit) {
        return;
    }

    rv_jit_stats_t rs;

    rv_jit_get_stats(&rs);
    emu_console_printf("  elided   ld %u  st %u\n"
                       "  helpers  muldiv %u  clmul %u  bit %u\n"
                       "  pt hits  %u armed %u\n",
                       (unsigned)rs.ld_elided, (unsigned)rs.st_elided,
                       (unsigned)rs.alu_calls_muldiv,
                       (unsigned)rs.alu_calls_clmul,
                       (unsigned)rs.alu_calls_bit,
                       (unsigned)rs.pt_hits, (unsigned)rs.pt_armed);
#ifdef EMU_JIT_PROFILE
    emu_console_printf("  cyc xlat %u compact %u\n",
                       (unsigned)rs.cyc_translate, (unsigned)rs.cyc_compact);
#endif

    /*
     * Reads per block that uses the register, x100. Below 100 a cache
     * cannot pay: the block would spend a load to save fewer than one.
     * This is the measurement that says the r8-r10 register cache was a
     * 15.5% regression rather than the win a frequency count predicted.
     */
    static const char *const nm[4] = { "sp", "ra", "a0", "a1" };

    emu_console_printf("  reads/blk");
    for (unsigned i = 0; i < 4u; i++) {
        if (rs.hot_blocks[i] != 0u) {
            const uint32_t x100 = rs.hot_reads[i] * 100u / rs.hot_blocks[i];

            emu_console_printf(" %s=%u.%02u in %u", nm[i],
                               (unsigned)(x100 / 100u),
                               (unsigned)(x100 % 100u),
                               (unsigned)rs.hot_blocks[i]);
        } else {
            emu_console_printf(" %s=- in %u", nm[i],
                               (unsigned)rs.hot_blocks[i]);
        }
    }
    emu_console_printf("\n");
#endif
}
