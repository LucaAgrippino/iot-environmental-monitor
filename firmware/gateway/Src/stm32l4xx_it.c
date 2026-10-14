/**
 * @file stm32l4xx_it.c
 * @brief Gateway EXTI interrupt vectors (system layer).
 *
 * Each handler tests the pending bit of the lines it owns, clears it through
 * ExtiDriver (the sole owner of the EXTI registers) and calls the driver's ISR
 * entry point. Lines that share a vector (5..9, 10..15) are dispatched
 * independently, so a new consumer adds one `if` block here.
 *
 * Why weak: every bring-up harness under integration-tests/ may define its own
 * EXTIx_IRQHandler to instrument a single module. A strong definition there
 * replaces these. The startup file also defines these names weakly (as aliases
 * of Default_Handler); the GNU linker keeps the FIRST weak definition it meets,
 * and the generated objects.list links Src/ before Startup/ (alphabetical), so
 * these win. Re-check with `arm-none-eabi-nm gateway.elf | grep IRQHandler` if
 * the link order ever changes.
 *
 * Only lines that were configured and unmasked by ExtiDriver can raise these
 * vectors, so an unowned pending bit cannot cause a re-entry storm.
 *
 * UART4 and USART1 handlers stay in their drivers (ModbusUartDriver,
 * DebugUartDriver); SVC, PendSV and SysTick are FreeRTOS's (FreeRTOSConfig.h).
 */

#include "stm32l4xx_it.h"

#include "stm32l475xx.h"

#include "exti/exti_driver.h"
#include "imu_driver/imu_driver_internal.h"
#include "magnetometer_driver/magnetometer_driver_internal.h"
#include "wifi_driver/wifi_driver.h"

/* EXTI line numbers (UM2153 pin map; see each driver). */
#define IT_LINE_WIFI_DATARDY (1U) /* PE1  — ISM43362 DATARDY */
#define IT_LINE_MAG_DRDY (8U)     /* PC8  — LIS3MDL DRDY     */
#define IT_LINE_IMU_INT1 (11U)    /* PD11 — LSM6DSL INT1     */

static inline bool it_line_pending(uint8_t line)
{
    return (EXTI->PR1 & (1UL << line)) != 0U;
}

__attribute__((weak)) void EXTI1_IRQHandler(void)
{
    if (it_line_pending(IT_LINE_WIFI_DATARDY))
    {
        exti_clear_pending(IT_LINE_WIFI_DATARDY);
        wifi_datardy_irq_handler();
    }
}

__attribute__((weak)) void EXTI9_5_IRQHandler(void)
{
    if (it_line_pending(IT_LINE_MAG_DRDY))
    {
        exti_clear_pending(IT_LINE_MAG_DRDY);
        magnetometer_drdy_irq_handler();
    }
}

__attribute__((weak)) void EXTI15_10_IRQHandler(void)
{
    if (it_line_pending(IT_LINE_IMU_INT1))
    {
        exti_clear_pending(IT_LINE_IMU_INT1);
        imu_int1_irq_handler();
    }
}
