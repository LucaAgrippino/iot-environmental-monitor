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

#define CP_OTA_CHUNK_HEADER_LEN                                                                    \
    8u /**< job (u32 LE) + offset (u32 LE), update-service-lld.md 6.3. */

#define CP_MAX_INSTANCES 1u
#define CP_JSON_BUF_SIZE 4096u /**< Matches MQTT_PKT_BUF_SIZE; see MQTT-O3. */
#define CP_ALARM_QUEUE_LEN 8u
#define CP_CMD_QUEUE_LEN 4u
#define CP_OTA_QUEUE_LEN 4u /**< Outbound OTA requests and reports (CPC-5). */
#define CP_TASK_STACK_WORDS 768u
#define CP_TASK_PRIORITY 3u

#define CP_TELEMETRY_PERIOD_S_DEFAULT 60u
#define CP_HEALTH_PERIOD_S_DEFAULT 600u
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
/* Bit 5 (MQTT_CLIENT_WIFI_RECV_READY_BIT, mqtt_client.h) is set by MqttClient
 * itself, not CloudPublisher — included here so xTaskNotifyWait()'s
 * clear-on-exit mask actually clears it (WIFITASK-O1 Phase 3). Leaving it
 * out of this mask would still wake the task on eSetBits (any bit change
 * unblocks xTaskNotifyWait regardless of mask), but the bit would never be
 * cleared afterwards, so every following wait call would return
 * immediately — a busy-loop, not a missed wake. */
#define CP_NOTIFY_ALL_BITS                                                                         \
    (0x1Fu | MQTT_CLIENT_WIFI_RECV_READY_BIT | CP_NOTIFY_FLUSH_REQUEST | CP_NOTIFY_OTA_OUT)

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
    /* Injected dependencies (all opaque handles) */
    mqtt_client_handle_t mqtt;
    modbus_poller_handle_t poller;
    store_and_forward_handle_t saf;
    health_monitor_handle_t health_read;
    health_monitor_handle_t health_write;
    config_service_handle_t cfg_read;
    config_service_handle_t cfg_write;
    const iupdate_service_t *update_ops;
    update_service_t *update;
    lifecycle_handle_t lifecycle;

    /* Working state */
    char scratch_buf[CP_JSON_BUF_SIZE];
    mqtt_stats_t last_mqtt_stats;
    char device_serial[CP_DEVICE_SERIAL_LEN];
    mqtt_connect_cfg_t mqtt_connect_cfg; /**< CP-D9: owned here for connect()/reconnect(). */
    uint32_t reconnect_countdown_s;      /**< CP-D9: stats ticks left before next retry. */
    uint32_t reconnect_backoff_s;        /**< CP-D13: current backoff, doubles per failure. */

    char topic_telemetry[CP_TOPIC_MAX_LEN];
    char topic_health[CP_TOPIC_MAX_LEN];
    char topic_alarms[CP_TOPIC_MAX_LEN];
    char topic_results[CP_TOPIC_MAX_LEN];
    char topic_ota_cmd[CP_TOPIC_MAX_LEN];     /**< Subscribed: command (QoS 1).            */
    char topic_ota_data[CP_TOPIC_MAX_LEN];    /**< Subscribed: chunk (QoS 0).              */
    char topic_ota_request[CP_TOPIC_MAX_LEN]; /**< Published: chunk request (QoS 0).       */
    char topic_ota_result[CP_TOPIC_MAX_LEN];  /**< Published: result report (QoS 1).       */
    bool ota_subscribed;                      /**< Task only: SUBSCRIBEs done on this link. */
    volatile bool link_up;                    /**< Mirror of is_connected() for any task.  */

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
static void prv_drain_saf(struct cloud_publisher_inst *inst);
static void prv_drain_alarm_queue(struct cloud_publisher_inst *inst);
static void prv_drain_command_queue(struct cloud_publisher_inst *inst);
static void prv_flush(struct cloud_publisher_inst *inst);
static void prv_drain_ota_out(struct cloud_publisher_inst *inst);
static void prv_subscribe_ota(struct cloud_publisher_inst *inst);
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
    inst->poller = config->poller;
    inst->saf = config->saf;
    inst->health_read = config->health_read;
    inst->health_write = config->health_write;
    inst->cfg_read = config->cfg_read;
    inst->cfg_write = config->cfg_write;
    inst->update_ops = config->update_ops;
    inst->update = config->update;
    inst->ota_subscribed = false;
    inst->link_up = false;
    inst->lifecycle = config->lifecycle;
    inst->mqtt_connect_cfg = config->mqtt_connect_cfg;
    inst->reconnect_countdown_s = 0u; /* CP-D9: attempt connect on the first stats tick */
    inst->reconnect_backoff_s = CP_RECONNECT_BACKOFF_MIN_S; /* CP-D13 */

    (void) memset(&inst->last_mqtt_stats, 0, sizeof(inst->last_mqtt_stats));

    prv_read_device_serial(inst->device_serial, (uint32_t) sizeof(inst->device_serial));
    prv_build_topics(inst);

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

    saf_err_t saf_rc = store_and_forward_enqueue(inst->saf, topic, buf, len, qos);
    if (saf_rc == SAF_ERR_FULL)
    {
        health_report_push_event(inst->health_write, CP_HEALTH_EVENT_SAF_FULL);
        LOG_WARN(CP_LOG_MODULE, "SAF full, message dropped: %s", topic);
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
    modbus_poller_fd_reading_t fd = modbus_poller_get_latest_fd(inst->poller);

    uint32_t n = cp_serialise_telemetry(inst->scratch_buf, CP_JSON_BUF_SIZE, &snap, &fd,
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
        inst->reconnect_countdown_s = 0u; /* ready to retry immediately after a future drop */
        inst->reconnect_backoff_s = CP_RECONNECT_BACKOFF_MIN_S; /* CP-D13: fresh curve next drop */
        prv_subscribe_ota(inst); /* CPC-1: after every successful connect; retried until it holds */
        return;
    }

    inst->ota_subscribed = false; /* a new session starts with no subscriptions */

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
                 (unsigned) inst->reconnect_backoff_s);
        inst->reconnect_countdown_s = inst->reconnect_backoff_s;
        /* CP-D13: double for the next consecutive failure, capped at MAX. */
        uint32_t next = inst->reconnect_backoff_s * 2u;
        inst->reconnect_backoff_s =
            (next > CP_RECONNECT_BACKOFF_MAX_S) ? CP_RECONNECT_BACKOFF_MAX_S : next;
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

/* Subscribe to the command and chunk topics. Both must succeed for the flag to be set; a failure
 * is retried on the next stats tick. A Gateway without OTA (update_ops NULL) subscribes to
 * nothing. */
static void prv_subscribe_ota(struct cloud_publisher_inst *inst)
{
    if ((inst->update_ops == NULL) || inst->ota_subscribed)
    {
        return;
    }

    const mqtt_client_err_t rc_cmd =
        mqtt_client_subscribe(inst->mqtt, inst->topic_ota_cmd, MQTT_QOS_1);
    const mqtt_client_err_t rc_data =
        mqtt_client_subscribe(inst->mqtt, inst->topic_ota_data, MQTT_QOS_0);

    if ((rc_cmd == MQTT_CLIENT_ERR_OK) && (rc_data == MQTT_CLIENT_ERR_OK))
    {
        inst->ota_subscribed = true;
        LOG_INFO(CP_LOG_MODULE, "subscribed to the OTA topics");
    }
    else
    {
        LOG_WARN(CP_LOG_MODULE, "OTA subscribe failed (cmd=%d, data=%d), retrying", (int) rc_cmd,
                 (int) rc_data);
    }
}

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
            const uint32_t len =
                cp_serialise_ota_report(inst->scratch_buf, CP_JSON_BUF_SIZE, &entry.u.report);
            if (len > 0u)
            {
                (void) prv_enqueue_or_publish(inst, inst->topic_ota_result,
                                              (const uint8_t *) inst->scratch_buf, len, MQTT_QOS_1);
            }
        }
        else if (mqtt_client_is_connected(inst->mqtt))
        {
            const uint32_t len =
                cp_serialise_ota_request(inst->scratch_buf, CP_JSON_BUF_SIZE, &entry.u.request);
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
        /* Outcome goes to the health report either way: PUBLISHED is the
         * REQ-NF-113 "detection -> publish" endpoint, BUFFERED means the
         * alarm is safe in SAF but still owes the cloud a publish. (SAF_FULL
         * is already pushed inside prv_enqueue_or_publish().) This is also
         * the seam the hardware bring-up times the alarm path through —
         * TC-HW-CP-011, main_test_cloud_publisher.c. */
        if (rc == CP_ERR_OK)
        {
            health_report_push_event(inst->health_write, CP_HEALTH_EVENT_ALARM_PUBLISHED);
        }
        else if (rc == CP_ERR_BUFFERED)
        {
            health_report_push_event(inst->health_write, CP_HEALTH_EVENT_ALARM_BUFFERED);
        }
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
        const uint32_t len = cp_serialise_ota_report(inst->scratch_buf, CP_JSON_BUF_SIZE, &rep);

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

static void prv_route_command(struct cloud_publisher_inst *inst, const cp_command_entry_t *entry)
{
    if (prv_topic_ends_with(entry->topic, (uint32_t) strlen(entry->topic),
                            MQTT_TOPIC_SUFFIX_CONFIG))
    {
        bool ok = config_manager_apply(inst->cfg_write, entry->payload, entry->payload_len);
        const char *result = ok ? "{\"result\":\"ack\"}" : "{\"result\":\"reject\"}";
        (void) prv_enqueue_or_publish(inst, inst->topic_results, (const uint8_t *) result,
                                      (uint32_t) strlen(result), MQTT_QOS_1);
    }
    else if (prv_topic_ends_with(entry->topic, (uint32_t) strlen(entry->topic),
                                 MQTT_TOPIC_SUFFIX_OTA))
    {
        prv_route_ota_command(inst, entry);
    }
    else if (prv_topic_ends_with(entry->topic, (uint32_t) strlen(entry->topic), "/control"))
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
    (void) snprintf(inst->topic_ota_cmd, CP_TOPIC_MAX_LEN, "%s%s%s", MQTT_TOPIC_PREFIX_SUBSCRIBE,
                    inst->device_serial, MQTT_TOPIC_SUFFIX_OTA);
    (void) snprintf(inst->topic_ota_data, CP_TOPIC_MAX_LEN, "%s%s%s", MQTT_TOPIC_PREFIX_SUBSCRIBE,
                    inst->device_serial, MQTT_TOPIC_SUFFIX_OTA_DATA);
    (void) snprintf(inst->topic_ota_request, CP_TOPIC_MAX_LEN, "%s%s%s", MQTT_TOPIC_PREFIX_PUBLISH,
                    inst->device_serial, MQTT_TOPIC_SUFFIX_OTA_REQUEST);
    (void) snprintf(inst->topic_ota_result, CP_TOPIC_MAX_LEN, "%s%s%s", MQTT_TOPIC_PREFIX_PUBLISH,
                    inst->device_serial, MQTT_TOPIC_SUFFIX_OTA_RESULT);
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
