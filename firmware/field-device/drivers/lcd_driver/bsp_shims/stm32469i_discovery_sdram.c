/**
 * @file stm32469i_discovery_sdram.c
 * @brief SDRAM BSP shim — satisfies BSP_LCD's SDRAM dependency without
 *        invoking ST's SDRAM init (which would re-initialise our already-
 *        running FMC controller).
 *
 * BSP_SDRAM_Init delegates to sdram_is_ready() rather than repeating the
 * FMC command sequence.  Read/write helpers memcpy against the memory-mapped
 * region; no register interaction is needed.
 *
 * See companion §4.3 and §4.4 for the design rationale.
 */

#include "stm32469i_discovery_sdram.h"
#include "sdram_driver/sdram_driver.h"

#include <string.h>

uint8_t BSP_SDRAM_Init(void)
{
    /* Our SDRAM driver completed init before lcd_init() ran.
     * Report success without re-issuing the FMC command sequence. */
    return sdram_is_ready() ? SDRAM_OK : SDRAM_ERROR;
}

uint8_t BSP_SDRAM_DeInit(void)
{
    /* Never called by BSP_LCD; provided for completeness. */
    return SDRAM_OK;
}

uint8_t BSP_SDRAM_ReadData(uint32_t uwStartAddress, uint32_t *pData, uint32_t uwDataSize)
{
    /* SDRAM is memory-mapped; a plain copy is sufficient. */
    (void) memcpy(pData, (const void *) (uintptr_t) uwStartAddress, uwDataSize * sizeof(uint32_t));
    return SDRAM_OK;
}

uint8_t BSP_SDRAM_WriteData(uint32_t uwStartAddress, const uint32_t *pData, uint32_t uwDataSize)
{
    (void) memcpy((void *) (uintptr_t) uwStartAddress, pData, uwDataSize * sizeof(uint32_t));
    return SDRAM_OK;
}
