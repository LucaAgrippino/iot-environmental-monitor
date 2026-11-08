/**
 * @file watchdog_driver.h
 * @brief WatchdogDriver — starts and refreshes the independent watchdog (Field Device).
 *
 * A stateless platform singleton. Policy (what must be alive before a kick) belongs to the
 * caller; see docs/lld/drivers/watchdog-driver.md.
 */

#ifndef WATCHDOG_DRIVER_H
#define WATCHDOG_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

/** Result codes of watchdog_start(). */
typedef enum
{
    WATCHDOG_OK = 0,
    WATCHDOG_ERR_RANGE = 1, /**< Timeout outside WATCHDOG_TIMEOUT_MIN_MS..MAX_MS. */
} watchdog_err_t;

#define WATCHDOG_TIMEOUT_MIN_MS (100U)   /**< Shortest accepted watchdog timeout, in ms. */
#define WATCHDOG_TIMEOUT_MAX_MS (30000U) /**< Longest accepted watchdog timeout, in ms. */

/**
 * @brief Start the independent watchdog with the given timeout. It cannot be stopped.
 *
 * The timeout is nominal: the 32 kHz LSI that clocks the IWDG is accurate to about +-20 %.
 * A second call is ignored. The counter is frozen while a debugger halts the core.
 *
 * @param[in] timeout_ms  WATCHDOG_TIMEOUT_MIN_MS .. WATCHDOG_TIMEOUT_MAX_MS.
 * @return WATCHDOG_OK; WATCHDOG_ERR_RANGE (nothing started).
 * @note Threading: task context.
 */
watchdog_err_t watchdog_start(uint32_t timeout_ms);

/**
 * @brief Refresh the counter. No effect before watchdog_start().
 * @note Threading: task context.
 */
void watchdog_kick(void);

/**
 * @brief true after a successful watchdog_start().
 *
 * @return true if the watchdog is running.
 */
bool watchdog_is_running(void);

#ifdef TEST
/** @brief Forget the started state (host tests only). */
void watchdog_reset_for_test(void);
#endif

#endif /* WATCHDOG_DRIVER_H */
