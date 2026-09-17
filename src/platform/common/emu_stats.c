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

void emu_print_run_summary(uint64_t retired, uint32_t host_cycles)
{
    {
        char n[21];

        emu_console_printf("\n-- done --\n  retired  %s instructions\n",
                           emu_u64_str(n, (unsigned long long)retired));
    }

    /*
     * A host with no cycle counter worth quoting passes 0, and gets
     * neither line rather than "0 cycles" and a ratio of 0.00 -- a
     * number that looks measured and is not.
     */
    if (host_cycles == 0u || retired == 0u) {
        return;
    }

    emu_console_printf("  host     %u cycles\n", (unsigned)host_cycles);

    /*
     * Host cycles per emulated guest instruction, x100 so the fractional
     * part survives integer division -- picolibc is built without float
     * formatting, which is right on a part whose FPU is single precision
     * and whose guest owns it.
     */
    const uint32_t x100 = (uint32_t)((uint64_t)host_cycles * 100u / retired);

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
    /*
     * Nothing translated and no block entered: either there is no JIT
     * here or it was never asked to do anything, and a block of zeros
     * says neither. code_size alone is not the test -- a backend
     * allocates its buffer at init, so an interpreter run reports a
     * cache size and nothing in it.
     */
    if (js.translations == 0u && js.block_entries == 0u) {
        return false;
    }

    emu_console_printf("\n-- jit --\n"
                       "  blocks   %u\n"
                       "  code     %u/%u bytes\n"
                       "  blks/xlat %u\n"
                       "  compact  %u (%u evicted)\n"
                       "  flushes  %u\n",
                       (unsigned)js.blocks, (unsigned)js.code_used,
                       (unsigned)js.code_size, (unsigned)js.translations,
                       (unsigned)js.compactions, (unsigned)js.evictions,
                       (unsigned)js.flushes);

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
    /*
     * `links` beside the entries on purpose: the two are the whole
     * story of chaining. An exit patched to jump straight into its
     * successor removes a dispatch, so entries falling while links rise
     * is what the optimisation doing its job looks like -- and a
     * chaining that never fires looks exactly like one that does not
     * pay unless both numbers are in front of you.
     */
    emu_console_printf("  interp   %u instructions fell back\n"
                       "  declined %u overflow %u\n"
                       "  blk entr %u  links %u\n",
                       (unsigned)js.interp_fallbacks, (unsigned)js.declined,
                       (unsigned)js.overflowed, (unsigned)js.block_entries,
                       (unsigned)js.links);

#ifdef EMU_JIT_PROFILE
    /*
     * Host cycles by phase, and the framework's rather than a frontend's:
     * translation reaching 65% of all host cycles is a defect this tree
     * has actually had, and no frontend could have reported it.
     */
    emu_console_printf("  cyc xlat %u compact %u\n", (unsigned)js.cyc_translate,
                       (unsigned)js.cyc_compact);
#endif
    return true;
}
