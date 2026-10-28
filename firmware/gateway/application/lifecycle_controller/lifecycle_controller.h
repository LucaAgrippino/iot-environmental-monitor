/**
 * @file lifecycle_controller.h
 * @brief LifecycleController — boot orchestrator and runtime event router (Gateway).
 *
 * Provides ILifecycle (see ilifecycle.h). Drives the Gateway's Init sub-state chain (Machine 1),
 * then routes runtime events — EditingConfig, Restarting, UpdatingFirmware, Faulted — from a
 * static FreeRTOS queue. It is also the single dispatch point for remote commands that change
 * system state (LLD-D12, LLD-D15).
 *
 * This is the Gateway build (B-L475E-IOT01A). The Field Device has its own LifecycleController with
 * a different dependency list and no Restarting / UpdatingFirmware states; the two share nothing
 * but the basename.
 *
 * @see docs/lld/application/lifecycle-controller-lld.md
 */

#ifndef LIFECYCLE_CONTROLLER_H
#define LIFECYCLE_CONTROLLER_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#ifndef TEST
#include "event_groups.h"
#endif /* TEST */

#include "ilifecycle.h"
#include "lifecycle_deps.h"

/* ======================================================================= */
/* Configuration                                                            */
/* ======================================================================= */

/** @brief EditingConfig timeout (REQ-NF-214): 5 minutes. */
#define LIFECYCLE_EDIT_TIMEOUT_MS (5U * 60U * 1000U)

/** @brief Init time budget (REQ-NF-213 TBD, LC-O2): provisional 10 s. */
#define LIFECYCLE_INIT_TIMEOUT_MS (10U * 1000U)

/** @brief Restart confirmation window (REQ-DM-020 TBD, LC-O4): provisional 30 s. */
#define LIFECYCLE_RESTART_TIMEOUT_MS (30U * 1000U)

/** Wait between flushing CloudPublisher and the reset that follows an activated update (I18). */
#define LC_OTA_RESET_GRACE_MS (2000U)

/** @brief Event queue depth (§4.2). */
#define LIFECYCLE_QUEUE_DEPTH (4U)

/** @brief Longest block on the event queue, and so the watchdog kick period (§8). */
#define LIFECYCLE_QUEUE_WAIT_MS (1000U)

/**
 * @brief Size of the configuration snapshot buffer.
 *
 * Holds the stored configuration blob during LoadingConfig and the EditingConfig snapshot. The
 * LLD sized it for CONFIG_STORE_MAX_DATA_BYTES (32 712 B, LC-O1); the real blob is config_params_t
 * plus the store header, under 600 B, and 32 KB would not fit the Gateway's 96 KB bank next to the
 * TLS heap (LC-D3). A blob larger than this makes LoadingConfig fail rather than truncate.
 */
#ifndef LIFECYCLE_CFG_SNAPSHOT_BYTES
#define LIFECYCLE_CFG_SNAPSHOT_BYTES (1024U)
#endif

/** @brief Bit set in the start gate on Operational entry (LC-O3: provisional bit 0). */
#define LIFECYCLE_START_GATE_BIT (1U << 0)

/* ======================================================================= */
/* Reset-cause detection (§13)                                              */
/* ======================================================================= */

/**
 * @brief Read RCC->CSR, clear the flags for the next boot, and classify the reset.
 *
 * Call from main() before any other initialisation and pass the result to
 * lifecycle_controller_init(). Watchdog wins over soft reset, soft reset over pin / brown-out.
 *
 * @return The classified cause; LIFECYCLE_RESET_UNKNOWN if no flag was set.
 * @note Threading: boot only, before the scheduler starts.
 */
lifecycle_reset_cause_t lifecycle_detect_reset_cause(void);

/* ======================================================================= */
/* Initialisation (§4.3)                                                    */
/* ======================================================================= */

/**
 * @brief Initialise LifecycleController.
 *
 * Copies the injected interfaces, creates the event queue, the three timers and the start gate,
 * and records the reset cause. Does not run the state machine: that starts when LifecycleTask
 * first runs.
 *
 * @param[in] reset_cause  Value from lifecycle_detect_reset_cause().
 * @param[in] deps         Injected services. Every member except @c watchdog must be non-NULL,
 *                         and so must each function pointer the controller calls.
 * @return LIFECYCLE_ERR_OK; LIFECYCLE_ERR_NULL_ARG if @p deps, a member or a function pointer is
 *         NULL.
 * @note Threading: call once, before the scheduler starts, after the services it will start.
 */
lifecycle_err_t lifecycle_controller_init(lifecycle_reset_cause_t reset_cause,
                                          const lifecycle_deps_t *deps);

/* ======================================================================= */
/* Task entry (§4.4)                                                        */
/* ======================================================================= */

/**
 * @brief FreeRTOS task body: run the Init sequence once, then the event loop forever.
 *
 * In a TEST build it runs the Init sequence if it has not run, handles at most one event and
 * returns, so a unit test can drive the state machine one step at a time (LC-O5).
 *
 * @param[in] arg  Unused.
 * @note Threading: LifecycleTask only.
 */
void lifecycle_task_body(void *arg);

/* ======================================================================= */
/* Start gate (§4.5)                                                        */
/* ======================================================================= */

/**
 * @brief The event group other tasks block on until the system is Operational.
 *
 * @return The group handle, or NULL before lifecycle_controller_init().
 * @note Threading: any task.
 */
EventGroupHandle_t lifecycle_get_start_gate(void);

/* ======================================================================= */
/* Test-only hooks (LC-O5)                                                  */
/* ======================================================================= */

#ifdef TEST
/** @brief Which timer callback lifecycle_timer_callback_for_test() returns. */
typedef enum
{
    LC_TEST_TIMER_EDIT = 0,
    LC_TEST_TIMER_INIT = 1,
    LC_TEST_TIMER_RESTART = 2,
} lc_test_timer_t;

/** @brief Zero all file-scope state. Call from setUp(). */
void lifecycle_controller_reset_for_test(void);

/** @brief The result the most recent event produced (LIFECYCLE_ERR_BAD_STATE for a refused OTA). */
lifecycle_err_t lifecycle_last_event_result_for_test(void);

/** @brief The callback registered for @p which, so a test can fire a timer expiry. */
TimerCallbackFunction_t lifecycle_timer_callback_for_test(lc_test_timer_t which);

/** @brief Number of events held back during EditingConfig. */
uint32_t lifecycle_deferred_count_for_test(void);
#endif /* TEST */

#endif /* LIFECYCLE_CONTROLLER_H */
