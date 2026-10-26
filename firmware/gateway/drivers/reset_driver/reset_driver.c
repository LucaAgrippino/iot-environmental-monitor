/**
 * @file reset_driver.c
 * @brief ResetDriver implementation (Gateway).
 *
 * @see docs/lld/drivers/reset-driver.md
 */

#include "reset_driver.h"

#include "stm32l475xx.h"

#include "cpu/cpu_hw.h"

void reset_trigger(void)
{
    /* Let any pending write (a flag just stored, a debug GPIO) drain before the reset. */
    CPU_HW_DATA_BARRIER();
    CPU_HW_SYSTEM_RESET();
}
