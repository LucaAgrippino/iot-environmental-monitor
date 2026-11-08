/**
 * @file cpu.c
 * @brief CpuDriver implementation for the STM32F469 (Field Device): panic subset.
 *
 * Platform singleton: one MCU, one fault vector set. Same module, API names, panic record and
 * report as the Gateway CpuDriver; this board has no clock or delay functions here (its clock is
 * Src/system_clock.c). Design: docs/lld/drivers/cpu-driver.md.
 *
 * Implements REQ-NF-507 (design: docs/lld/drivers/cpu-driver.md).
 *
 * Consumes: CMSIS register definitions (stm32f469xx.h) and nothing else. No FreeRTOS header, no
 * Logger, no RtcDriver: the fault path must work when everything else is broken.
 */

#include "cpu.h"
#include "cpu_hw.h"
#include "panic_symbols.h"

#ifdef STM32F469xx
#include "stm32f469xx.h"
#endif

#include <stddef.h>

/* --------------------------------------------------------------------- */
/* Configuration                                                          */
/* --------------------------------------------------------------------- */

#define CPU_PCLK1_HZ (45000000U)

#define CPU_PANIC_MAGIC (0xDEADC0DEU)

/** SCB->CFSR bit 15: BFAR holds the faulting address. */
#define CPU_CFSR_BFARVALID (1UL << 15U)

/** Post-mortem record: CPU_PANIC_RECORD_WORDS consecutive RTC backup registers starting at
 * CPU_PANIC_BKP_BASE. Register 0 is TimeProvider's sync flag (TIME_PROVIDER_BKUP_REG), so the
 * record starts at 2; the F469 has registers 0 to 19 (cpu-driver.md section 3.3, CPU-D2). */
#define CPU_PANIC_BKP_BASE (2U)
#define CPU_PANIC_RECORD_WORDS (17U)
enum
{
    REC_MAGIC = 0,
    REC_CFSR = 1,
    REC_HFSR = 2,
    REC_BFAR = 3,
    REC_PC = 4,
    REC_LR = 5,
    REC_SP = 6,
    REC_XPSR = 7,
    REC_R0 = 8,
    REC_R1 = 9,
    REC_R2 = 10,
    REC_R3 = 11,
    REC_R12 = 12,
    REC_SOURCE = 13,
    REC_REASON_HASH = 14,
    REC_TIME = 15, /* RTC->TR: BCD hh:mm:ss, meaningful once TimeProvider has set the clock */
    REC_DATE = 16, /* RTC->DR */
};

#define CPU_PANIC_UART_BAUD (115200U)
/** BRR (oversampling by 16) = PCLK1 / baud, rounded: 45 000 000 / 115 200 = 391. */
#define CPU_PANIC_UART_BRR ((CPU_PCLK1_HZ + (CPU_PANIC_UART_BAUD / 2U)) / CPU_PANIC_UART_BAUD)

/** AF7 = USART3 on PB10 (same pin as DebugUartDriver). */
#define CPU_PANIC_UART_TX_PIN (10U)
#define CPU_PANIC_UART_TX_AF (7U)

/* Polling-loop iteration limit; tight in test builds to avoid hangs. */
#ifdef TEST
#define CPU_POLL_TIMEOUT (3U)
#else
#define CPU_POLL_TIMEOUT (100000U)
#endif

/* --------------------------------------------------------------------- */
/* Private state                                                          */
/* --------------------------------------------------------------------- */

static volatile bool g_panic_active;

/* Stacked exception frame: set by cpu_fault_entry() before cpu_panic(). */
static volatile uint32_t *s_fault_frame;

/** Backup register holding word n of the post-mortem record. */
static volatile uint32_t *panic_bkp(uint32_t n)
{
    return &RTC->BKP0R + (CPU_PANIC_BKP_BASE + n);
}

/* --------------------------------------------------------------------- */
/* Private helpers: UART panic output                                     */
/* --------------------------------------------------------------------- */

static void panic_uart_init(void)
{
    /* Enable the GPIOB and USART3 clocks. */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;
    RCC->APB1ENR |= RCC_APB1ENR_USART3EN;

    /* PB10: AF7 (USART3_TX), push-pull, very high speed, no pull. AFR[1] covers pins 8 to 15. */
    GPIOB->MODER &= ~(3UL << (CPU_PANIC_UART_TX_PIN * 2U));
    GPIOB->MODER |= (2UL << (CPU_PANIC_UART_TX_PIN * 2U));
    GPIOB->OTYPER &= ~(1UL << CPU_PANIC_UART_TX_PIN);
    GPIOB->OSPEEDR |= (3UL << (CPU_PANIC_UART_TX_PIN * 2U));
    GPIOB->PUPDR &= ~(3UL << (CPU_PANIC_UART_TX_PIN * 2U));
    GPIOB->AFR[1] &= ~(0xFUL << ((CPU_PANIC_UART_TX_PIN - 8U) * 4U));
    GPIOB->AFR[1] |= ((uint32_t) CPU_PANIC_UART_TX_AF << ((CPU_PANIC_UART_TX_PIN - 8U) * 4U));

    /* USART3: 115 200 baud, 8N1, transmit only. */
    USART3->CR1 = 0U;
    USART3->BRR = CPU_PANIC_UART_BRR;
    USART3->CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void panic_uart_write_byte(uint8_t byte)
{
    uint32_t timeout = CPU_POLL_TIMEOUT;
    while (!(USART3->SR & USART_SR_TXE))
    {
        if (--timeout == 0U)
        {
            return; /* best effort: already in a fault handler */
        }
    }
    USART3->DR = (uint32_t) byte;
}

static void panic_uart_write_str(const char *s)
{
    while (*s != '\0')
    {
        panic_uart_write_byte((uint8_t) *s);
        ++s;
    }
}

/** Wait (bounded) until the last byte has left the shift register, so a reset does not cut it. */
static void panic_uart_drain(void)
{
    uint32_t timeout = CPU_POLL_TIMEOUT;
    while (!(USART3->SR & USART_SR_TC))
    {
        if (--timeout == 0U)
        {
            return;
        }
    }
}

static void panic_uart_write_hex32(uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    panic_uart_write_str("0x");
    for (int32_t shift = 28; shift >= 0; shift -= 4)
    {
        panic_uart_write_byte((uint8_t) hex[(value >> (uint32_t) shift) & 0xFU]);
    }
}

/**
 * @brief Print " (function+0xNN file.o)" for a code address, from the table generated out of the
 * linker map (scripts/gen-panic-symbols.py, panic_symbols_data.c, gitignored). Compiled out in the
 * host tests; degrades to a note when the table is absent or belongs to another image.
 */
#ifndef TEST
extern char _etext; /* end of code, from the linker script */
#endif

static void panic_uart_write_symbol(uint32_t addr)
{
#ifndef TEST
    if (!panic_symbols_match(g_panic_anchor_addr, (uint32_t) (uintptr_t) &cpu_panic,
                             g_panic_text_end, (uint32_t) (uintptr_t) &_etext,
                             g_panic_symbol_count))
    {
        panic_uart_write_str(" (no symbols for this image)");
        return;
    }
    const panic_symbol_t *sym = panic_symbol_lookup(g_panic_symbols, g_panic_symbol_count, addr);
    if (sym != NULL)
    {
        panic_uart_write_str(" (");
        panic_uart_write_str(sym->name);
        panic_uart_write_str("+");
        panic_uart_write_hex32(addr - sym->addr);
        panic_uart_write_str(" ");
        panic_uart_write_str(g_panic_files[sym->file_index]);
        panic_uart_write_str(")");
    }
    else
    {
        panic_uart_write_str(" (unknown)");
    }
#else
    (void) addr;
#endif
}

/* --------------------------------------------------------------------- */
/* Private helpers: CFSR cause decoding                                   */
/* --------------------------------------------------------------------- */

static const struct
{
    uint32_t bit;
    const char *cause;
} s_cfsr_table[] = {
    {SCB_CFSR_IACCVIOL_Msk, "Instruction access violation"},
    {SCB_CFSR_DACCVIOL_Msk, "Data access violation"},
    {SCB_CFSR_MUNSTKERR_Msk, "MemManage unstacking error"},
    {SCB_CFSR_MSTKERR_Msk, "MemManage stacking error"},
    {SCB_CFSR_MLSPERR_Msk, "MemManage FP lazy state"},
    {SCB_CFSR_IBUSERR_Msk, "Instruction bus error"},
    {SCB_CFSR_PRECISERR_Msk, "Precise data bus error"},
    {SCB_CFSR_IMPRECISERR_Msk, "Imprecise data bus error"},
    {SCB_CFSR_UNSTKERR_Msk, "BusFault unstacking error"},
    {SCB_CFSR_STKERR_Msk, "BusFault stacking error"},
    {SCB_CFSR_LSPERR_Msk, "BusFault FP lazy state"},
    {SCB_CFSR_UNDEFINSTR_Msk, "Undefined instruction"},
    {SCB_CFSR_INVSTATE_Msk, "Invalid state (Thumb bit)"},
    {SCB_CFSR_INVPC_Msk, "Invalid PC load"},
    {SCB_CFSR_NOCP_Msk, "No coprocessor"},
    {SCB_CFSR_UNALIGNED_Msk, "Unaligned access"},
    {SCB_CFSR_DIVBYZERO_Msk, "Divide by zero"},
};

#define CFSR_TABLE_SIZE ((uint32_t) (sizeof(s_cfsr_table) / sizeof(s_cfsr_table[0])))

static const char *cfsr_cause_string(uint32_t cfsr)
{
    for (uint32_t i = 0U; i < CFSR_TABLE_SIZE; ++i)
    {
        if ((cfsr & s_cfsr_table[i].bit) != 0U)
        {
            return s_cfsr_table[i].cause;
        }
    }
    return "Unknown fault";
}

static const char *panic_source_string(cpu_panic_source_t source)
{
    switch (source)
    {
    case CPU_PANIC_HARDFAULT:
        return "HardFault";
    case CPU_PANIC_BUSFAULT:
        return "BusFault";
    case CPU_PANIC_MEMMANAGE:
        return "MemManage";
    case CPU_PANIC_USAGEFAULT:
        return "UsageFault";
    case CPU_PANIC_ASSERT:
        return "Assert";
    case CPU_PANIC_STACK_OVERFLOW:
        return "StackOverflow";
    case CPU_PANIC_MALLOC_FAILED:
        return "MallocFailed";
    case CPU_PANIC_WATCHDOG:
        return "Watchdog";
    case CPU_PANIC_USER:
        return "User";
    default:
        return "Unknown";
    }
}

/* --------------------------------------------------------------------- */
/* Private helpers: reason hash                           */
/* --------------------------------------------------------------------- */

static uint32_t djb2_hash(const char *str)
{
    uint32_t hash = 5381U;
    if (str == NULL)
    {
        return 0U;
    }
    while (*str != '\0')
    {
        hash = (hash * 33U) ^ (uint32_t) (uint8_t) *str;
        ++str;
    }
    return hash;
}

/* --------------------------------------------------------------------- */
/* Private helpers: panic record write, report and post-mortem            */
/* --------------------------------------------------------------------- */

static void unlock_backup_domain(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    PWR->CR |= PWR_CR_DBP;
}

static void write_panic_record(cpu_panic_source_t source, const char *reason, uint32_t cfsr,
                               uint32_t hfsr, uint32_t bfar)
{
    unlock_backup_domain();

    *panic_bkp(REC_CFSR) = cfsr;
    *panic_bkp(REC_HFSR) = hfsr;
    *panic_bkp(REC_BFAR) = bfar;

    if (s_fault_frame != NULL)
    {
        *panic_bkp(REC_PC) = s_fault_frame[6];
        *panic_bkp(REC_LR) = s_fault_frame[5];
        *panic_bkp(REC_SP) = (uint32_t) (uintptr_t) s_fault_frame;
        *panic_bkp(REC_XPSR) = s_fault_frame[7];
        *panic_bkp(REC_R0) = s_fault_frame[0];
        *panic_bkp(REC_R1) = s_fault_frame[1];
        *panic_bkp(REC_R2) = s_fault_frame[2];
        *panic_bkp(REC_R3) = s_fault_frame[3];
        *panic_bkp(REC_R12) = s_fault_frame[4];
    }
    else
    {
        for (uint32_t n = REC_PC; n <= REC_R12; ++n)
        {
            *panic_bkp(n) = 0U;
        }
    }

    *panic_bkp(REC_SOURCE) = (uint32_t) source;
    *panic_bkp(REC_REASON_HASH) = djb2_hash(reason);
    *panic_bkp(REC_TIME) = RTC->TR;
    *panic_bkp(REC_DATE) = RTC->DR;

    /* Magic last: a partly written record is never reported. */
    *panic_bkp(REC_MAGIC) = CPU_PANIC_MAGIC;
}

static void emit_panic_uart(cpu_panic_source_t source, const char *reason, uint32_t cfsr,
                            uint32_t hfsr, uint32_t bfar)
{
    panic_uart_init();

    panic_uart_write_str("\r\n========== PANIC ==========\r\n");
    panic_uart_write_str("Source: ");
    panic_uart_write_str(panic_source_string(source));
    panic_uart_write_str("\r\nCFSR:   ");
    panic_uart_write_hex32(cfsr);
    panic_uart_write_str("\r\nCause:  ");
    panic_uart_write_str(cfsr_cause_string(cfsr));
    panic_uart_write_str("\r\nHFSR:   ");
    panic_uart_write_hex32(hfsr);
    panic_uart_write_str("\r\nBFAR:   ");
    panic_uart_write_hex32(bfar);

    if (s_fault_frame != NULL)
    {
        panic_uart_write_str("\r\nPC:     ");
        panic_uart_write_hex32(s_fault_frame[6]);
        panic_uart_write_symbol(s_fault_frame[6]);
        panic_uart_write_str("\r\nLR:     ");
        panic_uart_write_hex32(s_fault_frame[5]);
        panic_uart_write_symbol(s_fault_frame[5]);
        panic_uart_write_str("\r\nxPSR:   ");
        panic_uart_write_hex32(s_fault_frame[7]);
        panic_uart_write_str("\r\nR0-R3:  ");
        panic_uart_write_hex32(s_fault_frame[0]);
        panic_uart_write_str(" ");
        panic_uart_write_hex32(s_fault_frame[1]);
        panic_uart_write_str(" ");
        panic_uart_write_hex32(s_fault_frame[2]);
        panic_uart_write_str(" ");
        panic_uart_write_hex32(s_fault_frame[3]);
        panic_uart_write_str("\r\nR12:    ");
        panic_uart_write_hex32(s_fault_frame[4]);
    }

    panic_uart_write_str("\r\nReason: ");
    panic_uart_write_str((reason != NULL) ? reason : "(none)");
    panic_uart_write_str("\r\n================================\r\n");
}

/** Print and invalidate a record left by the previous boot (needs backup-domain write access). */
static void check_panic_record(void)
{
    if (*panic_bkp(REC_MAGIC) != CPU_PANIC_MAGIC)
    {
        return;
    }

    const uint32_t cfsr = *panic_bkp(REC_CFSR);
    const uint32_t hfsr = *panic_bkp(REC_HFSR);
    const uint32_t bfar = *panic_bkp(REC_BFAR);
    const uint32_t source = *panic_bkp(REC_SOURCE);

    panic_uart_init();

    panic_uart_write_str("\r\n--- POST-MORTEM (previous boot) ---\r\n");
    panic_uart_write_str("Source: ");
    panic_uart_write_str(panic_source_string((cpu_panic_source_t) source));
    panic_uart_write_str("\r\nCFSR:   ");
    panic_uart_write_hex32(cfsr);
    panic_uart_write_str("\r\nCause:  ");
    panic_uart_write_str(cfsr_cause_string(cfsr));
    panic_uart_write_str("\r\nHFSR:   ");
    panic_uart_write_hex32(hfsr);
    panic_uart_write_str("\r\nBFAR:   ");
    panic_uart_write_hex32(bfar);
    panic_uart_write_str("\r\nPC:     ");
    panic_uart_write_hex32(*panic_bkp(REC_PC));
    panic_uart_write_symbol(*panic_bkp(REC_PC));
    panic_uart_write_str("\r\nLR:     ");
    panic_uart_write_hex32(*panic_bkp(REC_LR));
    panic_uart_write_symbol(*panic_bkp(REC_LR));
    panic_uart_write_str("\r\nTime:   ");
    panic_uart_write_hex32(*panic_bkp(REC_TIME));
    panic_uart_write_str(" ");
    panic_uart_write_hex32(*panic_bkp(REC_DATE));
    panic_uart_write_str("\r\n-----------------------------------\r\n");

    /* Invalidate the record so it is not replayed on the next boot. */
    panic_uart_drain();
    *panic_bkp(REC_MAGIC) = 0U;
}

/* ====================================================================== */
/* Public API                                                              */
/* ====================================================================== */

status_t cpu_init(void)
{
    /* Each configurable fault reaches its own vector (and its own panic source) instead of
     * escalating to HardFault. */
    SCB->SHCSR |= SCB_SHCSR_MEMFAULTENA_Msk | SCB_SHCSR_BUSFAULTENA_Msk | SCB_SHCSR_USGFAULTENA_Msk;

    /* Report and clear any panic record from the previous boot. */
    unlock_backup_domain();
    check_panic_record();

    return STATUS_OK;
}

#ifdef TEST
void cpu_reset(void)
#else
_Noreturn void cpu_reset(void)
#endif
{
    CPU_HW_DATA_BARRIER();
    CPU_HW_SYSTEM_RESET();
}

/** End of the panic: stop for an attached debugger (DEBUG build only), otherwise reset. A BKPT with
 *  no debugger attached is itself a fault, so it is never executed blind (cpu-driver.md CPU-D3). */
#ifdef TEST
static void panic_end(bool report_written)
#else
static _Noreturn void panic_end(bool report_written)
#endif
{
#ifdef DEBUG
    if (CPU_HW_DEBUGGER_ATTACHED())
    {
        CPU_HW_BREAKPOINT();
#ifdef TEST
        return;
#else
        for (;;)
        {
        }
#endif
    }
#endif
    if (report_written)
    {
        panic_uart_drain(); /* do not cut the last bytes of the report */
    }
    CPU_HW_SYSTEM_RESET();
}

#ifdef TEST
void cpu_panic(cpu_panic_source_t source, const char *reason)
#else
_Noreturn void cpu_panic(cpu_panic_source_t source, const char *reason)
#endif
{
    if (g_panic_active)
    {
        /* Fault inside the panic path: skip all output. */
        panic_end(false);
#ifdef TEST
        return;
#endif
    }

    g_panic_active = true;
    CPU_HW_DISABLE_IRQ();

    const uint32_t cfsr = SCB->CFSR;
    const uint32_t hfsr = SCB->HFSR;
    const uint32_t bfar =
        ((cfsr & CPU_CFSR_BFARVALID) != 0U) ? SCB->BFAR : 0U; /* stale otherwise */

    /* Record first: the UART is the part most likely to hang. */
    write_panic_record(source, reason, cfsr, hfsr, bfar);
    emit_panic_uart(source, reason, cfsr, hfsr, bfar);

    panic_end(true);
}

/* --------------------------------------------------------------------- */
/* Fault stub entry point (called from the assembly in cpu_fault.c)       */
/* --------------------------------------------------------------------- */

void cpu_fault_entry(uint32_t *frame, cpu_panic_source_t source)
{
    s_fault_frame = frame;
    cpu_panic(source, NULL);
}

/* ====================================================================== */
/* Test-only hooks                                                         */
/* ====================================================================== */

#ifdef TEST
void cpu_reset_for_test(void)
{
    g_panic_active = false;
    s_fault_frame = NULL;
}

const char *cpu_cfsr_cause_string_for_test(uint32_t cfsr)
{
    return cfsr_cause_string(cfsr);
}
#endif
