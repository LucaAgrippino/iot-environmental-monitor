/**
 * @file circular_flash_log.h
 * @brief CircularFlashLog — ICircularFlashLog for the Gateway.
 *
 * Implements a FIFO record ring over the 1 MB CircularFlashLog QSPI
 * partition, preserving chronological order across power cycles. It is the
 * non-volatile backing store for StoreAndForward: outbound MQTT frames
 * written while the cloud link is down survive a reboot and are replayed
 * oldest-first on reconnection.
 *
 * This component owns only the mechanics — erase, write, read, record
 * tracking and the overflow event. The policy (when to enqueue, when to
 * drain, when to discard the oldest record) belongs to StoreAndForward;
 * see CFL-D2 in the companion.
 *
 * Companion: docs/lld/middleware/circular-flash-log.md
 * Requirements: REQ-BF-000, REQ-BF-010, REQ-BF-020, REQ-NF-407.
 *
 * @note Threading: task-context only. The caller serialises all access —
 *       CircularFlashLog is reached exclusively from CloudPublisherTask via
 *       StoreAndForward, so it holds no synchronisation primitive of its own
 *       (companion §8).
 */

#ifndef CIRCULAR_FLASH_LOG_H
#define CIRCULAR_FLASH_LOG_H

#include <stdbool.h>
#include <stdint.h>

/* The QspiFlashDriver vtable (iqspi_flash_t) is the injected dependency.
 * Including the driver header here is safe for test isolation: Ceedling
 * auto-links only on headers included by the test translation unit itself,
 * not on nested includes — the same arrangement wifi_task.h already uses for
 * wifi_driver.h. */
#include "qspi_flash/qspi_flash.h"

/* ====================================================================== */
/* Record format (companion §3)                                           */
/* ====================================================================== */

/** @brief Marks the start of a valid record. */
#define CFL_MAGIC (0xBEEF5A5AUL)

/** @brief Largest payload a single record can carry, in bytes. */
#define CFL_MAX_RECORD_BYTES (4078U)

/** @brief Record header size: magic + seq_num + data_len. */
#define CFL_RECORD_HEADER_SZ (12U)

/** @brief Trailing CRC-16/IBM size. */
#define CFL_RECORD_CRC_SZ (2U)

/* ====================================================================== */
/* Error codes                                                            */
/* ====================================================================== */

/** @brief Error codes returned by all CircularFlashLog operations. */
typedef enum
{
    CFL_ERR_OK = 0,          /**< Operation succeeded.                        */
    CFL_ERR_NULL_PTR = 1,    /**< A required pointer argument was NULL.       */
    CFL_ERR_NO_RESOURCE = 2, /**< Static instance pool exhausted.             */
    CFL_ERR_NOT_INIT = 3,    /**< Handle does not refer to a created instance. */
    CFL_ERR_TOO_LARGE = 4,   /**< Payload exceeds CFL_MAX_RECORD_BYTES, or the
                                  caller's peek buffer is smaller than the
                                  record (see cfl_peek_oldest()).            */
    CFL_ERR_EMPTY = 5,       /**< No records to peek, consume or drop.        */
    CFL_ERR_FULL = 6,        /**< Ring full — caller must drop_oldest first.  */
    CFL_ERR_FLASH_ERASE = 7, /**< QspiFlashDriver sector erase failed.        */
    CFL_ERR_FLASH_WRITE = 8, /**< QspiFlashDriver page program failed.        */
    CFL_ERR_FLASH_READ = 9,  /**< QspiFlashDriver read failed.                */
    CFL_ERR_CORRUPT = 10     /**< Record CRC mismatch on read.                */
} cfl_err_t;

/* ====================================================================== */
/* Placeholder dependency types — IHealthReport                           */
/*                                                                        */
/* The Gateway has no HealthMonitor implementation yet, so the handle and  */
/* the push entry point below stand in for it, following the same guarded  */
/* convention cloud_publisher.h already uses for its unbuilt GW peers.     */
/* The real health_monitor header can supersede the handle typedef without */
/* a duplicate-definition error once that module lands for GW (CFL-O5).    */
/* ====================================================================== */

#ifndef HEALTH_MONITOR_HANDLE_T_DEFINED
#define HEALTH_MONITOR_HANDLE_T_DEFINED
typedef struct health_monitor_inst *health_monitor_handle_t;
#endif /* HEALTH_MONITOR_HANDLE_T_DEFINED */

#ifndef CFL_HEALTH_EVENT_T_DEFINED
#define CFL_HEALTH_EVENT_T_DEFINED
/**
 * @brief Placeholder subset of health_event_t — the two events this
 *        component raises (companion §6.5).
 */
typedef enum
{
    CFL_HEALTH_EVENT_BUFFER_OVERFLOW = 0, /**< Oldest record discarded to make
                                               room (REQ-BF-020).            */
    CFL_HEALTH_EVENT_BUFFER_FLASH_ERR = 1 /**< Erase or write failed during an
                                               append.                       */
} cfl_health_event_t;
#endif /* CFL_HEALTH_EVENT_T_DEFINED */

/**
 * @brief Push a buffer event to HealthMonitor through IHealthReport.
 *
 * Placeholder for the GW HealthMonitor entry point. Deliberately named
 * distinctly from cloud_publisher.h's own placeholder push so that both
 * headers can appear in one translation unit without a conflicting
 * declaration.
 *
 * @param[in] handle  IHealthReport handle; a NULL handle is a no-op.
 * @param[in] event   Event to record.
 */
void health_report_push_buffer_event(health_monitor_handle_t handle, cfl_health_event_t event);

/* ====================================================================== */
/* Public API — ICircularFlashLog                                         */
/* ====================================================================== */

/** @brief Opaque handle to a CircularFlashLog instance. */
typedef struct cfl_inst *cfl_handle_t;

/**
 * @brief CircularFlashLog creation configuration.
 *
 * Both dependencies are injected (CFL-D1). @c qspi is the QspiFlashDriver
 * vtable — production code passes the driver singleton @c qspi_flash_driver;
 * unit tests pass a RAM-backed fake.
 */
typedef struct
{
    const iqspi_flash_t *qspi;      /**< QspiFlashDriver operations.        */
    health_monitor_handle_t health; /**< IHealthReport handle; may be NULL. */
} cfl_config_t;

/**
 * @brief Create and initialise a CircularFlashLog instance.
 *
 * Reads the metadata zone to recover the head sector (A/B rotation, D40),
 * then scans the data ring forward from the head to locate the tail and
 * rebuild the record and byte counts. Must be called after
 * qspi_flash_init() has succeeded.
 *
 * @param[in]  config  Injected dependencies (QSPI vtable + IHealthReport).
 * @param[out] handle  Receives the created handle on success.
 * @return CFL_ERR_OK on success; CFL_ERR_NULL_PTR if @p config, @p handle or
 *         @c config->qspi is NULL; CFL_ERR_NO_RESOURCE if the pool is
 *         exhausted; CFL_ERR_FLASH_READ if the ring or metadata read fails.
 * @note Threading: task-context only. Call before the scheduler starts.
 */
cfl_err_t cfl_create(const cfl_config_t *config, cfl_handle_t *handle);

/**
 * @brief Append a record to the tail of the ring.
 *
 * If the tail sector has insufficient space the tail advances to the next
 * sector, erasing it first. If that next sector is the head sector the ring
 * is full and CFL_ERR_FULL is returned — the caller must call
 * cfl_drop_oldest() to make room (CFL-D2; REQ-BF-020 policy lives in
 * StoreAndForward).
 *
 * @param[in] handle  CFL handle.
 * @param[in] data    Payload to store.
 * @param[in] len     Byte count, 1 .. CFL_MAX_RECORD_BYTES.
 * @return CFL_ERR_OK on success; CFL_ERR_NULL_PTR on a NULL argument;
 *         CFL_ERR_NOT_INIT if @p handle was not created; CFL_ERR_TOO_LARGE if
 *         @p len is zero or exceeds CFL_MAX_RECORD_BYTES; CFL_ERR_FULL if the
 *         ring is full; CFL_ERR_FLASH_ERASE or CFL_ERR_FLASH_WRITE on flash
 *         failure (both also push CFL_HEALTH_EVENT_BUFFER_FLASH_ERR).
 * @note Threading: task-context only. Blocks for the duration of a sector
 *       erase when the tail advances.
 */
cfl_err_t cfl_append(cfl_handle_t handle, const uint8_t *data, uint32_t len);

/**
 * @brief Read the oldest record without removing it.
 *
 * Used by StoreAndForward before publishing — the record is held until the
 * publish is confirmed, then removed with cfl_consume_oldest().
 *
 * @param[in]  handle    CFL handle.
 * @param[out] data_out  Caller buffer receiving the payload.
 * @param[out] len_out   Receives the record's data_len.
 * @param[in]  max_len   Capacity of @p data_out in bytes.
 * @return CFL_ERR_OK on success; CFL_ERR_NULL_PTR on a NULL argument;
 *         CFL_ERR_NOT_INIT if @p handle was not created; CFL_ERR_EMPTY if the
 *         ring holds no records; CFL_ERR_TOO_LARGE if the record does not fit
 *         in @p max_len; CFL_ERR_FLASH_READ on read failure; CFL_ERR_CORRUPT
 *         on a record CRC mismatch.
 */
cfl_err_t cfl_peek_oldest(cfl_handle_t handle, uint8_t *data_out, uint32_t *len_out,
                          uint32_t max_len);

/**
 * @brief Remove the oldest record, advancing the head.
 *
 * The persistent head pointer is rewritten (A/B rotation) only when the head
 * crosses into a new sector — the slot stores a sector index, so a write per
 * record would add no information and would erase a metadata sector per
 * record (CFL-O4).
 *
 * @param[in] handle  CFL handle.
 * @return CFL_ERR_OK on success; CFL_ERR_NULL_PTR if @p handle is NULL;
 *         CFL_ERR_NOT_INIT if @p handle was not created; CFL_ERR_EMPTY if the
 *         ring holds no records; CFL_ERR_FLASH_READ, CFL_ERR_FLASH_ERASE or
 *         CFL_ERR_FLASH_WRITE on flash failure.
 */
cfl_err_t cfl_consume_oldest(cfl_handle_t handle);

/**
 * @brief Drop the oldest record to free space for a new append.
 *
 * Identical to cfl_consume_oldest() but additionally pushes
 * CFL_HEALTH_EVENT_BUFFER_OVERFLOW. Called by StoreAndForward when the ring
 * is full (REQ-BF-020).
 *
 * @param[in] handle  CFL handle.
 * @return As cfl_consume_oldest().
 */
cfl_err_t cfl_drop_oldest(cfl_handle_t handle);

/**
 * @brief Query whether the ring is empty.
 *
 * @param[in]  handle     CFL handle.
 * @param[out] empty_out  Receives true when no records are held.
 * @return CFL_ERR_OK on success; CFL_ERR_NULL_PTR on a NULL argument;
 *         CFL_ERR_NOT_INIT if @p handle was not created.
 */
cfl_err_t cfl_is_empty(cfl_handle_t handle, bool *empty_out);

/**
 * @brief Return buffer occupancy for health reporting.
 *
 * @param[in]  handle       CFL handle.
 * @param[out] entry_count  Number of unconsumed records.
 * @param[out] byte_count   Total payload bytes held, excluding record
 *                          overhead and padding.
 * @return CFL_ERR_OK on success; CFL_ERR_NULL_PTR on a NULL argument;
 *         CFL_ERR_NOT_INIT if @p handle was not created.
 */
cfl_err_t cfl_get_occupancy(cfl_handle_t handle, uint32_t *entry_count, uint32_t *byte_count);

#ifdef TEST
/**
 * @brief Reset module state between unit tests. Test-only.
 *
 * Clears the static instance pool and the allocation count.
 */
void cfl_reset_for_test(void);
#endif

#endif /* CIRCULAR_FLASH_LOG_H */
