/* SPDX-License-Identifier: Apache-2.0 */
/*
 * main.c - RV32 emulator firmware for the Nucleo-F746ZG.
 *
 * Bring-up is done entirely with ST's own driver pack: the CMSIS device
 * headers for register definitions, ST's startup file and
 * system_stm32f7xx.c, and the STM32Cube HAL for the clock tree, GPIO and
 * USART. Nothing here reimplements a peripheral the vendor already
 * supports.
 *
 * What this file adds is the glue: a guest address space built out of ARM
 * memory and ARM peripherals, and a run loop that hands control back often
 * enough for the ARM side to keep servicing its own interrupts.
 */

#include "stm32f7xx_hal.h"
#include "board.h"

#include "emu/emu_cpu.h"
#include "emu/emu_dev.h"
#include "emu/emu_memmap.h"

#if EMU_FRONTEND_RV32
#  include "rv32/rv_backend.h"  /* which backend came up */
#  include "rv32/rv_jit.h"      /* JIT statistics, reported below */
#endif

#if EMU_NET
#  include "emu_net.h"
#endif

#include <string.h>

/* The guest binary, embedded by guest_image.S. */
extern const uint8_t rv_guest_image[];
extern const uint32_t rv_guest_image_size;
extern const uint32_t rv_guest_ro_size;

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

/*
 * Guest RAM is not a fixed-size array: the link script places it between
 * the firmware's .bss and the stack and gives it everything in between, so
 * the guest automatically receives all the SRAM the ARM side is not using
 * (see .guest_ram in stm32f746retx.ld).
 */
extern uint8_t __guest_ram_start[];
extern uint8_t __guest_ram_end[];

#define GUEST_RAM_BASE_PTR  (__guest_ram_start)
#define GUEST_RAM_SIZE      ((uint32_t)(__guest_ram_end - __guest_ram_start))

/* Guest time runs at 1 MHz, derived from the DWT cycle counter. */
#define EMU_TIMER_HZ        1000000u

/* Instructions executed between returns to the ARM side. */
/*
 * A cap on how long a guest may run before the firmware gives up.
 *
 * The host runner has --max-insn and two of the Berkeley tests depend on
 * it to terminate at all -- they are *meant* to run away, and the cap is
 * what turns that into a reported failure. The board had no equivalent,
 * so the same guest hangs it: no output, no prompt, and the only way out
 * is a reset, which is indistinguishable from a firmware crash. That is
 * the difference between a suite that reports 273 passed and 1 failed
 * and one that stops after test 47 and needs a human.
 *
 * Zero disables it, which is what an interactive session or a benchmark
 * wants. The default is generous: CoreMark retires about 1.3 million
 * instructions a run and the architecture tests far fewer, so anything
 * reaching this is not making progress.
 */
#ifndef EMU_MAX_INSN
#define EMU_MAX_INSN         100000000u
#endif

#ifndef EMU_RUN_SLICE
#define EMU_RUN_SLICE        4096u
#endif

/* ------------------------------------------------------------------ */
/* State                                                               */
/* ------------------------------------------------------------------ */

#if RV_ENABLE_JIT
/*
 * Code cache for translated blocks. Ordinary .bss: the ARMv7-M default
 * memory map makes SRAM executable, so no MPU work is needed. This comes
 * out of what the guest would otherwise get, which is the trade the JIT
 * asks for.
 */
#endif

static emu_bus_t  g_bus;
static emu_core_t g_core;
static emu_uart_t g_uart;

/* ------------------------------------------------------------------ */
/* Console                                                             */
/* ------------------------------------------------------------------ */

/* Nothing this early can be reported; halt so a debugger sees where. */
static void fatal_halt(void)
{
    __disable_irq();
    for (;;) {
    }
}

/*
 * One console, two possible sinks. Before emu_net_init() succeeds it is
 * the UART; after, the UART carries SLIP and cannot carry text as well,
 * so everything goes to the telnet buffer instead.
 *
 * The branch is a load and a test per character, which is nothing: the
 * console is written by human-readable output and by the guest's virtual
 * UART, neither of which is on any measured hot path. The same is not
 * true of the run loop, which is why emu_net_poll() below is the thing
 * that had to be thought about.
 */
void rv_console_putc(uint8_t c)
{
#if EMU_NET
    if (emu_net_active()) {
        emu_net_console_putc(c);
        return;
    }
#endif
    board_console_putc(c);
}

static int console_getc(void)
{
#if EMU_NET
    if (emu_net_active()) {
        return emu_net_console_getc();
    }
#endif
    return board_console_getc();
}

#define console_putc rv_console_putc

static void console_puts(const char *s)
{
    while (*s != '\0') {
        if (*s == '\n') {
            console_putc('\r');   /* terminals expect CRLF */
        }
        console_putc((uint8_t)*s++);
    }
}

static void console_puthex(uint32_t v)
{
    static const char hex[] = "0123456789abcdef";
    console_puts("0x");
    for (int i = 28; i >= 0; i -= 4) {
        console_putc((uint8_t)hex[(v >> i) & 0xFu]);
    }
}

static void console_putu(uint32_t v)
{
    char tmp[10];
    unsigned n = 0;
    do {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v != 0u);
    while (n != 0u) {
        console_putc((uint8_t)tmp[--n]);
    }
}

#ifdef EMU_JIT_DIFF
/*
 * A block whose compiled code disagreed with the IR interpreter.
 *
 * `off` is a byte offset into the guest state, so the register file
 * starts at zero and the number is the register times four. Only the
 * first few are printed: everything after the first divergence is
 * downstream of the same bug, and a UART at 921600 is not a debugger.
 */
void emu_jit_diff_report(uint32_t pc, uint32_t off, uint32_t want,
                         uint32_t got);
void emu_jit_diff_report(uint32_t pc, uint32_t off, uint32_t want,
                         uint32_t got)
{
    static unsigned reported;

    if (reported++ >= 12u) {
        return;
    }
    console_puts("jit-diff pc ");
    console_puthex(pc);
    console_puts(" +");
    console_putu(off);
    console_puts(" want ");
    console_puthex(want);
    console_puts(" got ");
    console_puthex(got);
    console_puts("\n");
}
#endif


/* Transport hooks for the guest's virtual UART. */
static void guest_uart_tx(void *ctx, uint8_t c)
{
    (void)ctx;
    if (c == '\n') {
        console_putc('\r');
    }
    console_putc(c);
}

static int guest_uart_rx(void *ctx)
{
    (void)ctx;
    return console_getc();
}

/* ------------------------------------------------------------------ */
/* ECALL services                                                      */
/* ------------------------------------------------------------------ */

/*
 * The same newlib-style calls the host runner implements, so a guest built
 * once behaves identically in both places. Anything else falls through to
 * the architectural trap, leaving guests with their own handler unaffected.
 *
 *   nr = 64  write(fd, buf, len)
 *   nr = 93  exit(code)
 *
 * The frontend has already unpacked its own calling convention into
 * emu_syscall_t, so nothing here knows which registers those arrived in.
 */
static uint32_t g_exit_code;
static bool     g_exited;

static bool guest_syscall(emu_cpu_t *cpu, emu_syscall_t *sc, void *user)
{
    (void)user;

    switch (sc->nr) {
    case 64: {
        const uint32_t buf = sc->arg[1];
        const uint32_t len = sc->arg[2];
        for (uint32_t i = 0; i < len; i++) {
            uint32_t byte;
            if (emu_bus_read(&g_bus, buf + i, 1u, &byte) != EMU_FAULT_NONE) {
                break;
            }
            guest_uart_tx(NULL, (uint8_t)byte);
        }
        sc->ret = len;
        return true;
    }

    case 93:
        g_exit_code = sc->arg[0];
        g_exited = true;
        g_core.ops->halt(cpu);
        return true;

    default:
        return false;
    }
}

/* ------------------------------------------------------------------ */
/* Cache maintenance                                                   */
/* ------------------------------------------------------------------ */

/*
 * Zicbom mapped onto ARMv7-M cache maintenance.
 *
 * The Cortex-M7 in the STM32F746 has both caches, so unlike on the F446 --
 * where every one of these was a no-op the spec explicitly permits -- these
 * are real operations on the lines backing the guest's block. A guest that
 * hands a buffer to DMA and skips the cbo is now wrong in a way it was not
 * on the M4, which is the honest consequence of the part having a cache
 * rather than something the emulator should paper over.
 */
static void arm_cache_maint(void *ctx, void *host, uint32_t len,
                            emu_cache_op_t op)
{
    (void)ctx;

#if defined(__DCACHE_PRESENT) && (__DCACHE_PRESENT == 1U)
    switch (op) {
    case EMU_CACHE_CLEAN:
        SCB_CleanDCache_by_Addr((uint32_t *)host, (int32_t)len);
        break;
    case EMU_CACHE_INVAL:
        SCB_InvalidateDCache_by_Addr((uint32_t *)host, (int32_t)len);
        break;
    case EMU_CACHE_FLUSH:
        SCB_CleanInvalidateDCache_by_Addr((uint32_t *)host, (int32_t)len);
        break;
    }
#else
    (void)host;
    (void)len;
    (void)op;
    /* No data cache on this part: nothing to maintain. */
#endif
}

static const emu_cache_ops_t g_cache_ops = {
    .maint = arm_cache_maint,
    .ctx = NULL,
};

/* ------------------------------------------------------------------ */
/* Guest address space                                                 */
/* ------------------------------------------------------------------ */

/*
 * Peripheral passthrough policy.
 *
 * Guest 0x40000000..0x5FFFFFFF maps one-to-one onto the STM32's own
 * peripheral space, so a guest driver uses exactly the addresses printed
 * in RM0385 with no translation to reason about. This is what keeps
 * peripheral drivers in the guest and the ARM-side port thin: moving to
 * another STM32 means changing the clock setup and this table, not writing
 * new drivers.
 *
 * Only what would take the emulator down with the guest is withheld:
 *
 *   PWR           dropping over-drive at 180 MHz stalls the core
 *   RCC CR/PLL    reconfiguring the PLL kills the clock the emulator runs on
 *   FLASH         the flash controller can erase this firmware underneath us
 *
 * Note what is deliberately *not* withheld: the rest of RCC, including
 * AHB1ENR/APB1ENR/APB2ENR and the peripheral reset registers. A guest
 * driver has to be able to ungate its own peripheral's clock, and denying
 * that would push every driver back into the firmware.
 *
 * This table is the same as the F446's, and that is a checked result
 * rather than an assumption: all three withheld blocks sit at identical
 * addresses in RM0385 and RM0390 -- PWR at 0x40007000, RCC at 0x40023800,
 * the flash interface at 0x40023C00. What this part adds (LPTIM1, SPDIFRX,
 * I2C4, CAN2, HDMI-CEC, UART7/8, SAI2, LCD-TFT, Ethernet, CRYP/HASH/RNG)
 * all falls inside spans that were already read-write, and none of it can
 * take the emulator down with the guest.
 *
 * The console moved to USART3 for this board, and USART3 at 0x40004800 is
 * inside the guest's read-write span -- exactly as USART2 was on the F446.
 * A guest that reprograms it silences the console, which is the same
 * bargain as everywhere else here: the guest owns the peripherals.
 */
static const struct {
    const char *name;
    uint32_t    base;
    uint32_t    size;
    uint8_t     perm;
} g_periph_map[] = {
    /* APB1 up to PWR: timers, RTC, WWDG, SPI2/3, USART2/3, UART4/5, I2C */
    { "apb1",       0x40000000u, 0x00007000u, EMU_PERM_RW },
    { "pwr",        0x40007000u, 0x00000400u, EMU_PERM_R  },
    /* Rest of APB1, all of APB2, GPIO and CRC */
    { "apb1b+apb2", 0x40007400u, 0x0001C400u, EMU_PERM_RW },
    /* RCC clock tree: CR, PLLCFGR, CFGR, CIR */
    { "rcc-clock",  0x40023800u, 0x00000010u, EMU_PERM_R  },
    /* RCC resets and peripheral clock enables: the guest's to drive */
    { "rcc-periph", 0x40023810u, 0x000003F0u, EMU_PERM_RW },
    /* Flash interface: ACR, keys, control, option bytes */
    { "flash-ctl",  0x40023C00u, 0x00000400u, EMU_PERM_R  },
    /* BKPSRAM, DMA1/2, USB OTG HS, and AHB2 up to 0x5FFFFFFF */
    { "ahb1b+ahb2", 0x40024000u, 0x1FFDC000u, EMU_PERM_RW },
};

/* ------------------------------------------------------------------ */
/* Real interrupt lines                                                */
/* ------------------------------------------------------------------ */

/*
 * Bridging the NVIC to the APLIC.
 *
 * An interrupt is the one thing the passthrough window cannot carry. A
 * guest driver reaches a peripheral by using its address, but when that
 * peripheral raises an interrupt the NVIC vectors here, into the emulator,
 * with the guest nowhere in sight.
 *
 * The handshake is forced by one fact: nothing on this side can service the
 * device. Only the guest's driver knows how, and it will not run until the
 * emulator returns from the ISR. A level-triggered peripheral therefore
 * re-asserts the moment the handler exits, and the emulator would spin in
 * interrupt entry forever without the guest ever making progress. So the
 * line is masked on entry and stays masked until the guest clears the
 * APLIC pending bit, which is its way of saying the device has been dealt
 * with -- see irq_unmask_line, reached through the APLIC's eoi hook.
 *
 * Adding a peripheral is one table entry and one handler; the table is the
 * policy, the same way g_periph_map is for addresses.
 */
/*
 * An APLIC source number *is* the NVIC line number, so there is no mapping
 * table -- and, more to the point, none in the guest either. A driver that
 * would call HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn) writes that same 54 to the
 * APLIC's setienum, and the two numbering spaces never have to be
 * reconciled. It is the reason RV_APLIC_SOURCES is 128 rather than 32.
 */
static const IRQn_Type g_bridged[] = {
    TIM6_DAC_IRQn,
};

static bool irq_is_bridged(uint32_t source)
{
    for (unsigned i = 0; i < sizeof(g_bridged) / sizeof(g_bridged[0]); i++) {
        if ((uint32_t)g_bridged[i] == source) {
            return true;
        }
    }
    return false;
}

static void irq_line_entry(IRQn_Type irqn)
{
    NVIC_DisableIRQ(irqn);
    emu_core_set_irq(&g_core, (uint32_t)irqn, true);
}

static void irq_unmask_line(void *ctx, uint32_t source)
{
    (void)ctx;
    if (irq_is_bridged(source)) {
        NVIC_ClearPendingIRQ((IRQn_Type)source);
        NVIC_EnableIRQ((IRQn_Type)source);
    }
}

/*
 * Enable the bridged lines at the NVIC. Priority is left at the default:
 * these handlers do almost nothing, and the emulator has no other interrupt
 * to rank them against.
 */
static void bridged_irqs_init(void)
{
    for (unsigned i = 0; i < sizeof(g_bridged) / sizeof(g_bridged[0]); i++) {
        NVIC_EnableIRQ(g_bridged[i]);
    }
}

void TIM6_DAC_IRQHandler(void)
{
    irq_line_entry(TIM6_DAC_IRQn);
}

static bool build_address_space(void)
{
    emu_bus_init(&g_bus);

    /*
     * The guest's read-only half is served straight out of the part's
     * flash, where guest_image.S already put it, and RAM starts above
     * it. The guest's own layout does not change at all -- it is still
     * linked contiguously from EMU_GUEST_RAM_BASE and still resets to
     * offset 0 -- only which backing store answers the low addresses.
     *
     * The saving is real: the SRAM buffer now covers guest addresses
     * [ro, ro + GUEST_RAM_SIZE) instead of [0, GUEST_RAM_SIZE), so the
     * guest gains rv_guest_ro_size of address space for nothing. On the
     * largest architecture tests that is 140 KiB of the 345 they need.
     *
     * Both regions are registered even when rv_guest_ro_size is zero or
     * the whole image, because emu_bus rejects a zero-length region and
     * a guest with no .data is the common case here -- two of the three
     * in the tree have one.
     */
    const uint32_t guest_ro = rv_guest_ro_size;

    if (guest_ro != 0u &&
        !emu_bus_add_rom(&g_bus, "guest-ro", EMU_GUEST_RAM_BASE,
                         rv_guest_image, guest_ro)) {
        return false;
    }
    if (!emu_bus_add_ram(&g_bus, "ram", EMU_GUEST_RAM_BASE + guest_ro,
                        GUEST_RAM_BASE_PTR, GUEST_RAM_SIZE)) {
        return false;
    }

    /*
     * The guest image stays in ARM flash and is exposed read-only, so a
     * guest linked for execute-in-place costs no RAM at all.
     */
    if (!emu_bus_add_rom(&g_bus, "rom", EMU_GUEST_ROM_BASE,
                        rv_guest_image, rv_guest_image_size)) {
        return false;
    }

    if (!emu_bus_add_mmio(&g_bus, "uart0", EMU_GUEST_UART_BASE,
                         EMU_UART_SIZE, &emu_uart_ops, &g_uart)) {
        return false;
    }

    for (unsigned i = 0; i < sizeof(g_periph_map) / sizeof(g_periph_map[0]); i++) {
        /* Identity map: host base == guest base. */
        if (!emu_bus_add_passthru(&g_bus, g_periph_map[i].name,
                                 g_periph_map[i].base,
                                 g_periph_map[i].size,
                                 (uintptr_t)g_periph_map[i].base,
                                 g_periph_map[i].perm, EMU_WANY)) {
            return false;
        }
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* Diagnostics                                                         */
/* ------------------------------------------------------------------ */

/* emu_print_fn onto the console, for the frontend's own state dump. */
static void console_out(void *ctx, const char *s)
{
    (void)ctx;
    console_puts(s);
}

/*
 * Decoding the trap cause and naming the registers is the frontend's job:
 * only it knows what its status registers are and which of them matter
 * after a fault. This used to be a copy of that knowledge here, kept in
 * step with the core's by hand and with the other platform's by hand again.
 */
static void report_state(void)
{
    console_puts("\n-- guest state --");
    g_core.ops->dump(g_core.cpu, console_out, NULL);
}

/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    board_init();

#if RV32_NATIVE_COREMARK
    /*
     * Native baseline: the same CoreMark sources compiled for Cortex-M7
     * and run directly, with no emulation, so the interpreter and JIT
     * numbers can be put against something absolute.
     */
    {
        extern int coremark_native_main(void);
        console_puts("\n\nrv32cortex-m: NATIVE CoreMark on Cortex-M7 @ ");
        console_putu(SystemCoreClock / 1000000u);
        console_puts(" MHz\n\n");
        const uint32_t c0 = board_cycles();
        (void)coremark_native_main();
        console_puts("\n-- native --\n  host     ");
        console_putu(board_cycles() - c0);
        console_puts(" cycles\n");
        for (;;) { __WFI(); }
    }
#endif
    /*
     * The frontend names itself, and builds its ISA string from the
     * extensions actually compiled in, so the banner cannot drift from what
     * the core implements -- which it could when this file spelled the
     * string out itself.
     */
    const emu_cpu_ops_t *const ops = emu_frontend_default();

    console_puts("\n\nrv32cortex-m: ");
    console_puts(ops->desc);
    console_puts(" on Cortex-M7 @ ");
    console_putu(SystemCoreClock / 1000000u);
    console_puts(" MHz\n");

#if EMU_NET
    /*
     * The handover happens here, before the rest of the banner, so that
     * everything describing what is about to run -- guest size, guest
     * RAM, which backend came up -- reaches a telnet client rather than
     * a serial port nobody is watching. It is buffered until one
     * connects, which is what net_telnet.c's output ring is for.
     *
     * The two lines below are the last thing the UART ever carries as
     * text, and they are deliberately the two that matter when nothing
     * works: whether the stack started at all, and what address to
     * connect to. After this, silence on the serial port is expected and
     * silence on the network is the fault.
     */
    console_puts("net    SLIP on this port; telnet ");
    console_puts(emu_net_addr_str());
    console_puts(" 23\n");
    if (!emu_net_init()) {
        console_puts("net    failed to start; staying on the serial console\n");
    }
#endif

    if (!build_address_space()) {
        console_puts("fatal: could not build the guest address space\n");
        fatal_halt();
    }

    if (!emu_core_open(&g_core, ops, &g_bus, 0u)) {
        console_puts("fatal: frontend has no core 0\n");
        fatal_halt();
    }
    /*
     * The frontend's own devices: the shared ones and this core's. The
     * firmware is single-core, so there is one bus and one of each.
     */
    if ((ops->add_shared_devices != NULL &&
         !ops->add_shared_devices(&g_bus)) ||
        (ops->add_core_devices != NULL &&
         !ops->add_core_devices(g_core.cpu, &g_bus, 0u))) {
        console_puts("fatal: could not map the guest platform devices\n");
        fatal_halt();
    }

    ops->set_unmask_hook(g_core.cpu, irq_unmask_line, NULL);
    bridged_irqs_init();
    emu_uart_init(&g_uart, guest_uart_tx, guest_uart_rx, NULL);
    ops->set_syscall(g_core.cpu, guest_syscall, NULL);
    ops->set_cache(g_core.cpu, &g_cache_ops);

    /*
     * The image is linked to run from guest RAM, so copy it out of flash.
     * Guests linked for the ROM window can skip this and reset straight to
     * EMU_GUEST_ROM_BASE.
     */
    /*
     * Only the writable tail. The read-only half is already reachable
     * as a bus region pointing into flash, and copying it would put it
     * in RAM twice -- which is the whole cost this removes.
     */
    if (rv_guest_image_size - rv_guest_ro_size > GUEST_RAM_SIZE) {
        console_puts("fatal: guest image larger than guest RAM\n");
        fatal_halt();
    }
    memcpy(GUEST_RAM_BASE_PTR, rv_guest_image + rv_guest_ro_size,
           rv_guest_image_size - rv_guest_ro_size);

    console_puts("guest  ");
    console_putu(rv_guest_image_size);
    console_puts(" bytes at ");
    console_puthex(EMU_GUEST_RESET_PC);
    console_puts("\nram    ");
    console_putu(GUEST_RAM_SIZE / 1024u);
    console_puts(" KiB (");
    console_putu(GUEST_RAM_SIZE);
    console_puts(" bytes)\nbackend ");
    emu_core_reset(&g_core, EMU_GUEST_RESET_PC);
    emu_core_boot(&g_core, EMU_GUEST_RAM_BASE, GUEST_RAM_SIZE);

    emu_cpu_status_t st;
    emu_core_status(&g_core, &st);
    console_puts(st.backend);
    console_puts("\n\n");

    const uint32_t cycles_per_tick = SystemCoreClock / EMU_TIMER_HZ;
    const uint32_t start_cycles = board_cycles();
    uint64_t retired_total = 0;

    bool capped = false;

    for (;;) {
        uint32_t retired = 0;
        const emu_run_reason_t why = emu_core_run(&g_core, EMU_RUN_SLICE,
                                                  &retired);
        retired_total += retired;

#if EMU_NET
        /*
         * The stack advances only when called, so this is its entire
         * schedule. Once per slice is 4096 guest instructions, a few
         * hundred microseconds -- finer than any timer lwIP keeps and
         * far finer than the receive ring can fill, so nothing here
         * needs its own interrupt beyond the one taking bytes off the
         * wire.
         *
         * It is also the reason this port does not want an RTOS. A
         * scheduler would preempt on its tick, which is coarser than
         * this loop already is, and would buy nothing in exchange for
         * two stacks and lwIP's threaded API.
         */
        emu_net_poll();
#endif

        /*
         * Compared against the running total rather than a budget
         * counted down to zero. A block backend may retire more than
         * the slice it was given -- it can only stop between blocks --
         * so a remaining-budget subtraction goes below zero and wraps,
         * and the cap silently stops existing. That exact bug shipped in
         * the host runner and was invisible for the life of the project,
         * because the interpreter happens to land on the boundary
         * exactly.
         */
        if (EMU_MAX_INSN != 0u && retired_total >= (uint64_t)EMU_MAX_INSN) {
            capped = true;
            break;
        }

        /* Guest time tracks real time through the DWT cycle counter. */
        ops->set_time(g_core.cpu,
                      (uint64_t)(board_cycles() - start_cycles) / cycles_per_tick);

        if (why == EMU_RUN_HALTED) {
            break;
        }
        if (why == EMU_RUN_WFI) {
            emu_core_status(&g_core, &st);
            if (st.wakeable) {
                continue;
            }
            console_puts("\nguest parked with no interrupts enabled\n");
            break;
        }
    }

    const uint32_t elapsed = board_cycles() - start_cycles;

    if (capped) {
        /*
         * Named on its own line and before the statistics, so a harness
         * reading the console can tell "did not terminate" from "ran and
         * failed" without parsing the numbers.
         */
        console_puts("\nemu: instruction cap reached, guest did not halt\n");
    }

    console_puts("\n-- done --\n  retired  ");
    console_putu((uint32_t)retired_total);
    console_puts(" instructions\n  host     ");
    console_putu(elapsed);
    console_puts(" cycles\n  ratio    ");
    if (retired_total != 0u) {
        /* Host ARM cycles per emulated guest instruction, x100 so the
         * fractional part survives integer division. */
        const uint32_t x100 = (uint32_t)((uint64_t)elapsed * 100u / retired_total);
        console_putu(x100 / 100u);
        console_putc('.');
        console_putu((x100 % 100u) / 10u);
        console_putu(x100 % 10u);
        console_puts(" host cycles per guest instruction\n  speed    ");
        const uint32_t kips =
            (uint32_t)((uint64_t)retired_total * (SystemCoreClock / 1000u) / elapsed);
        console_putu(kips);
        console_puts(" KIPS\n");
    }

/*
 * JIT statistics: the one place in this file that names a frontend, and
 * unavoidably so -- the Thumb-2 JIT is the rv32 frontend's second backend.
 */
#if EMU_FRONTEND_RV32 && RV_ENABLE_JIT
    if (rv_backend == &rv_backend_jit) {
        rv_jit_stats_t js;
        rv_jit_get_stats(&js);
        console_puts("\n-- jit --\n  blocks   ");
        console_putu(js.blocks);
        console_puts("\n  code     ");
        console_putu(js.code_used);
        console_putc('/');
        console_putu(js.code_size);
        console_puts(" bytes\n  blks/xlat ");
        console_putu(js.translations);
        console_puts("\n  compact  ");
        console_putu(js.compactions);
        console_puts(" (");
        console_putu(js.evictions);
        console_puts(" evicted)\n  flushes  ");
        console_putu(js.flushes);
        /*
         * Instructions the translator declined and the interpreter ran.
         * A high share here is the first place to look when the speedup
         * is smaller than expected: it names exactly which encodings are
         * worth teaching the translator next.
         */
        console_puts("\n  interp   ");
        console_putu(js.interp_fallbacks);
        console_puts(" instructions fell back\n  helpers  muldiv ");
        console_putu(js.alu_calls_muldiv);
        console_puts("  clmul ");
        console_putu(js.alu_calls_clmul);
        console_puts("  bit ");
        console_putu(js.alu_calls_bit);
        console_puts("\n  pt hits  ");
        console_putu(js.pt_hits);
        console_puts(" armed ");
        console_putu(js.pt_armed);
        console_puts("\n  declined ");
        console_putu(js.declined);
        console_puts(" overflow ");
        console_putu(js.overflowed);
#ifdef EMU_JIT_PROFILE
        console_puts("\n  cyc xlat ");
        console_putu(js.cyc_translate);
        console_puts(" compact ");
        console_putu(js.cyc_compact);
#endif
        console_puts("\n  blk entr ");
        console_putu(js.block_entries);
        /*
         * Reads per block that uses the register, x100. Below 100 a cache
         * cannot pay: the block would spend a load to save fewer than one.
         */
        {
            static const char *const nm[4] = { "sp", "ra", "a0", "a1" };
            console_puts("\n  reads/blk");
            for (unsigned i = 0; i < 4u; i++) {
                console_putc(' ');
                console_puts(nm[i]);
                console_putc('=');
                if (js.hot_blocks[i] != 0u) {
                    const uint32_t x100 = js.hot_reads[i] * 100u / js.hot_blocks[i];
                    console_putu(x100 / 100u);
                    console_putc('.');
                    console_putu((x100 % 100u) / 10u);
                    console_putu(x100 % 10u);
                } else {
                    console_puts("-");
                }
                console_puts(" in ");
                console_putu(js.hot_blocks[i]);
            }
        }
        console_putc('\n');
    }
#endif

    report_state();

#if EMU_NET
    /*
     * Bytes the wire delivered and nothing collected. Reported next to
     * the guest's own numbers because it is the one failure that makes
     * *those* numbers untrustworthy without looking wrong: a dropped
     * byte is a dropped SLIP frame, which is a retransmission at best
     * and a truncated image at worst.
     */
    console_puts("\n-- net --\n  rx drops ");
    console_putu(board_console_rx_overruns());
    console_putc('\n');
#endif

    for (;;) {
#if EMU_NET
        /*
         * Everything above is still sitting in the output ring: the run
         * loop stopped, and with it the only thing that was delivering.
         * Parking in __WFI without draining first would lose the entire
         * report -- which is the part a harness came for.
         *
         * __WFI is still right rather than a busy loop. The UART receive
         * interrupt is what wakes it, and that fires on the first byte
         * of anything the host sends, including the ACK for what is
         * being flushed here.
         */
        emu_net_poll();
#endif
        __WFI();
    }
}
