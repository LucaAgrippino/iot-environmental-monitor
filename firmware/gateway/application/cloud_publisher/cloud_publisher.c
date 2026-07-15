/**
 * @file cloud_publisher.c
 * @brief CloudPublisher (Gateway) implementation — task, timers, queues.
 *
 * @see docs/lld/application/cloud-publisher-lld.md for the design
 *      specification this file implements, including deviations CP-D9
 *      and CP-D10 (documented where applied below). CP-D7 (connectivity
 *      gate) is RESOLVED as of mqtt_client_is_connected() being added to
 *      MqttClient — prv_enqueue_or_publish() now matches the companion's
 *      original two-branch §5.3 pseudocode exactly.
 */

#include "cloud_publisher.h"
#include "cloud_publisher_json.h"

#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "queue.h"

#include "logger/logger.h"
#include "mqtt_topic_config.h"

#define CP_LOG_MODULE "CloudPublisher"

#define CP_MAX_INSTANCES 1u
#define CP_JSON_BUF_SIZE 4096u /**< Matches MQTT_PKT_BUF_SIZE; see MQTT-O3. */
#define CP_ALARM_QUEUE_LEN 8u
#define CP_CMD_QUEUE_LEN 4u
#define CP_TASK_STACK_WORDS 768u
#define CP_TASK_PRIORITY 3u

#define CP_TELEMETRY_PERIOD_S_DEFAULT 60u
#define CP_HEALTH_PERIOD_S_DEFAULT 600u
#define CP_STATS_PERIOD_MS 1000u

/** CP-D9: retry backoff between failed reconnect attempts (stats ticks). */
#define CP_RECONNECT_RETRY_PERIOD_S 30u

#define CP_NOTIFY_TELEMETRY_TICK (1u << 0)
#define CP_NOTIFY_HEALTH_TICK (1u << 1)
#define CP_NOTIFY_STATS_TICK (1u << 2)
#define CP_NOTIFY_ALARM_PENDING (1u << 3)
#define CP_NOTIFY_COMMAND_PENDING (1u << 4)
#define CP_NOTIFY_ALL_BITS 0x1Fu

#define CP_DEVICE_SERIAL_LEN 25u /**< 24 hex chars (96-bit UID) + null. */
#define CP_TOPIC_MAX_LEN 64u
#define CP_CMD_PAYLOAD_MAX_LEN                                                                     \
    128u /**< Config/OTA/control commands are small; unlike SAF entries. */

/** STM32L4 96-bit unique device ID base address (RM0351). */
#define CP_UID_BASE_ADDR 0x1FFF7590UL

/* ===================================================================== */
/* Private types                                                         */
/* ===================================================================== */

typedef struct
{
    char topic[CP_TOPIC_MAX_LEN];
    uint8_t payload[CP_CMD_PAYLOAD_MAX_LEN];
    uint32_t payload_len;
} cp_command_entry_t;

struct cloud_publisher_inst
{
    /* Injected dependencies (all opaque handles) */
    mqtt_client_handle_t mqtt;
    sensor_service_handle_t sensors;
    alarm_service_handle_t alarms;
    modbus_poller_handle_t poller;
    store_and_forward_handle_t saf;
    health_monitor_handle_t health_read;
    health_monitor_handle_t health_write;
    config_service_handle_t cfg_read;
    config_service_handle_t cfg_write;
    update_service_handle_t update_svc;
    lifecycle_handle_t lifecycle;

    /* Working state */
    char scratch_buf[CP_JSON_BUF_SIZE];
    mqtt_stats_t last_mqtt_stats;
    char device_serial[CP_DEVICE_SERIAL_LEN];
    mqtt_connect_cfg_t mqtt_connect_cfg; /**< CP-D9: owned here for connect()/reconnect(). */
    uint32_t reconnect_countdown_s;      /**< CP-D9: stats ticks left before next retry. */

    char topic_telemetry[CP_TOPIC_MAX_LEN];
    char topic_health[CP_TOPIC_MAX_LEN];
    char topic_alarms[CP_TOPIC_MAX_LEN];
    char topic_results[CP_TOPIC_MAX_LEN];

    /* FreeRTOS objects */
    QueueHandle_t alarm_queue;
    QueueHandle_t command_queue;
    TimerHandle_t telemetry_timer;
    TimerHandle_t health_timer;
    TimerHandle_t stats_timer;
    TaskHandle_t task_handle;

    bool in_use;
};

static struct cloud_publisher_inst g_pool[CP_MAX_INSTANCES];
static uint8_t g_count;

/* Pool size is 1 — a single module-level pointer lets FreeRTOS timer
 * callbacks (which carry no user-data parameter in this codebase's mock
 * surface) reach the instance without needing xTimerGetTimerID(). */
static struct cloud_publisher_inst *g_active_inst;

static uint8_t s_alarm_queue_storage[CP_ALARM_QUEUE_LEN * sizeof(alarm_event_t)];
static StaticQueue_t s_alarm_queue_ctrl;
static uint8_t s_command_queue_storage[CP_CMD_QUEUE_LEN * sizeof(cp_command_entry_t)];
static StaticQueue_t s_command_queue_ctrl;

static StaticTimer_t s_telemetry_timer_ctrl;
static StaticTimer_t s_health_timer_ctrl;
static StaticTimer_t s_stats_timer_ctrl;

static StackType_t s_task_stack[CP_TASK_STACK_WORDS];
static StaticTask_t s_task_tcb;

/* ===================================================================== */
/* Private function prototypes                                           */
/* ===================================================================== */

static void prv_task_body(void *arg);
static void prv_task_step(struct cloud_publisher_inst *inst);
static cloud_publisher_err_t prv_enqueue_or_publish(struct cloud_publisher_inst *inst,
                                                    const char *topic, const uint8_t *buf,
                                                    uint32_t len, mqtt_qos_t qos);
static void prv_publish_telemetry(struct cloud_publisher_inst *inst);
static void prv_publish_health(struct cloud_publisher_inst *inst);
static void prv_poll_stats(struct cloud_publisher_inst *inst);
static void prv_maybe_reconnect(struct cloud_publisher_inst *inst);
static void prv_drain_saf(struct cloud_publisher_inst *inst);
static void prv_drain_alarm_queue(struct cloud_publisher_inst *inst);
static void prv_drain_command_queue(struct cloud_publisher_inst *inst);
static void prv_route_command(struct cloud_publisher_inst *inst, const cp_command_entry_t *entry);
static void prv_alarm_event_cb(const alarm_event_t *event, void *ctx);
static void prv_msg_cb(struct cloud_publisher_inst *inst, const char *topic, uint16_t topic_len,
                       const uint8_t *payload, uint32_t payload_len);
static void prv_build_topics(struct cloud_publisher_inst *inst);
static void prv_read_device_serial(char *out, uint32_t out_size);
static void prv_timer_telemetry_cb(TimerHandle_t timer);
static void prv_timer_health_cb(TimerHandle_t timer);
static void prv_timer_stats_cb(TimerHandle_t timer);

/* ===================================================================== */
/* Public API                                                             */
/* ===================================================================== */

cloud_publisher_err_t cloud_publisher_create(const cloud_publisher_config_t *config,
                                             cloud_publisher_handle_t *handle)
{
    if ((config == NULL) || (handle == NULL))
    {
        return CP_ERR_NULL_PTR;
    }

    if (g_count >= CP_MAX_INSTANCES)
    {
        return CP_ERR_NO_RESOURCE;
    }

    struct cloud_publisher_inst *inst = &g_pool[g_count];
    g_count++;

    inst->mqtt = config->mqtt;
    inst->sensors = config->sensors;
    inst->alarms = config->alarms;
    inst->poller = config->poller;
    inst->saf = config->saf;
    inst->health_read = config->health_read;
    inst->health_write = config->health_write;
    inst->cfg_read = config->cfg_read;
    inst->cfg_write = config->cfg_write;
    inst->update_svc = config->update_svc;
    inst->lifecycle = config->lifecycle;
    inst->mqtt_connect_cfg = config->mqtt_connect_cfg;
    inst->reconnect_countdown_s = 0u; /* CP-D9: attempt connect on the first stats tick */

    (void) memset(&inst->last_mqtt_stats, 0, sizeof(inst->last_mqtt_stats));

    prv_read_device_serial(inst->device_serial, (uint32_t) sizeof(inst->device_serial));
    prv_build_topics(inst);

    inst->alarm_queue = xQueueCreateStatic(CP_ALARM_QUEUE_LEN, (UBaseType_t) sizeof(alarm_event_t),
                                           s_alarm_queue_storage, &s_alarm_queue_ctrl);
    inst->command_queue =
        xQueueCreateStatic(CP_CMD_QUEUE_LEN, (UBaseType_t) sizeof(cp_command_entry_t),
                           s_command_queue_storage, &s_command_queue_ctrl);

    g_active_inst = inst;

    inst->task_handle = xTaskCreateStatic(prv_task_body, "CloudPublisherTask", CP_TASK_STACK_WORDS,
                                          inst, CP_TASK_PRIORITY, s_task_stack, &s_task_tcb);

    uint32_t telemetry_period_s = config_provider_get_telemetry_interval_s(inst->cfg_read);
    if (telemetry_period_s == 0u)
    {
        telemetry_period_s = CP_TELEMETRY_PERIOD_S_DEFAULT;
    }
    uint32_t health_period_s = config_provider_get_health_interval_s(inst->cfg_read);
    if (health_period_s == 0u)
    {
        health_period_s = CP_HEALTH_PERIOD_S_DEFAULT;
    }

    inst->telemetry_timer =
        xTimerCreateStatic("cp_telemetry", pdMS_TO_TICKS(telemetry_period_s * 1000u), pdTRUE, NULL,
                           prv_timer_telemetry_cb, &s_telemetry_timer_ctrl);
    inst->health_timer =
        xTimerCreateStatic("cp_health", pdMS_TO_TICKS(health_period_s * 1000u), pdTRUE, NULL,
                           prv_timer_health_cb, &s_health_timer_ctrl);
    inst->stats_timer = xTimerCreateStatic("cp_stats", pdMS_TO_TICKS(CP_STATS_PERIOD_MS), pdTRUE,
                                           NULL, prv_timer_stats_cb, &s_stats_timer_ctrl);

    alarm_service_subscribe(inst->alarms, prv_alarm_event_cb, inst);

    (void) xTimerStart(inst->telemetry_timer, 0u);
    (void) xTimerStart(inst->health_timer, 0u);
    (void) xTimerStart(inst->stats_timer, 0u);

    inst->in_use = true;
    *handle = inst;
    return CP_ERR_OK;
}

/* ===================================================================== */
/* Task body                                                             */
/* ===================================================================== */

static void prv_task_body(void *arg)
{
    struct cloud_publisher_inst *inst = (struct cloud_publisher_inst *) arg;
    for (;;)
    {
        prv_task_step(inst);
    }
}

static void prv_task_step(struct cloud_publisher_inst *inst)
{
    uint32_t notif = 0u;
    (void) xTaskNotifyWait(0u, CP_NOTIFY_ALL_BITS, &notif, portMAX_DELAY);

    if ((notif & CP_NOTIFY_TELEMETRY_TICK) != 0u)
    {
        prv_publish_telemetry(inst);
        uint32_t period_s = config_provider_get_telemetry_interval_s(inst->cfg_read);
        if (period_s == 0u)
        {
            period_s = CP_TELEMETRY_PERIOD_S_DEFAULT;
        }
        (void) xTimerChangePeriod(inst->telemetry_timer, pdMS_TO_TICKS(period_s * 1000u), 0u);
    }
    if ((notif & CP_NOTIFY_HEALTH_TICK) != 0u)
    {
        prv_publish_health(inst);
        uint32_t period_s = config_provider_get_health_interval_s(inst->cfg_read);
        if (period_s == 0u)
        {
            period_s = CP_HEALTH_PERIOD_S_DEFAULT;
        }
        (void) xTimerChangePeriod(inst->health_timer, pdMS_TO_TICKS(period_s * 1000u), 0u);
    }
    if ((notif & CP_NOTIFY_STATS_TICK) != 0u)
    {
        prv_poll_stats(inst);
    }
    if ((notif & CP_NOTIFY_ALARM_PENDING) != 0u)
    {
        prv_drain_alarm_queue(inst);
    }
    if ((notif & CP_NOTIFY_COMMAND_PENDING) != 0u)
    {
        prv_drain_command_queue(inst);
    }

    (void) mqtt_client_process(inst->mqtt);
}

/* ===================================================================== */
/* Connectivity gate                                                     */
/*                                                                        */
/* Matches the companion's §5.3 pseudocode exactly, now that              */
/* mqtt_client_is_connected() exists on the real MqttClient API.         */
/* ===================================================================== */

static cloud_publisher_err_t prv_enqueue_or_publish(struct cloud_publisher_inst *inst,
                                                    const char *topic, const uint8_t *buf,
                                                    uint32_t len, mqtt_qos_t qos)
{
    if (mqtt_client_is_connected(inst->mqtt))
    {
        mqtt_client_err_t rc = mqtt_client_publish(inst->mqtt, topic, buf, len, qos);
        if (rc == MQTT_CLIENT_ERR_OK)
        {
            return CP_ERR_OK;
        }
        /* Publish failed while nominally connected — fall through to SAF. */
    }

    saf_err_t saf_rc = store_and_forward_enqueue(inst->saf, topic, buf, len, qos);
    if (saf_rc == SAF_ERR_FULL)
    {
        health_report_push_event(inst->health_write, CP_HEALTH_EVENT_SAF_FULL);
        LOG_WARN(CP_LOG_MODULE, "SAF full, message dropped: %s", topic);
        return CP_ERR_SAF_FULL;
    }

    return CP_ERR_OK; /* buffered for later delivery */
}

/* ===================================================================== */
/* Publish paths                                                         */
/* ===================================================================== */

static void prv_publish_telemetry(struct cloud_publisher_inst *inst)
{
    sensor_reading_t reading = sensor_service_get_latest(inst->sensors);
    modbus_poller_fd_reading_t fd = modbus_poller_get_latest_fd(inst->poller);

    uint32_t n = cp_serialise_telemetry(inst->scratch_buf, CP_JSON_BUF_SIZE, &reading, &fd,
                                        inst->device_serial);
    if (n == 0u)
    {
        LOG_WARN(CP_LOG_MODULE, "telemetry payload exceeded %u bytes", CP_JSON_BUF_SIZE);
        return;
    }

    (void) prv_enqueue_or_publish(inst, inst->topic_telemetry, (const uint8_t *) inst->scratch_buf,
                                  n, MQTT_QOS_0);
}

static void prv_publish_health(struct cloud_publisher_inst *inst)
{
    cp_health_snapshot_t snap = health_snapshot_get(inst->health_read);

    uint32_t n =
        cp_serialise_health(inst->scratch_buf, CP_JSON_BUF_SIZE, &snap, inst->device_serial);
    if (n == 0u)
    {
        LOG_WARN(CP_LOG_MODULE, "health payload exceeded %u bytes", CP_JSON_BUF_SIZE);
        return;
    }

    (void) prv_enqueue_or_publish(inst, inst->topic_health, (const uint8_t *) inst->scratch_buf, n,
                                  MQTT_QOS_0);
}

static void prv_poll_stats(struct cloud_publisher_inst *inst)
{
    mqtt_stats_t stats;
    if (mqtt_client_get_stats(inst->mqtt, &stats) == MQTT_CLIENT_ERR_OK)
    {
        health_report_update_mqtt(inst->health_write, &stats, &inst->last_mqtt_stats);
        inst->last_mqtt_stats = stats;
    }

    prv_maybe_reconnect(inst);

    /* MqttClient exposes no "just reconnected" callback, so the SAF drain
     * (companion §5.4, normally event-driven off a state-change callback)
     * is instead attempted once per STATS_TICK (1 Hz) — the same tick that
     * drives prv_maybe_reconnect() above, so a successful reconnect is
     * followed by a drain attempt within the same cycle. Any in-flight
     * live publish still goes through prv_enqueue_or_publish directly and
     * does not jump this drain (REQ-BF-010 ordering). */
    prv_drain_saf(inst);
}

/**
 * @brief CP-D9: CloudPublisher owns the connect/reconnect state machine.
 *
 * MqttClient runs no thread of its own and does not retry internally
 * (mqtt_client.h: "Does not own the Cloud Connectivity state machine or
 * the reconnect timer — those belong to CloudPublisher"). The initial
 * connect (config->mqtt is handed to CloudPublisher unconnected) and every
 * later reconnect after a drop both go through this single path, driven
 * by the existing 1 Hz stats tick rather than a dedicated task — avoids
 * both an inverted MqttClient->CloudPublisher call and the RAM cost of a
 * new task in an already tight GW memory budget (MQTT-O1).
 *
 * CP-D10: calls mqtt_client_connect_step() (ticked) rather than the
 * blocking mqtt_client_connect(). A stalled reconnect attempt previously
 * froze this whole task — and with it telemetry, health, alarm, and
 * command handling, all driven from the same task — for up to
 * MQTT_CONNECT_TIMEOUT_MS in one call (confirmed on hardware: ~19 s on a
 * TLS handshake timeout). connect_step() advances at most one bounded
 * phase per call, resuming from where the previous stats tick left off
 * (its progress lives on the MqttClient handle, not here) — so a stalled
 * attempt now blocks this task for at most that one phase's own bound
 * per tick instead of the whole sequence at once. MQTT_CLIENT_ERR_IN_PROGRESS
 * means the sequence is still advancing: retried on the next stats tick
 * with no backoff, since it is not a failure. The fixed backoff
 * (CP_RECONNECT_RETRY_PERIOD_S) only arms once connect_step() reports a
 * terminal failure, same as before — it exists so a persistently-down
 * broker does not re-attempt the whole sequence on every single tick.
 */
static void prv_maybe_reconnect(struct cloud_publisher_inst *inst)
{
    if (mqtt_client_is_connected(inst->mqtt))
    {
        inst->reconnect_countdown_s = 0u; /* ready to retry immediately after a future drop */
        return;
    }

    if (inst->reconnect_countdown_s > 0u)
    {
        inst->reconnect_countdown_s--;
        return;
    }

    mqtt_client_err_t rc = mqtt_client_connect_step(inst->mqtt, &inst->mqtt_connect_cfg);
    if (rc == MQTT_CLIENT_ERR_OK)
    {
        LOG_INFO(CP_LOG_MODULE, "MQTT (re)connected");
    }
    else if (rc == MQTT_CLIENT_ERR_IN_PROGRESS)
    {
        /* Sequence still advancing (CP-D10) — call again next stats tick,
         * no backoff; not a failure. */
    }
    else
    {
        LOG_WARN(CP_LOG_MODULE, "MQTT connect attempt failed (rc=%d), retry in %u s", (int) rc,
                 CP_RECONNECT_RETRY_PERIOD_S);
        inst->reconnect_countdown_s = CP_RECONNECT_RETRY_PERIOD_S;
    }
}

static void prv_drain_saf(struct cloud_publisher_inst *inst)
{
    saf_entry_t entry;
    while (store_and_forward_dequeue(inst->saf, &entry) == SAF_ERR_OK)
    {
        mqtt_client_err_t rc =
            mqtt_client_publish(inst->mqtt, entry.topic, entry.payload, entry.len, entry.qos);
        if (rc == MQTT_CLIENT_ERR_OK)
        {
            (void) store_and_forward_confirm(inst->saf);
        }
        else
        {
            break; /* stop drain; re-attempt on next stats tick */
        }
    }
}

static void prv_drain_alarm_queue(struct cloud_publisher_inst *inst)
{
    alarm_event_t ev;
    while (xQueueReceive(inst->alarm_queue, &ev, 0u) == pdTRUE)
    {
        uint32_t n =
            cp_serialise_alarm(inst->scratch_buf, CP_JSON_BUF_SIZE, &ev, inst->device_serial);
        if (n == 0u)
        {
            LOG_WARN(CP_LOG_MODULE, "alarm payload exceeded %u bytes", CP_JSON_BUF_SIZE);
            continue;
        }

        (void) prv_enqueue_or_publish(inst, inst->topic_alarms, (const uint8_t *) inst->scratch_buf,
                                      n, MQTT_QOS_1);
    }
}

/* ===================================================================== */
/* Alarm subscriber callback (runs in SensorTask context)                */
/* ===================================================================== */

static void prv_alarm_event_cb(const alarm_event_t *event, void *ctx)
{
    struct cloud_publisher_inst *inst = (struct cloud_publisher_inst *) ctx;
    (void) xQueueSend(inst->alarm_queue, event, 0u); /* non-blocking */
    (void) xTaskNotify(inst->task_handle, CP_NOTIFY_ALARM_PENDING, eSetBits);
}

/* ===================================================================== */
/* Inbound command handling (CP-O5)                                      */
/*                                                                        */
/* The companion's §5.5 assumes CloudPublisher's msg_cb is registered     */
/* with MqttClient. The real mqtt_client_config_t requires msg_cb at      */
/* mqtt_client_create() time, upstream of CloudPublisher's config         */
/* injection (CloudPublisher receives an already-created mqtt handle) —   */
/* so this wiring is not yet possible and is tracked as an open item      */
/* (whichever module creates the MqttClient instance, likely              */
/* LifecycleController per boot order, needs updating to plumb it         */
/* through). prv_msg_cb is exercised directly via the TEST-only           */
/* injection hook below in the meantime.                                  */
/* ===================================================================== */

static void prv_msg_cb(struct cloud_publisher_inst *inst, const char *topic, uint16_t topic_len,
                       const uint8_t *payload, uint32_t payload_len)
{
    cp_command_entry_t entry;
    (void) memset(&entry, 0, sizeof(entry));

    uint32_t topic_copy_len =
        (topic_len < (CP_TOPIC_MAX_LEN - 1u)) ? (uint32_t) topic_len : (CP_TOPIC_MAX_LEN - 1u);
    (void) memcpy(entry.topic, topic, topic_copy_len);
    entry.topic[topic_copy_len] = '\0';

    uint32_t payload_copy_len =
        (payload_len < CP_CMD_PAYLOAD_MAX_LEN) ? payload_len : CP_CMD_PAYLOAD_MAX_LEN;
    (void) memcpy(entry.payload, payload, payload_copy_len);
    entry.payload_len = payload_copy_len;

    (void) xQueueSend(inst->command_queue, &entry, 0u);
    (void) xTaskNotify(inst->task_handle, CP_NOTIFY_COMMAND_PENDING, eSetBits);
}

static void prv_drain_command_queue(struct cloud_publisher_inst *inst)
{
    cp_command_entry_t entry;
    while (xQueueReceive(inst->command_queue, &entry, 0u) == pdTRUE)
    {
        prv_route_command(inst, &entry);
    }
}

static void prv_route_command(struct cloud_publisher_inst *inst, const cp_command_entry_t *entry)
{
    if (strstr(entry->topic, "/config") != NULL)
    {
        bool ok = config_manager_apply(inst->cfg_write, entry->payload, entry->payload_len);
        const char *result = ok ? "{\"result\":\"ack\"}" : "{\"result\":\"reject\"}";
        (void) prv_enqueue_or_publish(inst, inst->topic_results, (const uint8_t *) result,
                                      (uint32_t) strlen(result), MQTT_QOS_1);
    }
    else if (strstr(entry->topic, "/ota") != NULL)
    {
        if (inst->update_svc != NULL) /* CP-O1: update_svc may be NULL. */
        {
            update_service_handle_command(inst->update_svc, entry->payload, entry->payload_len);
        }
    }
    else if (strstr(entry->topic, "/control") != NULL)
    {
        lifecycle_handle_remote(inst->lifecycle, entry->payload, entry->payload_len);
    }
    else
    {
        LOG_WARN(CP_LOG_MODULE, "unknown command topic: %s", entry->topic);
    }
}

/* ===================================================================== */
/* Timer callbacks                                                       */
/* ===================================================================== */

static void prv_timer_telemetry_cb(TimerHandle_t timer)
{
    (void) timer;
    (void) xTaskNotify(g_active_inst->task_handle, CP_NOTIFY_TELEMETRY_TICK, eSetBits);
}

static void prv_timer_health_cb(TimerHandle_t timer)
{
    (void) timer;
    (void) xTaskNotify(g_active_inst->task_handle, CP_NOTIFY_HEALTH_TICK, eSetBits);
}

static void prv_timer_stats_cb(TimerHandle_t timer)
{
    (void) timer;
    (void) xTaskNotify(g_active_inst->task_handle, CP_NOTIFY_STATS_TICK, eSetBits);
}

/* ===================================================================== */
/* Topic construction and device serial                                  */
/* ===================================================================== */

static void prv_build_topics(struct cloud_publisher_inst *inst)
{
    (void) snprintf(inst->topic_telemetry, CP_TOPIC_MAX_LEN, "%s%s%s", MQTT_TOPIC_PREFIX_PUBLISH,
                    inst->device_serial, MQTT_TOPIC_SUFFIX_TELEMETRY);
    (void) snprintf(inst->topic_health, CP_TOPIC_MAX_LEN, "%s%s%s", MQTT_TOPIC_PREFIX_PUBLISH,
                    inst->device_serial, MQTT_TOPIC_SUFFIX_HEALTH);
    (void) snprintf(inst->topic_alarms, CP_TOPIC_MAX_LEN, "%s%s%s", MQTT_TOPIC_PREFIX_PUBLISH,
                    inst->device_serial, MQTT_TOPIC_SUFFIX_ALARMS);
    /* No shared MQTT_TOPIC_SUFFIX_RESULTS constant exists yet — mqtt_topic_
     * config.h only has OTA_RESULT ("/ota/result"), while this companion's
     * §6 specifies a general "/results" topic for all command acks. Kept
     * as its own literal pending that naming reconciliation. */
    (void) snprintf(inst->topic_results, CP_TOPIC_MAX_LEN, "%s%s/results",
                    MQTT_TOPIC_PREFIX_PUBLISH, inst->device_serial);
}

static void prv_read_device_serial(char *out, uint32_t out_size)
{
#ifdef TEST
    /* Host build has no STM32 UID region; use a fixed placeholder. */
    (void) snprintf(out, out_size, "0011223344556677889900aa");
#else
    const uint32_t *uid = (const uint32_t *) CP_UID_BASE_ADDR;
    (void) snprintf(out, out_size, "%08lx%08lx%08lx", (unsigned long) uid[0],
                    (unsigned long) uid[1], (unsigned long) uid[2]);
#endif
}

#ifdef TEST

void cloud_publisher_reset_for_test(void)
{
    (void) memset(g_pool, 0, sizeof(g_pool));
    g_count = 0u;
    g_active_inst = NULL;
}

void cloud_publisher_task_step_for_test(cloud_publisher_handle_t handle)
{
    prv_task_step(handle);
}

void cloud_publisher_inject_alarm_for_test(cloud_publisher_handle_t handle,
                                           const alarm_event_t *event)
{
    prv_alarm_event_cb(event, handle);
}

void cloud_publisher_inject_command_for_test(cloud_publisher_handle_t handle, const char *topic,
                                             const uint8_t *payload, uint32_t len)
{
    prv_msg_cb(handle, topic, (uint16_t) strlen(topic), payload, len);
}

#endif /* TEST */
