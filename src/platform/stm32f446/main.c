/* SPDX-License-Identifier: Apache-2.0 */
/*
 * main.c - Emulator firmware for the Nucleo-F446RE.
 *
 * Bring-up is done entirely with ST's own driver pack: the CMSIS device
 * headers for register definitions, ST's startup file and
 * system_stm32f4xx.c, and the STM32Cube HAL for the clock tree, GPIO and
 * USART. Nothing here reimplements a peripheral the vendor already
 * supports.
 *
 * What this file adds is the glue: a guest address space built out of ARM
 * memory and ARM peripherals, and a run loop that hands control back often
 * enough for the ARM side to keep servicing its own interrupts.
 *
 * Which guest ISA runs on it is not this file's business. It opens a core
 * through emu_cpu_ops_t and drives that; the interrupt controller, the
 * timer and the state dump all come from the frontend. The only part below
 * that names an architecture is the JIT statistics block, and that is
 * because the Thumb-2 JIT is one particular frontend's backend.
 */

#include "stm32f4xx_hal.h"
#include "board.h"
#include "emu_board.h"
#include "emu_run.h"
#include "emu_console.h"

#if EMU_NET
#  include "emu_net.h"
#endif

#include "emu/emu_cpu.h"
#include "emu/emu_dev.h"
#include "emu/emu_memmap.h"

#if EMU_GUEST_ARCH_RV32
#  include "rv32/rv_backend.h"  /* which backend came up */
#  include "rv32/rv_jit.h"      /* JIT statistics, reported below */
#endif

#include <string.h>

/* The guest binary, embedded by guest_image.S. */
extern const uint8_t emu_guest_image[];
extern const uint32_t emu_guest_image_size;
extern const uint32_t emu_guest_ro_size;

/* ------------------------------------------------------------------ */
/* Configuration                                                       */
/* ------------------------------------------------------------------ */

/*
 * Guest RAM is not a fixed-size array: the link script places it between
 * the firmware's .bss and the stack and gives it everything in between, so
 * the guest automatically receives all the SRAM the ARM side is not using
 * (see .guest_ram in stm32f446retx.ld).
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

void emu_console_putc(uint8_t c)
{
#if EMU_NET
    /*
     * One console, two sinks. Before emu_net_init() succeeds the byte
     * goes to the UART; after it, the UART carries IP and the console is
     * a ring a telnet client drains. The handover is one-way.
     */
    if (emu_net_active()) {
        emu_net_console_putc(c);
        return;
    }
#endif
    board_console_putc(c);
}

#if EMU_NET
/*
 * Uploads are declined on this board.
 *
 * The stack's TFTP server needs somewhere to put an image, and on the
 * F746 that is a flash arena in sectors 5-7 with an erase-and-retry
 * recovery. This part has flash and no arena carved out of it, so the
 * honest answer is a refusal the client can see -- an "access violation"
 * at the far end -- rather than a partial write into whatever follows
 * the firmware.
 *
 * Everything else the stack offers works: ping, the telnet console, and
 * the gdb stub. Implementing the arena here is a self-contained addition
 * and the reason these are functions rather than an #if.
 */
bool emu_net_image_begin(emu_net_image_t which)
{
    (void)which;
    return false;
}

bool emu_net_image_data(emu_net_image_t which, const void *data,
                        uint32_t len, uint32_t off)
{
    (void)which; (void)data; (void)len; (void)off;
    return false;
}

void emu_net_image_end(emu_net_image_t which, uint32_t len, bool ok)
{
    (void)which; (void)len; (void)ok;
}
#endif

#define console_putc   emu_console_putc
#define console_puts   emu_console_puts
#define console_printf emu_console_printf
#define console_putu   emu_console_putu
#define console_puthex emu_console_puthex




/* Transport hooks for the guest's virtual UART. */

static int guest_uart_rx(void *ctx)
{
    (void)ctx;
    return board_console_getc();
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
static emu_guest_exit_t g_exit;

/* What emu_emu_guest_syscall needs from this board. */
static emu_syscall_ctx_t g_sc_ctx = {
    .bus = &g_bus, .core = &g_core, .exit = &g_exit,
};


/* ------------------------------------------------------------------ */
/* Cache maintenance                                                   */
/* ------------------------------------------------------------------ */

/*
 * Zicbom mapped onto ARMv7-M cache maintenance.
 *
 * The Cortex-M4 in the STM32F446 has no data cache -- only the ART flash
 * accelerator, which is transparent and needs no maintenance -- so on this
 * part every operation is a no-op, which the RISC-V spec explicitly
 * permits. The code below is written against __DCACHE_PRESENT so the same
 * platform file does the right thing when built for a Cortex-M7, where
 * these become real cache operations on the lines backing the guest block.
 */

/* ------------------------------------------------------------------ */
/* Guest address space                                                 */
/* ------------------------------------------------------------------ */

/*
 * Peripheral passthrough policy.
 *
 * Guest 0x40000000..0x5FFFFFFF maps one-to-one onto the STM32's own
 * peripheral space, so a guest driver uses exactly the addresses printed
 * in RM0390 with no translation to reason about. This is what keeps
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
 * Bridging the NVIC to the guest's interrupt controller.
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
 * with -- see emu_board_irq_unmask, reached through the frontend's unmask hook.
 *
 * Adding a peripheral is one table entry and one handler; the table is the
 * policy, the same way g_periph_map is for addresses.
 */
/*
 * A source number *is* the NVIC line number, so there is no mapping table
 * -- and, more to the point, none in the guest either. A driver that would
 * call HAL_NVIC_EnableIRQ(TIM6_DAC_IRQn) writes that same 54 to its own
 * controller's enable register, and the two numbering spaces never have to
 * be reconciled. It is the reason RV_APLIC_SOURCES is 128 rather than 32.
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

void emu_board_irq_unmask(void *ctx, uint32_t source)
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
void emu_board_irqs_init(void)
{
    for (unsigned i = 0; i < sizeof(g_bridged) / sizeof(g_bridged[0]); i++) {
        NVIC_EnableIRQ(g_bridged[i]);
    }
}

void TIM6_DAC_IRQHandler(void)
{
    irq_line_entry(TIM6_DAC_IRQn);
}

/* ------------------------------------------------------------------ */
/* What this board owes the shared runner (see emu_board.h)            */
/* ------------------------------------------------------------------ */

/*
 * Guest time, from the cycle counter. The divisor is fixed at start-up
 * because SystemCoreClock is, and the epoch is the first slice rather
 * than reset so a guest's clock starts near zero.
 */
static uint32_t g_cycles_per_tick;
static uint32_t g_start_cycles;

uint64_t emu_board_time_now(void)
{
    return (uint64_t)(board_cycles() - g_start_cycles) / g_cycles_per_tick;
}

const char *const emu_board_core_name = "Cortex-M4";

/*
 * Fixed: this part takes no uploads, so the image is the linked-in one
 * and these never move. A board that can be reloaded assigns them.
 */
const uint8_t *emu_board_img      = emu_guest_image;
uint32_t       emu_board_img_size = 0u;   /* set in main, see below */
uint32_t       emu_board_img_ro   = 0u;

uint8_t *emu_board_ram      = NULL;   /* both set in main, see below */
uint32_t emu_board_ram_size = 0u;

/*
 * The passthrough windows. Identity-mapped, so a guest driver writing
 * what its datasheet says reaches the real peripheral -- which is the
 * point of this emulator and the reason this table cannot be shared:
 * the windows differ per part, and so does which of them a guest may
 * write.
 */
void emu_board_image_published(void) { }

bool emu_board_add_regions(emu_bus_t *bus)
{
    for (unsigned i = 0; i < sizeof(g_periph_map) / sizeof(g_periph_map[0]);
         i++) {
        if (!emu_bus_add_passthru(bus, g_periph_map[i].name,
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



/* ------------------------------------------------------------------ */
/* Entry                                                               */
/* ------------------------------------------------------------------ */

int main(void)
{
    board_init();

#if RV32_NATIVE_COREMARK
    /*
     * Native baseline: the same CoreMark sources compiled for Cortex-M4
     * and run directly, with no emulation, so the interpreter and JIT
     * numbers can be put against something absolute.
     */
    {
        extern int coremark_native_main(void);
        console_printf("\n\nrv32cortex-m: NATIVE CoreMark on %s @ %u MHz\n\n",
                       emu_board_core_name,
                       (unsigned)(SystemCoreClock / 1000000u));
        const uint32_t c0 = board_cycles();
        (void)coremark_native_main();
        console_printf("\n-- native --\n  host     %u cycles\n",
                       (unsigned)(board_cycles() - c0));
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

    console_printf("\n\nrv32cortex-m: %s on %s @ %u MHz\n",
                   ops->desc, emu_board_core_name,
                   (unsigned)(SystemCoreClock / 1000000u));


    if (!emu_core_open(&g_core, ops, &g_bus, 0u)) {
        console_puts("fatal: frontend has no core 0\n");
        fatal_halt();
    }
    /*
     * The frontend's own devices: the shared ones and this core's. The
     * firmware is single-core -- 64 KiB of local RAM per G4MH PE does not
     * fit in 128 KiB of SRAM -- so there is one bus and one of each.
     */

    ops->set_unmask_hook(g_core.cpu, emu_board_irq_unmask, NULL);
    emu_board_irqs_init();
    emu_uart_init(&g_uart, emu_console_uart_tx, guest_uart_rx, NULL);
    ops->set_syscall(g_core.cpu, emu_guest_syscall, &g_sc_ctx);
    ops->set_cache(g_core.cpu, &emu_arm_cache_ops);

    emu_board_img      = emu_guest_image;
    emu_board_img_size = emu_guest_image_size;
    emu_board_img_ro   = emu_guest_ro_size;
    emu_board_ram      = GUEST_RAM_BASE_PTR;
    emu_board_ram_size = GUEST_RAM_SIZE;

    if (!emu_start_guest(&g_core, &g_bus, &g_uart, &g_exit)) {
        console_puts("fatal: could not bring the guest up\n");
        fatal_halt();
    }

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
    /*
     * **Zeroed, not installed.** The guest copies its own .data now --
     * start.S does it from __data_lma, in the read-only image window --
     * so all this owes is memory in a known state. This still memcpy'd
     * the writable half in after the F746 stopped, which was harmless
     * (the guest overwrote it with the same bytes) and exactly the kind
     * of divergence a shared runner is meant to make impossible.
     *
     * Zeroing the whole of it matters for a different reason: without
     * it one test's leftovers become the next test's initial state, and
     * a suite's results start depending on the order it ran in.
     */


    emu_cpu_status_t st;
    emu_core_status(&g_core, &st);

    console_printf("guest  %u bytes at 0x%08x\n"
                   "ram    %u KiB (%u bytes)\nbackend ",
                   (unsigned)emu_guest_image_size, (unsigned)EMU_GUEST_RESET_PC,
                   (unsigned)(GUEST_RAM_SIZE / 1024u),
                   (unsigned)GUEST_RAM_SIZE);
    console_printf("%s\n", st.backend);

#if EMU_NET
    /*
     * The handover, and the last two lines the UART ever carries as
     * text: whether the stack started, and where to connect. After this
     * silence on the serial port is expected and silence on the network
     * is the fault.
     */
    console_printf("net    %s on this port; telnet %s 23\n",
                   EMU_NET_LINK_PPP ? "PPP" : "SLIP", emu_net_addr_str());
    if (!emu_net_init()) {
        console_puts("net    failed to start; staying on the serial console\n");
    }
#endif
    console_puts("\n");

    g_cycles_per_tick = SystemCoreClock / EMU_TIMER_HZ;
    const uint32_t start_cycles = board_cycles();

    g_start_cycles = start_cycles;
    uint64_t retired_total = 0;

    bool capped = false;

    {
        const emu_run_env_t env = {
            .slice       = EMU_RUN_SLICE,
            .max_insn    = EMU_MAX_INSN,
            /* This board takes no uploads; see emu_net_image_begin. */
            .take_upload = NULL,
        };
        const emu_run_outcome_t out =
            emu_run_guest(&g_core, ops, &env, &retired_total);

        capped = (out == EMU_RUN_OUTCOME_CAPPED);
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
 * JIT statistics. The one place in this file that names a frontend, and
 * unavoidably so: the Thumb-2 JIT is the rv32 frontend's second backend,
 * and what it counts -- translations, evictions, which encodings fell back
 * to the interpreter -- has no meaning for any other. A frontend without a
 * JIT simply does not compile this block in.
 */
#if EMU_GUEST_ARCH_RV32 && EMU_HAVE_JIT
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
        console_puts("\n  elided   ld ");
        console_putu(js.ld_elided);
        console_puts("  st ");
        console_putu(js.st_elided);
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

    emu_report_state(g_core.cpu, g_core.ops);

    for (;;) {
        __WFI();
    }
}
