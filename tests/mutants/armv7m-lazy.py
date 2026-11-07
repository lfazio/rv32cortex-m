# SPDX-License-Identifier: Apache-2.0
"""armv7m-lazy.py - mutants of the ARMv7-M lazy FP save, the tail-chain
and FPCCR's writable bits, each a rule the F746 answered (sys.c `lazy-*`,
`fpccr-writable`, `fault-pend-disabled`) broken on purpose.

    scripts/unit-mutants.py BUILD tests/mutants/armv7m-lazy.py

Every one must fail test_lazy_board_answers (tests/unit/test_armv7m.c);
19 of 19 did on 2026-11-06, once the tail-chain's LR check stopped
starting from the value the bug leaves.
"""

E = "src/frontend/armv7m/armv7m_exc.c"
F = "src/frontend/armv7m/armv7m_fpu.c"
N = "src/frontend/armv7m/armv7m_nvic.c"
M = [
    ("save-no-mpu", E, "if (mpu && !mpu_permits(c, addr, priv, ARMV7M_ACC_WRITE)) {", "if (false) {"),
    ("save-handler-priv", E, "const bool priv = (c->fpccr & FPCCR_USER) == 0u;\n    const bool hfrdy", "const bool priv = armv7m_privileged(c);\n    const bool hfrdy"),
    ("save-continues-past-refusal", E, "            goto refused;\n", "            continue;\n"),
    ("save-17-words-16", E, "for (uint32_t i = 0u; i < 17u; i++) {\n        const uint32_t addr = c->fpcar", "for (uint32_t i = 0u; i < 16u; i++) {\n        const uint32_t addr = c->fpcar"),
    ("bus-error-ignored", E, "            bus_error_imprecise(c);\n        } else {\n            armv7m_wrote(c, addr);\n        }\n    }\n    c->fpccr &= ~FPCCR_LSPACT;", "        } else {\n            armv7m_wrote(c, addr);\n        }\n    }\n    c->fpccr &= ~FPCCR_LSPACT;"),
    ("mlsperr-not-set", E, "refused:\n    c->cfsr |= MMFSR_MLSPERR;", "refused:\n    c->cfsr |= 0u;"),
    ("lspact-always-cleared", E, "group(c, exc_prio(c, ARMV7M_EXC_MEMMANAGE)) >= now) {\n        c->fpccr &= ~FPCCR_LSPACT;\n    } else if", "group(c, exc_prio(c, ARMV7M_EXC_MEMMANAGE)) >= now) {\n    } else if"),
    ("lspact-never-cleared", E, "if ((c->shcsr & SHCSR_MEMFAULTENA) == 0u ||\n        group(c, exc_prio(c, ARMV7M_EXC_MEMMANAGE)) >= now) {", "if (false) {"),
    ("escalate-by-enable-now", E, "    } else if ((c->fpccr & FPCCR_MMRDY) != 0u) {\n        return ARMV7M_X_MEMMANAGE;\n    }\n    if ((c->fpccr & FPCCR_MMRDY) != 0u) {", "    } else {\n        return ARMV7M_X_MEMMANAGE;\n    }\n    if (true) {"),
    ("no-forced", E, "    c->hfsr |= HFSR_FORCED;\n    if (now > -1) {", "    if (now > -1) {"),
    ("pend-instead-of-hardfault", E, "    if (now > -1) {\n        return ARMV7M_X_HARDFAULT;\n    }", "    if (false) {\n        return ARMV7M_X_HARDFAULT;\n    }"),
    ("pend-not-set", E, "        armv7m_set_pending(c, ARMV7M_EXC_MEMMANAGE, true);\n        return OK;", "        return OK;"),
    ("lspact-not-cleared-on-success", E, "    }\n    c->fpccr &= ~FPCCR_LSPACT;\n    return OK;\n\nrefused:", "    }\n    return OK;\n\nrefused:"),
    ("raise-hardfault-as-lockup", E, "    case ARMV7M_X_HARDFAULT:\n        exc = ARMV7M_EXC_HARDFAULT;\n        break;\n", ""),
    ("no-tail-chain", E, "if (EMU_UNLIKELY(c->pending_any) && tail_chain(c, excret)) {", "if (false) {"),
    ("tail-chain-lr", E, "    c->r[ARMV7M_LR] = excret;\n    (void)vector_to(c, exc, excret);", "    (void)vector_to(c, exc, excret);"),
    ("disabled-fault-takeable", E, "    const uint32_t off = (~c->shcsr >> 12) & 0x70u;", "    const uint32_t off = 0u;"),
    ("fpccr-readonly-again", N, "c->fpccr = v & 0xC000017Bu;", "c->fpccr = (c->fpccr & ~0xC0000000u) | (v & 0xC0000000u);"),
    ("lockup-no-iaccviol", E, "        c->cfsr |= MMFSR_IACCVIOL;\n        c->hfsr |= HFSR_FORCED;", "        c->hfsr |= HFSR_FORCED;"),
    ("lockup-no-forced", E, "        c->hfsr |= HFSR_FORCED;\n        return ARMV7M_X_LOCKUP;", "        return ARMV7M_X_LOCKUP;"),
    ("lockup-never", E, "    if (now <= -1 && !hfrdy) {", "    if (false) {"),
    ("lockup-ignores-hfrdy", E, "    if (now <= -1 && !hfrdy) {", "    if (now <= -1) {"),
    ("fp-check-ignores-save-fault", F, "        const X x = armv7m_fp_lazy_save(c);\n\n        if (x != OK) {\n            return x;\n        }", "        (void)armv7m_fp_lazy_save(c);"),
]
