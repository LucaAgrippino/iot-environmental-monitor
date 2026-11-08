/**
 * @file update_service_task.h
 * @brief UpdateService private: instance state shared by update_service.c and
 *        update_service_task.c.
 *
 * Not for consumers. update_service.c owns the intake side (any task); update_service_task.c
 * owns Machine 3 (UpdateServiceTask only).
 *
 * @see docs/lld/application/update-service-lld.md §9, §16
 */

#ifndef UPDATE_SERVICE_TASK_H
#define UPDATE_SERVICE_TASK_H

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"
#include "update_service.h"

/** @name Task notification bits (§9.3) */
/**@{*/
#define US_N_COMMAND (1UL << 0)        /**< Task notification bit: a command arrived. */
#define US_N_START (1UL << 1)          /**< Task notification bit: start an update. */
#define US_N_CHUNK (1UL << 2)          /**< Task notification bit: an image chunk arrived. */
#define US_N_SELF_CHECK (1UL << 3)     /**< Task notification bit: run the self-check. */
#define US_N_AFTER_ROLLBACK (1UL << 4) /**< Task notification bit: handle a rollback. */
/**@}*/

#define US_TASK_PRIORITY (1U)         /**< FreeRTOS priority of the UpdateService task. */
#define US_TASK_STACK_WORDS (704U)    /**< D-1 F: 438 of 1024 words used (verify path) */
#define US_LOG_MODULE "UpdateService" /**< Module name used in log lines. */

/** @brief Machine 3 state mapping (§5, §9.1). */
typedef enum
{
    US_ST_IDLE = 0,
    US_ST_AWAITING_START,
    US_ST_DOWNLOADING,
    US_ST_VALIDATING,
    US_ST_INSTALLING,
    US_ST_ACTIVATED,
    US_ST_SELF_CHECKING,
    US_ST_ROLLING_BACK
} us_state_t;

/** @brief State of the chunk slot. */
typedef enum
{
    US_SLOT_FREE = 0,
    US_SLOT_EXPECTING,
    US_SLOT_FULL
} us_slot_state_t;

struct update_service
{
    bool created;                /**< True once the module has been created. */
    update_service_config_t cfg; /**< Configuration given at creation. */

    /** --- shared with other tasks: under s_mutex --- */
    us_state_t state;
    firmware_boot_info_t boot;  /**< Cached; refreshed only by the task. */
    bool inbox_full;            /**< True while a command waits in inbox. */
    us_ota_command_t inbox;     /**< Command received from the cloud, waiting for the task. */
    uint32_t active_job;        /**< Job number of the update in progress. */
    us_slot_state_t slot_state; /**< State of the chunk slot. */
    uint32_t exp_offset;        /**< Offset of the chunk that was requested. */
    uint32_t exp_len;           /**< Length of the chunk that was requested. */
    uint32_t got_len;           /**< Bytes of the chunk received so far. */
    uint32_t pending;           /**< US_N_* bits not yet taken by the task. */

    /** --- task only --- */
    us_ota_command_t cmd;
    firmware_slot_t target_slot; /**< Flash slot the image is written to. */
    uint32_t file_len;           /**< Length of the signed image file, in bytes. */
    uint32_t offset;             /**< Offset of the next byte to request. */
    uint8_t failures;            /**< Consecutive, REQ-DM-052. */
    bool start_armed;            /**< True while the start timeout runs. */
    bool req_armed;              /**< True while a chunk request timeout runs. */
    bool check_armed;            /**< True while the self-check timeout runs. */
    bool paused;                 /**< Link down while downloading. */
    bool event_retry_armed;      /**< True while a refused lifecycle event is being retried. */
    lifecycle_event_type_t event_retry_type; /**< Type of the lifecycle event to retry. */
    TickType_t start_deadline;               /**< Tick at which the start timeout expires. */
    TickType_t req_deadline;   /**< Tick at which the chunk request timeout expires. */
    TickType_t last_progress;  /**< Tick of the last progress, to detect a stalled transfer. */
    TickType_t check_deadline; /**< Tick at which the self-check timeout expires. */
    TickType_t next_poll;      /**< Tick of the next poll of the boot information. */
    TickType_t event_retry_at; /**< Tick of the next retry of the lifecycle event. */

    TaskHandle_t task; /**< Handle of the UpdateService task. */
};

/** The single instance and its chunk slot (update_service.c). */
extern struct update_service g_us;
/** @brief Buffer that receives one image chunk. */
extern uint8_t g_us_chunk[US_CHUNK_SIZE];

/** @brief Take the instance mutex. Held only for field copies, never across a call out. */
void us_lock(void);

/** @brief Give the instance mutex taken by us_lock(). */
void us_unlock(void);

/** @brief Run one pass of the task body: pending notifications, then deadlines (§9.2). */
void us_task_pass(void);

/**
 * @brief Ticks the task may sleep: until the nearest armed deadline, or portMAX_DELAY.
 *
 * @return Ticks to wait before the next deadline.
 */
TickType_t us_task_wait_ticks(void);

/**
 * @brief Task entry created by update_service_create().
 *
 * @param arg Unused.
 */
void us_task_body(void *arg);

#endif /* UPDATE_SERVICE_TASK_H */
