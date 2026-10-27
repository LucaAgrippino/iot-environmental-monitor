/**
 * @file update_service_types.h
 * @brief Error codes, command/request/report structures, reasons and constants of UpdateService.
 *
 * Kept apart from update_service.h so that the three interface headers can share these types
 * without including the creation API.
 *
 * @see docs/lld/application/update-service-lld.md §8
 */

#ifndef UPDATE_SERVICE_TYPES_H
#define UPDATE_SERVICE_TYPES_H

#include <stdbool.h>
#include <stdint.h>

#include "firmware_store/firmware_store_types.h"

/** @brief Opaque UpdateService instance, the first parameter of every operation. */
typedef struct update_service update_service_t;

/** @brief UpdateService error codes. Codes 0..3 follow the project-wide ADT convention. */
typedef enum
{
    UPDATE_SERVICE_ERR_OK = 0,
    UPDATE_SERVICE_ERR_NULL_POINTER = 1,
    UPDATE_SERVICE_ERR_NOT_INITIALISED = 2,
    UPDATE_SERVICE_ERR_NO_INSTANCE = 3,
    UPDATE_SERVICE_ERR_BAD_PARAM = 4,
    UPDATE_SERVICE_ERR_BAD_STATE = 5,
    UPDATE_SERVICE_ERR_BUSY = 6,
    UPDATE_SERVICE_ERR_STALE = 7
} update_service_err_t;

/* ---------------------------------------------------------------------- */
/* Constants (§8.3)                                                        */
/* ---------------------------------------------------------------------- */

#define US_CHUNK_SIZE (1024U)          /**< Bytes per chunk request.                          */
#define US_CHUNK_TIMEOUT_MS (5000U)    /**< Wait for one chunk.                               */
#define US_MAX_RETRIES (3U)            /**< Consecutive failures while connected (REQ-DM-052). */
#define US_STALL_LIMIT_MS (600000UL)   /**< No progress for this long: fail (US-D7).          */
#define US_START_TIMEOUT_MS (360000UL) /**< LifecycleController has this long to call start.  */
#define US_SELF_CHECK_TIMEOUT_MS (60000UL) /**< Self-check budget, includes MQTT reconnect.    */
#define US_SELF_CHECK_POLL_MS (500U)       /**< Probe and link poll period.                       */
#define US_REPORT_RETRY_MS (100U)          /**< Transport queue full: wait, then retry.           */
#define US_REPORT_RETRY_MAX (20U)          /**< Attempts before the report is given up.           */
#define US_EVENT_RETRY_MS (1000U)          /**< A lifecycle event that was not accepted.          */

/** Smallest file: header (128 B) plus one byte. Largest: header plus a full 480 KB slot. */
#define US_FILE_MIN (129UL)
#define US_FILE_MAX (491648UL)

/** Job values that are never valid (0 and the erased pattern). */
#define US_JOB_INVALID_LOW (0UL)
#define US_JOB_INVALID_ERASED (0xFFFFFFFFUL)

/* ---------------------------------------------------------------------- */
/* Command, request, report (§8.1)                                         */
/* ---------------------------------------------------------------------- */

/** @brief An OTA command as CloudPublisher parsed it (§6.1). */
typedef struct
{
    uint32_t job;
    uint32_t size_a; /**< Signed file length for Slot A. */
    uint32_t size_b; /**< Signed file length for Slot B. */
} us_ota_command_t;

/** @brief One chunk request (§6.2). */
typedef struct
{
    uint32_t job;
    firmware_slot_t slot; /**< FIRMWARE_SLOT_A or _B. */
    uint32_t offset;
    uint32_t len; /**< 1 .. US_CHUNK_SIZE. */
} us_chunk_request_t;

/** @brief Outcome carried by a report (§6.4). */
typedef enum
{
    US_RESULT_ACCEPTED = 0,
    US_RESULT_REJECTED,
    US_RESULT_FAILED,
    US_RESULT_SUCCESS,
    US_RESULT_ROLLED_BACK
} us_result_t;

/** @brief Reason of a rejected or failed report (§8.4). */
typedef enum
{
    US_REASON_NONE = 0, /**< accepted, success */
    US_REASON_BUSY,
    US_REASON_NOT_OTA_CAPABLE,
    US_REASON_NOT_READY,
    US_REASON_BAD_COMMAND,
    US_REASON_START_TIMEOUT,
    US_REASON_DOWNLOAD_FAILED,
    US_REASON_STALLED,
    US_REASON_STAGING_ERROR,
    US_REASON_TOO_LARGE,
    US_REASON_BAD_HEADER,
    US_REASON_WRONG_SLOT,
    US_REASON_SIG_INVALID,
    US_REASON_HASH_MISMATCH,
    US_REASON_BAD_IMAGE,
    US_REASON_CRYPTO,
    US_REASON_VERIFY_IO,
    US_REASON_INSTALL_FAILED,
    US_REASON_ACTIVATE_FAILED,
    US_REASON_ROLLBACK
} us_reason_t;

/** @brief One result report (§6.4); CloudPublisher serialises it. */
typedef struct
{
    bool job_known;
    uint32_t job;
    us_result_t result;
    us_reason_t reason;
    uint32_t version; /**< Running version, major<<24 | minor<<16 | patch. */
    uint32_t rollback_count;
} us_ota_report_t;

/** @brief Self-check probes (§8.1). */
typedef struct
{
    void *ctx;
    bool (*sensors_ready)(void *ctx); /**< SensorService initialised and producing. */
    bool (*modbus_ready)(void *ctx);  /**< modbus_poller_is_ready() (MBP-D7).       */
} us_probe_if_t;

#endif /* UPDATE_SERVICE_TYPES_H */
