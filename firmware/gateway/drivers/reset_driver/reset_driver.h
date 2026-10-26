/**
 * @file reset_driver.h
 * @brief ResetDriver — triggers a full software reset of the MCU (Gateway).
 *
 * A stateless platform singleton: one function, no init, no handle. Reached by
 * LifecycleController (controlled restart, rollback) and, later, UpdateService (bank swap).
 *
 * @note See docs/lld/drivers/reset-driver.md for the design. The companion names the function
 *       three different ways; the API block in its section 2.4 (reset_trigger) is the one used.
 */

#ifndef RESET_DRIVER_H
#define RESET_DRIVER_H

/**
 * @brief Reset the MCU now.
 *
 * Issues a data synchronisation barrier, then NVIC_SystemReset(). All RAM is lost. On the target
 * this function does not return.
 *
 * Pre-condition: the caller has already written to non-volatile storage everything that must
 * survive the reset (restart flag, pending OTA flags). The driver cannot check this.
 *
 * @note Threading: task context only, non-blocking. Not ISR-safe.
 */
void reset_trigger(void);

#endif /* RESET_DRIVER_H */
