/**
 * @file itime_service.h
 * @brief ITimeService — the interface TimeService provides (vtable and sync state only).
 *
 * Split out of time_service.h so a consumer such as CloudPublisher includes the interface, not
 * the whole module header (interface-segregated headers, CP-D23).
 *
 * @note Companion: docs/lld/application/time-service-lld.md
 */

#ifndef ITIME_SERVICE_H
#define ITIME_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

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

#endif /* ITIME_SERVICE_H */
