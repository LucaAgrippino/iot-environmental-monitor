/**
 * @file console_service.h
 * @brief ConsoleService (Gateway) — three-panel ANSI console over the debug UART.
 *
 * Splits the terminal into a header (north), the log (centre) and a command panel with its prompt
 * (south), using the terminal's own scroll region so the firmware keeps no screen copy. Log lines
 * reach the centre panel through logger_set_sink(). Keystrokes come from the same port and drive
 * the system through the injected service interfaces. It owns no sensor, alarm, configuration or
 * health data: everything is pulled when drawn (P7).
 *
 * Runs in ConsoleTask (priority 1), woken by received bytes and a 1 Hz tick. The task is created by
 * whoever owns boot (LifecycleController, or a bring-up harness); this module provides
 * console_task_body().
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md
 */

#ifndef CONSOLE_SERVICE_H
#define CONSOLE_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "console_types.h"

/** Words of stack ConsoleTask needs (task-breakdown.md: 768 words / 3 KB). */
#define CONSOLE_TASK_STACK_WORDS (704U) /* D-1 F: 438 of 768 words used */
/** ConsoleTask priority (lowest application priority). */
#define CONSOLE_TASK_PRIORITY (1U)

/** @brief ConsoleService vtable. */
typedef struct iconsole_service
{
    /**
     * @brief One drain-input / update-state / render cycle.
     *
     * Blocks until a notification arrives (a received byte, the 1 Hz tick),
     * then handles all pending input and refreshes the header.
     *
     * @return CONSOLE_SERVICE_ERR_OK; CONSOLE_SERVICE_ERR_NOT_INIT before init.
     * @note Threading: ConsoleTask only.
     */
    console_service_err_t (*run_once)(void);
} iconsole_service_t;

/** Singleton accessor. */
extern const iconsole_service_t *const console_service;

/**
 * @brief Initialise ConsoleService and attach the receive path.
 *
 * Stores the injected interfaces, clears all state, creates the output lock and the 1 Hz timer
 * (stopped) and attaches the per-byte receive callback to the debug UART. Nothing is drawn yet: the
 * layout is drawn when ConsoleTask first runs.
 *
 * @param[in] deps  Injected services; every member must be non-NULL. Copied.
 * @return CONSOLE_SERVICE_ERR_OK; CONSOLE_SERVICE_ERR_NULL_ARG if @p deps or any member is NULL;
 *         CONSOLE_SERVICE_ERR_APPLY_FAILED if the UART receive path cannot be attached.
 * @note Threading: call once, before the scheduler starts, after the services it uses.
 */
console_service_err_t console_service_init(const console_deps_t *deps);

/**
 * @brief ConsoleTask entry point.
 *
 * Starts the timer, draws the layout, then loops on console_service->run_once().
 *
 * @param[in] arg  Unused.
 */
void console_task_body(void *arg);

#ifdef TEST
/** @brief Reset all module state (unit tests only). */
void console_service_reset_for_test(void);

/** @brief Run one cycle with the given notification bits instead of waiting (unit tests only). */
console_service_err_t console_service_cycle_for_test(uint32_t notify_bits);

/** @brief Feed bytes as the receive ISR would (unit tests only). */
void console_service_rx_for_test(const uint8_t *bytes, uint32_t len);

#endif /* TEST */

#endif /* CONSOLE_SERVICE_H */
