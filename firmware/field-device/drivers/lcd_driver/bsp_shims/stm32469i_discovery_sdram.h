/*
 * SHADOW of vendor/stm32/f469-disco/Drivers/BSP/STM32469I-Discovery/
 *           stm32469i_discovery_sdram.h.
 *
 * The vendored BSP file is intentionally NOT present in the vendor tree
 * (see companion §3.4). This file satisfies the BSP_LCD include
 * dependency and delegates the BSP_SDRAM_* contract to our SDRAM driver.
 *
 * Include path order in CubeIDE places bsp_shims/ first; this file
 * is therefore the only one the preprocessor finds for the include
 * "stm32469i_discovery_sdram.h".
 *
 * See bsp_shims/README.md for the rationale and include-path ordering
 * rules.
 */
#ifndef STM32469I_DISCOVERY_SDRAM_H
#define STM32469I_DISCOVERY_SDRAM_H

#include <stdint.h>

#include "sdram_driver/sdram_driver.h"

/* The BSP uses this as the framebuffer base. Resolves to the same macro
 * defined in sdram_driver.h — single source of truth at compile time. */
#define LCD_FB_START_ADDRESS SDRAM_BASE_ADDR

/* Status codes matching ST BSP convention. */
#define SDRAM_OK ((uint8_t) 0x00U)
#define SDRAM_ERROR ((uint8_t) 0x01U)

/* BSP entry points — shimmed in stm32469i_discovery_sdram.c. */
uint8_t BSP_SDRAM_Init(void);
uint8_t BSP_SDRAM_DeInit(void);
uint8_t BSP_SDRAM_ReadData(uint32_t uwStartAddress, uint32_t *pData, uint32_t uwDataSize);
uint8_t BSP_SDRAM_WriteData(uint32_t uwStartAddress, const uint32_t *pData, uint32_t uwDataSize);

#endif /* STM32469I_DISCOVERY_SDRAM_H */
