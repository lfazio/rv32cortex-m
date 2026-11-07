# SPDX-License-Identifier: Apache-2.0
"""g4mh-jit-ccrh.py - mutants of the G4MH translator's lowerings of what
CC-RH's code made it decline: ZXB/SXB/ZXH/SXH, MULH (both forms), MULHI,
SUBR, LOOP, and the 48-bit JR/JARL disp32 and JMP disp32[reg1]
(src/frontend/g4mh/g4mh_ir.c).

    scripts/unit-mutants.py BUILD tests/mutants/g4mh-jit-ccrh.py

Each must fail test_jit_cc_rh_loops_and_extensions (tests/unit/
test_g4mh.c), which runs every case on both backends: 19 of 19 on
2026-11-06, after the test gained a retired count (LOOP's `counted`) and
the saturating forms beside the extensions. Not here, because it is
equivalent: the 48-bit jumps without `counted`, where the caller's
RETIRE lands after an unconditional EXIT and never runs.
"""

F = "src/frontend/g4mh/g4mh_ir.c"
M = [
    ("loop-declined", F, "        if (((uint32_t)w1 & 1u) == 0u) {\n            return false;\n        }\n\n        const uint16_t x = emu_ir_get(b, r1);", "        if (true) {\n            return false;\n        }\n\n        const uint16_t x = emu_ir_get(b, r1);"),
    ("loop-no-flags", F, "        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_ADD, x, m1, 0u, F_ARITH);\n        (void)emu_ir_emit(b, EMU_IR_RETIRE,", "        (void)emu_ir_emit(b, EMU_IR_RETIRE,"),
    ("loop-target-off", F, "emu_ir_const(b, 0u), pc - ((uint32_t)w1 & 0xFFFEu),", "emu_ir_const(b, 0u), pc - ((uint32_t)w1 & 0xFFFCu),"),
    ("loop-cond-eq", F, "        (void)emu_ir_emit(b, EMU_IR_EXIT_IF, EMU_IR_C_NE, v,", "        (void)emu_ir_emit(b, EMU_IR_EXIT_IF, EMU_IR_C_EQ, v,"),
    ("loop-not-counted", F, "        *counted = true;\n        (void)emu_ir_emit(b, EMU_IR_EXIT_IF, EMU_IR_C_NE, v,", "        (void)emu_ir_emit(b, EMU_IR_EXIT_IF, EMU_IR_C_NE, v,"),
    ("ext-table-swapped", F, "static const uint8_t k_ext[4] = {EMU_IR_ZEXT8, EMU_IR_SEXT8,\n                                             EMU_IR_ZEXT16, EMU_IR_SEXT16};", "static const uint8_t k_ext[4] = {EMU_IR_ZEXT8, EMU_IR_SEXT8,\n                                             EMU_IR_SEXT16, EMU_IR_ZEXT16};"),
    ("mulh-reg1-twice", F, "                   emu_ir_alu(b, EMU_IR_MUL, sext16(b, emu_ir_get(b, r2)),\n                              sext16(b, emu_ir_get(b, r1))));", "                   emu_ir_alu(b, EMU_IR_MUL, sext16(b, emu_ir_get(b, r1)),\n                              sext16(b, emu_ir_get(b, r1))));"),
    ("mulh-imm5-unsigned", F, "                              emu_ir_const(b, (uint32_t)g4mh_imm5(w0))));\n        return true;\n\n    case 0x08:", "                              emu_ir_const(b, (uint32_t)(w0 & 0x1Fu))));\n        return true;\n\n    case 0x08:"),
    ("mulhi-imm-unsigned", F, "emu_ir_const(b, (uint32_t)emu_sext(w1, 16))));", "emu_ir_const(b, (uint32_t)w1)));"),
    ("mulhi-reg2-operand", F, "emu_ir_alu(b, EMU_IR_MUL, sext16(b, emu_ir_get(b, r1)),\n                                  emu_ir_const(b,", "emu_ir_alu(b, EMU_IR_MUL, sext16(b, emu_ir_get(b, r2)),\n                                  emu_ir_const(b,"),
    ("sext16-identity", F, "    return emu_ir_emit(b, EMU_IR_SEXT16, 0u, v, EMU_IR_NO_TEMP, 0u, 0u);", "    (void)b;\n    return v;"),
    ("subr-as-sub", F, "        emu_ir_put(b, r2, emu_ir_alu(b, EMU_IR_SUB, x, y));\n        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_SUB, x, y, 0u, F_ARITH);\n        return true;\n    }\n\n    case 0x17:", "        emu_ir_put(b, r2, emu_ir_alu(b, EMU_IR_SUB, y, x));\n        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_SUB, x, y, 0u, F_ARITH);\n        return true;\n    }\n\n    case 0x17:"),
    ("subr-flags-swapped", F, "        emu_ir_put(b, r2, emu_ir_alu(b, EMU_IR_SUB, x, y));\n        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_SUB, x, y, 0u, F_ARITH);\n        return true;\n    }\n\n    case 0x17:", "        emu_ir_put(b, r2, emu_ir_alu(b, EMU_IR_SUB, x, y));\n        (void)emu_ir_emit(b, EMU_IR_SETF, EMU_IR_FS_SUB, y, x, 0u, F_ARITH);\n        return true;\n    }\n\n    case 0x17:"),
    ("sat-forms-lowered-as-ext", F, "        if (op != 0x07u) {\n            return false;\n        }", "        if (op != 0x07u) {\n            emu_ir_put(b, r2, emu_ir_get(b, r1));\n            return true;\n        }"),
    ("jump48-declined", F, "    if (op == 0x17u || (op == 0x37u && ((uint32_t)w1 & 1u) == 0u)) {", "    if (false) {"),
    ("jarl48-link-plus-4", F, "            emu_ir_put(b, r1, emu_ir_const(b, pc + 6u));", "            emu_ir_put(b, r1, emu_ir_const(b, pc + 4u));"),
    ("jr48-target-next", F, "                              EMU_IR_NO_TEMP, pc + d, 0u);", "                              EMU_IR_NO_TEMP, pc + 6u + d, 0u);"),
    ("jmp48-bit0-kept", F, "                emu_ir_const(b, ~1u));\n\n            (void)emu_ir_emit(b, EMU_IR_EXIT, 0u, tgt,", "                emu_ir_const(b, ~0u));\n\n            (void)emu_ir_emit(b, EMU_IR_EXIT, 0u, tgt,"),
    ("jmp48-reg-ignored", F, "        } else if (r1 == 0u) {\n            (void)emu_ir_emit(b, EMU_IR_EXIT, 0u, EMU_IR_NO_TEMP,\n                              EMU_IR_NO_TEMP, d & ~1u, 0u);", "        } else if (true) {\n            (void)emu_ir_emit(b, EMU_IR_EXIT, 0u, EMU_IR_NO_TEMP,\n                              EMU_IR_NO_TEMP, d & ~1u, 0u);"),
]
