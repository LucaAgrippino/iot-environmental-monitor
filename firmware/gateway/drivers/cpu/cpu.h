#ifndef CPU_H
#define CPU_H

#include <stdbool.h>
#include <stdint.h>

#include "status.h"

/* ====================================================================== */
/* Types                                                                   */
/* ====================================================================== */

/**
 * @brief Panic source identifiers.
 *
 * Stored in RTC backup register BKP13R so the source survives a reset and
 * can be reported on the next boot.
 */
typedef enum
{
    CPU_PANIC_HARDFAULT = 0x01u,      /**< Cortex-M4 HardFault (escalated). */
    CPU_PANIC_BUSFAULT = 0x02u,       /**< BusFault (before escalation). */
    CPU_PANIC_MEMMANAGE = 0x03u,      /**< MemManage fault. */
    CPU_PANIC_USAGEFAULT = 0x04u,     /**< UsageFault. */
    CPU_PANIC_ASSERT = 0x05u,         /**< CPU_ASSERT() macro triggered. */
    CPU_PANIC_STACK_OVERFLOW = 0x06u, /**< FreeRTOS stack-overflow hook. */
    CPU_PANIC_MALLOC_FAILED = 0x07u,  /**< FreeRTOS malloc-failed hook. */
    CPU_PANIC_WATCHDOG = 0x08u,       /**< Independent watchdog timeout. */
    CPU_PANIC_USER = 0x09u,           /**< Application-defined panic. */
} cpu_panic_source_t;

/* ====================================================================== */
/* Public API                                                              */
/* ====================================================================== */

/**
 * @brief Initialise the MCU platform.
 *
 * Configures the clock tree (MSI → PLL → 80 MHz SYSCLK), sets Flash wait
 * states to 4 WS, enables the DWT cycle counter, then checks the RTC backup
 * registers for a post-mortem panic record from the previous boot.  If a
 * record is found it is formatted and emitted over the debug UART, then
 * cleared.
 *
 * Must be the first call in main(), before any driver or RTOS initialisation.
 *
 * @return STATUS_OK on success.
 * @return STATUS_ERR_TIMEOUT if the PLL fails to lock within the guard window.
 * @return STATUS_ERR_HW if the DWT cycle counter is not present.
 */
status_t cpu_init(void);

/**
 * @brief Blocking delay in microseconds.
 *
 * Uses the DWT cycle counter.  Accuracy: ±1 cycle (12.5 ns at 80 MHz).
 * Safe to call from ISR context; does not depend on the RTOS scheduler.
 *
 * @param[in] us  Delay duration in microseconds.
 */
void cpu_delay_us(uint32_t us);

/**
 * @brief Blocking delay in milliseconds.
 *
 * Delegates to cpu_delay_us(ms × 1000).  For delays longer than ~50 ms
 * prefer vTaskDelay() when the scheduler is running.
 *
 * @param[in] ms  Delay duration in milliseconds.
 */
void cpu_delay_ms(uint32_t ms);

/**
 * @brief Return the SYSCLK frequency in Hz.
 * @return 80 000 000 after a successful cpu_init().
 */
uint32_t cpu_get_sysclk_hz(void);

/**
 * @brief Return the APB1 peripheral clock frequency in Hz.
 * @return 80 000 000 (no prescaler on the L475 at 80 MHz).
 */
uint32_t cpu_get_pclk1_hz(void);

/**
 * @brief Return the APB2 peripheral clock frequency in Hz.
 * @return 80 000 000 (no prescaler on the L475 at 80 MHz).
 */
uint32_t cpu_get_pclk2_hz(void);

/**
 * @brief Perform a software system reset.
 *
 * Wraps NVIC_SystemReset().  Does not return.  Absorbs the ResetDriver
 * responsibility for the Gateway.
 */
#ifdef TEST
void cpu_reset(void);
#else
_Noreturn void cpu_reset(void);
#endif

/**
 * @brief Panic: emit diagnostics, persist a post-mortem record, then halt
 *        (DEBUG) or reset (release).
 *
 * - Disables interrupts immediately on entry.
 * - Writes a human-readable report to the debug UART (busy-wait).
 * - Stores a compact record in RTC backup registers (survives reset).
 * - DEBUG builds halt at a breakpoint; release builds reset after the UART
 *   drains.
 *
 * Idempotent: a second entry (recursive fault) skips output and goes
 * directly to halt/reset.  Safe to call from ISR context.
 *
 * @param[in] source  Panic source identifier.
 * @param[in] reason  Human-readable reason string (may be NULL).
 */
#ifdef TEST
void cpu_panic(cpu_panic_source_t source, const char *reason);
#else
_Noreturn void cpu_panic(cpu_panic_source_t source, const char *reason);
#endif

/* ====================================================================== */
/* Assert macro                                                            */
/* ====================================================================== */

#define CPU_STRINGIFY_(x) #x
#define CPU_STRINGIFY(x) CPU_STRINGIFY_(x)

#ifdef DEBUG
/**
 * @brief Assert macro — calls cpu_panic() on failure.
 *
 * Compiled out in release builds unless CPU_ASSERT_IN_RELEASE is defined.
 */
#define CPU_ASSERT(expr)                                                                           \
    do                                                                                             \
    {                                                                                              \
        if (!(expr))                                                                               \
        {                                                                                          \
            cpu_panic(CPU_PANIC_ASSERT, #expr " @ " __FILE__ ":" CPU_STRINGIFY(__LINE__));         \
        }                                                                                          \
    } while (0)
#else
#define CPU_ASSERT(expr) ((void) 0)
#endif

/* ====================================================================== */
/* Test-only hooks                                                         */
/* ====================================================================== */

#ifdef TEST
/**
 * @brief Reset module static state for unit tests.
 *
 * Clears g_sysclk_hz, g_pclk1_hz, g_pclk2_hz, g_panic_active, and
 * s_fault_frame to their post-BSS values so each Unity test case starts
 * from a clean slate.
 */
void cpu_reset_for_test(void);

/**
 * @brief Expose the internal CFSR cause-string lookup for TC-CPU-027.
 *
 * @param[in] cfsr  A CFSR value with exactly one fault bit set.
 * @return  Human-readable cause string, or "Unknown fault" if no match.
 */
const char *cpu_cfsr_cause_string_for_test(uint32_t cfsr);

/**
 * @brief Entry point called from assembly fault trampolines in cpu_fault.c.
 *
 * Saves the Cortex-M4 stacked exception frame so cpu_panic() can include
 * PC, LR, and general-purpose registers in the post-mortem record.
 *
 * @param[in] frame  Pointer to the stacked exception frame pushed by hardware:
 *                   [R0, R1, R2, R3, R12, LR, PC, xPSR].
 */
void cpu_fault_entry(uint32_t *frame);
#endif

#endif /* CPU_H */
