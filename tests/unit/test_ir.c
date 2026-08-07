/* SPDX-License-Identifier: Apache-2.0 */
/*
 * test_ir.c - The optimisation passes between a frontend and a host.
 *
 * These are tested on hand-built IR rather than through a frontend, for
 * the same reason the G4MH tests are hand-assembled: a test that drove
 * the passes through the G4MH lowering would pass whenever the lowering
 * and the pass were wrong in the same direction, and the whole value of
 * a shared optimiser is that it is shared -- a bug here is a bug for
 * every frontend and every host at once.
 *
 * Each test states what the pass must *not* do as well as what it must,
 * because every one of these passes deletes code, and a pass that
 * deletes too much produces a wrong answer some distance away rather
 * than a crash.
 */

#include "tests.h"

#include "emu/emu_ir.h"

#include <string.h>

static emu_ir_block_t g_b;

/* Instructions the backend would actually emit. */
static uint32_t live_count(void)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_b.count; i++) {
        if (!g_b.insn[i].dead) {
            n++;
        }
    }
    return n;
}

static uint32_t count_op(emu_ir_op_t op)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_b.count; i++) {
        if (!g_b.insn[i].dead && g_b.insn[i].op == (uint8_t)op) {
            n++;
        }
    }
    return n;
}

/*
 * Two flag-setting operations in a row with no conditional between them.
 * The first definition is dead: nothing can observe it.
 *
 * This is the case the IR exists for on a flag machine. G4MH sets Z/S/OV/CY
 * on nearly every arithmetic instruction and x86-64 needs `seto`+`lahf`
 * plus masking to materialise them, so deleting the ones nobody reads is
 * the largest single saving available to that frontend.
 */
static void test_dead_flags_removed(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t b = emu_ir_get(&g_b, 2u);

    const uint16_t s1 = emu_ir_alu(&g_b, EMU_IR_ADD, a, b);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_ADD, s1, b, 0u,
                      EMU_IR_F_ALL);              /* dead */

    const uint16_t s2 = emu_ir_alu(&g_b, EMU_IR_SUB, s1, b);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_SUB, s2, b, 0u,
                      EMU_IR_F_ALL);              /* live: block exit */
    emu_ir_put(&g_b, 3u, s2);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.flags_removed, 1u);
    CHECK_EQ(count_op(EMU_IR_SETF), 1u);
    /* The arithmetic itself must survive -- only the flags were dead. */
    CHECK_EQ(count_op(EMU_IR_ADD), 1u);
    CHECK_EQ(count_op(EMU_IR_SUB), 1u);
}

/*
 * The same shape with a conditional in between. Now the first definition
 * is read, and deleting it would take a branch the wrong way.
 *
 * Without this case the pass above could be implemented as "delete every
 * SETF but the last" and still pass.
 */
static void test_flags_kept_when_read(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t b = emu_ir_get(&g_b, 2u);

    const uint16_t s1 = emu_ir_alu(&g_b, EMU_IR_SUB, a, b);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_SUB, s1, b, 0u,
                      EMU_IR_F_ALL);
    const uint16_t c = emu_ir_emit(&g_b, EMU_IR_GETCOND, EMU_IR_C_EQ,
                                   EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 0u, 0u);
    emu_ir_put(&g_b, 4u, c);

    const uint16_t s2 = emu_ir_alu(&g_b, EMU_IR_ADD, a, b);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_ADD, s2, b, 0u,
                      EMU_IR_F_ALL);
    emu_ir_put(&g_b, 3u, s2);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.flags_removed, 0u);
    CHECK_EQ(count_op(EMU_IR_SETF), 2u);
}

/*
 * A flag definition at the end of a block is live even though nothing in
 * the block reads it: the next block, an interrupt handler and the
 * debugger can all see the guest's flag word.
 *
 * Checked by asking for a live-out set of nothing and seeing the count
 * change -- which is what proves the parameter is honoured rather than
 * ignored.
 */
static void test_flags_live_out(void)
{
    emu_ir_reset(&g_b);
    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t s = emu_ir_alu(&g_b, EMU_IR_ADD, a, a);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_ADD, s, a, 0u,
                      EMU_IR_F_ALL);
    emu_ir_put(&g_b, 2u, s);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, EMU_IR_F_ALL, &st);
    CHECK_EQ(st.flags_removed, 0u);

    /* Same block, but the frontend proved no later reader exists. */
    emu_ir_reset(&g_b);
    const uint16_t a2 = emu_ir_get(&g_b, 1u);
    const uint16_t s2 = emu_ir_alu(&g_b, EMU_IR_ADD, a2, a2);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_ADD, s2, a2, 0u,
                      EMU_IR_F_ALL);
    emu_ir_put(&g_b, 2u, s2);

    emu_ir_optimise(&g_b, 0u, &st);
    CHECK_EQ(st.flags_removed, 1u);
}

/*
 * A guest register read straight after it was written comes from the
 * temp, not from memory.
 *
 * This is the measured register-file round trip: a quarter to a third of
 * adjacent instruction pairs in this project's guests are data
 * dependent, and each emits a store immediately followed by a load of
 * the same slot.
 */
static void test_redundant_get_elided(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t b = emu_ir_get(&g_b, 2u);
    const uint16_t s = emu_ir_alu(&g_b, EMU_IR_ADD, a, b);
    emu_ir_put(&g_b, 3u, s);

    /* The next guest instruction reads r3 back. */
    const uint16_t reread = emu_ir_get(&g_b, 3u);
    const uint16_t s2 = emu_ir_alu(&g_b, EMU_IR_XOR, reread, a);
    emu_ir_put(&g_b, 4u, s2);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.gets_removed, 1u);
    /* Two real loads remain: r1 and r2. */
    CHECK_EQ(count_op(EMU_IR_GET), 2u);
    /*
     * And the reload became nothing at all rather than a MOV -- the
     * value pass sweeps it up once the operand is taken directly.
     */
    CHECK_EQ(count_op(EMU_IR_MOV), 0u);
}

/*
 * A guest register written twice with nothing observable in between
 * needs only the second store...
 */
static void test_dead_put_removed(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t x = emu_ir_alu(&g_b, EMU_IR_ADD, a, a);
    emu_ir_put(&g_b, 5u, x);          /* dead: overwritten below */
    const uint16_t y = emu_ir_alu(&g_b, EMU_IR_XOR, a, a);
    emu_ir_put(&g_b, 5u, y);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.puts_removed, 1u);
    CHECK_EQ(count_op(EMU_IR_PUT), 1u);
}

/*
 * ...and neither may a *faulting* memory operation be stepped over.
 *
 * A store to a register followed by a second store to the same register
 * looks like a dead first store -- unless what sits between them can
 * trap, because then the second never runs and the first is the value
 * the trap handler reads.
 *
 * riscv-tests' rv32mi/ma_addr is exactly this shape:
 *
 *     addi t1, s0, 1     ; t1 = the address about to fault
 *     lh   t1, 1(s0)     ; traps; never writes t1
 *
 * and its handler compares mtval against t1. Deleting the first store
 * left t1 stale and failed one sub-test, while the other 76 riscv-tests,
 * the architecture suite, isatest and every unit test here passed.
 */
static void test_put_kept_across_faulting_load(void)
{
    emu_ir_reset(&g_b);

    const uint16_t base = emu_ir_get(&g_b, 1u);
    const uint16_t addr = emu_ir_alu(&g_b, EMU_IR_ADD, base,
                                     emu_ir_const(&g_b, 1u));
    emu_ir_put(&g_b, 6u, addr);                  /* must survive */

    /* The load writes the same guest register, and can fault. */
    emu_ir_put(&g_b, 6u, emu_ir_emit(&g_b, EMU_IR_LOAD,
                                     EMU_IR_MEM_AUX(2u, 1u), base,
                                     EMU_IR_NO_TEMP, 1u, 0u));

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.puts_removed, 0u);
    CHECK_EQ(count_op(EMU_IR_PUT), 2u);
}

/*
 * ...but a helper call in between can read the register file, so the
 * first store has to stay.
 *
 * This is the case that separates a correct pass from one that just
 * counts stores per register, and it is the direction that produces a
 * wrong answer rather than slow code.
 */
static void test_put_kept_across_helper(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t x = emu_ir_alu(&g_b, EMU_IR_ADD, a, a);
    emu_ir_put(&g_b, 5u, x);
    (void)emu_ir_emit(&g_b, EMU_IR_HELPER, 0u, a, EMU_IR_NO_TEMP, 0u, 0u);
    const uint16_t y = emu_ir_alu(&g_b, EMU_IR_XOR, a, a);
    emu_ir_put(&g_b, 5u, y);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.puts_removed, 0u);
    CHECK_EQ(count_op(EMU_IR_PUT), 2u);
}

/*
 * A helper may also write the register file, so a read after one cannot
 * be served from a temp captured before it.
 */
static void test_get_not_elided_across_helper(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    emu_ir_put(&g_b, 3u, a);
    (void)emu_ir_emit(&g_b, EMU_IR_HELPER, 0u, a, EMU_IR_NO_TEMP, 0u, 0u);
    const uint16_t reread = emu_ir_get(&g_b, 3u);
    emu_ir_put(&g_b, 4u, reread);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.gets_removed, 0u);
    CHECK_EQ(count_op(EMU_IR_GET), 2u);
}

/*
 * A computed value nothing consumes goes, but one consumed only by a
 * store stays. The first is what makes the other passes pay; the second
 * is the way a dead-value pass most easily becomes a wrong-answer bug.
 */
static void test_dead_values(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    (void)emu_ir_alu(&g_b, EMU_IR_ADD, a, a);     /* nothing reads it */
    const uint16_t keep = emu_ir_alu(&g_b, EMU_IR_XOR, a, a);
    emu_ir_put(&g_b, 2u, keep);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.dead_removed, 1u);
    CHECK_EQ(count_op(EMU_IR_ADD), 0u);
    CHECK_EQ(count_op(EMU_IR_XOR), 1u);
    CHECK_EQ(count_op(EMU_IR_PUT), 1u);
}

/*
 * A frontend with no condition flags at all -- RISC-V -- must pay
 * nothing. No SETF is ever emitted, so there is nothing for the flag
 * pass to find, and the register passes still apply.
 */
static void test_flagless_frontend(void)
{
    emu_ir_reset(&g_b);

    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t b = emu_ir_get(&g_b, 2u);
    const uint16_t s = emu_ir_alu(&g_b, EMU_IR_ADD, a, b);
    emu_ir_put(&g_b, 3u, s);
    const uint16_t reread = emu_ir_get(&g_b, 3u);
    emu_ir_put(&g_b, 4u, reread);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, EMU_IR_F_ALL, &st);

    CHECK_EQ(st.flags_removed, 0u);
    CHECK_EQ(st.gets_removed, 1u);
    CHECK(live_count() < g_b.count);
}

/*
 * Overflowing the block must be reported and must leave the passes
 * alone: a partially built block is not a correct one, and optimising it
 * would produce something that looked runnable.
 */
static void test_overflow_not_optimised(void)
{
    emu_ir_reset(&g_b);
    for (uint32_t i = 0; i < EMU_IR_MAX_INSNS + 4u; i++) {
        (void)emu_ir_const(&g_b, i);
    }
    CHECK(g_b.overflow);

    emu_ir_opt_stats_t st;
    emu_ir_optimise(&g_b, EMU_IR_F_ALL, &st);
    CHECK_EQ(st.dead_removed, 0u);
}

/* ------------------------------------------------------------------ */
/* Lowering, executed                                                  */
/* ------------------------------------------------------------------ */

#if defined(EMU_JIT_X86_64)

#include "emu/emu_x86_64.h"

#include <stddef.h>
#include <sys/mman.h>

/*
 * A stand-in guest, laid out the way a real frontend's state is: a
 * register file, a flag word and a pc, reached by byte offset.
 *
 * The point of these tests is that the emitted bytes are *run*. A
 * lowering can be read and reviewed and still encode the wrong ModRM
 * byte -- and a wrong ModRM does not fail to assemble, it addresses
 * something else. Nothing short of executing it says otherwise.
 */
typedef struct {
    uint32_t r[16];
    uint32_t flags;
    uint32_t pc;
} fake_cpu_t;

#define FAKE_F_Z (1u << 0)
#define FAKE_F_S (1u << 1)
#define FAKE_F_V (1u << 2)
#define FAKE_F_C (1u << 3)

static uint32_t fake_reg_offset(uint32_t n)
{
    return (uint32_t)offsetof(fake_cpu_t, r) + n * 4u;
}

static bool fake_reg_is_zero(uint32_t n) { return n == 0u; }

/*
 * A tiny guest memory, so the memory operations can be lowered and run
 * rather than declined. Deliberately not the real bus: what is under
 * test is the lowering's call sequence and its trap path, not any
 * frontend's address map.
 */
static uint8_t g_mem[256];

static uint32_t fake_load(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                          uint32_t *out)
{
    (void)cpu;
    const uint32_t size = EMU_IR_MEM_SIZE(spec);

    if (addr + size > sizeof(g_mem)) {
        return 1u;                       /* "trapped" */
    }
    uint32_t v = 0u;
    for (uint32_t i = 0; i < size; i++) {
        v |= (uint32_t)g_mem[addr + i] << (8u * i);
    }
    if ((spec & EMU_IR_MEM_SIGNED) != 0u && size < 4u) {
        const uint32_t sign = 1u << (size * 8u - 1u);
        if ((v & sign) != 0u) {
            v |= ~((sign << 1u) - 1u);
        }
    }
    *out = v;
    return 0u;
}

static uint32_t fake_store(emu_cpu_t *cpu, uint32_t addr, uint32_t spec,
                           uint32_t val)
{
    (void)cpu;
    const uint32_t size = EMU_IR_MEM_SIZE(spec);

    if (addr + size > sizeof(g_mem)) {
        return 1u;
    }
    for (uint32_t i = 0; i < size; i++) {
        g_mem[addr + i] = (uint8_t)(val >> (8u * i));
    }
    return 0u;
}

static const emu_ir_target_t g_fake_target = {
    .reg_offset   = fake_reg_offset,
    .flags_offset = (uint32_t)offsetof(fake_cpu_t, flags),
    .flag_bit     = { FAKE_F_Z, FAKE_F_S, FAKE_F_V, FAKE_F_C },
    .reg_is_zero  = fake_reg_is_zero,
    .pc_offset    = (uint32_t)offsetof(fake_cpu_t, pc),
    .helpers      = NULL,
    .helper_count = 0u,
    .load         = fake_load,
    .store        = fake_store,
};

#define IR_TEST_CODE_BYTES 8192u

/*
 * Lower the current block into an executable mapping and call it.
 *
 * Its own mmap rather than the framework's buffer: that one belongs to
 * the dispatch loop, and this test has no block cache behind it.
 */
static bool lower_and_run(fake_cpu_t *cpu)
{
    static uint8_t *exec;

    if (exec == NULL) {
        void *const p = mmap(NULL, IR_TEST_CODE_BYTES,
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) {
            return false;
        }
        exec = (uint8_t *)p;
    }

    emu_ir_optimise(&g_b, EMU_IR_F_ALL, NULL);

    /*
     * No prologue or epilogue here: emu_ir_lower emits the whole block
     * including its frame, which is what lets emu_ir_jit.c name no host.
     * Emitting them here as well pushed twice and popped once.
     */
    emu_jit_emit_begin(exec, IR_TEST_CODE_BYTES);
    const bool ok = emu_ir_lower(&g_b, &g_fake_target);
    if (!ok || emu_jit_overflowed()) {
        return false;
    }

    ((uint32_t (*)(void *))(void *)exec)(cpu);
    return true;
}

/*
 * The register file round trip, executed: read two guest registers, add
 * them, write a third.
 */
static void test_lower_add(void)
{
    emu_ir_reset(&g_b);
    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t b = emu_ir_get(&g_b, 2u);
    const uint16_t s = emu_ir_alu(&g_b, EMU_IR_ADD, a, b);
    emu_ir_put(&g_b, 3u, s);

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 40u;
    cpu.r[2] = 2u;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.r[3], 42u);
}

/* Register 0 reads as zero and discards writes, as both guests define. */
static void test_lower_zero_register(void)
{
    emu_ir_reset(&g_b);
    const uint16_t z = emu_ir_get(&g_b, 0u);
    const uint16_t c = emu_ir_const(&g_b, 0x1234u);
    const uint16_t s = emu_ir_alu(&g_b, EMU_IR_OR, z, c);
    emu_ir_put(&g_b, 4u, s);
    emu_ir_put(&g_b, 0u, c);          /* discarded */

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[0] = 0xDEADBEEFu;           /* must not be read */

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.r[4], 0x1234u);
    CHECK_EQ(cpu.r[0], 0xDEADBEEFu);  /* the write went nowhere */
}

/*
 * The bit and byte group, which is the reason these are IR operations
 * rather than shift sequences. Each lowers to one host instruction, and
 * each is checked on the input that separates it from its neighbours.
 */
static void test_lower_bit_ops(void)
{
    emu_ir_reset(&g_b);
    const uint16_t v = emu_ir_get(&g_b, 1u);
    emu_ir_put(&g_b, 2u, emu_ir_emit(&g_b, EMU_IR_BSWAP32, 0u, v,
                                     EMU_IR_NO_TEMP, 0u, 0u));
    emu_ir_put(&g_b, 3u, emu_ir_emit(&g_b, EMU_IR_HSWAP, 0u, v,
                                     EMU_IR_NO_TEMP, 0u, 0u));
    emu_ir_put(&g_b, 4u, emu_ir_emit(&g_b, EMU_IR_BSWAP16, 0u, v,
                                     EMU_IR_NO_TEMP, 0u, 0u));

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0x11223344u;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.r[2], 0x44332211u);   /* BSWAP32 */
    CHECK_EQ(cpu.r[3], 0x33441122u);   /* HSWAP   */
    CHECK_EQ(cpu.r[4], 0x22114433u);   /* BSWAP16 */
}

/*
 * CLZ and CTZ, on zero as well as on a normal input. Zero is the case a
 * lowering built on bsr/bsf gets wrong -- those leave the destination
 * untouched -- and it is exactly the input a bit search is most often
 * handed.
 */
static void test_lower_bit_counts(void)
{
    emu_ir_reset(&g_b);
    const uint16_t v = emu_ir_get(&g_b, 1u);
    const uint16_t z = emu_ir_get(&g_b, 2u);
    emu_ir_put(&g_b, 3u, emu_ir_emit(&g_b, EMU_IR_CLZ, 0u, v,
                                     EMU_IR_NO_TEMP, 0u, 0u));
    emu_ir_put(&g_b, 4u, emu_ir_emit(&g_b, EMU_IR_CTZ, 0u, v,
                                     EMU_IR_NO_TEMP, 0u, 0u));
    emu_ir_put(&g_b, 5u, emu_ir_emit(&g_b, EMU_IR_CLZ, 0u, z,
                                     EMU_IR_NO_TEMP, 0u, 0u));

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0x00100000u;   /* bit 20 */
    cpu.r[2] = 0u;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.r[3], 11u);
    CHECK_EQ(cpu.r[4], 20u);
    CHECK_EQ(cpu.r[5], 32u);   /* defined for zero */
}

/*
 * Flags, and the thing the whole IR was built for: a SETF whose
 * definition is dead emits nothing at all, while the live one writes the
 * guest's flag word without disturbing the bits around it.
 */
static void test_lower_flags(void)
{
    emu_ir_reset(&g_b);
    const uint16_t a = emu_ir_get(&g_b, 1u);
    const uint16_t b = emu_ir_get(&g_b, 2u);

    const uint16_t s1 = emu_ir_alu(&g_b, EMU_IR_ADD, a, b);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_LOGIC, s1,
                      EMU_IR_NO_TEMP, 0u, EMU_IR_F_ALL);   /* dead */
    const uint16_t s2 = emu_ir_alu(&g_b, EMU_IR_SUB, a, a);
    (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_LOGIC, s2,
                      EMU_IR_NO_TEMP, 0u, EMU_IR_F_ALL);   /* live */
    emu_ir_put(&g_b, 3u, s2);

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 7u;
    cpu.r[2] = 5u;
    /* A bit outside the four the IR knows about, which must survive. */
    cpu.flags = 0x80000000u;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.r[3], 0u);
    CHECK((cpu.flags & FAKE_F_Z) != 0u);        /* 7 - 7 == 0 */
    CHECK((cpu.flags & FAKE_F_S) == 0u);
    CHECK_EQ(cpu.flags & 0x80000000u, 0x80000000u);
}

#endif /* EMU_JIT_X86_64 */

/*
 * The backend's two execution strategies, run against each other.
 *
 * emu_ir_interp evaluates the IR in C; emu_ir_lower emits x86-64 and the
 * result is called. They share nothing but emu_ir.h, so a disagreement
 * is a real bug in one of them rather than a stale expectation -- the
 * same reason this project runs SoftFloat against the VFP path.
 *
 * This is the check that would have caught lzcnt-decodes-as-bsr without
 * anyone having worked out by hand what CLZ(0x00100000) should be.
 */
static void diff_one(uint32_t seed_r1, uint32_t seed_r2)
{
    fake_cpu_t a, b;

    memset(&a, 0, sizeof(a));
    a.r[1] = seed_r1;
    a.r[2] = seed_r2;
    a.flags = 0x80000000u;
    b = a;

    /* The block is optimised in place, so interpret first. */
    emu_ir_optimise(&g_b, EMU_IR_F_ALL, NULL);
    if (!emu_ir_interp(&g_b, (emu_cpu_t *)&a, &g_fake_target)) {
        CHECK(false);
        return;
    }
    if (!lower_and_run(&b)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(memcmp(&a, &b, sizeof(a)), 0);
}

static void test_interp_matches_jit(void)
{
    static const uint32_t k_seeds[][2] = {
        { 0x00000000u, 0x00000000u },
        { 0x00000001u, 0xFFFFFFFFu },
        { 0x80000000u, 0x80000000u },
        { 0x11223344u, 0x0000000Fu },
        { 0x00100000u, 0x00000001u },
    };

    for (unsigned i = 0; i < sizeof(k_seeds) / sizeof(k_seeds[0]); i++) {
        /* A block touching the value ops, the bit group and the flags. */
        emu_ir_reset(&g_b);
        const uint16_t r1 = emu_ir_get(&g_b, 1u);
        const uint16_t r2 = emu_ir_get(&g_b, 2u);

        const uint16_t sum = emu_ir_alu(&g_b, EMU_IR_ADD, r1, r2);
        (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_ADD, sum, r2, 0u,
                          EMU_IR_F_ALL);
        emu_ir_put(&g_b, 3u, sum);

        emu_ir_put(&g_b, 4u, emu_ir_emit(&g_b, EMU_IR_BSWAP32, 0u, r1,
                                         EMU_IR_NO_TEMP, 0u, 0u));
        emu_ir_put(&g_b, 5u, emu_ir_emit(&g_b, EMU_IR_HSWAP, 0u, r1,
                                         EMU_IR_NO_TEMP, 0u, 0u));
        emu_ir_put(&g_b, 6u, emu_ir_emit(&g_b, EMU_IR_BSWAP16, 0u, r1,
                                         EMU_IR_NO_TEMP, 0u, 0u));
        emu_ir_put(&g_b, 7u, emu_ir_emit(&g_b, EMU_IR_CLZ, 0u, r1,
                                         EMU_IR_NO_TEMP, 0u, 0u));
        emu_ir_put(&g_b, 8u, emu_ir_emit(&g_b, EMU_IR_CTZ, 0u, r1,
                                         EMU_IR_NO_TEMP, 0u, 0u));

        const uint16_t d = emu_ir_alu(&g_b, EMU_IR_SUB, r1, r2);
        (void)emu_ir_emit(&g_b, EMU_IR_SETF, EMU_IR_FS_LOGIC, d,
                          EMU_IR_NO_TEMP, 0u, EMU_IR_F_ALL);
        emu_ir_put(&g_b, 9u, d);

        emu_ir_put(&g_b, 10u, emu_ir_alu(&g_b, EMU_IR_SHL, r1, r2));
        emu_ir_put(&g_b, 11u, emu_ir_alu(&g_b, EMU_IR_SAR, r1, r2));
        emu_ir_put(&g_b, 12u, emu_ir_emit(&g_b, EMU_IR_BEXT, 0u, r1, r2,
                                          0u, 0u));
        emu_ir_put(&g_b, 13u, emu_ir_emit(&g_b, EMU_IR_BINV, 0u, r1, r2,
                                          0u, 0u));

        diff_one(k_seeds[i][0], k_seeds[i][1]);
    }
}

/*
 * Loads, stores and the memory bit ops, lowered and executed, and then
 * run again through the IR interpreter for comparison.
 *
 * These were the four the lowering used to decline, and declining is
 * exactly the failure that hides: a frontend emitting them silently
 * loses its whole block to the interpreter while every suite stays
 * green.
 */
static void test_lower_memory(void)
{
    memset(g_mem, 0, sizeof(g_mem));
    g_mem[0x40] = 0x12u;

    emu_ir_reset(&g_b);
    const uint16_t base = emu_ir_get(&g_b, 1u);
    /* r2 = load.b [r1 + 0x40], sign-extended */
    emu_ir_put(&g_b, 2u, emu_ir_emit(&g_b, EMU_IR_LOAD,
                                     EMU_IR_MEM_AUX(1u, 1u), base,
                                     EMU_IR_NO_TEMP, 0x40u, 0u));
    /* store.w r3 -> [r1 + 0x10] */
    (void)emu_ir_emit(&g_b, EMU_IR_STORE, EMU_IR_MEM_AUX(4u, 0u), base,
                      emu_ir_get(&g_b, 3u), 0x10u, 0u);

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0u;
    cpu.r[3] = 0xDEADBEEFu;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.r[2], 0x12u);
    CHECK_EQ((uint32_t)g_mem[0x10], 0xEFu);
    CHECK_EQ((uint32_t)g_mem[0x13], 0xDEu);
}

/*
 * A load whose address the target refuses. The block must stop there --
 * the instructions after it belong to whatever the trap preempted, and
 * running them is not a wrong value but a wrong program.
 */
static void test_lower_memory_trap(void)
{
    memset(g_mem, 0, sizeof(g_mem));

    emu_ir_reset(&g_b);
    const uint16_t base = emu_ir_get(&g_b, 1u);
    emu_ir_put(&g_b, 2u, emu_ir_emit(&g_b, EMU_IR_LOAD,
                                     EMU_IR_MEM_AUX(4u, 0u), base,
                                     EMU_IR_NO_TEMP, 0u, 0u));
    /* Must not run. */
    emu_ir_put(&g_b, 4u, emu_ir_const(&g_b, 0xABCDu));

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0x1000u;                 /* out of range -> refused */

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ(cpu.r[4], 0u);
}

/*
 * The bit ops on memory, both encodings' worth of behaviour: Z reports
 * the bit before the change, TST does not write back, and the bits of
 * the flag word outside Z are left alone.
 */
static void test_lower_bitop_memory(void)
{
    memset(g_mem, 0, sizeof(g_mem));
    g_mem[0x20] = 0x01u;

    emu_ir_reset(&g_b);
    const uint16_t base = emu_ir_get(&g_b, 1u);
    const uint16_t bit1 = emu_ir_const(&g_b, 1u);

    emu_ir_bitop(&g_b, EMU_IR_BITOP_SET, base, bit1, 0x20u, 0u);
    emu_ir_put(&g_b, 5u, emu_ir_emit(&g_b, EMU_IR_GETCOND, EMU_IR_C_EQ,
                                     EMU_IR_NO_TEMP, EMU_IR_NO_TEMP, 0u, 0u));

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0u;
    cpu.flags = 0x80000000u | FAKE_F_C;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    /* bit 1 was 0 -> set, and Z reports that it had been clear. */
    CHECK_EQ((uint32_t)g_mem[0x20], 0x03u);
    CHECK_EQ(cpu.r[5], 1u);
    /* Only Z moved. */
    CHECK_EQ(cpu.flags & FAKE_F_C, FAKE_F_C);
    CHECK_EQ(cpu.flags & 0x80000000u, 0x80000000u);
}

/* TST must not write, checked on a clear bit so a stray write shows. */
static void test_lower_bitop_tst(void)
{
    memset(g_mem, 0, sizeof(g_mem));
    g_mem[0x20] = 0x80u;

    emu_ir_reset(&g_b);
    const uint16_t base = emu_ir_get(&g_b, 1u);

    emu_ir_bitop(&g_b, EMU_IR_BITOP_TST, base, emu_ir_const(&g_b, 0u),
                 0x20u, 0u);

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0u;

    if (!lower_and_run(&cpu)) {
        CHECK(false);
        return;
    }
    CHECK_EQ((uint32_t)g_mem[0x20], 0x80u);
    CHECK((cpu.flags & FAKE_F_Z) != 0u);   /* bit 0 was clear */
}

/*
 * The extended registers, executed.
 *
 * REX support is otherwise dead code: no lowering uses r8-r15 yet, so
 * every suite passes whether the prefix is right or absent. A wrong or
 * missing REX does not fault -- it names a *different* register, which
 * is the quietest possible failure.
 *
 * r8-r11 are caller-saved in System V, so this needs no save/restore
 * beyond rbx, which carries the cpu pointer.
 */
static void test_encode_rex(void)
{
    static uint8_t *exec;

    if (exec == NULL) {
        void *const m = mmap(NULL, IR_TEST_CODE_BYTES,
                             PROT_READ | PROT_WRITE | PROT_EXEC,
                             MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) {
            CHECK(false);
            return;
        }
        exec = (uint8_t *)m;
    }

    emu_jit_emit_begin(exec, IR_TEST_CODE_BYTES);
    emu_jit_emit8(0x53);                          /* push rbx      */
    emu_jit_emit8(0x48); emu_jit_emit8(0x89);
    emu_jit_emit8(0xFB);                          /* mov rbx, rdi  */

    x86_mov_imm32(X86_R8, 0x00001000u);
    x86_mov_imm32(X86_R9, 0x00000234u);
    x86_alu_rr(X86_ADD, X86_R8, X86_R9);          /* r8 = 0x1234   */
    x86_st_cpu(X86_R8, (uint32_t)offsetof(fake_cpu_t, r) + 3u * 4u);

    x86_ld_cpu(X86_R10, (uint32_t)offsetof(fake_cpu_t, r) + 1u * 4u);
    x86_shift_imm(X86_R10, X86_SHL, 4u);
    x86_mov_rr(X86_R11, X86_R10);
    x86_st_cpu(X86_R11, (uint32_t)offsetof(fake_cpu_t, r) + 4u * 4u);

    emu_jit_emit8(0x5B);                          /* pop rbx       */
    emu_jit_emit8(0xC3);                          /* ret           */
    if (emu_jit_overflowed()) {
        CHECK(false);
        return;
    }

    fake_cpu_t cpu;
    memset(&cpu, 0, sizeof(cpu));
    cpu.r[1] = 0x0000000Fu;
    ((void (*)(void *))(void *)exec)(&cpu);

    CHECK_EQ(cpu.r[3], 0x00001234u);   /* imm, add, store via r8/r9 */
    CHECK_EQ(cpu.r[4], 0x000000F0u);   /* load, shift, mov via r10/r11 */
}

/* ------------------------------------------------------------------ */

void test_ir(void)
{
    test_dead_flags_removed();
    test_flags_kept_when_read();
    test_flags_live_out();
    test_redundant_get_elided();
    test_dead_put_removed();
    test_put_kept_across_helper();
    test_put_kept_across_faulting_load();
    test_get_not_elided_across_helper();
    test_dead_values();
    test_flagless_frontend();
    test_overflow_not_optimised();
#if defined(EMU_JIT_X86_64)
    test_lower_add();
    test_lower_zero_register();
    test_lower_bit_ops();
    test_lower_bit_counts();
    test_lower_flags();
    test_interp_matches_jit();
    test_encode_rex();
    test_lower_memory();
    test_lower_memory_trap();
    test_lower_bitop_memory();
    test_lower_bitop_tst();
#endif
}
