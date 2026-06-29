/**
 * @file cpu.c
 * @brief CpuDriver implementation for the STM32L475 (Gateway).
 *
 * Platform singleton — one MCU, one clock tree, one fault handler set.
 * Documented deviation from the Gateway ADT default (see LLD companion §0).
 *
 * Consumes: CMSIS register definitions (stm32l475xx.h) — nothing else.
 * No FreeRTOS headers are included in this file; the FreeRTOS hook symbols
 * are provided as weak-override definitions using only primitive C types.
 */

#include "cpu.h"
#include "cpu_hw.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* --------------------------------------------------------------------- */
/* Configuration                                                          */
/* --------------------------------------------------------------------- */

#define CPU_SYSCLK_HZ (80000000U)
#define CPU_PCLK1_HZ (80000000U)
#define CPU_PCLK2_HZ (80000000U)

#define CPU_PANIC_MAGIC (0xDEADC0DEU)
#define CPU_PANIC_UART_BAUD (115200U)

/* BRR = PCLK2 / baud = 80 000 000 / 115 200 ≈ 694 */
#define CPU_PANIC_UART_BRR (CPU_PCLK2_HZ / CPU_PANIC_UART_BAUD)

/* AF7 = USART1 on PB6 */
#define CPU_PANIC_UART_TX_PIN (6U)
#define CPU_PANIC_UART_TX_AF (7U)

/* Polling-loop iteration limits — tight in test builds to avoid hangs. */
#ifdef TEST
#define CPU_POLL_TIMEOUT (3U)
#define CPU_DELAY_MAX_ITER (3U)
#else
#define CPU_POLL_TIMEOUT (100000U)
#define CPU_DELAY_MAX_ITER (0U) /* unused in firmware */
#endif

/* --------------------------------------------------------------------- */
/* Private state                                                          */
/* --------------------------------------------------------------------- */

static volatile uint32_t g_sysclk_hz;
static volatile uint32_t g_pclk1_hz;
static volatile uint32_t g_pclk2_hz;
static volatile bool g_panic_active;

/* Stacked exception frame — set by cpu_fault_entry() before cpu_panic(). */
static volatile uint32_t *s_fault_frame;

/* --------------------------------------------------------------------- */
/* Private helpers — UART panic output                                    */
/* --------------------------------------------------------------------- */

static void panic_uart_init(void)
{
    /* Enable GPIOB and USART1 clocks. */
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    RCC->APB2ENR |= RCC_APB2ENR_USART1EN;

    /* Configure PB6: AF7 (USART1_TX), push-pull, very high speed, no pull. */
    GPIOB->MODER &= ~(3UL << (CPU_PANIC_UART_TX_PIN * 2U));
    GPIOB->MODER |= (2UL << (CPU_PANIC_UART_TX_PIN * 2U));   /* AF mode */
    GPIOB->OTYPER &= ~(1UL << CPU_PANIC_UART_TX_PIN);        /* push-pull */
    GPIOB->OSPEEDR |= (3UL << (CPU_PANIC_UART_TX_PIN * 2U)); /* very high */
    GPIOB->PUPDR &= ~(3UL << (CPU_PANIC_UART_TX_PIN * 2U));  /* no pull */
    GPIOB->AFR[0] &= ~(0xFUL << (CPU_PANIC_UART_TX_PIN * 4U));
    GPIOB->AFR[0] |= ((uint32_t) CPU_PANIC_UART_TX_AF << (CPU_PANIC_UART_TX_PIN * 4U));

    /* Configure USART1: 115 200 baud, 8N1, TX only. */
    USART1->CR1 = 0U; /* disable while configuring */
    USART1->BRR = CPU_PANIC_UART_BRR;
    USART1->CR1 = USART_CR1_TE | USART_CR1_UE;
}

static void panic_uart_write_byte(uint8_t byte)
{
    uint32_t timeout = CPU_POLL_TIMEOUT;
    while (!(USART1->ISR & USART_ISR_TXE))
    {
        if (--timeout == 0U)
        {
            return; /* abort — already in fault handler, best-effort only */
        }
    }
    USART1->TDR = (uint32_t) byte;
}

static void panic_uart_write_str(const char *s)
{
    while (*s != '\0')
    {
        panic_uart_write_byte((uint8_t) *s);
        ++s;
    }
}

static void panic_uart_write_hex32(uint32_t value)
{
    static const char hex[] = "0123456789ABCDEF";
    panic_uart_write_str("0x");
    for (int8_t shift = 28; shift >= 0; shift -= 4)
    {
        panic_uart_write_byte((uint8_t) hex[(value >> (uint32_t) shift) & 0xFU]);
    }
}

/* --------------------------------------------------------------------- */
/* Private helpers — CFSR cause decoding                                  */
/* --------------------------------------------------------------------- */

static const struct
{
    uint32_t bit;
    const char *cause;
} s_cfsr_table[] = {
    {SCB_CFSR_IACCVIOL, "Instruction access violation"},
    {SCB_CFSR_DACCVIOL, "Data access violation"},
    {SCB_CFSR_MUNSTKERR, "MemManage unstacking error"},
    {SCB_CFSR_MSTKERR, "MemManage stacking error"},
    {SCB_CFSR_MLSPERR, "MemManage FP lazy state"},
    {SCB_CFSR_IBUSERR, "Instruction bus error"},
    {SCB_CFSR_PRECISERR, "Precise data bus error"},
    {SCB_CFSR_IMPRECISERR, "Imprecise data bus error"},
    {SCB_CFSR_UNSTKERR, "BusFault unstacking error"},
    {SCB_CFSR_STKERR, "BusFault stacking error"},
    {SCB_CFSR_LSPERR, "BusFault FP lazy state"},
    {SCB_CFSR_UNDEFINSTR, "Undefined instruction"},
    {SCB_CFSR_INVSTATE, "Invalid state (Thumb bit)"},
    {SCB_CFSR_INVPC, "Invalid PC load"},
    {SCB_CFSR_NOCP, "No coprocessor"},
    {SCB_CFSR_UNALIGNED, "Unaligned access"},
    {SCB_CFSR_DIVBYZERO, "Divide by zero"},
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
/* Private helpers — DJB2 hash                                            */
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
/* Private helpers — uptime estimate from DWT                             */
/* --------------------------------------------------------------------- */

static uint32_t uptime_ms(void)
{
    if (g_sysclk_hz == 0U)
    {
        return 0U;
    }
    return DWT->CYCCNT / (g_sysclk_hz / 1000U);
}

/* --------------------------------------------------------------------- */
/* Private helpers — panic record write and emit                          */
/* --------------------------------------------------------------------- */

static void write_panic_record(cpu_panic_source_t source, const char *reason, uint32_t cfsr,
                               uint32_t hfsr, uint32_t bfar)
{
    /* Enable backup domain write access. */
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    PWR->CR1 |= PWR_CR1_DBP;

    /* Write fault registers and frame data (BKP1R–BKP16R). */
    RTC->BKP1R = cfsr;
    RTC->BKP2R = hfsr;
    RTC->BKP3R = bfar;

    if (s_fault_frame != NULL)
    {
        RTC->BKP4R = s_fault_frame[6];  /* PC  */
        RTC->BKP5R = s_fault_frame[5];  /* LR  */
        RTC->BKP6R = 0U;                /* SP: not recoverable here */
        RTC->BKP7R = s_fault_frame[7];  /* xPSR */
        RTC->BKP8R = s_fault_frame[0];  /* R0  */
        RTC->BKP9R = s_fault_frame[1];  /* R1  */
        RTC->BKP10R = s_fault_frame[2]; /* R2  */
        RTC->BKP11R = s_fault_frame[3]; /* R3  */
        RTC->BKP12R = s_fault_frame[4]; /* R12 */
    }
    else
    {
        RTC->BKP4R = 0U;
        RTC->BKP5R = 0U;
        RTC->BKP6R = 0U;
        RTC->BKP7R = 0U;
        RTC->BKP8R = 0U;
        RTC->BKP9R = 0U;
        RTC->BKP10R = 0U;
        RTC->BKP11R = 0U;
        RTC->BKP12R = 0U;
    }

    RTC->BKP13R = (uint32_t) source;
    RTC->BKP14R = djb2_hash(reason);
    RTC->BKP15R = 0U;
    RTC->BKP16R = uptime_ms();

    /* Write magic last — atomic commit marker. */
    RTC->BKP0R = CPU_PANIC_MAGIC;
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
        panic_uart_write_str("\r\nLR:     ");
        panic_uart_write_hex32(s_fault_frame[5]);
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

/* --------------------------------------------------------------------- */
/* Private helpers — post-mortem recovery on boot                         */
/* --------------------------------------------------------------------- */

static void check_panic_record(void)
{
    if (RTC->BKP0R != CPU_PANIC_MAGIC)
    {
        return;
    }

    /* Valid record from previous boot: read and emit. */
    uint32_t cfsr = RTC->BKP1R;
    uint32_t hfsr = RTC->BKP2R;
    uint32_t bfar = RTC->BKP3R;
    uint32_t source = RTC->BKP13R;

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
    panic_uart_write_hex32(RTC->BKP4R);
    panic_uart_write_str("\r\nLR:     ");
    panic_uart_write_hex32(RTC->BKP5R);
    panic_uart_write_str("\r\n-----------------------------------\r\n");

    /* Invalidate the record so it is not replayed on the next boot. */
    RTC->BKP0R = 0U;
}

/* ====================================================================== */
/* Public API                                                              */
/* ====================================================================== */

status_t cpu_init(void)
{
    /* Step 1: Enable PWR clock; set voltage regulator to Range 1 for 80 MHz. */
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    PWR->CR1 = (PWR->CR1 & ~PWR_CR1_VOS) | PWR_CR1_VOS_0; /* Range 1 */

    /* Step 2: Set Flash to 4 WS before increasing the clock. */
    FLASH->ACR = FLASH_ACR_LATENCY_4WS | FLASH_ACR_PRFTEN | FLASH_ACR_ICEN;

    /* Step 3: Configure PLL: source = MSI, M = 1 (÷1), N = 40, R = 2 (÷2).
     *   VCO input  = MSI 4 MHz / 1 = 4 MHz
     *   VCO output = 4 MHz × 40   = 160 MHz
     *   SYSCLK     = 160 MHz / 2  = 80 MHz                              */
    RCC->PLLCFGR = RCC_PLLCFGR_PLLSRC_MSI          /* source */
                   | (0U << RCC_PLLCFGR_PLLM_Pos)  /* M = 1  */
                   | (40U << RCC_PLLCFGR_PLLN_Pos) /* N = 40 */
                   | (0U << RCC_PLLCFGR_PLLR_Pos)  /* R = 2  */
                   | RCC_PLLCFGR_PLLREN;           /* enable R output */

    /* Step 4: Enable the PLL and wait for lock. */
    RCC->CR |= RCC_CR_PLLON;
    {
        uint32_t timeout = CPU_POLL_TIMEOUT;
        while ((RCC->CR & RCC_CR_PLLRDY) == 0U)
        {
            if (--timeout == 0U)
            {
                return STATUS_ERR_TIMEOUT;
            }
        }
    }

    /* Step 5: Switch SYSCLK source to PLL and confirm. */
    RCC->CFGR = (RCC->CFGR & ~RCC_CFGR_SW) | RCC_CFGR_SW_PLL;
    {
        uint32_t timeout = CPU_POLL_TIMEOUT;
        while ((RCC->CFGR & RCC_CFGR_SWS) != RCC_CFGR_SWS_PLL)
        {
            if (--timeout == 0U)
            {
                return STATUS_ERR_TIMEOUT;
            }
        }
    }

    /* Step 6: Update internal frequency variables. */
    g_sysclk_hz = CPU_SYSCLK_HZ;
    g_pclk1_hz = CPU_PCLK1_HZ;
    g_pclk2_hz = CPU_PCLK2_HZ;

    /* Step 7: Enable the DWT cycle counter. */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA;
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA;

    if ((DWT->CTRL & DWT_CTRL_CYCCNTENA) == 0U)
    {
        return STATUS_ERR_HW; /* cycle counter not implemented on this core */
    }

    /* Step 8: Check for and report any panic record from the previous boot. */
    check_panic_record();

    return STATUS_OK;
}

void cpu_delay_us(uint32_t us)
{
    const uint32_t cycles = us * (g_sysclk_hz / 1000000U);
    const uint32_t start = DWT->CYCCNT;

#ifdef TEST
    uint32_t iter = 0U;
    while (((DWT->CYCCNT - start) < cycles) && (iter < CPU_DELAY_MAX_ITER))
    {
        ++iter;
    }
#else
    while ((DWT->CYCCNT - start) < cycles)
    {
        /* spin */
    }
#endif
}

void cpu_delay_ms(uint32_t ms)
{
    cpu_delay_us(ms * 1000U);
}

uint32_t cpu_get_sysclk_hz(void)
{
    return g_sysclk_hz;
}

uint32_t cpu_get_pclk1_hz(void)
{
    return g_pclk1_hz;
}

uint32_t cpu_get_pclk2_hz(void)
{
    return g_pclk2_hz;
}

#ifdef TEST
void cpu_reset(void)
#else
_Noreturn void cpu_reset(void)
#endif
{
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
        /* Recursive fault — skip all output and go directly to halt/reset. */
        CPU_HW_BREAKPOINT();
        CPU_HW_SYSTEM_RESET();
        return;
    }

    g_panic_active = true;
    CPU_HW_DISABLE_IRQ();

    /* Capture hardware fault status registers. */
    uint32_t cfsr = SCB->CFSR;
    uint32_t hfsr = SCB->HFSR;
    uint32_t bfar = SCB->BFAR;

    /* Emit diagnostic output over the one-shot panic UART. */
    emit_panic_uart(source, reason, cfsr, hfsr, bfar);

    /* Persist post-mortem record to RTC backup registers. */
    write_panic_record(source, reason, cfsr, hfsr, bfar);

#ifdef DEBUG
    CPU_HW_BREAKPOINT();
#else
    CPU_HW_SYSTEM_RESET();
#endif
}

/* --------------------------------------------------------------------- */
/* Fault trampoline entry point (called from assembly in cpu_fault.c)     */
/* --------------------------------------------------------------------- */

void cpu_fault_entry(uint32_t *frame)
{
    s_fault_frame = frame;
    cpu_panic(CPU_PANIC_HARDFAULT, NULL);
}

/* --------------------------------------------------------------------- */
/* FreeRTOS hooks                                                          */
/* --------------------------------------------------------------------- */

/* TaskHandle_t is typedef void * in FreeRTOS.  Using void * avoids pulling
 * in FreeRTOS headers from a driver-layer module. */
void vApplicationStackOverflowHook(void *xTask, char *pcTaskName)
{
    (void) xTask;
    cpu_panic(CPU_PANIC_STACK_OVERFLOW, pcTaskName);
}

void vApplicationMallocFailedHook(void)
{
    cpu_panic(CPU_PANIC_MALLOC_FAILED, "heap exhausted");
}

/* ====================================================================== */
/* Test-only hooks                                                         */
/* ====================================================================== */

#ifdef TEST
void cpu_reset_for_test(void)
{
    g_sysclk_hz = 0U;
    g_pclk1_hz = 0U;
    g_pclk2_hz = 0U;
    g_panic_active = false;
    s_fault_frame = NULL;
}

const char *cpu_cfsr_cause_string_for_test(uint32_t cfsr)
{
    return cfsr_cause_string(cfsr);
}
#endif
