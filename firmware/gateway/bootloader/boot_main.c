/**
 * @file boot_main.c
 * @brief Gateway bootloader: vector table, reset handler, and the boot sequence.
 *
 * Runs from reset at 0x0800_0000 before anything else on the board (hld.md §13.5, D36). It
 * initialises its own RAM, raises the clock, lets boot_core decide, reports the decision on
 * USART1 and either jumps or halts. It never returns.
 *
 * @see docs/lld/drivers/bootloader.md §3.5
 */

#include <stdint.h>

#include "boot_core.h"
#include "boot_hw.h"
#include "internal_flash/internal_flash.h"

extern uint32_t _estack;
extern uint32_t _sidata;
extern uint32_t _sdata;
extern uint32_t _edata;
extern uint32_t _sbss;
extern uint32_t _ebss;

void boot_reset_handler(void);
void boot_nmi_handler(void);
void boot_fault_handler(void);

/** Initial stack pointer, then the 15 system vectors; no external interrupt is ever enabled. */
__attribute__((section(".isr_vector"), used)) const uint32_t g_boot_vectors[16] = {
    (uint32_t) &_estack,
    (uint32_t) boot_reset_handler,
    (uint32_t) boot_nmi_handler,
    (uint32_t) boot_fault_handler, /* HardFault   */
    (uint32_t) boot_fault_handler, /* MemManage   */
    (uint32_t) boot_fault_handler, /* BusFault    */
    (uint32_t) boot_fault_handler, /* UsageFault  */
    0U,
    0U,
    0U,
    0U,
    (uint32_t) boot_fault_handler, /* SVCall      */
    (uint32_t) boot_fault_handler, /* DebugMon    */
    0U,
    (uint32_t) boot_fault_handler, /* PendSV      */
    (uint32_t) boot_fault_handler, /* SysTick     */
};

static void init_ram(void)
{
    const uint32_t *src = &_sidata;

    /* cppcheck-suppress comparePointers -- linker-script symbols delimiting one region */
    for (uint32_t *dst = &_sdata; dst < &_edata; dst++)
    {
        *dst = *src;
        src++;
    }

    /* cppcheck-suppress comparePointers -- linker-script symbols delimiting one region */
    for (uint32_t *dst = &_sbss; dst < &_ebss; dst++)
    {
        *dst = 0U;
    }
}

static const char *outcome_name(boot_outcome_t o)
{
    switch (o)
    {
    case BOOT_OUTCOME_RUN:
        return "run";
    case BOOT_OUTCOME_HALT_NO_IMAGE:
        return "HALT no image";
    case BOOT_OUTCOME_HALT_ROLLBACK_LIMIT:
        return "HALT rollback limit";
    default:
        return "HALT flash fault";
    }
}

static void print_report(const boot_report_t *r, uint32_t cycles)
{
    boot_hw_console_puts("BL in=");
    boot_hw_console_dec(r->state_in);
    boot_hw_console_puts(" out=");
    boot_hw_console_dec(r->state_out);
    boot_hw_console_puts(" bank=");
    boot_hw_console_puts((r->active_bank == FIRMWARE_BANK_B) ? "B" : "A");
    boot_hw_console_puts(" gen=");
    boot_hw_console_dec(r->generation);
    boot_hw_console_puts(" rb=");
    boot_hw_console_dec(r->rollback_count);
    boot_hw_console_puts(" ev=");
    boot_hw_console_hex(r->events);
    boot_hw_console_puts(" ver=");
    boot_hw_console_hex(r->version);
    boot_hw_console_puts(" len=");
    boot_hw_console_dec(r->image_len);
    boot_hw_console_puts(" ms=");
    boot_hw_console_dec(cycles / (BOOT_HW_FAST_HZ / 1000UL));
    boot_hw_console_puts(" ecc=");
    boot_hw_console_dec(boot_hw_ecc_events());
    boot_hw_console_puts("\r\nBL ");
    boot_hw_console_puts(outcome_name(r->outcome));

    if (r->outcome == BOOT_OUTCOME_RUN)
    {
        boot_hw_console_puts(" ");
        boot_hw_console_hex(r->slot_base);
    }

    boot_hw_console_puts("\r\n");
}

void boot_reset_handler(void)
{
    boot_report_t report;

    init_ram();

    (void) boot_hw_cycles();
    boot_hw_clocks_fast();
    boot_hw_console_init();
    boot_hw_console_puts("\r\nBL start\r\n");

    if (internal_flash_init() != INTERNAL_FLASH_OK)
    {
        boot_hw_console_puts("BL HALT flash init\r\n");
        boot_hw_halt();
    }

    const uint32_t t0 = boot_hw_cycles();
    const boot_outcome_t outcome = boot_core_run(internal_flash_driver, &report);
    const uint32_t cycles = boot_hw_cycles() - t0;

    print_report(&report, cycles);

    if (outcome == BOOT_OUTCOME_RUN)
    {
        boot_hw_jump(report.slot_base);
    }

    boot_hw_halt();
}
