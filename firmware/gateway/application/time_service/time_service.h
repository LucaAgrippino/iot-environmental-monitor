/**
 * @file time_service.h
 * @brief TimeService (Gateway) — NTP synchronisation orchestrator and ITimeService vtable.
 *
 * Synchronises the RTC from NTP at boot and periodically, rejects implausible jumps,
 * pushes the accepted time to the Field Device through ModbusPoller, and publishes a
 * three-state sync flag that CloudPublisher and SensorService read at the point of use.
 * NtpClient only transports NTP; sync policy lives here.
 *
 * The sync enumeration is time_service_sync_t, not time_sync_state_t: that name already
 * belongs to TimeProvider's two-state flag (UNSYNCHRONISED / SYNCHRONISED), and
 * TimeService adds the STALE state on top (TS-D11).
 *
 * @note Companion: docs/lld/application/time-service-lld.md
 */

#ifndef TIME_SERVICE_H
#define TIME_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

#include "ntp_client/ntp_client.h"

/* ======================================================================= */
/* Data types                                                               */
/* ======================================================================= */

/** @brief Synchronisation state of the Gateway clock. */
typedef enum
{
    TIME_SERVICE_SYNC_NEVER = 0, /**< No successful NTP sync since boot.                     */
    TIME_SERVICE_SYNC_OK = 1,    /**< The last sync attempt succeeded.                       */
    TIME_SERVICE_SYNC_STALE = 2, /**< The last attempt failed after an earlier good sync.   */
} time_service_sync_t;

/** @brief Error codes returned by TimeService operations. */
typedef enum
{
    TIME_SERVICE_OK = 0,
    TIME_SERVICE_ERR_NTP_FAILED = 1,     /**< NtpClient returned an error.                */
    TIME_SERVICE_ERR_DELTA_REJECTED = 2, /**< New time differs from the RTC by too much.  */
    TIME_SERVICE_ERR_RTC_WRITE = 3,      /**< TimeProvider rejected the new time.         */
    TIME_SERVICE_ERR_FD_PUSH = 4,        /**< The Field Device time push failed.          */
    TIME_SERVICE_ERR_NULL_ARG = 5,       /**< NULL handle passed to init.                 */
    TIME_SERVICE_ERR_NOT_INIT = 6,       /**< Service not initialised.                    */
} time_service_err_t;

/* ======================================================================= */
/* Constants                                                                */
/* ======================================================================= */

/** Largest accepted difference between NTP time and the RTC after the first sync (TS-D5). */
#define TIME_SERVICE_NTP_MAX_DELTA_S (3600U)

/** Default period between NTP syncs, used until the Gateway has an IConfigProvider (TS-O4). */
#define TIME_SERVICE_NTP_INTERVAL_S_DEFAULT (3600U)

/* ======================================================================= */
/* Provided interface — ITimeService                                        */
/* ======================================================================= */

/**
 * @brief Provided interface, consumed by CloudPublisher and SensorService.
 *
 * Consumers pass @c ctx back to every call. It is opaque and must never be cast.
 */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- opaque pointer handed back to each call */
    void *ctx;

    /**
     * @brief Read the current synchronisation state (REQ-TS-040, REQ-NF-212).
     *
     * Non-blocking: reads one volatile word.
     */
    /* cppcheck-suppress unusedStructMember -- called via vtable by CloudPublisher */
    time_service_sync_t (*get_sync_state)(void *ctx);

    /**
     * @brief Ask for an immediate NTP attempt (REQ-TS-0E1).
     *
     * Non-blocking: posts a task notification; the attempt runs in TimeServiceTask.
     */
    /* cppcheck-suppress unusedStructMember -- called via vtable by CloudPublisher */
    void (*trigger_retry)(void *ctx);
} itime_service_t;

/* ======================================================================= */
/* Public API                                                               */
/* ======================================================================= */

/**
 * @brief Initialise TimeService and create its task and periodic timer.
 *
 * TimeProvider and ModbusPoller must already be initialised. The task makes the boot
 * sync as soon as the scheduler runs (TS-D3), then starts the periodic timer.
 * Calling init again after success is a no-op that returns TIME_SERVICE_OK.
 *
 * @param[in] ntp  NtpClient instance used for every sync; must not be NULL (TS-D2).
 * @return TIME_SERVICE_OK; TIME_SERVICE_ERR_NULL_ARG if @p ntp is NULL.
 * @note Threading: call before the scheduler starts.
 */
time_service_err_t time_service_init(ntp_client_handle_t ntp);

/**
 * @brief Obtain the ITimeService vtable.
 *
 * @return The static vtable, or NULL until time_service_init() has succeeded.
 */
const itime_service_t *time_service_get_interface(void);

#ifdef TEST
/** @brief Reset all module state (unit tests only). */
void time_service_reset_for_test(void);

/** @brief Run one sync attempt exactly as the task does (unit tests only). */
time_service_err_t time_service_sync_for_test(void);

/** @brief Run the task's boot sequence: boot sync, then arm the timer (unit tests only). */
void time_service_boot_for_test(void);

/** @brief Handle one notification word as the task loop does (unit tests only). */
void time_service_notify_for_test(uint32_t bits);

/** @brief Override the NTP interval the next periodic tick will read (unit tests only). */
void time_service_set_interval_for_test(uint32_t seconds);
#endif /* TEST */

#endif /* TIME_SERVICE_H */
