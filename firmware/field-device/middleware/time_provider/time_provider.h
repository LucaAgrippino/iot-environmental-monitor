/**
 * @file time_provider.h
 * @brief TimeProvider middleware — unified timestamping interface (ITimeProvider).
 *
 * Wraps RtcDriver and exposes a single timestamp + sync-state struct to all
 * consumers. Operates as a passive singleton protected by an internal mutex.
 *
 * Boards: Field Device (STM32F469I-DISCO) and Gateway (B-L475E-IOT01A).
 *
 * @see docs/lld/middleware/time-provider.md
 */

#ifndef TIME_PROVIDER_H
#define TIME_PROVIDER_H

#include <stdint.h>
#include <stdbool.h>

/* ========================================================================= */
/* Error codes                                                               */
/* ========================================================================= */

typedef enum
{
    TIME_PROVIDER_ERR_OK = 0,
    TIME_PROVIDER_ERR_NOT_INIT = 1,
    TIME_PROVIDER_ERR_RTC_FAIL = 2,
    TIME_PROVIDER_ERR_NULL_ARG = 3,
} time_provider_err_t;

/* ========================================================================= */
/* Sync state                                                                */
/* ========================================================================= */

/**
 * @brief Time synchronisation state.
 *
 * Also declared in health_monitor.h (forward copy). The TIME_SYNC_STATE_DEFINED
 * guard prevents redefinition when both headers are included.
 */
#ifndef TIME_SYNC_STATE_DEFINED
#define TIME_SYNC_STATE_DEFINED
typedef enum
{
    TIME_SYNC_UNSYNCHRONISED = 0,
    TIME_SYNC_SYNCHRONISED = 1,
} time_sync_state_t;
#endif /* TIME_SYNC_STATE_DEFINED */

/* ========================================================================= */
/* Timestamp                                                                 */
/* ========================================================================= */

/**
 * @brief Timestamped value returned by time_provider_get().
 *
 * When sync_state == TIME_SYNC_SYNCHRONISED, epoch is Unix epoch seconds.
 * When TIME_SYNC_UNSYNCHRONISED, epoch is uptime seconds since last reset.
 * Consumers MUST check sync_state before interpreting epoch (REQ-TS-040).
 */
typedef struct
{
    /* cppcheck-suppress unusedStructMember */
    uint32_t epoch;
    /* cppcheck-suppress unusedStructMember */
    time_sync_state_t sync_state;
} time_provider_ts_t;

/* ========================================================================= */
/* IHealthReport forward declaration                                         */
/* ========================================================================= */

/* In production builds include health_monitor.h for the full ihealth_report_t
 * definition. In test builds only a forward declaration is provided here;
 * the test TU includes health_monitor_stub.h to complete the struct body
 * before instantiating any ihealth_report_t. This keeps health_monitor.c
 * and its LED/driver dependencies out of the test link unit. */
#ifndef TEST
#include "health_monitor/health_monitor.h"
#else
struct ihealth_report_s;
typedef struct ihealth_report_s ihealth_report_t;
#endif /* TEST */

/* ========================================================================= */
/* Public API — ITimeProvider                                                */
/* ========================================================================= */

/**
 * @brief Initialise TimeProvider.
 *
 * Reads the sync-state backup register (BKP0R). Sets initial sync_state
 * to SYNCHRONISED if the magic value is present, otherwise UNSYNCHRONISED.
 * Must be called after rtc_init() and logger_init(), before the scheduler.
 *
 * @param  health  IHealthReport handle for sync-state event reporting.
 * @return TIME_PROVIDER_ERR_OK or TIME_PROVIDER_ERR_RTC_FAIL.
 */
time_provider_err_t time_provider_init(const ihealth_report_t *health);

/**
 * @brief Get the current timestamp.
 *
 * Thread-safe. Never ISR-safe. Returns uptime seconds when UNSYNCHRONISED,
 * RTC wall-clock epoch when SYNCHRONISED.
 *
 * @param[out] ts_out  Filled with epoch + sync_state on success.
 * @return TIME_PROVIDER_ERR_OK, TIME_PROVIDER_ERR_NOT_INIT, or
 *         TIME_PROVIDER_ERR_NULL_ARG.
 */
time_provider_err_t time_provider_get(time_provider_ts_t *ts_out);

/**
 * @brief Set the current time and mark the provider as synchronised.
 *
 * Writes epoch to the RTC and persists the sync flag in BKP0R. Pushes
 * HEALTH_EVENT_TIME_SYNC_ACQUIRED if the previous state was UNSYNCHRONISED.
 * Applies a delta sanity check when already synchronised (defence-in-depth).
 * Thread-safe.
 *
 * @param  new_epoch  Unix epoch seconds to set.
 * @return TIME_PROVIDER_ERR_OK or TIME_PROVIDER_ERR_RTC_FAIL.
 */
time_provider_err_t time_provider_set_time(uint32_t new_epoch);

/**
 * @brief Mark the provider as unsynchronised.
 *
 * Clears BKP0R. Pushes HEALTH_EVENT_TIME_SYNC_LOST if previously SYNCHRONISED.
 * Thread-safe.
 *
 * @return TIME_PROVIDER_ERR_OK.
 */
time_provider_err_t time_provider_mark_unsynchronised(void);

/**
 * @brief Return the current sync state without an RTC read.
 *
 * Returns the cached state under mutex. Not ISR-safe. Not callable before init
 * (returns TIME_SYNC_UNSYNCHRONISED if called before init as a safe default).
 *
 * @return Current time_sync_state_t value.
 */
time_sync_state_t time_provider_get_sync_state(void);

/* ========================================================================= */
/* Singleton vtable — ITimeProvider (LLD-D10)                               */
/* ========================================================================= */

typedef struct
{
    /* cppcheck-suppress unusedStructMember */
    time_provider_err_t (*init)(const ihealth_report_t *health);
    /* cppcheck-suppress unusedStructMember */
    time_provider_err_t (*get)(time_provider_ts_t *ts_out);
    /* cppcheck-suppress unusedStructMember */
    time_provider_err_t (*set_time)(uint32_t new_epoch);
    /* cppcheck-suppress unusedStructMember */
    time_provider_err_t (*mark_unsynchronised)(void);
    /* cppcheck-suppress unusedStructMember */
    time_sync_state_t (*get_sync_state)(void);
} itime_provider_t;

/** Singleton pointer to the TimeProvider vtable (FD + GW). */
extern const itime_provider_t *const time_provider;

/* ========================================================================= */
/* Test-only visibility macro                                                */
/* ========================================================================= */

#ifdef TIME_PROVIDER_TEST_VISIBLE
#undef TIME_PROVIDER_TEST_VISIBLE
#endif

#ifdef TEST
#define TIME_PROVIDER_TEST_VISIBLE
#else
#define TIME_PROVIDER_TEST_VISIBLE static
#endif

/* ========================================================================= */
/* Test-only hooks                                                           */
/* ========================================================================= */

#ifdef TEST
/**
 * @brief Reset module state for unit tests (clears s_tp to BSS values).
 */
void time_provider_reset_for_test(void);
#endif /* TEST */

#endif /* TIME_PROVIDER_H */
