/**
 * @file store_and_forward.h
 * @brief StoreAndForward — power-loss-safe outbound message buffer (Gateway).
 *
 * Implements IStoreAndForward per docs/lld/application/store-and-forward-lld.md.
 * Telemetry that cannot be published while the cloud is unreachable is
 * serialised into CircularFlashLog and replayed, oldest first, on reconnect.
 *
 * StoreAndForward owns policy (drop-oldest, two-phase dequeue/confirm,
 * occupancy reporting); CircularFlashLog owns the flash mechanics.
 *
 * Naming note: cloud_publisher.h still carries a bring-up placeholder that
 * declares its own saf_err_t / saf_entry_t. The two sets are deliberately not
 * guarded against each other — a translation unit that includes both fails to
 * compile, which is the signal to move CloudPublisher onto this header.
 */

#ifndef STORE_AND_FORWARD_H
#define STORE_AND_FORWARD_H

#include <stdbool.h>
#include <stdint.h>

#include "circular_flash_log/circular_flash_log.h"

/** @brief Opaque handle to a StoreAndForward instance. */
typedef struct saf_inst *saf_handle_t;

/** @brief Maximum topic length in characters (excluding the terminator). */
#define SAF_TOPIC_MAX (64U)

/**
 * @brief Serialised header bytes at maximum topic length.
 *
 * topic_len(1) + topic(64) + qos(1) + payload_len(2).
 */
#define SAF_HEADER_OVERHEAD_MAX (68U)

/**
 * @brief Maximum payload size in bytes.
 *
 * CFL_MAX_RECORD_BYTES (4078) minus the header overhead at maximum topic
 * length, so every entry fits in a single CircularFlashLog record (SAF-D6).
 */
#define SAF_PAYLOAD_MAX (CFL_MAX_RECORD_BYTES - SAF_HEADER_OVERHEAD_MAX)

/** @brief Error codes returned by every StoreAndForward function. */
typedef enum
{
    SAF_ERR_OK = 0,          /**< Operation succeeded.                         */
    SAF_ERR_NULL_PTR = 1,    /**< A required pointer argument was NULL.        */
    SAF_ERR_NO_RESOURCE = 2, /**< Static instance pool exhausted.              */
    SAF_ERR_NOT_INIT = 3,    /**< Handle does not refer to a created instance. */
    SAF_ERR_TOO_LARGE = 4,   /**< Payload is 0 or exceeds SAF_PAYLOAD_MAX, or
                                  the topic is empty or exceeds SAF_TOPIC_MAX. */
    SAF_ERR_EMPTY = 5,       /**< Dequeue called on an empty buffer.           */
    SAF_ERR_FLASH = 6        /**< CircularFlashLog I/O failure.                */
} saf_err_t;

/**
 * @brief Dequeued entry returned to the caller.
 *
 * The @c payload pointer is valid only until the next call to saf_dequeue()
 * or saf_confirm(), and is invalidated when saf_enqueue() has to drop the
 * entry it refers to.
 */
typedef struct
{
    char topic[SAF_TOPIC_MAX + 1U]; /**< Null-terminated.                  */
    uint8_t qos;                    /**< MQTT QoS as enqueued.             */
    uint16_t payload_len;           /**< Payload byte count.               */
    const uint8_t *payload;         /**< Points into the instance buffer.  */
} saf_entry_t;

/** @brief StoreAndForward creation configuration. */
typedef struct
{
    cfl_handle_t cfl;               /**< CircularFlashLog handle (created).  */
    health_monitor_handle_t health; /**< IHealthReport handle.               */
    uint32_t capacity;              /**< Worst-case entry count, used as the
                                         denominator of the occupancy %.     */
} saf_config_t;

/* ====================================================================== */
/* Placeholder dependency — IHealthReport occupancy push                  */
/*                                                                        */
/* The Gateway has no HealthMonitor yet. This declaration stands in for    */
/* the real entry point; store_and_forward.c supplies a weak no-op default */
/* that a real implementation, a unit-test spy or a bring-up harness       */
/* overrides, following the convention set by CircularFlashLog (CFL-O5).   */
/* ====================================================================== */

/**
 * @brief Push the buffer occupancy to IHealthReport.
 *
 * @param[in] handle    IHealthReport handle from the config.
 * @param[in] count     Entries currently buffered.
 * @param[in] capacity  Configured worst-case entry count.
 * @param[in] percent   Occupancy, 0–100.
 */
void health_update_buffer_occupancy(health_monitor_handle_t handle, uint32_t count,
                                    uint32_t capacity, uint8_t percent);

/* ====================================================================== */
/* Public API — IStoreAndForward                                          */
/* ====================================================================== */

/**
 * @brief Create and initialise a StoreAndForward instance.
 *
 * Recovers the entry count from cfl_get_occupancy() and pushes the initial
 * occupancy to IHealthReport.
 *
 * @param[in]  config  Injected dependencies.
 * @param[out] handle  Receives the created handle on success.
 * @return SAF_ERR_OK on success; SAF_ERR_NULL_PTR if config, handle or
 *         config->cfl is NULL; SAF_ERR_NO_RESOURCE if the pool is exhausted;
 *         SAF_ERR_FLASH if cfl_get_occupancy() fails.
 * @note Threading: task-context only. Call after cfl_create().
 */
saf_err_t saf_create(const saf_config_t *config, saf_handle_t *handle);

/**
 * @brief Serialise and append an entry to the buffer.
 *
 * If the CircularFlashLog ring is full, drops the oldest entries one at a
 * time via cfl_drop_oldest() until the append succeeds (REQ-BF-020). Each
 * drop raises the buffer-overflow health event inside CircularFlashLog. A
 * dropped entry that was dequeued but not yet confirmed is forgotten, so a
 * later saf_confirm() cannot consume a different record.
 *
 * @param[in] handle  SAF handle.
 * @param[in] topic   Null-terminated MQTT topic, 1..SAF_TOPIC_MAX characters.
 * @param[in] buf     Payload bytes.
 * @param[in] len     Payload byte count, 1..SAF_PAYLOAD_MAX.
 * @param[in] qos     MQTT QoS (0 or 1), stored as given.
 * @return SAF_ERR_OK on success; SAF_ERR_NULL_PTR if handle, topic or buf is
 *         NULL; SAF_ERR_NOT_INIT for a handle that was never created;
 *         SAF_ERR_TOO_LARGE if len is 0 or above SAF_PAYLOAD_MAX, or the topic
 *         is empty or too long; SAF_ERR_FLASH on CircularFlashLog failure.
 */
saf_err_t saf_enqueue(saf_handle_t handle, const char *topic, const uint8_t *buf, uint16_t len,
                      uint8_t qos);

/**
 * @brief Return a pointer to the oldest entry without removing it.
 *
 * If an earlier dequeue has not been confirmed, returns the same entry
 * without touching flash. Records that fail their CRC, or whose contents
 * cannot be parsed, are consumed and skipped.
 *
 * @param[in]  handle  SAF handle.
 * @param[out] out     Receives a pointer to the entry.
 * @return SAF_ERR_OK on success; SAF_ERR_EMPTY if the buffer is empty;
 *         SAF_ERR_NULL_PTR if handle or out is NULL; SAF_ERR_NOT_INIT for a
 *         handle that was never created; SAF_ERR_FLASH on a read failure or
 *         when a corrupt record cannot be consumed.
 */
saf_err_t saf_dequeue(saf_handle_t handle, const saf_entry_t **out);

/**
 * @brief Consume the last dequeued entry.
 *
 * Call exactly once after a successful MQTT publish. Idempotent: with no
 * pending entry it returns SAF_ERR_OK without touching flash.
 *
 * @param[in] handle  SAF handle.
 * @return SAF_ERR_OK on success; SAF_ERR_NULL_PTR if handle is NULL;
 *         SAF_ERR_NOT_INIT for a handle that was never created;
 *         SAF_ERR_FLASH on a consume failure (the entry stays pending).
 */
saf_err_t saf_confirm(saf_handle_t handle);

/**
 * @brief Query whether the buffer is empty.
 *
 * @param[in]  handle     SAF handle.
 * @param[out] empty_out  Receives true if no entries are buffered.
 * @return SAF_ERR_OK on success; SAF_ERR_NULL_PTR if handle or empty_out is
 *         NULL; SAF_ERR_NOT_INIT for a handle that was never created.
 */
saf_err_t saf_is_empty(saf_handle_t handle, bool *empty_out);

/**
 * @brief Return buffer occupancy for diagnostics.
 *
 * @param[in]  handle       SAF handle.
 * @param[out] entry_count  Number of entries in the buffer.
 * @param[out] capacity     Configured worst-case capacity.
 * @return SAF_ERR_OK on success; SAF_ERR_NULL_PTR if handle or an out
 *         parameter is NULL; SAF_ERR_NOT_INIT for a handle that was never
 *         created.
 */
saf_err_t saf_get_occupancy(saf_handle_t handle, uint32_t *entry_count, uint32_t *capacity);

#ifdef TEST
/** @brief Test hook — forget the instance pool so each test starts clean. */
void saf_reset_for_test(void);
#endif

#endif /* STORE_AND_FORWARD_H */
