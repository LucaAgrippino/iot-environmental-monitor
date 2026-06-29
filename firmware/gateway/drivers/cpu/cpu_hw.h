#ifndef CPU_HW_H
#define CPU_HW_H

#include <stdint.h>

/**
 * @brief Mockable indirection layer for CPU-level CMSIS intrinsics.
 *
 * In firmware builds the macros expand to real ARM CMSIS calls.  In host
 * unit-test builds they expand to instrumented stub functions defined in
 * stm32l475_cmsis_mock.c, allowing Unity tests to verify that
 * interrupt-disable, breakpoint, and system-reset code paths are exercised
 * without executing privileged instructions on the host.
 */

#ifdef TEST

/* --------------------------------------------------------------------- */
/* Test-mode prototypes (implementations in stm32l475_cmsis_mock.c)       */
/* --------------------------------------------------------------------- */

void cpu_hw_disable_irq(void);
void cpu_hw_breakpoint(void);
void cpu_hw_system_reset(void);

/** Call counters — test TUs read these after exercising the module. */
extern uint32_t g_cpu_hw_disable_irq_count;
extern uint32_t g_cpu_hw_breakpoint_count;
extern uint32_t g_cpu_hw_system_reset_count;

#define CPU_HW_DISABLE_IRQ() cpu_hw_disable_irq()
#define CPU_HW_BREAKPOINT() cpu_hw_breakpoint()
#define CPU_HW_SYSTEM_RESET() cpu_hw_system_reset()

#else /* TARGET BUILD */

/* --------------------------------------------------------------------- */
/* Real CMSIS intrinsics — available once stm32l475xx.h is included       */
/* --------------------------------------------------------------------- */

#define CPU_HW_DISABLE_IRQ() __disable_irq()
#define CPU_HW_BREAKPOINT() __BKPT(0)
#define CPU_HW_SYSTEM_RESET() NVIC_SystemReset()

#endif /* TEST */

#endif /* CPU_HW_H */
