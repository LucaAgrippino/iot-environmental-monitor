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
#define US_N_COMMAND (1UL << 0)
#define US_N_START (1UL << 1)
#define US_N_CHUNK (1UL << 2)
#define US_N_SELF_CHECK (1UL << 3)
#define US_N_AFTER_ROLLBACK (1UL << 4)
/**@}*/

#define US_TASK_PRIORITY (1U)
#define US_TASK_STACK_WORDS (1024U)
#define US_LOG_MODULE "UpdateService"

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

typedef enum
{
    US_SLOT_FREE = 0,
    US_SLOT_EXPECTING,
    US_SLOT_FULL
} us_slot_state_t;

struct update_service
{
    bool created;
    update_service_config_t cfg;

    /* --- shared with other tasks: under s_mutex --- */
    us_state_t state;
    firmware_boot_info_t boot; /**< Cached; refreshed only by the task. */
    bool inbox_full;
    us_ota_command_t inbox;
    uint32_t active_job;
    us_slot_state_t slot_state;
    uint32_t exp_offset;
    uint32_t exp_len;
    uint32_t got_len;
    uint32_t pending; /**< US_N_* bits not yet taken by the task. */

    /* --- task only --- */
    us_ota_command_t cmd;
    firmware_slot_t target_slot;
    uint32_t file_len;
    uint32_t offset;
    uint8_t failures; /**< Consecutive, REQ-DM-052. */
    bool start_armed;
    bool req_armed;
    bool check_armed;
    bool paused; /**< Link down while downloading. */
    bool event_retry_armed;
    lifecycle_event_type_t event_retry_type;
    TickType_t start_deadline;
    TickType_t req_deadline;
    TickType_t last_progress;
    TickType_t check_deadline;
    TickType_t next_poll;
    TickType_t event_retry_at;

    TaskHandle_t task;
};

/** The single instance and its chunk slot (update_service.c). */
extern struct update_service g_us;
extern uint8_t g_us_chunk[US_CHUNK_SIZE];

/** @brief Take or give the instance mutex. Held only for field copies, never across a call out. */
void us_lock(void);
void us_unlock(void);

/** @brief Run one pass of the task body: pending notifications, then deadlines (§9.2). */
void us_task_pass(void);

/** @brief Ticks the task may sleep: until the nearest armed deadline, or portMAX_DELAY. */
TickType_t us_task_wait_ticks(void);

/** @brief Task entry created by update_service_create(). */
void us_task_body(void *arg);

#endif /* UPDATE_SERVICE_TASK_H */
