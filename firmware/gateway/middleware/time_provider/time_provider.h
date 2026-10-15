/**
 * @file time_provider.h
 * @brief TimeProvider middleware — unified timestamping interface (Gateway).
 *
 * Wraps RtcDriver and exposes one timestamp + sync-state struct to all
 * consumers. A passive singleton protected by an internal mutex; it has no task
 * of its own.
 *
 * This is the Gateway build of the module (B-L475E-IOT01A). The Field Device
 * counterpart lives in firmware/field-device/middleware/time_provider/ — the
 * two boards are separate CubeIDE projects, so each keeps its own copy with the
 * same public API (docs/lld/middleware/time-provider.md, "Boards: FD + GW").
 *
 * On the Gateway the sole caller of time_provider_set_time() and
 * time_provider_mark_unsynchronised() is TimeService.
 *
 * @see docs/lld/middleware/time-provider.md
 */

#ifndef TIME_PROVIDER_H
#define TIME_PROVIDER_H

#include <stdbool.h>
#include <stdint.h>

#include "health_monitor/health_monitor.h" /* health_monitor_handle_t */

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
/* Public API — ITimeProvider                                                */
/* ========================================================================= */

/**
 * @brief Initialise TimeProvider.
 *
 * Creates the internal mutex and reads the sync-state flag from RTC backup
 * register BKP0R. If the magic value is present the provider starts
 * SYNCHRONISED, otherwise UNSYNCHRONISED. Must be called after rtc_init() and
 * logger_init(), before the scheduler starts.
 *
 * @param[in] health  HealthMonitor handle for sync-state events; may be NULL, in
 *                    which case no events are pushed.
 * @return TIME_PROVIDER_ERR_OK, or TIME_PROVIDER_ERR_RTC_FAIL if the backup
 *         register cannot be read or the mutex cannot be created.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
time_provider_err_t time_provider_init(health_monitor_handle_t health);

/**
 * @brief Get the current timestamp.
 *
 * Returns uptime seconds when UNSYNCHRONISED and the RTC wall-clock epoch when
 * SYNCHRONISED. Thread-safe; never ISR-safe.
 *
 * @param[out] ts_out  Filled with epoch + sync_state on success.
 * @return TIME_PROVIDER_ERR_OK, TIME_PROVIDER_ERR_NOT_INIT,
 *         TIME_PROVIDER_ERR_NULL_ARG, or TIME_PROVIDER_ERR_RTC_FAIL if the RTC
 *         cannot be read.
 */
time_provider_err_t time_provider_get(time_provider_ts_t *ts_out);

/**
 * @brief Set the current time and mark the provider as synchronised.
 *
 * Writes the epoch to the RTC and persists the sync flag in BKP0R. Pushes
 * HEALTH_EVENT_TIME_SYNC_ACQUIRED if the previous state was UNSYNCHRONISED.
 * Rejects an epoch outside 2000-01-01..2099-12-31, which the RTC cannot hold.
 * When already SYNCHRONISED it also rejects a step larger than
 * TIME_PROVIDER_SANITY_DELTA_S (defence in depth; TimeService is the primary
 * validator). Thread-safe.
 *
 * @param[in] new_epoch  Unix epoch seconds to set.
 * @return TIME_PROVIDER_ERR_OK, TIME_PROVIDER_ERR_NOT_INIT, or
 *         TIME_PROVIDER_ERR_RTC_FAIL (RTC failure, out-of-range epoch or
 *         sanity-delta rejection; state unchanged).
 */
time_provider_err_t time_provider_set_time(uint32_t new_epoch);

/**
 * @brief Mark the provider as unsynchronised.
 *
 * Clears BKP0R. Pushes HEALTH_EVENT_TIME_SYNC_LOST if previously SYNCHRONISED.
 * Thread-safe.
 *
 * @return TIME_PROVIDER_ERR_OK or TIME_PROVIDER_ERR_NOT_INIT.
 */
time_provider_err_t time_provider_mark_unsynchronised(void);

/**
 * @brief Return the cached sync state without an RTC access.
 *
 * Returns TIME_SYNC_UNSYNCHRONISED when called before init. Not ISR-safe.
 *
 * @return Current time_sync_state_t value.
 */
time_sync_state_t time_provider_get_sync_state(void);

/* ========================================================================= */
/* Singleton vtable — ITimeProvider (LLD-D10)                                */
/* ========================================================================= */

typedef struct
{
    /* cppcheck-suppress unusedStructMember */
    time_provider_err_t (*init)(health_monitor_handle_t health);
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

#ifdef TEST
/** @brief Test hook — return the module to its post-bss state. */
void time_provider_reset_for_test(void);
#endif

#endif /* TIME_PROVIDER_H */
