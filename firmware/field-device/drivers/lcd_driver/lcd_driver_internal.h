/**
 * @file lcd_driver_internal.h
 * @brief LcdDriver private header — BSP dependency and constants.
 *
 * In production builds: includes real ST BSP and SDRAM headers.
 * In test builds: provides forward declarations so lcd_driver.c compiles
 * without the vendor tree; stub implementations live in test_lcd_driver.c.
 *
 * This header may ONLY be included by:
 *   - firmware/field-device/drivers/lcd_driver/lcd_driver.c
 *   - firmware/field-device/drivers/lcd_driver/bsp_shims/ (.c files)
 */

#ifndef LCD_DRIVER_INTERNAL_H
#define LCD_DRIVER_INTERNAL_H

#ifdef TEST

/* ------------------------------------------------------------------ */
/* Test-build shim — BSP forward declarations + constants.            */
/* Implementations are inline stubs in test_lcd_driver.c.            */
/* ------------------------------------------------------------------ */

#include <stdint.h>

#define LCD_ORIENTATION_LANDSCAPE (0x01U)
#define LCD_RELOAD_VERTICAL_BLANKING (0x02U)

uint8_t BSP_LCD_Init(uint32_t orientation);
uint8_t BSP_LCD_Reload(uint32_t reload_type);

/** Forward declaration — stub defined inline in test TU. */
uint32_t sdram_get_base_addr(void);

/** Compile-time constant matching the stub's return value. */
#define SDRAM_BASE_ADDR ((uint32_t) 0xC0000000U)

#else

/* ------------------------------------------------------------------ */
/* Production build — real BSP and SDRAM headers.                     */
/* Include path order (CubeIDE) places bsp_shims/ first so the SDRAM  */
/* shim shadows vendor/BSP/STM32469I-Discovery/stm32469i_discovery_   */
/* sdram.h (see companion §3.6).                                      */
/* ------------------------------------------------------------------ */

#include "stm32469i_discovery_lcd.h"
#include "sdram_driver/sdram_driver.h"

#endif /* TEST */

#endif /* LCD_DRIVER_INTERNAL_H */
