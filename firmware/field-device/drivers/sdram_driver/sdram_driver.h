/**
 * @file sdram_driver.h
 * @brief SdramDriver public API — FMC SDRAM Bank 1 initialisation.
 *
 * Initialises the FMC controller for the 128 Mbit (16 MB) external SDRAM
 * (ISSI IS42S32400F-6BL) and exposes the memory-mapped base address at
 * 0xC000_0000 to the caller (LcdDriver).
 *
 * Call once from main() before lcd_init() and before vTaskStartScheduler().
 * No FreeRTOS dependency — no RTOS primitives are used.
 *
 * Traces to REQ-LD-010.
 */

#ifndef SDRAM_DRIVER_H
#define SDRAM_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

#include "stm32f469xx.h"

/* ===================================================================== */
/* Test visibility macro                                                 */
/* ===================================================================== */

#ifdef TEST
#define SDRAM_TEST_VISIBLE
#else
#define SDRAM_TEST_VISIBLE static
#endif

/* ===================================================================== */
/* Public types                                                          */
/* ===================================================================== */

typedef enum
{
    SDRAM_ERR_OK = 0,
    SDRAM_ERR_TIMEOUT = 1, /**< FMC BUSY flag did not clear within timeout. */
} sdram_err_t;

/* ===================================================================== */
/* Public API                                                            */
/* ===================================================================== */

/**
 * @brief Initialise the FMC controller for SDRAM.
 *
 * Configures FMC SDRAM Bank 1 (SDNE0), sets SDCR and SDTR timing
 * registers, issues the initialisation command sequence (clock enable,
 * precharge all, two auto-refresh cycles, mode register set), and
 * configures the auto-refresh timer (SDRTR).
 *
 * After this call, the SDRAM address space at 0xC000_0000 is accessible
 * as memory-mapped RAM. The caller (LcdDriver) may then obtain the
 * framebuffer base address via sdram_get_base_addr().
 *
 * Must be called once from main() before lcd_init(). No FreeRTOS.
 *
 * @return SDRAM_ERR_OK on success; SDRAM_ERR_TIMEOUT if the FMC
 *         status register BUSY flag does not clear.
 * @note   Threading: task-context only, non-blocking. Must be called
 *         before the scheduler starts.
 */
sdram_err_t sdram_init(void);

/**
 * @brief Return the memory-mapped base address of the SDRAM.
 *
 * Returns the constant base address 0xC000_0000 (FMC SDRAM Bank 1,
 * selected by SDNE0). LcdDriver uses this to locate the framebuffer.
 *
 * Must only be called after a successful sdram_init().
 *
 * @return 0xC000_0000.
 * @note   Threading: task-context only, non-blocking. Not ISR-safe.
 */
uint32_t sdram_get_base_addr(void);

/* ===================================================================== */
/* Test-only API                                                         */
/* ===================================================================== */

#ifdef TEST
extern bool s_initialised; /**< Exposed for test assertions. */
/**
 * @brief Reset driver to uninitialised state (test use only).
 */
void sdram_driver_reset(void);
#endif /* TEST */

#endif /* SDRAM_DRIVER_H */
