/**
 * @file health_monitor.c
 * @brief HealthMonitor implementation — passive metric aggregator and LED driver.
 *
 * @see docs/lld/application/health-monitor.md
 *
 * DEVIATION from companion §6 (LED mapping):
 *   The companion references led_driver_set() with LED_ORANGE, LED_BLUE, and
 *   blink-state constants (LED_BLINK_SLOW, LED_BLINK_FAST). The implemented
 *   LedDriver (led-driver.md) provides only LED_GREEN and LED_RED, with
 *   led_on() / led_off() / led_toggle() — no hardware blink, no orange/blue
 *   LEDs on the STM32F469 board as wired. The LED mapping is adapted:
 *     Faulted   → RED on,  GREEN off
 *     Alarm     → RED on,  GREEN on   (both on = alarm, distinct from fault)
 *     Operational → RED off, GREEN on
 *     Init/other  → RED off, GREEN off
 */

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "task.h"

#ifdef TEST
/* Exclude real driver and logger headers — prevent Ceedling auto-linking
 * the real .c files. LED calls are intercepted via the UNIT_TEST macro. */
#include "led_driver_stub.h"
#define LOG_ERROR(m, f, ...) ((void) 0)
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#define LOG_DEBUG(m, f, ...) ((void) 0)
#else
#include "led/led_driver.h"
#include "logger/logger.h"
#endif

#include "health_monitor/health_monitor.h"

/* ======================================================================= */
/* Compile-time invariant (HM-O3)                                         */
/* ======================================================================= */

_Static_assert(sizeof(device_health_snapshot_t) <= 512U,
               "HM-O3: device_health_snapshot_t exceeds 512-byte size budget");

/* ======================================================================= */
/* Module tag                                                              */
/* ======================================================================= */

#define MODULE_TAG "HM"

/* ======================================================================= */
/* Internal LED state type                                                 */
/* ======================================================================= */

typedef enum
{
    HM_LED_OFF       = 0U,
    HM_LED_ON        = 1U,
    HM_LED_BLINK_SLOW = 2U,
    HM_LED_BLINK_FAST = 3U,
} hm_led_target_t;

/* ======================================================================= */
/* Internal state                                                          */
/* ======================================================================= */

/** Maximum length of a registered task name including NUL terminator. */
#define TASK_NAME_MAX_LEN 16U

typedef struct
{
    bool initialised;
    device_health_snapshot_t snapshot;
    StaticSemaphore_t mutex_buf;
    SemaphoreHandle_t mutex;
    void *task_handles[HEALTH_TASK_COUNT];
    char task_names[HEALTH_TASK_COUNT][TASK_NAME_MAX_LEN];
    uint8_t task_count;
} health_monitor_state_t;

static health_monitor_state_t s_hm;

/* ======================================================================= */
/* Internal helpers                                                        */
/* ======================================================================= */

/**
 * In production, wrap the iled_t vtable on/off calls into a single call site
 * that matches the companion's led_driver_set(id, state) pattern.
 * In UNIT_TEST builds, this function is replaced entirely by the macro
 * defined in health_monitor.h — the #ifndef guard prevents a redefinition.
 *
 * DEVIATION from companion §6: blink states map to LED_STATE_ON since
 * LedDriver does not implement hardware blinking.
 */
#ifndef UNIT_TEST
static void led_driver_set(led_id_t id, hm_led_target_t state)
{
    if (state == HM_LED_OFF)
    {
        (void) led_driver->off(id);
    }
    else
    {
        (void) led_driver->on(id);
    }
}
#endif /* UNIT_TEST */

/**
 * Update board LEDs based on the current snapshot state.
 * Priority: Faulted > Alarm > Operational > Init.
 * Called while the mutex is HELD (companion §3).
 *
 * DEVIATION from companion §6: LED_ORANGE and LED_BLUE do not exist on
 * the fitted LedDriver. Alarm is indicated by both RED and GREEN on.
 */
static void update_led_state(void)
{
    bool any_alarm;
    int i;

    if (s_hm.snapshot.lifecycle_state == LIFECYCLE_STATE_FAULTED)
    {
        led_driver_set(LED_RED, HM_LED_ON);
        led_driver_set(LED_GREEN, HM_LED_OFF);
        return;
    }

    any_alarm = false;
    for (i = 0; i < (int) SENSOR_ID_COUNT; i++)
    {
        if (s_hm.snapshot.alarm_state[i] != ALARM_STATE_CLEAR)
        {
            any_alarm = true;
            break;
        }
    }

    if (any_alarm)
    {
        led_driver_set(LED_RED, HM_LED_ON);
        led_driver_set(LED_GREEN, HM_LED_ON);
        return;
    }

    if (s_hm.snapshot.lifecycle_state == LIFECYCLE_STATE_OPERATIONAL)
    {
        led_driver_set(LED_GREEN, HM_LED_BLINK_SLOW);
        led_driver_set(LED_RED, HM_LED_OFF);
    }
    else
    {
        led_driver_set(LED_GREEN, HM_LED_BLINK_FAST);
        led_driver_set(LED_RED, HM_LED_OFF);
    }
}

/* ======================================================================= */
/* IHealthReport — public API                                              */
/* ======================================================================= */

health_monitor_err_t health_monitor_init(void)
{
    (void) memset(&s_hm, 0, sizeof(s_hm));

    s_hm.mutex = xSemaphoreCreateMutexStatic(&s_hm.mutex_buf);
    if (s_hm.mutex == NULL)
    {
        LOG_ERROR(MODULE_TAG, "Mutex creation failed");
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }

    s_hm.initialised = true;

    update_led_state();

    LOG_INFO(MODULE_TAG, "Initialised");
    return HEALTH_MONITOR_ERR_OK;
}

health_monitor_err_t health_monitor_push_event(health_event_t event, uint32_t param)
{
    bool update_led = false;

    if (!s_hm.initialised)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }

    if (xSemaphoreTake(s_hm.mutex, portMAX_DELAY) != pdTRUE)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }

    switch (event)
    {
    case HEALTH_EVENT_TIME_SYNC_ACQUIRED:
        s_hm.snapshot.time_sync_state = TIME_SYNC_SYNCHRONISED;
        break;

    case HEALTH_EVENT_TIME_SYNC_LOST:
        s_hm.snapshot.time_sync_state = TIME_SYNC_UNSYNCHRONISED;
        break;

    case HEALTH_EVENT_CONFIG_WRITE_FAIL:
        s_hm.snapshot.config_write_failed = true;
        break;

    case HEALTH_EVENT_CONFIG_READ_FAIL:
        /* No dedicated snapshot field; event is observable via Logger. */
        break;

    case HEALTH_EVENT_CONFIG_NO_VALID_SLOT:
        s_hm.snapshot.config_write_failed = true;
        break;

    case HEALTH_EVENT_SENSOR_FAIL:
        if (param < (uint32_t) SENSOR_ID_COUNT)
        {
            s_hm.snapshot.sensor_valid[param] = false;
        }
        s_hm.snapshot.sensor_fail_count++;
        break;

    case HEALTH_EVENT_ALARM_RAISED:
        if (param < (uint32_t) SENSOR_ID_COUNT)
        {
            s_hm.snapshot.alarm_state[param] = ALARM_STATE_ACTIVE_HIGH;
        }
        s_hm.snapshot.alarm_raise_count++;
        update_led = true;
        break;

    case HEALTH_EVENT_ALARM_CLEARED:
        if (param < (uint32_t) SENSOR_ID_COUNT)
        {
            s_hm.snapshot.alarm_state[param] = ALARM_STATE_CLEAR;
        }
        update_led = true;
        break;

    case HEALTH_EVENT_FAULT:
        s_hm.snapshot.lifecycle_state = LIFECYCLE_STATE_FAULTED;
        update_led = true;
        break;

#if defined(BOARD_GATEWAY)
    case HEALTH_EVENT_NTP_SYNC_FAILED:
        s_hm.snapshot.ntp_sync_fail_count++;
        break;

    case HEALTH_EVENT_NTP_BAD_RESPONSE:
        /* Counted with NTP sync fails. */
        s_hm.snapshot.ntp_sync_fail_count++;
        break;

    case HEALTH_EVENT_BUFFER_OVERFLOW:
        s_hm.snapshot.buffer_overflow_count++;
        break;

    case HEALTH_EVENT_BUFFER_FLASH_ERR:
        /* No dedicated snapshot field; observable via Logger. */
        break;

    case HEALTH_EVENT_MODBUS_LINK_UP:
        s_hm.snapshot.modbus_link_online = true;
        break;

    case HEALTH_EVENT_MODBUS_NODE_OFFLINE:
        s_hm.snapshot.modbus_link_online = false;
        break;
#else
    case HEALTH_EVENT_NTP_SYNC_FAILED:
    case HEALTH_EVENT_NTP_BAD_RESPONSE:
    case HEALTH_EVENT_BUFFER_OVERFLOW:
    case HEALTH_EVENT_BUFFER_FLASH_ERR:
    case HEALTH_EVENT_MODBUS_LINK_UP:
    case HEALTH_EVENT_MODBUS_NODE_OFFLINE:
        /* GW-only events: no-op on FD. */
        break;
#endif /* BOARD_GATEWAY */

    default:
        (void) xSemaphoreGive(s_hm.mutex);
        LOG_WARN(MODULE_TAG, "Unknown health event %u", (unsigned) event);
        return HEALTH_MONITOR_ERR_NULL_ARG;
    }

    if (update_led)
    {
        update_led_state();
    }

    (void) xSemaphoreGive(s_hm.mutex);
    return HEALTH_MONITOR_ERR_OK;
}

health_monitor_err_t health_monitor_update_modbus_slave_stats(const modbus_slave_stats_t *stats)
{
    if (!s_hm.initialised)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }
    if (stats == NULL)
    {
        return HEALTH_MONITOR_ERR_NULL_ARG;
    }

    if (xSemaphoreTake(s_hm.mutex, portMAX_DELAY) != pdTRUE)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }

    s_hm.snapshot.modbus_valid_frames = stats->valid_frames;
    s_hm.snapshot.modbus_crc_errors = stats->crc_errors;
    s_hm.snapshot.modbus_addr_mismatches = stats->address_mismatches;
    s_hm.snapshot.modbus_exception_responses = stats->exception_responses;

    (void) xSemaphoreGive(s_hm.mutex);
    return HEALTH_MONITOR_ERR_OK;
}

#if defined(BOARD_GATEWAY)

health_monitor_err_t health_monitor_update_modbus_master_stats(const modbus_master_stats_t *stats)
{
    if (!s_hm.initialised)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }
    if (stats == NULL)
    {
        return HEALTH_MONITOR_ERR_NULL_ARG;
    }

    if (xSemaphoreTake(s_hm.mutex, portMAX_DELAY) != pdTRUE)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }

    s_hm.snapshot.modbus_transactions_ok = stats->transactions_ok;
    s_hm.snapshot.modbus_timeouts = stats->timeouts;

    (void) xSemaphoreGive(s_hm.mutex);
    return HEALTH_MONITOR_ERR_OK;
}

health_monitor_err_t health_monitor_update_mqtt_stats(const mqtt_stats_t *stats)
{
    if (!s_hm.initialised)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }
    if (stats == NULL)
    {
        return HEALTH_MONITOR_ERR_NULL_ARG;
    }

    if (xSemaphoreTake(s_hm.mutex, portMAX_DELAY) != pdTRUE)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }

    s_hm.snapshot.mqtt_publishes_sent = stats->publishes_sent;
    s_hm.snapshot.mqtt_publishes_failed = stats->publishes_failed;
    s_hm.snapshot.mqtt_reconnect_count = stats->reconnect_count;
    s_hm.snapshot.wifi_rssi_dbm = stats->rssi_dbm;

    (void) xSemaphoreGive(s_hm.mutex);
    return HEALTH_MONITOR_ERR_OK;
}

health_monitor_err_t health_monitor_update_buffer_occupancy(uint32_t entry_count)
{
    if (!s_hm.initialised)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }

    if (xSemaphoreTake(s_hm.mutex, portMAX_DELAY) != pdTRUE)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }

    s_hm.snapshot.buffer_entry_count = entry_count;

    (void) xSemaphoreGive(s_hm.mutex);
    return HEALTH_MONITOR_ERR_OK;
}

#endif /* BOARD_GATEWAY */

health_monitor_err_t
health_monitor_update_stack_watermarks(const uint16_t watermarks[HEALTH_TASK_COUNT])
{
    uint8_t i;

    if (!s_hm.initialised)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }
    if (watermarks == NULL)
    {
        return HEALTH_MONITOR_ERR_NULL_ARG;
    }

    if (xSemaphoreTake(s_hm.mutex, portMAX_DELAY) != pdTRUE)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }

    for (i = 0U; i < HEALTH_TASK_COUNT; i++)
    {
        s_hm.snapshot.stack_watermark_words[i] = watermarks[i];
    }

    (void) xSemaphoreGive(s_hm.mutex);
    return HEALTH_MONITOR_ERR_OK;
}

/* ======================================================================= */
/* IHealthSnapshot — public API                                            */
/* ======================================================================= */

health_monitor_err_t health_monitor_get_snapshot(device_health_snapshot_t *snap_out)
{
    if (!s_hm.initialised)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }
    if (snap_out == NULL)
    {
        return HEALTH_MONITOR_ERR_NULL_ARG;
    }

    if (xSemaphoreTake(s_hm.mutex, portMAX_DELAY) != pdTRUE)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }

    (void) memcpy(snap_out, &s_hm.snapshot, sizeof(device_health_snapshot_t));

    (void) xSemaphoreGive(s_hm.mutex);
    return HEALTH_MONITOR_ERR_OK;
}

void health_monitor_set_led_fault(void)
{
    led_driver_set(LED_RED, HM_LED_ON);
    led_driver_set(LED_GREEN, HM_LED_OFF);
}

/* ======================================================================= */
/* IHealthAdmin — public API                                               */
/* ======================================================================= */

health_admin_err_t health_monitor_reset_metrics(void)
{
    if (!s_hm.initialised)
    {
        return HEALTH_ADMIN_ERR_NOT_INIT;
    }

    if (xSemaphoreTake(s_hm.mutex, portMAX_DELAY) != pdTRUE)
    {
        return HEALTH_ADMIN_ERR_NOT_INIT;
    }

    s_hm.snapshot.modbus_valid_frames = 0U;
    s_hm.snapshot.modbus_crc_errors = 0U;
    s_hm.snapshot.modbus_addr_mismatches = 0U;
    s_hm.snapshot.modbus_exception_responses = 0U;
    s_hm.snapshot.sensor_fail_count = 0U;
    s_hm.snapshot.alarm_raise_count = 0U;

#if defined(BOARD_GATEWAY)
    s_hm.snapshot.modbus_transactions_ok = 0U;
    s_hm.snapshot.modbus_timeouts = 0U;
    s_hm.snapshot.mqtt_publishes_sent = 0U;
    s_hm.snapshot.mqtt_publishes_failed = 0U;
    s_hm.snapshot.mqtt_reconnect_count = 0U;
    s_hm.snapshot.ntp_sync_fail_count = 0U;
    s_hm.snapshot.buffer_overflow_count = 0U;
#endif

    (void) xSemaphoreGive(s_hm.mutex);
    return HEALTH_ADMIN_ERR_OK;
}

/* ======================================================================= */
/* Polling                                                                 */
/* ======================================================================= */

void health_monitor_poll(void)
{
    uint16_t wm[HEALTH_TASK_COUNT];
    uint8_t i;
    uint32_t uptime;

    if (!s_hm.initialised)
    {
        return;
    }

    for (i = 0U; i < (uint8_t) HEALTH_TASK_COUNT; i++)
    {
        if (s_hm.task_handles[i] != NULL)
        {
            wm[i] = (uint16_t) uxTaskGetStackHighWaterMark((void *) s_hm.task_handles[i]);
        }
        else
        {
            wm[i] = 0U;
        }
    }
    (void) health_monitor_update_stack_watermarks(wm);

    uptime = (uint32_t) (xTaskGetTickCount() / configTICK_RATE_HZ);
    if (xSemaphoreTake(s_hm.mutex, portMAX_DELAY) == pdTRUE)
    {
        s_hm.snapshot.uptime_s = uptime;
        (void) xSemaphoreGive(s_hm.mutex);
    }
}

health_monitor_err_t health_monitor_register_task(const char *name, void *task_handle)
{
    if (!s_hm.initialised)
    {
        return HEALTH_MONITOR_ERR_NOT_INIT;
    }
    if (name == NULL || task_handle == NULL)
    {
        return HEALTH_MONITOR_ERR_NULL_ARG;
    }
    if (s_hm.task_count >= (uint8_t) HEALTH_TASK_COUNT)
    {
        return HEALTH_MONITOR_ERR_NULL_ARG;
    }

    s_hm.task_handles[s_hm.task_count] = task_handle;
    (void) strncpy(s_hm.task_names[s_hm.task_count], name, TASK_NAME_MAX_LEN - 1U);
    s_hm.task_names[s_hm.task_count][TASK_NAME_MAX_LEN - 1U] = '\0';
    s_hm.task_count++;

    return HEALTH_MONITOR_ERR_OK;
}

/* ======================================================================= */
/* Vtable instances and singleton pointers                                 */
/* ======================================================================= */

static const ihealth_report_t s_health_report_vtable = {
    .init = health_monitor_init,
    .push_event = health_monitor_push_event,
    .update_modbus_slave_stats = health_monitor_update_modbus_slave_stats,
#if defined(BOARD_GATEWAY)
    .update_modbus_master_stats = health_monitor_update_modbus_master_stats,
    .update_mqtt_stats = health_monitor_update_mqtt_stats,
    .update_buffer_occupancy = health_monitor_update_buffer_occupancy,
#endif
    .update_stack_watermarks = health_monitor_update_stack_watermarks,
    .set_led_fault = health_monitor_set_led_fault,
};

static const ihealth_snapshot_t s_health_snapshot_vtable = {
    .get_snapshot = health_monitor_get_snapshot,
};

static const ihealth_admin_t s_health_admin_vtable = {
    .reset_metrics = health_monitor_reset_metrics,
};

const ihealth_report_t *const health_report = &s_health_report_vtable;
const ihealth_snapshot_t *const health_snapshot = &s_health_snapshot_vtable;
const ihealth_admin_t *const health_admin = &s_health_admin_vtable;

/* ======================================================================= */
/* Test-only reset                                                         */
/* ======================================================================= */

#ifdef TEST
void health_monitor_reset_for_test(void)
{
    (void) memset(&s_hm, 0, sizeof(s_hm));
}
#endif /* TEST */
