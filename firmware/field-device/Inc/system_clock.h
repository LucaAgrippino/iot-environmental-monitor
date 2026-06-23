/**
 * @file system_clock.h
 * @brief F469 Discovery system clock configuration.
 *
 * Single source of truth for the clock-tree numbers used across all
 * drivers and middleware. Bus-rate constants are exposed here so
 * drivers can compute baud-rate divisors etc. without re-deriving the
 * tree.
 *
 * Closes the "PCLK values" open item in project_status.md.
 */

#ifndef SYSTEM_CLOCK_H
#define SYSTEM_CLOCK_H

#include <stdint.h>

/* --- Clock tree --------------------------------------------------------- */

#define SYSTEM_HCLK_HZ (180000000UL) /**< SYSCLK = HCLK = 180 MHz */
#define SYSTEM_PCLK1_HZ (45000000UL) /**< APB1 = HCLK / 4 = 45 MHz */
#define SYSTEM_PCLK2_HZ (90000000UL) /**< APB2 = HCLK / 2 = 90 MHz */

/* --- API ---------------------------------------------------------------- */

/**
 * @brief Switch the F469 to 180 MHz via HSE + PLL + over-drive.
 *
 * Configures, in order:
 *   - HSE enable (8 MHz crystal on the Discovery board)
 *   - Power scale 1 voltage regulator output
 *   - Flash: 5 wait states, prefetch, I-cache, D-cache enabled
 *   - AHB / APB1 / APB2 prescalers (1 / 4 / 2)
 *   - PLL: HSE × 360 / 8 / 2 = 180 MHz
 *   - Over-drive mode (required for SYSCLK > 168 MHz on F469)
 *   - SYSCLK source switch to PLL
 *
 * Must run once near the very start of main(), before any peripheral
 * driver init. Does not return on hardware failure (HSE missing,
 * PLL never locks, over-drive never ready) — blocks indefinitely on
 * the corresponding poll, so a debugger session shows where it hung.
 */
void system_clock_init(void);
void system_clock_enable_dwt(void);

#endif /* SYSTEM_CLOCK_H */
