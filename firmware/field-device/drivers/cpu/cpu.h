#ifndef CPU_H
#define CPU_H

/**
 * @file cpu.h
 * @brief CpuDriver, Field Device (STM32F469): fault capture, panic report and software reset.
 *
 * Platform singleton with a direct API, as on the Gateway (same module, same names). The Field
 * Device subset has no clock tree or delay functions: its clock is Src/system_clock.c.
 * Design: docs/lld/drivers/cpu-driver.md.
 */

#include <stdbool.h>
#include <stdint.h>

#include "status.h"

/* ====================================================================== */
/* Types                                                                   */
/* ====================================================================== */

/**
 * @brief Panic source identifiers.
 *
 * Stored in the post-mortem record (RTC backup registers) so the source survives a reset and can
 * be reported on the next boot. Same values as the Gateway.
 */
typedef enum
{
    CPU_PANIC_HARDFAULT = 0x01u,      /**< Cortex-M4 HardFault (escalated). */
    CPU_PANIC_BUSFAULT = 0x02u,       /**< BusFault. */
    CPU_PANIC_MEMMANAGE = 0x03u,      /**< MemManage fault. */
    CPU_PANIC_USAGEFAULT = 0x04u,     /**< UsageFault. */
    CPU_PANIC_ASSERT = 0x05u,         /**< CPU_ASSERT() macro triggered. */
    CPU_PANIC_STACK_OVERFLOW = 0x06u, /**< FreeRTOS stack-overflow hook. */
    CPU_PANIC_MALLOC_FAILED = 0x07u,  /**< Reserved: the image allocates statically. */
    CPU_PANIC_WATCHDOG = 0x08u, /**< Reserved: a watchdog reset is a reset cause, not a panic. */
    CPU_PANIC_USER = 0x09u,     /**< Application-defined panic. */
} cpu_panic_source_t;

/* ====================================================================== */
/* Public API                                                              */
/* ====================================================================== */

/**
 * @brief Initialise the fault diagnostics.
 *
 * Enables the Bus, Memory and Usage fault handlers (each fault then reaches its own vector),
 * then checks the RTC backup registers for a post-mortem record from the previous boot. A valid
 * record is printed on the debug UART pin (USART3, PB10, 115 200 8N1, polled) and invalidated.
 *
 * Call it right after system_clock_init(): the baud rate assumes the 45 MHz APB1 clock.
 * Threading: once, from main(), before the scheduler.
 *
 * @return STATUS_OK (kept as status_t so the call matches the Gateway CpuDriver).
 */
status_t cpu_init(void);

/**
 * @brief Perform a software system reset.
 *
 * Data barrier, then NVIC_SystemReset(). Does not return.
 * Threading: any task or ISR.
 */
#ifdef TEST
void cpu_reset(void);
#else
_Noreturn void cpu_reset(void);
#endif

/**
 * @brief Panic: persist a post-mortem record, print the report, then halt (DEBUG) or reset.
 *
 * Disables interrupts on entry, writes the record to the RTC backup registers, writes the report
 * to the polled debug UART, and in a DEBUG build stops at a breakpoint; otherwise it waits for the
 * UART to drain and resets. Idempotent: a second entry (a fault inside the panic path) skips all
 * output and goes straight to the breakpoint or reset.
 * Threading: any context, including fault handlers and ISRs. No RTOS call, no allocation.
 *
 * @param[in] source  Panic source identifier.
 * @param[in] reason  Human-readable reason (may be NULL); only its hash is kept for the next boot.
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
 * @brief Assert macro: calls cpu_panic() on failure. Compiled out unless DEBUG is defined.
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
/** @brief Reset module static state for unit tests. */
void cpu_reset_for_test(void);

/**
 * @brief Expose the CFSR cause-string lookup.
 * @param[in] cfsr  A CFSR value with exactly one fault bit set.
 * @return Human-readable cause string, or "Unknown fault" if no match.
 */
const char *cpu_cfsr_cause_string_for_test(uint32_t cfsr);

/**
 * @brief Entry point called from the assembly fault stubs in cpu_fault.c (declared here only for
 *        the host tests; the stubs reach it by symbol name).
 *
 * @param[in] frame   Stacked exception frame [R0, R1, R2, R3, R12, LR, PC, xPSR].
 * @param[in] source  The vector that fired.
 */
void cpu_fault_entry(uint32_t *frame, cpu_panic_source_t source);
#endif

#endif /* CPU_H */
