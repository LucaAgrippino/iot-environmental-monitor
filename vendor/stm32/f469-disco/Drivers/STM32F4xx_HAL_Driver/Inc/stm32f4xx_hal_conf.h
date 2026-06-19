/**
 * @file stm32f4xx_hal_conf.h
 * @brief HAL configuration — project-authored, not a verbatim ST file.
 *
 * Enables only the HAL modules required by BSP_LCD_Init and the LCD driver.
 * Adding any other HAL module enable is a PR review item (companion §3.5).
 *
 * Location: vendor/stm32/f469-disco/Drivers/STM32F4xx_HAL_Driver/Inc/
 * This is the ONLY file under vendor/stm32/f469-disco/ that differs from
 * the original ST SDK. All other files must be verbatim ST source.
 */

#ifndef STM32F4XX_HAL_CONF_H
#define STM32F4XX_HAL_CONF_H

/* ===================================================================== */
/* Module selection — enable only what BSP_LCD needs                    */
/* ===================================================================== */

#define HAL_MODULE_ENABLED
#define HAL_DSI_MODULE_ENABLED
#define HAL_LTDC_MODULE_ENABLED
#define HAL_RCC_MODULE_ENABLED
#define HAL_CORTEX_MODULE_ENABLED
#define HAL_GPIO_MODULE_ENABLED /* BSP uses HAL_GPIO_Init for LTDC AF pins. */

/* SysTick tick frequency (Hz). Not used by our tick override but required
 * by the HAL headers' VDD/sysclk range checks. */
#define HAL_TICK_FREQ_DEFAULT HAL_TICK_FREQ_1KHZ

/* ===================================================================== */
/* External oscillator values                                           */
/* ===================================================================== */

#define HSE_VALUE    ((uint32_t)8000000U) /**< F469-DISCO external crystal. */
#define HSE_STARTUP_TIMEOUT ((uint32_t)100U)
#define HSI_VALUE    ((uint32_t)16000000U)
#define LSE_VALUE    ((uint32_t)32768U)
#define LSE_STARTUP_TIMEOUT ((uint32_t)5000U)
#define LSI_VALUE    ((uint32_t)32000U)
#define EXTERNAL_CLOCK_VALUE ((uint32_t)12288000U)

/* ===================================================================== */
/* Pull in the HAL master header                                        */
/* ===================================================================== */

#include "stm32f4xx_hal_def.h"

#endif /* STM32F4XX_HAL_CONF_H */
