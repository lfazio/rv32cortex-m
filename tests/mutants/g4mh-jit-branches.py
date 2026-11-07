# SPDX-License-Identifier: Apache-2.0
"""g4mh-jit-branches.py - mutants of what the G4MH JIT learned on
2026-11-07: BN/BP/BV/BNV through the IR's sign and overflow conditions,
JARL [reg1], reg3, and DI/EI (src/frontend/g4mh/g4mh_ir.c, and the
conditions in both backends and the reference) -- and the dispatcher no
longer counting an interrupt's entry as a retired instruction for a
frontend whose interpreter does not (irq_not_retired).

    scripts/unit-mutants.py BUILD tests/mutants/g4mh-jit-branches.py

Each must fail test_jit_sign_overflow_jarl_ei (tests/unit/test_g4mh.c):
14 of 14 on 2026-11-07, once EI ran twice -- an exit is linked only
after it has been taken, so one pass could not see EI's exit chain.
Not here, being equivalent: EI's block not ended, where the translator
then adds only dead code after an unconditional exit.
The x86-64 backend's are the ones the host can see; Thumb-2's are the
board's to answer.
"""

G = "src/frontend/g4mh/g4mh_ir.c"
X = "src/backend/x86_64/ir_lower.c"
R = "src/backend/common/interp.c"
M = [
    ("bn-declined", G, "            EMU_IR_C_MI, /* 4 BN   negative                       */", "            0xFFu, /* 4 BN */"),
    ("bp-as-mi", G, "            EMU_IR_C_PL, /* C BP   positive                       */", "            EMU_IR_C_MI, /* C BP */"),
    ("bv-as-vc", G, "            EMU_IR_C_VS, /* 0 BV   overflow                       */", "            EMU_IR_C_VC, /* 0 BV */"),
    ("x86-mi-reads-v", X, "    case EMU_IR_C_MI:\n    case EMU_IR_C_PL:\n        flag_bit_to(T0, T1, t, 1); /* S */", "    case EMU_IR_C_MI:\n    case EMU_IR_C_PL:\n        flag_bit_to(T0, T1, t, 2); /* S */"),
    ("x86-pl-not-negated", X, "        c == EMU_IR_C_GT || c == EMU_IR_C_GTU || c == EMU_IR_C_PL ||\n        c == EMU_IR_C_VC) {\n        x86_alu_imm32(X86_X_XOR, T0, 1u);", "        c == EMU_IR_C_GT || c == EMU_IR_C_GTU ||\n        c == EMU_IR_C_VC) {\n        x86_alu_imm32(X86_X_XOR, T0, 1u);"),
    ("jarl-declined", G, "    if (op == 0x3Fu && ((uint32_t)w1 & 0x07FFu) == 0x160u && r2 == 0x18u) {", "    if (false) {"),
    ("jarl-link-plus-2", G, "        emu_ir_put(b, ((uint32_t)w1 >> 11) & 0x1Fu, emu_ir_const(b, pc + 4u));", "        emu_ir_put(b, ((uint32_t)w1 >> 11) & 0x1Fu, emu_ir_const(b, pc + 2u));"),
    ("jarl-link-before-target", G, "        const uint16_t tgt =\n            emu_ir_alu(b, EMU_IR_AND, emu_ir_get(b, r1), emu_ir_const(b, ~1u));\n\n        emu_ir_put(b, ((uint32_t)w1 >> 11) & 0x1Fu, emu_ir_const(b, pc + 4u));", "        emu_ir_put(b, ((uint32_t)w1 >> 11) & 0x1Fu, emu_ir_const(b, pc + 4u));\n        const uint16_t tgt =\n            emu_ir_alu(b, EMU_IR_AND, emu_ir_get(b, r1), emu_ir_const(b, ~1u));"),
    ("jarl-bit0-kept", G, "        const uint16_t tgt =\n            emu_ir_alu(b, EMU_IR_AND, emu_ir_get(b, r1), emu_ir_const(b, ~1u));\n\n        emu_ir_put(b, ((uint32_t)w1 >> 11)", "        const uint16_t tgt =\n            emu_ir_alu(b, EMU_IR_AND, emu_ir_get(b, r1), emu_ir_const(b, ~0u));\n\n        emu_ir_put(b, ((uint32_t)w1 >> 11)"),
    ("ei-declined", G, "        (r2 == 0x00u || r2 == 0x10u) && ((uint32_t)w1 >> 11) == 0u) {", "        (r2 == 0x00u) && ((uint32_t)w1 >> 11) == 0u) {"),
    ("ei-chained", G, "            (void)emu_ir_emit(b, EMU_IR_EXIT, 0u, emu_ir_const(b, pc + 4u),\n                              EMU_IR_NO_TEMP, 0u, 0u);", "            (void)emu_ir_emit(b, EMU_IR_EXIT, 0u, EMU_IR_NO_TEMP,\n                              EMU_IR_NO_TEMP, pc + 4u, 0u);"),
    ("ei-no-irq-dirty", G, "        c->psw &= ~G4MH_PSW_ID;\n        c->irq_dirty = true;\n    } else {\n        c->psw |= G4MH_PSW_ID;\n    }\n    return 0u;", "        c->psw &= ~G4MH_PSW_ID;\n    } else {\n        c->psw |= G4MH_PSW_ID;\n    }\n    return 0u;"),
    ("irq-entry-counted", G, "    .irq_not_retired = true, /* as g4mh_run: an entry retires nothing */", ""),
    ("dispatcher-always-counts", "src/emu/emu_jit.c", "            if (!ops->irq_not_retired) {\n                done++;", "            if (true) {\n                done++;"),
]
