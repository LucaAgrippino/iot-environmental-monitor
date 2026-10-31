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
#include "config_service/config_service.h"

#include <string.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include "queue.h"

#include "logger/logger.h"
#include "mqtt_topic_config.h"

#define CP_LOG_MODULE "CloudPublisher"

#define CP_OTA_CHUNK_HEADER_LEN                                                                    \
    8u /**< job (u32 LE) + offset (u32 LE), update-service-lld.md 6.3. */

#define CP_MAX_INSTANCES 1u
#define CP_JSON_BUF_SIZE 2048u /**< Largest payload (health 1.1 is about 900 B); CP-D28. */

_Static_assert(CP_JSON_BUF_SIZE <= SAF_PAYLOAD_MAX, "a payload must fit one StoreAndForward entry");
#define CP_ALARM_QUEUE_LEN 8u
#define CP_CMD_QUEUE_LEN 4u
#define CP_OTA_QUEUE_LEN 4u /**< Outbound OTA requests and reports (CPC-5). */
#define CP_TASK_STACK_WORDS 768u
#define CP_TASK_PRIORITY 3u

#define CP_TELEMETRY_PERIOD_MS_DEFAULT 60000u
#define CP_HEALTH_PERIOD_MS_DEFAULT 600000u
#define CP_STATS_PERIOD_MS 1000u

/** CP-D9: retry backoff between failed reconnect attempts (stats ticks). */
/* WIFITASK-O2 / CP-D13: reconnect backoff after a terminal connect_step()
 * failure — exponential from MIN, doubling each consecutive failure,
 * capped at MAX, reset to MIN on a successful connect. Bounds how hard a
 * persistently-down broker (or a wedged AP) is re-hammered, which also
 * spaces out the WiFi (re)associations that stress the AP. MIN is >= 2 s
 * deliberately: the retry countdown ticks at the 1 Hz stats rate, so a
 * 1 s floor would let a retry slip through on the very next tick. */
#define CP_RECONNECT_BACKOFF_MIN_S 2u
#define CP_RECONNECT_BACKOFF_MAX_S 60u

/* CP-D32: one store-and-forward replay pass is bounded, so a large backlog is sent over many
 * stats ticks instead of one burst. A pass stops at the first of the three limits; the next pass
 * runs on the next tick. A 4 KB entry is three Wi-Fi module sends (the 1460 B ceiling), and a
 * burst of them took the session down on the board (GM-O20). Measured 2026-10-31: 4 entries of
 * 4 KB per pass failed at the first send after connect; 1 entry per pass ran 72 entries without an
 * error. The 2 KB byte limit ends a pass after any large entry and still lets small ones go four at
 * a time. All four values can be overridden at build time. */
#ifndef CP_DRAIN_MAX_ENTRIES
#define CP_DRAIN_MAX_ENTRIES 4u
#endif
#ifndef CP_DRAIN_MAX_BYTES
#define CP_DRAIN_MAX_BYTES 2048u
#endif
#ifndef CP_DRAIN_MAX_MS
#define CP_DRAIN_MAX_MS 500u
#endif
/* CP-D32: at least this long between two passes. The pass limits above are per pass, and
 * prv_poll_stats() also runs on every receive-ready wake, so without this interval they would not
 * be a rate (hardware, 2026-10-31: 2-5 entries per second reached the broker). */
#ifndef CP_DRAIN_PASS_INTERVAL_MS
#define CP_DRAIN_PASS_INTERVAL_MS 1000u
#endif
/* CP-D32: after a failed replay publish on a live link the drain waits, doubling per consecutive
 * failure like the reconnect curve (CP-D13), instead of retrying every tick. */
#define CP_DRAIN_BACKOFF_MIN_S 5u
#define CP_DRAIN_BACKOFF_MAX_S 60u
/* CP-D32: an entry whose publish fails this many times in a row on a live link is dropped, so one
 * entry the module cannot send cannot hold back everything behind it. */
#define CP_DRAIN_POISON_FAILS 3u

#define CP_NOTIFY_TELEMETRY_TICK (1u << 0)
#define CP_NOTIFY_HEALTH_TICK (1u << 1)
#define CP_NOTIFY_STATS_TICK (1u << 2)
#define CP_NOTIFY_ALARM_PENDING (1u << 3)
#define CP_NOTIFY_COMMAND_PENDING (1u << 4)
/* Bit 5 is MQTT_CLIENT_WIFI_RECV_READY_BIT, below. */
#define CP_NOTIFY_FLUSH_REQUEST                                                                    \
    (1u << 6) /**< LifecycleController: drain what is pending (CP-D16). */
/* Bit 7 was the rollback report (CP-D16); UpdateService reports it now (CPC-6). */
#define CP_NOTIFY_OTA_OUT                                                                          \
    (1u << 8) /**< UpdateService: an OTA request or report is queued (CPC-5). */
#define CP_NOTIFY_CONFIG_CHANGED                                                                   \
    (1u << 9) /**< ConfigService: a publish interval changed (CP-D31). */
/* Bit 5 (MQTT_CLIENT_WIFI_RECV_READY_BIT, mqtt_client.h) is set by MqttClient
 * itself, not CloudPublisher — included here so xTaskNotifyWait()'s
 * clear-on-exit mask actually clears it (WIFITASK-O1 Phase 3). Leaving it
 * out of this mask would still wake the task on eSetBits (any bit change
 * unblocks xTaskNotifyWait regardless of mask), but the bit would never be
 * cleared afterwards, so every following wait call would return
 * immediately — a busy-loop, not a missed wake. */
#define CP_NOTIFY_ALL_BITS                                                                         \
    (0x1Fu | MQTT_CLIENT_WIFI_RECV_READY_BIT | CP_NOTIFY_FLUSH_REQUEST | CP_NOTIFY_OTA_OUT |       \
     CP_NOTIFY_CONFIG_CHANGED)

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

/** @brief One outbound OTA message waiting for CloudPublisherTask (CPC-5). */
typedef struct
{
    bool is_report; /**< true: report (QoS 1, buffered); false: chunk request (QoS 0). */
    union
    {
        us_chunk_request_t request;
        us_ota_report_t report;
    } u;
} cp_ota_out_t;

struct cloud_publisher_inst
{
    /* Injected dependencies (CP-D25) */
    mqtt_client_handle_t mqtt;
    saf_handle_t saf;
    health_monitor_handle_t health;
    const iconfig_provider_t *cfg_read;
    const iconfig_manager_t *cfg_write;
    const itime_service_t *time;
    const ilifecycle_t *lifecycle;
    cp_control_handler_t control_handler;
    uint8_t fd_slave_addr;
    uint32_t fw_version;
    const iupdate_service_t *update_ops;
    update_service_t *update;

    /* Working state */
    char scratch_buf[CP_JSON_BUF_SIZE];
    uint32_t telemetry_ms; /**< Period the telemetry timer runs at (CP-D31). */
    uint32_t health_ms;    /**< Period the health timer runs at (CP-D31).    */
    bool was_connected;    /**< Task only: the link state last reported (CP-D19). */
    char device_serial[CP_DEVICE_SERIAL_LEN];
    mqtt_connect_cfg_t mqtt_connect_cfg;  /**< CP-D9: owned here for connect()/reconnect(). */
    const char *(*connect_prepare)(void); /**< CP-D34: gets the link ready before an attempt. */
    bool connect_in_progress;             /**< CP-D34: a connect sequence is mid-way (CP-D10). */
    uint32_t reconnect_countdown_s;       /**< CP-D9: stats ticks left before next retry. */
    uint32_t reconnect_backoff_s;         /**< CP-D13: current backoff, doubles per failure. */
    bool drain_hold;                      /**< CP-D32: no replay before drain_hold_until.       */
    TickType_t drain_hold_until;          /**< CP-D32: tick at which the hold ends.             */
    uint32_t drain_backoff_s;             /**< CP-D32: replay backoff, doubles per failure.     */
    uint32_t drain_entry_fails;           /**< CP-D32: consecutive failures of the head entry.  */

    char topic_telemetry[CP_TOPIC_MAX_LEN];
    char topic_health[CP_TOPIC_MAX_LEN];
    char topic_alarms[CP_TOPIC_MAX_LEN];
    char topic_config_result[CP_TOPIC_MAX_LEN]; /**< Published: result of /config (CP-D30).  */
    char topic_config_cmd[CP_TOPIC_MAX_LEN];    /**< Subscribed: /config (QoS 1).            */
    char topic_control_cmd[CP_TOPIC_MAX_LEN];   /**< Subscribed: /control (QoS 1).           */
    char topic_ota_cmd[CP_TOPIC_MAX_LEN];       /**< Subscribed: command (QoS 1).            */
    char topic_ota_data[CP_TOPIC_MAX_LEN];      /**< Subscribed: chunk (QoS 0).              */
    char topic_ota_request[CP_TOPIC_MAX_LEN];   /**< Published: chunk request (QoS 0).       */
    char topic_ota_result[CP_TOPIC_MAX_LEN];    /**< Published: result report (QoS 1).       */
    bool sub_config;                            /**< Task only: SUBSCRIBE done on this link. */
    bool sub_control;
    bool sub_ota_cmd;
    bool sub_ota_data;
    volatile bool link_up; /**< Mirror of is_connected() for any task.  */

    /* FreeRTOS objects */
    QueueHandle_t alarm_queue;
    QueueHandle_t command_queue;
    QueueHandle_t ota_queue;
    TimerHandle_t telemetry_timer;
    TimerHandle_t health_timer;
    TimerHandle_t stats_timer;
    TaskHandle_t task_handle;

    alarm_event_t last_raised[SENSOR_ID_COUNT]; /**< Side last raised, for CLEARED payloads. */

    bool in_use;
};

static struct cloud_publisher_inst g_pool[CP_MAX_INSTANCES];
static uint8_t g_count;

/* Pool size is 1 — a single module-level pointer lets FreeRTOS timer
 * callbacks (which carry no user-data parameter in this codebase's mock
 * surface) reach the instance without needing xTimerGetTimerID(). */
static struct cloud_publisher_inst *g_active_inst;

/** The health snapshot is large: one static copy, since the pool has one instance (CP-D25). */
static health_snapshot_t s_health_snap;

#ifdef TEST
static uint32_t s_alarm_published_for_test; /**< REQ-NF-113 seam (CP-D19). */
static uint32_t s_alarm_buffered_for_test;
#endif

static uint8_t s_alarm_queue_storage[CP_ALARM_QUEUE_LEN * sizeof(cp_alarm_event_t)];
static StaticQueue_t s_alarm_queue_ctrl;
static uint8_t s_command_queue_storage[CP_CMD_QUEUE_LEN * sizeof(cp_command_entry_t)];
static StaticQueue_t s_command_queue_ctrl;
static uint8_t s_ota_queue_storage[CP_OTA_QUEUE_LEN * sizeof(cp_ota_out_t)];
static StaticQueue_t s_ota_queue_ctrl;

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
static void prv_note_connect_failure(struct cloud_publisher_inst *inst, mqtt_client_err_t rc);
static void prv_drain_saf(struct cloud_publisher_inst *inst);
static void prv_drain_publish_failed(struct cloud_publisher_inst *inst);
static void prv_drain_alarm_queue(struct cloud_publisher_inst *inst);
static void prv_drain_command_queue(struct cloud_publisher_inst *inst);
static void prv_flush(struct cloud_publisher_inst *inst);
static void prv_drain_ota_out(struct cloud_publisher_inst *inst);
static void prv_subscribe_commands(struct cloud_publisher_inst *inst);
static void prv_note_link(struct cloud_publisher_inst *inst, bool connected);
static void prv_apply_config(struct cloud_publisher_inst *inst, const cp_command_entry_t *entry);
static void prv_config_changed(config_param_id_t param_id);
static void prv_update_intervals(struct cloud_publisher_inst *inst);
static time_provider_ts_t prv_now(void);
static bool prv_ota_enqueue(struct cloud_publisher_inst *inst, const cp_ota_out_t *entry);
static void prv_route_command(struct cloud_publisher_inst *inst, const cp_command_entry_t *entry);
static void prv_route_ota_command(struct cloud_publisher_inst *inst,
                                  const cp_command_entry_t *entry);
static void prv_alarm_event_cb(sensor_id_t sensor, alarm_event_t event,
                               const sensor_reading_t *reading);
static void prv_enqueue_alarm(struct cloud_publisher_inst *inst, const cp_alarm_event_t *event);
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

/** The configured publish period in ms, or the default if ConfigService has none. */
static uint32_t prv_interval_ms(const struct cloud_publisher_inst *inst, bool telemetry)
{
    const config_params_t *params = inst->cfg_read->get_params(inst->cfg_read->ctx);
    uint32_t ms = 0u;

    if (params != NULL)
    {
        ms = telemetry ? params->telemetry_interval_ms : params->health_interval_ms;
    }
    if (ms == 0u)
    {
        ms = telemetry ? CP_TELEMETRY_PERIOD_MS_DEFAULT : CP_HEALTH_PERIOD_MS_DEFAULT;
    }
    return ms;
}

cloud_publisher_err_t cloud_publisher_create(const cloud_publisher_config_t *config,
                                             cloud_publisher_handle_t *handle)
{
    if ((config == NULL) || (handle == NULL) || (config->mqtt == NULL) || (config->saf == NULL) ||
        (config->health == NULL) || (config->cfg_read == NULL) || (config->cfg_write == NULL) ||
        (config->lifecycle == NULL) || ((config->update_ops == NULL) != (config->update == NULL)))
    {
        return CP_ERR_NULL_PTR;
    }

    if (g_count >= CP_MAX_INSTANCES)
    {
        return CP_ERR_NO_RESOURCE;
    }

    /* ConfigService tells us when a publish interval changes, whoever changed it (CP-D31). */
    if (config_service_register_change_cb(prv_config_changed) != CONFIG_SERVICE_OK)
    {
        return CP_ERR_NO_RESOURCE;
    }

    struct cloud_publisher_inst *inst = &g_pool[g_count];
    g_count++;

    inst->mqtt = config->mqtt;
    inst->saf = config->saf;
    inst->health = config->health;
    inst->cfg_read = config->cfg_read;
    inst->cfg_write = config->cfg_write;
    inst->time = config->time;
    inst->lifecycle = config->lifecycle;
    inst->control_handler = config->control_handler;
    inst->fd_slave_addr = config->fd_slave_addr;
    inst->fw_version = config->fw_version;
    inst->update_ops = config->update_ops;
    inst->update = config->update;
    inst->mqtt_connect_cfg = config->mqtt_connect_cfg;
    inst->connect_prepare = config->connect_prepare;
    inst->connect_in_progress = false;
    inst->sub_config = false;
    inst->sub_control = false;
    inst->sub_ota_cmd = false;
    inst->sub_ota_data = false;
    inst->link_up = false;
    inst->was_connected = false;
    inst->reconnect_countdown_s = 0u; /* CP-D9: attempt connect on the first stats tick */
    inst->reconnect_backoff_s = CP_RECONNECT_BACKOFF_MIN_S; /* CP-D13 */
    inst->drain_backoff_s = CP_DRAIN_BACKOFF_MIN_S;         /* CP-D32 */

    prv_read_device_serial(inst->device_serial, (uint32_t) sizeof(inst->device_serial));
    prv_build_topics(inst);
    if (inst->mqtt_connect_cfg.client_id == NULL)
    {
        /* CP-D33 (GM-O19): the client id, the AWS IoT Thing name and the topic prefix are one
         * name, so a ${iot:Connection.Thing.ThingName} policy matches all three. */
        inst->mqtt_connect_cfg.client_id = inst->device_serial;
    }

    inst->alarm_queue =
        xQueueCreateStatic(CP_ALARM_QUEUE_LEN, (UBaseType_t) sizeof(cp_alarm_event_t),
                           s_alarm_queue_storage, &s_alarm_queue_ctrl);
    inst->command_queue =
        xQueueCreateStatic(CP_CMD_QUEUE_LEN, (UBaseType_t) sizeof(cp_command_entry_t),
                           s_command_queue_storage, &s_command_queue_ctrl);
    inst->ota_queue = xQueueCreateStatic(CP_OTA_QUEUE_LEN, (UBaseType_t) sizeof(cp_ota_out_t),
                                         s_ota_queue_storage, &s_ota_queue_ctrl);

    g_active_inst = inst;

    inst->task_handle = xTaskCreateStatic(prv_task_body, "CloudPublisherTask", CP_TASK_STACK_WORDS,
                                          inst, CP_TASK_PRIORITY, s_task_stack, &s_task_tcb);

    inst->telemetry_ms = prv_interval_ms(inst, true);
    inst->health_ms = prv_interval_ms(inst, false);

    inst->telemetry_timer =
        xTimerCreateStatic("cp_telemetry", pdMS_TO_TICKS(inst->telemetry_ms), pdTRUE, NULL,
                           prv_timer_telemetry_cb, &s_telemetry_timer_ctrl);
    inst->health_timer = xTimerCreateStatic("cp_health", pdMS_TO_TICKS(inst->health_ms), pdTRUE,
                                            NULL, prv_timer_health_cb, &s_health_timer_ctrl);
    inst->stats_timer = xTimerCreateStatic("cp_stats", pdMS_TO_TICKS(CP_STATS_PERIOD_MS), pdTRUE,
                                           NULL, prv_timer_stats_cb, &s_stats_timer_ctrl);

    /* AlarmService's callback carries no context pointer, so it finds this
     * instance through g_active_inst (set above). */
    (void) alarm_service->subscribe(prv_alarm_event_cb);

    (void) xTimerStart(inst->telemetry_timer, 0u);
    (void) xTimerStart(inst->health_timer, 0u);
    (void) xTimerStart(inst->stats_timer, 0u);

    inst->in_use = true;
    *handle = inst;
    return CP_ERR_OK;
}

/** Timestamp for a payload: wall-clock when synchronised, uptime otherwise (REQ-TS-040). */
static time_provider_ts_t prv_now(void)
{
    time_provider_ts_t ts;

    if (time_provider_get(&ts) != TIME_PROVIDER_ERR_OK)
    {
        LOG_WARN(CP_LOG_MODULE, "TimeProvider unavailable: uptime, unsynchronised");
        ts.epoch = (uint32_t) (xTaskGetTickCount() / configTICK_RATE_HZ);
        ts.sync_state = TIME_SYNC_UNSYNCHRONISED;
    }
    return ts;
}

/** ConfigService change callback: runs in the caller's task, so it only wakes ours (CP-D31). */
static void prv_config_changed(config_param_id_t param_id)
{
    struct cloud_publisher_inst *inst = g_active_inst;

    if ((inst == NULL) || (inst->task_handle == NULL))
    {
        return;
    }
    if ((param_id == CONFIG_PARAM_TELEMETRY_INTERVAL) || (param_id == CONFIG_PARAM_HEALTH_INTERVAL))
    {
        (void) xTaskNotify(inst->task_handle, CP_NOTIFY_CONFIG_CHANGED, eSetBits);
    }
}

/** Re-read both intervals and retime the timer of each that changed (CP-D31). */
static void prv_update_intervals(struct cloud_publisher_inst *inst)
{
    const uint32_t telemetry_ms = prv_interval_ms(inst, true);
    const uint32_t health_ms = prv_interval_ms(inst, false);

    if (telemetry_ms != inst->telemetry_ms)
    {
        inst->telemetry_ms = telemetry_ms;
        (void) xTimerChangePeriod(inst->telemetry_timer, pdMS_TO_TICKS(telemetry_ms), 0u);
    }
    if (health_ms != inst->health_ms)
    {
        inst->health_ms = health_ms;
        (void) xTimerChangePeriod(inst->health_timer, pdMS_TO_TICKS(health_ms), 0u);
    }
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

    /* The mirror UpdateService reads through IOtaTransport.is_connected (CPC-5). */
    inst->link_up = mqtt_client_is_connected(inst->mqtt);

    /* Alarms and inbound commands run before the periodic telemetry/health
     * publishes: all four ultimately call prv_enqueue_or_publish(), which
     * for QoS1 blocks this single task ~350 ms on the PUBACK, so a telemetry
     * publish handled first would push a co-woken alarm past REQ-NF-113's
     * 500 ms budget (CP-D3; TC-HW-CP-011). This orders the co-wake case; it
     * cannot preempt a publish already in flight (see CP-O6). */
    if ((notif & CP_NOTIFY_ALARM_PENDING) != 0u)
    {
        prv_drain_alarm_queue(inst);
    }
    if ((notif & CP_NOTIFY_COMMAND_PENDING) != 0u)
    {
        prv_drain_command_queue(inst);
    }
    if ((notif & CP_NOTIFY_OTA_OUT) != 0u)
    {
        prv_drain_ota_out(inst);
    }
    if ((notif & CP_NOTIFY_FLUSH_REQUEST) != 0u)
    {
        prv_flush(inst);
    }
    if ((notif & CP_NOTIFY_CONFIG_CHANGED) != 0u)
    {
        prv_update_intervals(inst);
    }
    if ((notif & CP_NOTIFY_TELEMETRY_TICK) != 0u)
    {
        prv_publish_telemetry(inst);
    }
    if ((notif & CP_NOTIFY_HEALTH_TICK) != 0u)
    {
        prv_publish_health(inst);
    }
    /* WIFITASK-O1 Phase 3: also re-drive on MQTT_CLIENT_WIFI_RECV_READY_BIT,
     * not just the 1 Hz stats tick — otherwise a TLS handshake or MQTT read
     * that just got real data via wifitask_try_recv() (mqtt_client.c)
     * sits idle until the next stats tick instead of being retried
     * immediately, adding up to ~1 s of dead time per round-trip on top of
     * WIFI-O11's own irreducible module latency. See prv_maybe_reconnect()
     * below (CP-D9/CP-D10) for the actual retry driven here. */
    if ((notif & (CP_NOTIFY_STATS_TICK | MQTT_CLIENT_WIFI_RECV_READY_BIT)) != 0u)
    {
        prv_poll_stats(inst);
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

    /* The real SAF drops its oldest entry when the ring is full and succeeds (REQ-BF-020), so a
     * refusal is a flash error or an entry that does not fit: the message is lost (CP-D29). */
    const saf_err_t saf_rc = saf_enqueue(inst->saf, topic, buf, (uint16_t) len, (uint8_t) qos);
    if (saf_rc != SAF_ERR_OK)
    {
        LOG_ERROR(CP_LOG_MODULE, "SAF refused %s (code %d), message lost", topic, (int) saf_rc);
        return CP_ERR_SAF_FULL;
    }

    return CP_ERR_BUFFERED; /* queued for later delivery */
}

/* ===================================================================== */
/* Publish paths                                                         */
/* ===================================================================== */

static void prv_publish_telemetry(struct cloud_publisher_inst *inst)
{
    sensor_snapshot_t snap;
    if (sensor_service->get_snapshot(&snap) != SENSOR_SERVICE_ERR_OK)
    {
        LOG_WARN(CP_LOG_MODULE, "telemetry skipped: no sensor snapshot");
        return;
    }
    modbus_fd_reading_t fd;
    const bool has_fd =
        (inst->fd_slave_addr != 0u) &&
        (modbus_poller_get_fd_reading(inst->fd_slave_addr, &fd) == MODBUS_POLLER_ERR_OK);
    const time_provider_ts_t ts = prv_now();

    uint32_t n = cp_serialise_telemetry(inst->scratch_buf, CP_JSON_BUF_SIZE, &snap,
                                        has_fd ? &fd : NULL, &ts, inst->device_serial);
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
    /* The stack watermarks are polled on each health tick, before the snapshot (HealthMonitor). */
    (void) health_monitor_poll(inst->health);
    if (health_monitor_get_snapshot(inst->health, &s_health_snap) != HEALTH_MONITOR_ERR_OK)
    {
        LOG_WARN(CP_LOG_MODULE, "health skipped: no snapshot");
        return;
    }
    const time_provider_ts_t ts = prv_now();

    uint32_t n = cp_serialise_health(inst->scratch_buf, CP_JSON_BUF_SIZE, &s_health_snap,
                                     inst->fw_version, &ts, inst->device_serial);
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
        (void) health_monitor_update_mqtt_stats(inst->health, &stats);
    }

    prv_maybe_reconnect(inst);

    /* MqttClient exposes no "just reconnected" callback, so the SAF drain
     * (companion §5.4, normally event-driven off a state-change callback)
     * is instead attempted once per STATS_TICK (1 Hz) — the same tick that
     * drives prv_maybe_reconnect() above, so a successful reconnect is
     * followed by a drain attempt within the same cycle. Any in-flight
     * live publish still goes through prv_enqueue_or_publish directly and
     * does not jump this drain (REQ-BF-010 ordering). */
    if (inst->drain_hold &&
        ((TickType_t) (xTaskGetTickCount() - inst->drain_hold_until) < (TickType_t) 0x80000000u))
    {
        inst->drain_hold = false; /* CP-D32: the interval or the backoff has run out */
    }
    if (!inst->drain_hold)
    {
        prv_drain_saf(inst);
    }
}

/**
 * Record a change of the broker link (CP-D19, CP-D20, CP-D23). Task context only.
 *
 * Disconnected to connected: tell HealthMonitor, ask TimeService for an NTP attempt (REQ-TS-0E1:
 * the boot attempt usually ran before WiFi was up) and forget the subscriptions of the old
 * session. Connected to disconnected: tell HealthMonitor and mark the subscriptions lost.
 */
static void prv_note_link(struct cloud_publisher_inst *inst, bool connected)
{
    inst->link_up = connected;

    if (connected == inst->was_connected)
    {
        return;
    }
    inst->was_connected = connected;

    inst->sub_config = false;
    inst->sub_control = false;
    inst->sub_ota_cmd = false;
    inst->sub_ota_data = false;

    if (connected)
    {
        (void) health_monitor_push_event(inst->health, HEALTH_EVENT_CLOUD_CONNECTED, 0u);
        if (inst->time != NULL)
        {
            inst->time->trigger_retry(inst->time->ctx);
        }
    }
    else
    {
        (void) health_monitor_push_event(inst->health, HEALTH_EVENT_CLOUD_DISCONNECTED, 0u);
    }
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
 * with no backoff, since it is not a failure. The backoff (CP-D13,
 * WIFITASK-O2) only arms once connect_step() reports a terminal failure:
 * an exponential curve from CP_RECONNECT_BACKOFF_MIN_S, doubling each
 * consecutive failure up to CP_RECONNECT_BACKOFF_MAX_S, reset to MIN on a
 * successful connect. It keeps a persistently-down broker from being
 * re-hammered every tick and spaces out the WiFi re-associations that
 * stress the AP.
 */
static void prv_maybe_reconnect(struct cloud_publisher_inst *inst)
{
    if (mqtt_client_is_connected(inst->mqtt))
    {
        prv_note_link(inst, true);
        inst->reconnect_countdown_s = 0u; /* ready to retry immediately after a future drop */
        inst->reconnect_backoff_s = CP_RECONNECT_BACKOFF_MIN_S; /* CP-D13: fresh curve next drop */
        prv_subscribe_commands(
            inst); /* CPC-1, CP-D26: after every connect; retried until all hold */
        return;
    }

    prv_note_link(inst, false); /* a new session starts with no subscriptions */

    if (inst->reconnect_countdown_s > 0u)
    {
        inst->reconnect_countdown_s--;
        return;
    }

    /* CP-D34: a fresh attempt starts with the link made ready (rejoin, resolve the broker name
     * again: the address of a cloud endpoint changes over a long run). Not between the steps of
     * one attempt, whose socket and handshake are already under way. */
    if (!inst->connect_in_progress && (inst->connect_prepare != NULL))
    {
        const char *endpoint = inst->connect_prepare();

        if (endpoint == NULL)
        {
            prv_note_connect_failure(inst, MQTT_CLIENT_ERR_NOT_CONNECTED);
            return;
        }
        inst->mqtt_connect_cfg.broker_endpoint = endpoint;
    }

    mqtt_client_err_t rc = mqtt_client_connect_step(inst->mqtt, &inst->mqtt_connect_cfg);
    inst->connect_in_progress = (rc == MQTT_CLIENT_ERR_IN_PROGRESS);
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
        prv_note_connect_failure(inst, rc);
    }
}

/* CP-D13: a connect attempt failed (or could not start): wait, doubling per consecutive failure. */
static void prv_note_connect_failure(struct cloud_publisher_inst *inst, mqtt_client_err_t rc)
{
    LOG_WARN(CP_LOG_MODULE, "MQTT connect attempt failed (rc=%d), retry in %u s", (int) rc,
             (unsigned) inst->reconnect_backoff_s);
    inst->reconnect_countdown_s = inst->reconnect_backoff_s;
    uint32_t next = inst->reconnect_backoff_s * 2u;
    inst->reconnect_backoff_s =
        (next > CP_RECONNECT_BACKOFF_MAX_S) ? CP_RECONNECT_BACKOFF_MAX_S : next;
}

static void prv_drain_saf(struct cloud_publisher_inst *inst)
{
    const saf_entry_t *entry = NULL;
    uint32_t sent = 0u;
    uint32_t bytes = 0u;
    const TickType_t start = xTaskGetTickCount();
    bool failed = false;

    /* Replay is attempted on a live link only: a publish refused because the link is down says
     * nothing about the entry and must not start the backoff or count against it (CP-D32). */
    if (!mqtt_client_is_connected(inst->mqtt))
    {
        return;
    }

    /* The entry points into SAF's own buffer and is valid only until the next SAF call, so it is
     * published before anything else touches SAF. A repeated dequeue without a confirm returns
     * the same entry, which is how a failed publish is retried. One pass sends at most
     * CP_DRAIN_MAX_ENTRIES / _BYTES / _MS (CP-D32). */
    while ((sent < CP_DRAIN_MAX_ENTRIES) && (bytes < CP_DRAIN_MAX_BYTES) &&
           ((xTaskGetTickCount() - start) < pdMS_TO_TICKS(CP_DRAIN_MAX_MS)) &&
           (saf_dequeue(inst->saf, &entry) == SAF_ERR_OK))
    {
        mqtt_client_err_t rc = mqtt_client_publish(inst->mqtt, entry->topic, entry->payload,
                                                   entry->payload_len, (mqtt_qos_t) entry->qos);
        if (rc != MQTT_CLIENT_ERR_OK)
        {
            prv_drain_publish_failed(inst);
            failed = true;
            break; /* stop drain; re-attempt after the backoff */
        }
        inst->drain_entry_fails = 0u;
        inst->drain_backoff_s = CP_DRAIN_BACKOFF_MIN_S;
        sent++;
        bytes += entry->payload_len;
        if (saf_confirm(inst->saf) != SAF_ERR_OK)
        {
            /* The entry stays pending and is sent again: a duplicate, acceptable at QoS 1. */
            LOG_WARN(CP_LOG_MODULE, "SAF confirm failed, drain stops for this tick");
            break;
        }
        /* Serve the link between entries (CP-D32): keep-alive, acknowledgements and inbound
         * packets must not wait for the end of a replay pass. */
        (void) mqtt_client_process(inst->mqtt);
        if (!mqtt_client_is_connected(inst->mqtt))
        {
            break;
        }
    }

    if (!failed && (sent > 0u))
    {
        /* CP-D32: the next pass waits out the interval, which makes the limits a rate. */
        inst->drain_hold = true;
        inst->drain_hold_until = xTaskGetTickCount() + pdMS_TO_TICKS(CP_DRAIN_PASS_INTERVAL_MS);
    }
}

/* CP-D32: a replay publish failed on a live link. Back off, and drop the entry when it has now
 * failed CP_DRAIN_POISON_FAILS times in a row. The entry is still the head of the log (nothing
 * was confirmed), so the drop is a confirm without a publish. */
static void prv_drain_publish_failed(struct cloud_publisher_inst *inst)
{
    inst->drain_hold = true;
    inst->drain_hold_until = xTaskGetTickCount() + pdMS_TO_TICKS(inst->drain_backoff_s * 1000u);
    const uint32_t next = inst->drain_backoff_s * 2u;
    inst->drain_backoff_s = (next > CP_DRAIN_BACKOFF_MAX_S) ? CP_DRAIN_BACKOFF_MAX_S : next;

    inst->drain_entry_fails++;
    if (inst->drain_entry_fails >= CP_DRAIN_POISON_FAILS)
    {
        LOG_WARN(CP_LOG_MODULE, "SAF entry failed %u times, dropped",
                 (unsigned) inst->drain_entry_fails);
        inst->drain_entry_fails = 0u;
        if (saf_confirm(inst->saf) == SAF_ERR_OK)
        {
            (void) health_monitor_push_event(inst->health, HEALTH_EVENT_BUFFER_OVERFLOW, 0u);
        }
    }
}

/* Flush (CP-D16): hand over what is waiting. Alarms first (CP-D3), then the offline buffer if the
 * broker is up. Anything that cannot go out stays in the store-and-forward log, which is
 * flash-backed and so survives the reset that follows a restart. */
static void prv_flush(struct cloud_publisher_inst *inst)
{
    prv_drain_alarm_queue(inst);
    if (mqtt_client_is_connected(inst->mqtt))
    {
        prv_drain_saf(inst);
    }
}

/* ===================================================================== */
/* OTA transport (CPC-1, CPC-5)                                           */
/* ===================================================================== */

/* Subscribe to every command topic after a connect (CP-D26). One flag per topic, so a partial
 * failure retries only what is missing, on the next stats tick. The OTA topics are subscribed only
 * when UpdateService is wired, /control only when a handler is. */
static bool prv_subscribe_one(struct cloud_publisher_inst *inst, bool *done, const char *topic,
                              mqtt_qos_t qos)
{
    if (*done)
    {
        return true;
    }

    const mqtt_client_err_t rc = mqtt_client_subscribe(inst->mqtt, topic, qos);

    if (rc == MQTT_CLIENT_ERR_OK)
    {
        *done = true;
        return true;
    }
    LOG_WARN(CP_LOG_MODULE, "subscribe to %s failed (%d), retrying", topic, (int) rc);
    return false;
}

static void prv_subscribe_commands(struct cloud_publisher_inst *inst)
{
    bool all = prv_subscribe_one(inst, &inst->sub_config, inst->topic_config_cmd, MQTT_QOS_1);

    if (inst->control_handler != NULL)
    {
        all =
            prv_subscribe_one(inst, &inst->sub_control, inst->topic_control_cmd, MQTT_QOS_1) && all;
    }
    if (inst->update_ops != NULL)
    {
        all = prv_subscribe_one(inst, &inst->sub_ota_cmd, inst->topic_ota_cmd, MQTT_QOS_1) && all;
        all = prv_subscribe_one(inst, &inst->sub_ota_data, inst->topic_ota_data, MQTT_QOS_0) && all;
    }
    (void) all;
}

/* OTA transport (CPC-1, CPC-5): see also prv_subscribe_commands() above. */

static bool prv_ota_enqueue(struct cloud_publisher_inst *inst, const cp_ota_out_t *entry)
{
    if (xQueueSend(inst->ota_queue, entry, 0u) != pdTRUE)
    {
        return false;
    }

    (void) xTaskNotify(inst->task_handle, CP_NOTIFY_OTA_OUT, eSetBits);
    return true;
}

/* Send what UpdateService queued. A chunk request is fire-and-forget at QoS 0 and is dropped if
 * the link is down (UpdateService re-requests at its deadline); a report goes through the normal
 * gate at QoS 1 so it is buffered while the broker is away. */
static void prv_drain_ota_out(struct cloud_publisher_inst *inst)
{
    cp_ota_out_t entry;

    while (xQueueReceive(inst->ota_queue, &entry, 0u) == pdTRUE)
    {
        if (entry.is_report)
        {
            const time_provider_ts_t ts = prv_now();
            const uint32_t len = cp_serialise_ota_report(inst->scratch_buf, CP_JSON_BUF_SIZE,
                                                         &entry.u.report, &ts, inst->device_serial);
            if (len > 0u)
            {
                (void) prv_enqueue_or_publish(inst, inst->topic_ota_result,
                                              (const uint8_t *) inst->scratch_buf, len, MQTT_QOS_1);
            }
        }
        else if (mqtt_client_is_connected(inst->mqtt))
        {
            const time_provider_ts_t ts = prv_now();
            const uint32_t len = cp_serialise_ota_request(
                inst->scratch_buf, CP_JSON_BUF_SIZE, &entry.u.request, &ts, inst->device_serial);
            if (len > 0u)
            {
                (void) mqtt_client_publish(inst->mqtt, inst->topic_ota_request,
                                           (const uint8_t *) inst->scratch_buf, len, MQTT_QOS_0);
            }
        }
        else
        {
            /* Link down: not buffered, UpdateService asks again. */
        }
    }
}

/* cppcheck-suppress constParameterCallback -- the ctx type is fixed by iota_transport_t */
static bool prv_ota_is_connected(void *ctx)
{
    return ((const struct cloud_publisher_inst *) ctx)->link_up;
}

static bool prv_ota_request_chunk(void *ctx, const us_chunk_request_t *req)
{
    cp_ota_out_t entry;

    (void) memset(&entry, 0, sizeof(entry));
    entry.is_report = false;
    entry.u.request = *req;
    return prv_ota_enqueue((struct cloud_publisher_inst *) ctx, &entry);
}

static bool prv_ota_report(void *ctx, const us_ota_report_t *rep)
{
    cp_ota_out_t entry;

    (void) memset(&entry, 0, sizeof(entry));
    entry.is_report = true;
    entry.u.report = *rep;
    return prv_ota_enqueue((struct cloud_publisher_inst *) ctx, &entry);
}

static void prv_drain_alarm_queue(struct cloud_publisher_inst *inst)
{
    cp_alarm_event_t ev;
    while (xQueueReceive(inst->alarm_queue, &ev, 0u) == pdTRUE)
    {
        uint32_t n =
            cp_serialise_alarm(inst->scratch_buf, CP_JSON_BUF_SIZE, &ev, inst->device_serial);
        if (n == 0u)
        {
            LOG_WARN(CP_LOG_MODULE, "alarm payload exceeded %u bytes", CP_JSON_BUF_SIZE);
            continue;
        }

        const cloud_publisher_err_t rc = prv_enqueue_or_publish(
            inst, inst->topic_alarms, (const uint8_t *) inst->scratch_buf, n, MQTT_QOS_1);
        /* The REQ-NF-113 "detection -> publish" outcome is a local seam (CP-D19): nothing in the
         * health snapshot consumes it. PUBLISHED went out now; BUFFERED is safe in SAF but still
         * owes the cloud a publish. TC-HW-CP-011 times the alarm path from the broker's side. */
        LOG_DEBUG(CP_LOG_MODULE, "alarm %s: %s", ev.sensor_name,
                  (rc == CP_ERR_OK) ? "published"
                                    : ((rc == CP_ERR_BUFFERED) ? "buffered" : "lost"));
        (void) rc;
#ifdef TEST
        if (rc == CP_ERR_OK)
        {
            s_alarm_published_for_test++;
        }
        else if (rc == CP_ERR_BUFFERED)
        {
            s_alarm_buffered_for_test++;
        }
        else
        {
            /* lost: logged above */
        }
#endif
    }
}

/* ===================================================================== */
/* Alarm subscriber callback (runs in SensorTask context)                */
/* ===================================================================== */

static void prv_enqueue_alarm(struct cloud_publisher_inst *inst, const cp_alarm_event_t *event)
{
    (void) xQueueSend(inst->alarm_queue, event, 0u); /* non-blocking */
    (void) xTaskNotify(inst->task_handle, CP_NOTIFY_ALARM_PENDING, eSetBits);
}

static const char *prv_sensor_name(sensor_id_t sensor)
{
    switch (sensor)
    {
    case SENSOR_ID_TEMPERATURE:
        return "temperature";
    case SENSOR_ID_HUMIDITY:
        return "humidity";
    case SENSOR_ID_PRESSURE:
        return "pressure";
    default:
        return NULL; /* IMU / magnetometer channels never alarm (SS-O3) */
    }
}

static void prv_alarm_event_cb(sensor_id_t sensor, alarm_event_t event,
                               const sensor_reading_t *reading)
{
    struct cloud_publisher_inst *inst = g_active_inst;
    const char *name = prv_sensor_name(sensor);
    if ((inst == NULL) || (name == NULL) || (reading == NULL))
    {
        return;
    }

    /* A CLEARED event no longer says which limit was crossed, so remember the
     * side that was last raised for this sensor. Only SensorTask calls here. */
    if (event == ALARM_EVENT_RAISED_HIGH)
    {
        inst->last_raised[sensor] = ALARM_EVENT_RAISED_HIGH;
    }
    else if (event == ALARM_EVENT_RAISED_LOW)
    {
        inst->last_raised[sensor] = ALARM_EVENT_RAISED_LOW;
    }
    else
    {
        /* CLEARED keeps the remembered side. */
    }

    cp_alarm_event_t ev;
    (void) memset(&ev, 0, sizeof(ev));
    (void) strncpy(ev.sensor_name, name, sizeof(ev.sensor_name) - 1u);
    ev.alarm_type = (event == ALARM_EVENT_RAISED_HIGH)  ? CP_ALARM_TYPE_HIGH
                    : (event == ALARM_EVENT_RAISED_LOW) ? CP_ALARM_TYPE_LOW
                                                        : CP_ALARM_TYPE_CLEAR;
    ev.measured_value_raw = reading->value;
    ev.source = CP_ALARM_SOURCE_GATEWAY;
    ev.timestamp = reading->timestamp; /* time of detection, not of serialisation (CP-D22) */

    int32_t limit = 0;
    if (alarm_service->get_limit(sensor, inst->last_raised[sensor], &limit) == ALARM_SERVICE_ERR_OK)
    {
        ev.threshold_value_raw = limit;
    }

    prv_enqueue_alarm(inst, &ev);
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

/** @brief True if @p topic (of @p len bytes) ends with @p suffix. */
static bool prv_topic_ends_with(const char *topic, uint32_t len, const char *suffix)
{
    const uint32_t n = (uint32_t) strlen(suffix);

    return (len >= n) && (memcmp(&topic[len - n], suffix, n) == 0);
}

/* The OTA chunk (CPC-3): 8-byte header (job, offset; u32 little-endian) then 1..1024 bytes. It is
 * not queued: the payload does not fit the command queue, UpdateService copies it into its single
 * chunk slot and returns at once, and the pointer is valid only inside this callback. A chunk that
 * is not the one outstanding is STALE, which is normal after a retry. */
static void prv_handle_ota_chunk(struct cloud_publisher_inst *inst, const uint8_t *payload,
                                 uint32_t payload_len)
{
    if ((inst->update_ops == NULL) || (inst->update == NULL))
    {
        return;
    }
    if (payload_len <= CP_OTA_CHUNK_HEADER_LEN)
    {
        LOG_WARN(CP_LOG_MODULE, "OTA chunk too short (%lu bytes)", (unsigned long) payload_len);
        return;
    }

    const uint32_t job = (uint32_t) payload[0] | ((uint32_t) payload[1] << 8) |
                         ((uint32_t) payload[2] << 16) | ((uint32_t) payload[3] << 24);
    const uint32_t offset = (uint32_t) payload[4] | ((uint32_t) payload[5] << 8) |
                            ((uint32_t) payload[6] << 16) | ((uint32_t) payload[7] << 24);
    const update_service_err_t rc =
        inst->update_ops->on_chunk(inst->update, job, offset, &payload[CP_OTA_CHUNK_HEADER_LEN],
                                   payload_len - CP_OTA_CHUNK_HEADER_LEN);

    if (rc == UPDATE_SERVICE_ERR_STALE)
    {
        LOG_DEBUG(CP_LOG_MODULE, "stale OTA chunk at %lu", (unsigned long) offset);
    }
    else if (rc != UPDATE_SERVICE_ERR_OK)
    {
        LOG_WARN(CP_LOG_MODULE, "OTA chunk refused (%d)", (int) rc);
    }
}

static void prv_msg_cb(struct cloud_publisher_inst *inst, const char *topic, uint16_t topic_len,
                       const uint8_t *payload, uint32_t payload_len)
{
    /* /ota/data first: routing is by exact suffix, and "/ota" must not capture it (CPC-2). */
    if (prv_topic_ends_with(topic, topic_len, MQTT_TOPIC_SUFFIX_OTA_DATA))
    {
        prv_handle_ota_chunk(inst, payload, payload_len);
        return;
    }

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

void cloud_publisher_on_message(const char *topic, uint16_t topic_len, const uint8_t *payload,
                                uint32_t payload_len)
{
    if ((g_active_inst == NULL) || (topic == NULL) || (payload == NULL))
    {
        return;
    }

    prv_msg_cb(g_active_inst, topic, topic_len, payload, payload_len);
}

void cloud_publisher_on_disconnect(void)
{
    struct cloud_publisher_inst *inst = g_active_inst;

    if (inst != NULL)
    {
        prv_note_link(inst, false);
    }
}

static void prv_drain_command_queue(struct cloud_publisher_inst *inst)
{
    cp_command_entry_t entry;
    while (xQueueReceive(inst->command_queue, &entry, 0u) == pdTRUE)
    {
        prv_route_command(inst, &entry);
    }
}

/* The OTA command (CPC-4): parse it, hand it to UpdateService, or reject it as malformed (UC-19
 * E2). Whether a well-formed command is acceptable (state, sizes, job range) is UpdateService's
 * decision and it reports that itself. */
static void prv_route_ota_command(struct cloud_publisher_inst *inst,
                                  const cp_command_entry_t *entry)
{
    if ((inst->update_ops == NULL) || (inst->update == NULL))
    {
        LOG_WARN(CP_LOG_MODULE, "OTA command ignored: no UpdateService");
        return;
    }

    us_ota_command_t cmd;
    uint32_t job = 0u;
    bool job_known = false;

    if (!cp_parse_ota_command(entry->payload, entry->payload_len, &cmd, &job, &job_known))
    {
        const us_ota_report_t rep = {
            .job_known = job_known,
            .job = job,
            .result = US_RESULT_REJECTED,
            .reason = US_REASON_BAD_COMMAND,
            .version = 0u,
            .rollback_count = 0u,
        };
        const time_provider_ts_t ts = prv_now();
        const uint32_t len = cp_serialise_ota_report(inst->scratch_buf, CP_JSON_BUF_SIZE, &rep, &ts,
                                                     inst->device_serial);

        LOG_WARN(CP_LOG_MODULE, "malformed OTA command rejected");
        if (len > 0u)
        {
            (void) prv_enqueue_or_publish(inst, inst->topic_ota_result,
                                          (const uint8_t *) inst->scratch_buf, len, MQTT_QOS_1);
        }
        return;
    }

    const update_service_err_t rc = inst->update_ops->handle_command(inst->update, &cmd);

    if (rc != UPDATE_SERVICE_ERR_OK)
    {
        LOG_WARN(CP_LOG_MODULE, "OTA command refused by UpdateService (%d)", (int) rc);
    }
}

/* ---------------------------------------------------------------------- */
/* Remote configuration (SD-07, CP-D21, CP-D27)                             */
/* ---------------------------------------------------------------------- */

/** One parsed command at a time and CloudPublisherTask alone uses it: static, not on the stack. */
static cp_cfg_cmd_t s_cfg_cmd;

static void prv_publish_config_result(struct cloud_publisher_inst *inst, cp_cfg_status_t status,
                                      const char *key)
{
    const time_provider_ts_t ts = prv_now();
    const uint32_t n =
        cp_serialise_config_result(inst->scratch_buf, CP_JSON_BUF_SIZE, s_cfg_cmd.req_known,
                                   s_cfg_cmd.req, status, key, &ts, inst->device_serial);

    if (n == 0u)
    {
        LOG_WARN(CP_LOG_MODULE, "config result exceeded %u bytes", CP_JSON_BUF_SIZE);
        return;
    }
    (void) prv_enqueue_or_publish(inst, inst->topic_config_result,
                                  (const uint8_t *) inst->scratch_buf, n, MQTT_QOS_1);
}

/** The current value of a parameter, as the wide integer the parser uses. */
static int64_t prv_read_param(const config_params_t *params, const cp_cfg_pair_t *pair)
{
    const uint8_t *src = &((const uint8_t *) params)[pair->offset];

    if (pair->size == 4u)
    {
        uint32_t v;
        (void) memcpy(&v, src, sizeof(v));
        return (int64_t) v;
    }
    if (pair->is_signed)
    {
        int16_t v;
        (void) memcpy(&v, src, sizeof(v));
        return (int64_t) v;
    }
    uint16_t v;
    (void) memcpy(&v, src, sizeof(v));
    return (int64_t) v;
}

/** set_param_deferred() with the C type of the field (applied in RAM, never writes flash). */
static config_service_err_t prv_set_param(const struct cloud_publisher_inst *inst,
                                          const cp_cfg_pair_t *pair, int64_t value)
{
    if (pair->size == 4u)
    {
        const uint32_t v = (uint32_t) value;
        return inst->cfg_write->set_param_deferred(inst->cfg_write->ctx, pair->id, &v);
    }
    if (pair->is_signed)
    {
        const int16_t v = (int16_t) value;
        return inst->cfg_write->set_param_deferred(inst->cfg_write->ctx, pair->id, &v);
    }
    const uint16_t v = (uint16_t) value;
    return inst->cfg_write->set_param_deferred(inst->cfg_write->ctx, pair->id, &v);
}

/*
 * /config: parse a flat JSON object of integers, apply every parameter or none. Nothing is written
 * to flash here (LifecycleController persists on its idle tick, LC-D23), so the MQTT task never
 * blocks. Refused unless the Gateway is Operational: a local cancel would restore the snapshot and
 * silently undo an acknowledged remote change (REQ-LI-120).
 */
static void prv_apply_config(struct cloud_publisher_inst *inst, const cp_command_entry_t *entry)
{
    const cp_cfg_status_t status =
        cp_parse_config_command(entry->payload, entry->payload_len, &s_cfg_cmd);

    if (inst->lifecycle->get_state() != LIFECYCLE_STATE_OPERATIONAL)
    {
        prv_publish_config_result(inst, CP_CFG_BUSY, NULL);
        return;
    }
    if (status != CP_CFG_OK)
    {
        prv_publish_config_result(inst, status, s_cfg_cmd.bad_key);
        return;
    }

    const config_params_t *params = inst->cfg_read->get_params(inst->cfg_read->ctx);
    int64_t old[CP_CFG_MAX_KEYS] = {0};

    for (uint8_t i = 0u; (i < s_cfg_cmd.count) && (params != NULL); i++)
    {
        old[i] = prv_read_param(params, &s_cfg_cmd.pairs[i]);
    }

    for (uint8_t i = 0u; i < s_cfg_cmd.count; i++)
    {
        if (prv_set_param(inst, &s_cfg_cmd.pairs[i], s_cfg_cmd.pairs[i].value) != CONFIG_SERVICE_OK)
        {
            /* Back out what was applied, newest first, so every state in between stays valid. */
            for (uint8_t j = i; j > 0u; j--)
            {
                if (prv_set_param(inst, &s_cfg_cmd.pairs[j - 1u], old[j - 1u]) != CONFIG_SERVICE_OK)
                {
                    LOG_WARN(CP_LOG_MODULE, "could not restore %s", s_cfg_cmd.pairs[j - 1u].key);
                }
            }
            prv_publish_config_result(inst, CP_CFG_INVALID_VALUE, s_cfg_cmd.pairs[i].key);
            return;
        }
    }

    prv_publish_config_result(inst, CP_CFG_OK, NULL);
}

static void prv_route_command(struct cloud_publisher_inst *inst, const cp_command_entry_t *entry)
{
    const uint32_t topic_len = (uint32_t) strlen(entry->topic);

    if (prv_topic_ends_with(entry->topic, topic_len, MQTT_TOPIC_SUFFIX_CONFIG))
    {
        prv_apply_config(inst, entry);
    }
    else if (prv_topic_ends_with(entry->topic, topic_len, MQTT_TOPIC_SUFFIX_OTA))
    {
        prv_route_ota_command(inst, entry);
    }
    else if (prv_topic_ends_with(entry->topic, topic_len, MQTT_TOPIC_SUFFIX_CONTROL))
    {
        if (inst->control_handler != NULL)
        {
            inst->control_handler(entry->payload, entry->payload_len);
        }
        else
        {
            LOG_WARN(CP_LOG_MODULE, "/control ignored: no handler");
        }
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
    (void) snprintf(inst->topic_ota_cmd, CP_TOPIC_MAX_LEN, "%s%s%s", MQTT_TOPIC_PREFIX_SUBSCRIBE,
                    inst->device_serial, MQTT_TOPIC_SUFFIX_OTA);
    (void) snprintf(inst->topic_ota_data, CP_TOPIC_MAX_LEN, "%s%s%s", MQTT_TOPIC_PREFIX_SUBSCRIBE,
                    inst->device_serial, MQTT_TOPIC_SUFFIX_OTA_DATA);
    (void) snprintf(inst->topic_ota_request, CP_TOPIC_MAX_LEN, "%s%s%s", MQTT_TOPIC_PREFIX_PUBLISH,
                    inst->device_serial, MQTT_TOPIC_SUFFIX_OTA_REQUEST);
    (void) snprintf(inst->topic_ota_result, CP_TOPIC_MAX_LEN, "%s%s%s", MQTT_TOPIC_PREFIX_PUBLISH,
                    inst->device_serial, MQTT_TOPIC_SUFFIX_OTA_RESULT);
    (void) snprintf(inst->topic_config_result, CP_TOPIC_MAX_LEN, "%s%s%s",
                    MQTT_TOPIC_PREFIX_PUBLISH, inst->device_serial,
                    MQTT_TOPIC_SUFFIX_CONFIG_RESULT);
    (void) snprintf(inst->topic_config_cmd, CP_TOPIC_MAX_LEN, "%s%s%s", MQTT_TOPIC_PREFIX_SUBSCRIBE,
                    inst->device_serial, MQTT_TOPIC_SUFFIX_CONFIG);
    (void) snprintf(inst->topic_control_cmd, CP_TOPIC_MAX_LEN, "%s%s%s",
                    MQTT_TOPIC_PREFIX_SUBSCRIBE, inst->device_serial, MQTT_TOPIC_SUFFIX_CONTROL);
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

/* ===================================================================== */
/* LifecycleController operations (CP-D16)                               */
/* ===================================================================== */

bool cloud_publisher_is_ready(cloud_publisher_handle_t handle)
{
    return (handle != NULL) && handle->in_use && (handle->task_handle != NULL);
}

cloud_publisher_err_t cloud_publisher_flush(cloud_publisher_handle_t handle)
{
    if (handle == NULL)
    {
        return CP_ERR_NULL_PTR;
    }
    if (!cloud_publisher_is_ready(handle))
    {
        return CP_ERR_NOT_INIT;
    }
    return (xTaskNotify(handle->task_handle, CP_NOTIFY_FLUSH_REQUEST, eSetBits) == pdTRUE)
               ? CP_ERR_OK
               : CP_ERR_NO_RESOURCE;
}

cloud_publisher_err_t cloud_publisher_get_ota_transport(cloud_publisher_handle_t handle,
                                                        iota_transport_t *out)
{
    if ((handle == NULL) || (out == NULL))
    {
        return CP_ERR_NULL_PTR;
    }
    if (!cloud_publisher_is_ready(handle))
    {
        return CP_ERR_NOT_INIT;
    }

    out->ctx = handle;
    out->is_connected = prv_ota_is_connected;
    out->request_chunk = prv_ota_request_chunk;
    out->report = prv_ota_report;

    return CP_ERR_OK;
}

#ifdef TEST

void cloud_publisher_reset_for_test(void)
{
    (void) memset(g_pool, 0, sizeof(g_pool));
    g_count = 0u;
    g_active_inst = NULL;
    s_alarm_published_for_test = 0u;
    s_alarm_buffered_for_test = 0u;
    (void) memset(&s_cfg_cmd, 0, sizeof(s_cfg_cmd));
}

uint32_t cloud_publisher_alarm_published_for_test(void)
{
    return s_alarm_published_for_test;
}

uint32_t cloud_publisher_alarm_buffered_for_test(void)
{
    return s_alarm_buffered_for_test;
}

void cloud_publisher_task_step_for_test(cloud_publisher_handle_t handle)
{
    prv_task_step(handle);
}

void cloud_publisher_inject_alarm_for_test(cloud_publisher_handle_t handle,
                                           const cp_alarm_event_t *event)
{
    prv_enqueue_alarm(handle, event);
}

uint32_t cloud_publisher_ota_entry_size_for_test(void)
{
    return (uint32_t) sizeof(cp_ota_out_t);
}

void cloud_publisher_inject_command_for_test(cloud_publisher_handle_t handle, const char *topic,
                                             const uint8_t *payload, uint32_t len)
{
    prv_msg_cb(handle, topic, (uint16_t) strlen(topic), payload, len);
}

#endif /* TEST */
