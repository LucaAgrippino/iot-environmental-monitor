/**
 * @file exti_driver.h
 * @brief EXTI interrupt line configuration — sole owner of SYSCFG_EXTICRx,
 *        EXTI trigger/mask registers, and NVIC enable/disable for EXTI lines.
 *
 * Shared between both boards (Field Device and Gateway). The register
 * layout differs (STM32L475 uses IMR1/RTSR1/FTSR1/PR1; STM32F469 uses the
 * bare IMR/RTSR/FTSR/PR names) but the public API and behaviour are
 * identical; the difference is resolved at compile time in exti_driver.c
 * via STM32L475xx / STM32F469xx.
 *
 * Platform singleton (free functions), not an ADT: the MCU has exactly one
 * EXTI peripheral and one SYSCFG block.
 *
 * @note See docs/lld/drivers/exti-driver.md for the full design specification.
 */

#ifndef EXTI_DRIVER_H
#define EXTI_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

/**
 * @brief ExtiDriver result codes.
 */
typedef enum
{
    EXTI_ERR_OK = 0,            /**< Success. */
    EXTI_ERR_INVALID_ARG = 1,   /**< line > 15, or invalid port/edge value. */
    EXTI_ERR_CONFLICT = 2,      /**< Line already configured. */
    EXTI_ERR_NOT_CONFIGURED = 3 /**< enable/disable called before configure. */
} exti_err_t;

/**
 * @brief GPIO port mapped to an EXTI line via SYSCFG_EXTICRx.
 */
typedef enum
{
    EXTI_PORT_A = 0U,
    EXTI_PORT_B = 1U,
    EXTI_PORT_C = 2U,
    EXTI_PORT_D = 3U,
    EXTI_PORT_E = 4U,
    EXTI_PORT_F = 5U, /**< STM32F469 only. */
    EXTI_PORT_G = 6U, /**< STM32F469 only. */
    EXTI_PORT_H = 7U  /**< STM32L475 only. */
} exti_port_t;

/**
 * @brief EXTI trigger edge selection.
 */
typedef enum
{
    EXTI_EDGE_RISING = 0U,
    EXTI_EDGE_FALLING = 1U,
    EXTI_EDGE_BOTH = 2U
} exti_edge_t;

/**
 * @brief Configure an EXTI line for a given GPIO port and trigger edge.
 *
 * Programs SYSCFG_EXTICRx to map the line to the specified port, and sets
 * the trigger edge in EXTI RTSR/FTSR. Enables the SYSCFG clock if not
 * already enabled. Does NOT enable the interrupt (does not touch IMR or
 * NVIC).
 *
 * Call from Phase 1 (pre-scheduler).
 *
 * @param[in] line  EXTI line number (0..15).
 * @param[in] port  GPIO port to map to the EXTI line.
 * @param[in] edge  Trigger edge selection.
 * @return EXTI_ERR_OK on success; EXTI_ERR_INVALID_ARG if line > 15 or
 *         port/edge is out of range; EXTI_ERR_CONFLICT if the line is
 *         already configured.
 * @note Threading: pre-scheduler or single task context. Not ISR-safe.
 */
exti_err_t exti_configure(uint8_t line, exti_port_t port, exti_edge_t edge);

/**
 * @brief Enable the EXTI interrupt for a previously configured line.
 *
 * Sets the IMR bit and configures the NVIC priority and enable for the
 * corresponding IRQn. Caller must have called exti_configure() for this
 * line first.
 *
 * Call from Phase 2 (post-scheduler, inside the owning driver's
 * attach_callback()).
 *
 * @param[in] line           EXTI line number (0..15).
 * @param[in] nvic_priority  NVIC priority value to assign.
 * @return EXTI_ERR_OK on success; EXTI_ERR_INVALID_ARG if line > 15;
 *         EXTI_ERR_NOT_CONFIGURED if the line was not previously
 *         configured.
 * @note Threading: task-context only. Not ISR-safe.
 */
exti_err_t exti_enable(uint8_t line, uint32_t nvic_priority);

/**
 * @brief Disable the EXTI interrupt for a previously configured line.
 *
 * Clears the IMR bit and disables the NVIC for the corresponding IRQn.
 *
 * @param[in] line  EXTI line number (0..15).
 * @return EXTI_ERR_OK on success; EXTI_ERR_INVALID_ARG if line > 15;
 *         EXTI_ERR_NOT_CONFIGURED if the line was not previously
 *         configured.
 * @note Threading: task-context only. Not ISR-safe.
 */
exti_err_t exti_disable(uint8_t line);

/**
 * @brief Clear the pending flag for an EXTI line.
 *
 * Writes 1 to the corresponding bit in EXTI PR (write-1-to-clear). Called
 * from the ISR handler in stm32xxx_it.c before invoking the driver-specific
 * handler.
 *
 * @param[in] line  EXTI line number (0..15). No validation — the ISR path
 *                  must be fast and the line is known at compile time.
 * @note Threading: ISR-safe. This is the only ExtiDriver function callable
 *       from interrupt context.
 */
void exti_clear_pending(uint8_t line);

#ifdef TEST
/**
 * @brief Reset all internal state for unit testing.
 *
 * Clears the configured-lines bitmap so a previously conflicting line can
 * be re-configured within the next test case. Guarded by TEST — never
 * compiled into production firmware.
 */
void exti_reset_for_test(void);
#endif

#endif /* EXTI_DRIVER_H */
