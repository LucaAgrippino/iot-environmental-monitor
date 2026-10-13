/**
 * @file health_monitor.c
 * @brief HealthMonitor implementation (Gateway application layer).
 *
 * See docs/lld/application/health-monitor-lld.md. A passive aggregator: every
 * operation runs in the caller's task, serialised by one priority-inheritance
 * mutex. The status LED is updated while that mutex is held (see
 * update_led_state()), which the companion's §6 leaves to the caller.
 */

#include "health_monitor.h"

#include <stddef.h>
#include <string.h>

#include "semphr.h"

#ifdef TEST
/* Keep the real LedDriver and Logger out of the host link unit: Ceedling
 * auto-links any .c whose header a translation unit includes. */
#include "led_driver_stub.h"
#define LOG_ERROR(m, f, ...) ((void) 0)
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#define LOG_DEBUG(m, f, ...) ((void) 0)
#else
#include "led/led_driver.h"
#include "logger/logger.h"
#endif /* TEST */

#define HM_LOG_MODULE "HM"

#define HM_MAX_INSTANCES (1U)
#define HM_TASK_NAME_LEN (16U)
#define HM_SNAPSHOT_MAX_BYTES (256U)

/** The event carries no alarm direction, so a raised alarm is recorded as active-high. */
#define HM_ALARM_ACTIVE (ALARM_STATE_ACTIVE_HIGH)

_Static_assert(sizeof(health_snapshot_t) <= HM_SNAPSHOT_MAX_BYTES,
               "health_snapshot_t grew past the 256-byte budget (HM-O1)");

/** @brief What push_event() must log once the mutex has been released. */
typedef enum
{
    HM_LOG_NONE = 0,
    HM_LOG_BAD_PARAM,
    HM_LOG_FAULT
} hm_log_kind_t;

/** @brief Internal instance state — hidden from consumers. */
struct health_monitor_inst
{
    health_snapshot_t snapshot;

    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_buf;

    TaskHandle_t task_handles[HEALTH_MONITOR_MAX_TASKS];
    char task_names[HEALTH_MONITOR_MAX_TASKS][HM_TASK_NAME_LEN];
    uint8_t task_count;

    /** LED fault latch — set by set_led_fault(), never cleared (HM-D6). */
    bool fault_latched;

    bool in_use;
};

static struct health_monitor_inst g_pool[HM_MAX_INSTANCES];
static uint8_t g_count;

/* ====================================================================== */
/* Helpers                                                                */
/* ====================================================================== */

static health_monitor_err_t hm_check_handle(health_monitor_handle_t handle)
{
    if (handle == NULL)
    {
        return HEALTH_MONITOR_ERR_NULL_PTR;
    }

    if (!handle->in_use)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }

    return HEALTH_MONITOR_ERR_OK;
}

static void hm_lock(const struct health_monitor_inst *inst)
{
    (void) xSemaphoreTake(inst->mutex, portMAX_DELAY);
}

static void hm_unlock(const struct health_monitor_inst *inst)
{
    (void) xSemaphoreGive(inst->mutex);
}

/**
 * @brief Choose the LED pattern from the current state and set it.
 *
 * Priority: fault latch > any alarm > operational > init/transient (§6). The
 * caller holds the mutex, or the instance is not yet visible to other tasks
 * (create). Holding it makes "decide, then write" one step, so two tasks
 * cannot interleave and leave the LED showing the older decision. That is safe
 * because led_set() never blocks and takes no lock of its own.
 *
 * A LedDriver failure (for example LED not initialised) is logged and
 * otherwise ignored: the LED is an indicator, not a dependency.
 */
static void update_led_state(const struct health_monitor_inst *inst)
{
    led_pattern_t pattern = LED_PATTERN_BLINK_MEDIUM;
    bool any_alarm = false;
    led_err_t err;

    for (uint8_t i = 0U; i < (uint8_t) SENSOR_ID_COUNT; i++)
    {
        if (inst->snapshot.alarm_state[i] != ALARM_STATE_CLEAR)
        {
            any_alarm = true;
            break;
        }
    }

    if (inst->fault_latched)
    {
        pattern = LED_PATTERN_BLINK_FAST;
    }
    else if (any_alarm)
    {
        pattern = LED_PATTERN_BLINK_SLOW;
    }
    else if (inst->snapshot.lifecycle_state == LIFECYCLE_STATE_OPERATIONAL)
    {
        pattern = LED_PATTERN_ON;
    }
    else
    {
        pattern = LED_PATTERN_BLINK_MEDIUM;
    }

    err = led_set(LED_GREEN, pattern);
    if (err != LED_ERR_OK)
    {
        LOG_WARN(HM_LOG_MODULE, "led_set failed (%d)", (int) err);
    }
}

/* ====================================================================== */
/* Lifecycle                                                              */
/* ====================================================================== */

health_monitor_err_t health_monitor_create(const health_monitor_config_t *config,
                                           health_monitor_handle_t *handle)
{
    if ((config == NULL) || (handle == NULL))
    {
        return HEALTH_MONITOR_ERR_NULL_PTR;
    }

    if (g_count >= HM_MAX_INSTANCES)
    {
        return HEALTH_MONITOR_ERR_NO_RESOURCE;
    }

    struct health_monitor_inst *inst = &g_pool[g_count];

    (void) memset(inst, 0, sizeof(*inst));

    inst->mutex = xSemaphoreCreateMutexStatic(&inst->mutex_buf);
    if (inst->mutex == NULL)
    {
        return HEALTH_MONITOR_ERR_NO_RESOURCE;
    }

    inst->snapshot.last_reset_cause = config->boot_reset_cause;
    inst->snapshot.lifecycle_state = LIFECYCLE_STATE_INIT;

    /* Healthy until proven otherwise. */
    for (uint8_t i = 0U; i < (uint8_t) SENSOR_ID_COUNT; i++)
    {
        inst->snapshot.sensor_valid[i] = true;
    }

    inst->in_use = true;
    g_count++;
    *handle = inst;

    update_led_state(inst);
    LOG_INFO(HM_LOG_MODULE, "created, reset_cause=%u", (unsigned) config->boot_reset_cause);

    return HEALTH_MONITOR_ERR_OK;
}

health_monitor_err_t health_monitor_register_task(health_monitor_handle_t handle, const char *name,
                                                  TaskHandle_t task_handle)
{
    health_monitor_err_t err;

    if ((handle != NULL) && (name == NULL))
    {
        return HEALTH_MONITOR_ERR_NULL_PTR;
    }

    err = hm_check_handle(handle);
    if (err != HEALTH_MONITOR_ERR_OK)
    {
        return err;
    }

    hm_lock(handle);

    if (handle->task_count >= HEALTH_MONITOR_MAX_TASKS)
    {
        hm_unlock(handle);
        return HEALTH_MONITOR_ERR_NO_RESOURCE;
    }

    uint8_t idx = handle->task_count;
    uint8_t n = 0U;

    handle->task_handles[idx] = task_handle;

    while ((n < (HM_TASK_NAME_LEN - 1U)) && (name[n] != '\0'))
    {
        handle->task_names[idx][n] = name[n];
        n++;
    }

    handle->task_names[idx][n] = '\0';
    handle->task_count++;

    hm_unlock(handle);
    LOG_DEBUG(HM_LOG_MODULE, "registered task[%u] = '%s'", (unsigned) idx, name);

    return HEALTH_MONITOR_ERR_OK;
}

health_monitor_err_t health_monitor_poll(health_monitor_handle_t handle)
{
    health_monitor_err_t err = hm_check_handle(handle);
    uint16_t wm[HEALTH_MONITOR_MAX_TASKS] = {0U};
    uint8_t count;

    if (err != HEALTH_MONITOR_ERR_OK)
    {
        return err;
    }

    /* Another task's TCB is read outside the lock: it shares no state with
     * HealthMonitor. Only the copy into the snapshot is guarded. */
    hm_lock(handle);
    count = handle->task_count;
    hm_unlock(handle);

    for (uint8_t i = 0U; i < count; i++)
    {
        wm[i] = (uint16_t) uxTaskGetStackHighWaterMark(handle->task_handles[i]);
    }

    return health_monitor_update_stack_watermarks(handle, wm);
}

/* ====================================================================== */
/* IHealthReport                                                          */
/* ====================================================================== */

health_monitor_err_t health_monitor_push_event(health_monitor_handle_t handle, health_event_t event,
                                               uint32_t param)
{
    health_monitor_err_t err = hm_check_handle(handle);
    hm_log_kind_t log_kind = HM_LOG_NONE;
    bool led_update_needed = false;
    bool param_ok = (param < (uint32_t) SENSOR_ID_COUNT);

    if (err != HEALTH_MONITOR_ERR_OK)
    {
        return err;
    }

    if ((uint32_t) event >= (uint32_t) HEALTH_EVENT_COUNT)
    {
        LOG_WARN(HM_LOG_MODULE, "unknown event %u", (unsigned) event);
        return HEALTH_MONITOR_ERR_OK; /* best-effort — HM-D7 */
    }

    hm_lock(handle);

    switch (event)
    {
    case HEALTH_EVENT_TIME_SYNC_ACQUIRED:
        handle->snapshot.time_sync_state = TIME_SYNC_SYNCHRONISED;
        break;
    case HEALTH_EVENT_TIME_SYNC_LOST:
        handle->snapshot.time_sync_state = TIME_SYNC_UNSYNCHRONISED;
        break;
    case HEALTH_EVENT_CONFIG_WRITE_FAIL:
        handle->snapshot.config_write_failed = true;
        break;
    case HEALTH_EVENT_SENSOR_FAIL:
        if (param_ok)
        {
            handle->snapshot.sensor_valid[param] = false;
            handle->snapshot.sensor_fail_count++;
        }
        else
        {
            log_kind = HM_LOG_BAD_PARAM;
        }
        break;
    case HEALTH_EVENT_SENSOR_RECOVERED:
        if (param_ok)
        {
            handle->snapshot.sensor_valid[param] = true;
        }
        else
        {
            log_kind = HM_LOG_BAD_PARAM;
        }
        break;
    case HEALTH_EVENT_NTP_SYNC_FAILED:
        handle->snapshot.ntp_sync_fail_count++;
        break;
    case HEALTH_EVENT_NTP_SYNC_OK:
        handle->snapshot.last_ntp_sync_epoch = param;
        break;
    case HEALTH_EVENT_BUFFER_OVERFLOW:
        handle->snapshot.buffer_overflow_count++;
        break;
    case HEALTH_EVENT_MODBUS_LINK_UP:
        handle->snapshot.modbus_link_online = true;
        break;
    case HEALTH_EVENT_MODBUS_LINK_DOWN:
        handle->snapshot.modbus_link_online = false;
        break;
    case HEALTH_EVENT_CLOUD_CONNECTED:
        handle->snapshot.cloud_connected = true;
        break;
    case HEALTH_EVENT_CLOUD_DISCONNECTED:
        handle->snapshot.cloud_connected = false;
        break;
    case HEALTH_EVENT_ALARM_RAISED:
        if (param_ok)
        {
            handle->snapshot.alarm_state[param] = HM_ALARM_ACTIVE;
            handle->snapshot.alarm_raise_count++;
            led_update_needed = true;
        }
        else
        {
            log_kind = HM_LOG_BAD_PARAM;
        }
        break;
    case HEALTH_EVENT_ALARM_CLEARED:
        if (param_ok)
        {
            handle->snapshot.alarm_state[param] = ALARM_STATE_CLEAR;
            led_update_needed = true;
        }
        else
        {
            log_kind = HM_LOG_BAD_PARAM;
        }
        break;
    case HEALTH_EVENT_FAULT:
        log_kind = HM_LOG_FAULT; /* the LED is driven by set_led_fault() */
        break;
    case HEALTH_EVENT_NTP_BAD_RESPONSE:
        /* NtpClient logs the detail; the monitor keeps no counter for it. */
        break;
    default:
        break;
    }

    if (led_update_needed)
    {
        update_led_state(handle);
    }

    hm_unlock(handle);

    /* Logging happens after the lock is released so a slow sink never extends
     * the critical section. */
    if (log_kind == HM_LOG_BAD_PARAM)
    {
        LOG_WARN(HM_LOG_MODULE, "event %u param %lu out of range", (unsigned) event,
                 (unsigned long) param);
    }
    else if (log_kind == HM_LOG_FAULT)
    {
        LOG_ERROR(HM_LOG_MODULE, "FAULT event, code=%lu", (unsigned long) param);
    }
    else
    {
        /* Nothing to report. */
    }

    return HEALTH_MONITOR_ERR_OK;
}

health_monitor_err_t health_monitor_update_modbus_master_stats(health_monitor_handle_t handle,
                                                               const modbus_master_stats_t *stats)
{
    health_monitor_err_t err;

    if ((handle != NULL) && (stats == NULL))
    {
        return HEALTH_MONITOR_ERR_NULL_PTR;
    }

    err = hm_check_handle(handle);
    if (err != HEALTH_MONITOR_ERR_OK)
    {
        return err;
    }

    hm_lock(handle);
    handle->snapshot.modbus_transactions_ok = stats->transactions_ok;
    handle->snapshot.modbus_timeouts = stats->timeouts;
    hm_unlock(handle);

    return HEALTH_MONITOR_ERR_OK;
}

health_monitor_err_t health_monitor_update_mqtt_stats(health_monitor_handle_t handle,
                                                      const mqtt_stats_t *stats)
{
    health_monitor_err_t err;

    if ((handle != NULL) && (stats == NULL))
    {
        return HEALTH_MONITOR_ERR_NULL_PTR;
    }

    err = hm_check_handle(handle);
    if (err != HEALTH_MONITOR_ERR_OK)
    {
        return err;
    }

    hm_lock(handle);
    handle->snapshot.mqtt_publishes_sent = stats->publishes_sent;
    handle->snapshot.mqtt_publishes_failed = stats->publish_failures;
    handle->snapshot.mqtt_reconnect_count = stats->reconnect_count;
    hm_unlock(handle);

    return HEALTH_MONITOR_ERR_OK;
}

health_monitor_err_t health_monitor_update_buffer_occupancy(health_monitor_handle_t handle,
                                                            uint32_t entry_count)
{
    health_monitor_err_t err = hm_check_handle(handle);

    if (err != HEALTH_MONITOR_ERR_OK)
    {
        return err;
    }

    hm_lock(handle);
    handle->snapshot.buffer_entry_count = entry_count;
    hm_unlock(handle);

    return HEALTH_MONITOR_ERR_OK;
}

health_monitor_err_t health_monitor_update_stack_watermarks(health_monitor_handle_t handle,
                                                            const uint16_t *watermarks)
{
    health_monitor_err_t err;

    if ((handle != NULL) && (watermarks == NULL))
    {
        return HEALTH_MONITOR_ERR_NULL_PTR;
    }

    err = hm_check_handle(handle);
    if (err != HEALTH_MONITOR_ERR_OK)
    {
        return err;
    }

    hm_lock(handle);

    for (uint8_t i = 0U; i < handle->task_count; i++)
    {
        handle->snapshot.stack_watermark_words[i] = watermarks[i];
    }

    hm_unlock(handle);

    return HEALTH_MONITOR_ERR_OK;
}

health_monitor_err_t health_monitor_update_lifecycle(health_monitor_handle_t handle,
                                                     lifecycle_state_t state)
{
    health_monitor_err_t err = hm_check_handle(handle);

    if (err != HEALTH_MONITOR_ERR_OK)
    {
        return err;
    }

    hm_lock(handle);
    handle->snapshot.lifecycle_state = state;
    update_led_state(handle);
    hm_unlock(handle);

    return HEALTH_MONITOR_ERR_OK;
}

health_monitor_err_t health_monitor_update_wifi_rssi(health_monitor_handle_t handle,
                                                     int32_t rssi_dbm)
{
    health_monitor_err_t err = hm_check_handle(handle);

    if (err != HEALTH_MONITOR_ERR_OK)
    {
        return err;
    }

    hm_lock(handle);
    handle->snapshot.wifi_rssi_dbm = rssi_dbm;
    hm_unlock(handle);

    return HEALTH_MONITOR_ERR_OK;
}

/* ====================================================================== */
/* IHealthSnapshot                                                        */
/* ====================================================================== */

health_monitor_err_t health_monitor_get_snapshot(health_monitor_handle_t handle,
                                                 health_snapshot_t *snap_out)
{
    health_monitor_err_t err;

    if ((handle != NULL) && (snap_out == NULL))
    {
        return HEALTH_MONITOR_ERR_NULL_PTR;
    }

    err = hm_check_handle(handle);
    if (err != HEALTH_MONITOR_ERR_OK)
    {
        return err;
    }

    hm_lock(handle);

    /* Fresh, not stale from the last poll (HM-D5). */
    handle->snapshot.uptime_s = (uint32_t) (xTaskGetTickCount() / configTICK_RATE_HZ);
    (void) memcpy(snap_out, &handle->snapshot, sizeof(*snap_out));

    hm_unlock(handle);

    return HEALTH_MONITOR_ERR_OK;
}

/* ====================================================================== */
/* IHealthAdmin                                                           */
/* ====================================================================== */

health_monitor_err_t health_monitor_set_led_fault(health_monitor_handle_t handle)
{
    health_monitor_err_t err = hm_check_handle(handle);

    if (err != HEALTH_MONITOR_ERR_OK)
    {
        return err;
    }

    hm_lock(handle);
    handle->fault_latched = true;
    update_led_state(handle);
    hm_unlock(handle);

    return HEALTH_MONITOR_ERR_OK;
}

health_monitor_err_t health_monitor_reset_metrics(health_monitor_handle_t handle)
{
    health_monitor_err_t err = hm_check_handle(handle);

    if (err != HEALTH_MONITOR_ERR_OK)
    {
        return err;
    }

    hm_lock(handle);

    handle->snapshot.sensor_fail_count = 0U;
    handle->snapshot.alarm_raise_count = 0U;
    handle->snapshot.modbus_transactions_ok = 0U;
    handle->snapshot.modbus_timeouts = 0U;
    handle->snapshot.mqtt_publishes_sent = 0U;
    handle->snapshot.mqtt_publishes_failed = 0U;
    handle->snapshot.mqtt_reconnect_count = 0U;
    handle->snapshot.ntp_sync_fail_count = 0U;
    handle->snapshot.buffer_overflow_count = 0U;

    hm_unlock(handle);
    LOG_INFO(HM_LOG_MODULE, "metrics reset");

    return HEALTH_MONITOR_ERR_OK;
}

#ifdef TEST
void health_monitor_reset_for_test(void)
{
    (void) memset(g_pool, 0, sizeof(g_pool));
    g_count = 0U;
}
#endif
