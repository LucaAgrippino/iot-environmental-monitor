/**
 * @file test_cloud_publisher.c
 * @brief Unit tests for CloudPublisher (Gateway).
 *
 * Covers CP-T01..CP-T16 as defined in
 * docs/lld/application/cloud-publisher-lld.md §13, plus CP-T17/T18 (new,
 * post-CP-D9 connect/reconnect coverage — not in the companion's original
 * §13 table).
 *
 * Build defines (project.yml): STM32L475xx, BOARD_GATEWAY, TEST.
 *
 * Test strategy: spy stubs for every dependency (bodies defined inline
 * per the test isolation convention), FreeRTOS mock, single-step via
 * cloud_publisher_task_step_for_test().
 *
 * No separate dependency-stub header is used: cloud_publisher.h already
 * self-declares placeholder types/prototypes for every GW peer module
 * that does not yet exist (see cloud_publisher.h's own header comment),
 * so including it is sufficient. MqttClient is the one real, already-
 * built dependency; its types/prototypes arrive transitively through
 * cloud_publisher.h's real "mqtt_client.h" include — this test file
 * must NOT also #include "mqtt_client.h" directly (Ceedling's auto-link
 * only inspects a test file's own direct #includes, so keeping that
 * include indirect avoids linking the real mqtt_client.c and lets the
 * spy bodies below provide those symbols instead).
 *
 * Deviations from the companion exercised here (see session report):
 *  - CP-D9 (new): mqtt_client_is_connected() now exists on MqttClient, and
 *    CloudPublisher owns the connect/reconnect state machine (see
 *    prv_maybe_reconnect() in cloud_publisher.c) rather than assuming an
 *    already-connected handle. CP-T05/T08 assert publish() is NOT called
 *    while disconnected (matching the companion's original §5.3 intent
 *    exactly); CP-T17/T18 cover the reconnect attempt + backoff.
 *  - CP-D10 (new): prv_maybe_reconnect() now calls the ticked
 *    mqtt_client_connect_step() instead of the blocking
 *    mqtt_client_connect() (see mqtt-client.md MQTT-D8) — the spy below
 *    mocks connect_step() accordingly. CP-T17/T18 are unaffected (a
 *    single ERR_OK or ERR_CONNECT_FAIL tick behaves the same either way);
 *    CP-T19 covers the new ERR_IN_PROGRESS case specifically.
 *  - CP-O5: MqttClient's msg_cb is not yet wired to CloudPublisher (it is
 *    registered upstream, at mqtt_client_create() time). CP-T11/T12 use
 *    the cloud_publisher_inject_command_for_test() seam instead.
 */

#include "unity.h"

#include <stddef.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos_mock.h"
#include "logger_stub.h"
#include "cloud_publisher/cloud_publisher.h"
#include "cloud_publisher/cloud_publisher_json.h"
#include "us_reason.h" /* causes Ceedling to auto-link us_reason.c */

/* Notification bits — mirrors companion §4 (private in cloud_publisher.c,
 * re-declared here since they are an internal implementation detail). */
#define TC_TELEMETRY_TICK (1u << 0)
#define TC_HEALTH_TICK (1u << 1)
#define TC_STATS_TICK (1u << 2)
#define TC_ALARM_PENDING (1u << 3)
#define TC_COMMAND_PENDING (1u << 4)
#define TC_WIFI_RECV_READY (1u << 5) /**< Mirrors MQTT_CLIENT_WIFI_RECV_READY_BIT. */
#define TC_CONFIG_CHANGED (1u << 9)

/* Mirrors CP_JSON_BUF_SIZE (private to cloud_publisher.c). */
#define TC_JSON_BUF_SIZE 2048u

/* Mirrors sizeof(cp_command_entry_t): topic[64] + payload[128] + len(4)
 * (private to cloud_publisher.c) — keep in sync if that struct changes.
 * Used to enable xQueueSend mock capture of the command queue entry. */
#define TC_COMMAND_ENTRY_SIZE 196u

/* ======================================================================= */
/* Spy infrastructure — bodies for every stubbed dependency               */
/* ======================================================================= */

void logger_log(log_level_t level, const char *module, const char *msg)
{
    (void) level;
    (void) module;
    (void) msg;
}

/* SensorService / AlarmService are linked as spies: the test never includes their
 * headers directly, so Ceedling does not pull in the real .c files. */
static sensor_snapshot_t g_spy_snapshot;
static sensor_service_err_t g_spy_snapshot_return;
static uint32_t g_spy_sensor_get_snapshot_calls;
static sensor_service_err_t spy_sensor_get_snapshot(sensor_snapshot_t *snap)
{
    g_spy_sensor_get_snapshot_calls++;
    *snap = g_spy_snapshot;
    return g_spy_snapshot_return;
}
static const isensor_service_t g_spy_sensor_vtable = {.get_snapshot = spy_sensor_get_snapshot};
const isensor_service_t *const sensor_service = &g_spy_sensor_vtable;

static modbus_fd_reading_t g_spy_fd_reading;
static modbus_poller_err_t g_spy_fd_return;
static uint8_t g_spy_fd_slave_seen;
static uint32_t g_spy_fd_calls;
modbus_poller_err_t modbus_poller_get_fd_reading(uint8_t slave_addr, modbus_fd_reading_t *out)
{
    g_spy_fd_calls++;
    g_spy_fd_slave_seen = slave_addr;
    *out = g_spy_fd_reading;
    return g_spy_fd_return;
}

static modbus_poller_alarm_cb_t g_spy_fd_alarm_cb;
void modbus_poller_set_alarm_callback(modbus_poller_alarm_cb_t callback)
{
    g_spy_fd_alarm_cb = callback;
}

static void (*g_spy_alarm_cb)(sensor_id_t, alarm_event_t, const sensor_reading_t *);
static uint32_t g_spy_alarm_subscribe_calls;
static int32_t g_spy_alarm_limit;
static sensor_id_t g_spy_alarm_limit_sensor;
static alarm_event_t g_spy_alarm_limit_event;
static alarm_service_err_t spy_alarm_subscribe(void (*cb)(sensor_id_t, alarm_event_t,
                                                          const sensor_reading_t *))
{
    g_spy_alarm_subscribe_calls++;
    g_spy_alarm_cb = cb;
    return ALARM_SERVICE_ERR_OK;
}
static alarm_service_err_t spy_alarm_get_limit(sensor_id_t sensor, alarm_event_t event,
                                               int32_t *limit_out)
{
    g_spy_alarm_limit_sensor = sensor;
    g_spy_alarm_limit_event = event;
    *limit_out = g_spy_alarm_limit;
    return ALARM_SERVICE_ERR_OK;
}
static const ialarm_service_t g_spy_alarm_vtable = {.subscribe = spy_alarm_subscribe,
                                                    .get_limit = spy_alarm_get_limit};
const ialarm_service_t *const alarm_service = &g_spy_alarm_vtable;

/* One counter orders the calls the tests compare (poll before snapshot, publish before confirm). */
static uint32_t g_spy_seq;

static saf_err_t g_spy_saf_enqueue_return;
static uint32_t g_spy_saf_enqueue_calls;
static char g_spy_saf_enqueue_last_topic[SAF_TOPIC_MAX + 1u];
static uint8_t g_spy_saf_enqueue_last_qos;
static uint16_t g_spy_saf_enqueue_last_len;
saf_err_t saf_enqueue(saf_handle_t handle, const char *topic, const uint8_t *buf, uint16_t len,
                      uint8_t qos)
{
    (void) handle;
    (void) buf;
    g_spy_saf_enqueue_calls++;
    (void) strncpy(g_spy_saf_enqueue_last_topic, topic, SAF_TOPIC_MAX);
    g_spy_saf_enqueue_last_qos = qos;
    g_spy_saf_enqueue_last_len = len;
    return g_spy_saf_enqueue_return;
}

static saf_err_t g_spy_saf_dequeue_return;
static saf_entry_t g_spy_saf_dequeue_entry;
static uint8_t g_spy_saf_dequeue_payload[SAF_PAYLOAD_MAX];
static uint32_t g_spy_saf_dequeue_calls;
/* Number of times to deliver g_spy_saf_dequeue_entry while g_spy_saf_dequeue_return is OK before
 * falling back to EMPTY: a permanently-OK spy would spin prv_drain_saf()'s loop forever. A repeated
 * dequeue without a confirm returns the same entry, like the real module. */
static uint32_t g_spy_saf_dequeue_available;
saf_err_t saf_dequeue(saf_handle_t handle, const saf_entry_t **out)
{
    (void) handle;
    g_spy_saf_dequeue_calls++;
    if ((g_spy_saf_dequeue_return == SAF_ERR_OK) && (g_spy_saf_dequeue_available > 0u))
    {
        g_spy_saf_dequeue_available--;
        g_spy_saf_dequeue_entry.payload = g_spy_saf_dequeue_payload;
        *out = &g_spy_saf_dequeue_entry;
        return SAF_ERR_OK;
    }
    return SAF_ERR_EMPTY;
}

static uint32_t g_spy_saf_confirm_calls;
static saf_err_t g_spy_saf_confirm_return;
saf_err_t saf_confirm(saf_handle_t handle)
{
    (void) handle;
    g_spy_saf_confirm_calls++;
    return g_spy_saf_confirm_return;
}

/* HealthMonitor: the real entry points the module calls. */
static health_snapshot_t g_spy_health_snapshot;
static health_monitor_err_t g_spy_health_get_return;
static uint32_t g_spy_health_poll_seq;
static uint32_t g_spy_health_get_seq;
health_monitor_err_t health_monitor_poll(health_monitor_handle_t handle)
{
    (void) handle;
    g_spy_health_poll_seq = ++g_spy_seq;
    return HEALTH_MONITOR_ERR_OK;
}
health_monitor_err_t health_monitor_get_snapshot(health_monitor_handle_t handle,
                                                 health_snapshot_t *snap_out)
{
    (void) handle;
    g_spy_health_get_seq = ++g_spy_seq;
    *snap_out = g_spy_health_snapshot;
    return g_spy_health_get_return;
}
static uint32_t g_spy_health_update_mqtt_calls;
static mqtt_stats_t g_spy_health_update_mqtt_stats;
health_monitor_err_t health_monitor_update_mqtt_stats(health_monitor_handle_t handle,
                                                      const mqtt_stats_t *stats)
{
    (void) handle;
    g_spy_health_update_mqtt_calls++;
    g_spy_health_update_mqtt_stats = *stats;
    return HEALTH_MONITOR_ERR_OK;
}
#define TC_EVENT_LOG (8u)
static uint32_t g_spy_health_push_event_calls;
static health_event_t g_spy_health_events[TC_EVENT_LOG];
health_monitor_err_t health_monitor_push_event(health_monitor_handle_t handle, health_event_t event,
                                               uint32_t param)
{
    (void) handle;
    (void) param;
    if (g_spy_health_push_event_calls < TC_EVENT_LOG)
    {
        g_spy_health_events[g_spy_health_push_event_calls] = event;
    }
    g_spy_health_push_event_calls++;
    return HEALTH_MONITOR_ERR_OK;
}

/* ConfigService: the two vtables the module is given, and the change-callback registration. */
static config_params_t g_spy_params;
static const config_params_t *spy_get_params(void *ctx)
{
    (void) ctx;
    return &g_spy_params;
}
static const iconfig_provider_t g_spy_provider = {.ctx = NULL, .get_params = spy_get_params};

#define TC_SET_LOG (10u)
static uint32_t g_spy_set_deferred_calls;
static config_param_id_t g_spy_set_ids[TC_SET_LOG];
static int64_t g_spy_set_values[TC_SET_LOG];
static config_service_err_t g_spy_set_fail_on_call; /* 1-based call index that fails, 0: never */
static uint32_t g_spy_set_calls_blocking;           /* set_param / persist_pending: must stay 0 */
static uint32_t g_spy_set_fail_index;
static config_service_err_t spy_set_param_deferred(void *ctx, config_param_id_t id,
                                                   const void *value)
{
    (void) ctx;
    /* The module passes the C type of the field; the spy needs the value as a number. */
    int64_t v = 0;
    switch (id)
    {
    case CONFIG_PARAM_POLL_INTERVAL:
    case CONFIG_PARAM_MODBUS_POLL_PERIOD:
    case CONFIG_PARAM_TELEMETRY_INTERVAL:
    case CONFIG_PARAM_HEALTH_INTERVAL:
        v = (int64_t) * (const uint32_t *) value;
        break;
    case CONFIG_PARAM_TEMP_ALARM_HIGH:
    case CONFIG_PARAM_TEMP_ALARM_LOW:
    case CONFIG_PARAM_TEMP_HYSTERESIS:
        v = (int64_t) * (const int16_t *) value;
        break;
    default:
        v = (int64_t) * (const uint16_t *) value;
        break;
    }
    if (g_spy_set_deferred_calls < TC_SET_LOG)
    {
        g_spy_set_ids[g_spy_set_deferred_calls] = id;
        g_spy_set_values[g_spy_set_deferred_calls] = v;
    }
    g_spy_set_deferred_calls++;
    return (g_spy_set_deferred_calls == g_spy_set_fail_index) ? CONFIG_SERVICE_ERR_INVALID
                                                              : CONFIG_SERVICE_OK;
}
static config_service_err_t spy_set_param(void *ctx, config_param_id_t id, const void *value)
{
    (void) ctx;
    (void) id;
    (void) value;
    g_spy_set_calls_blocking++;
    return CONFIG_SERVICE_OK;
}
static config_service_err_t spy_persist_pending(void *ctx)
{
    (void) ctx;
    g_spy_set_calls_blocking++;
    return CONFIG_SERVICE_OK;
}
static const iconfig_manager_t g_spy_manager = {.ctx = NULL,
                                                .set_param = spy_set_param,
                                                .set_param_deferred = spy_set_param_deferred,
                                                .persist_pending = spy_persist_pending};

static config_change_cb_t g_spy_change_cb;
static config_service_err_t g_spy_register_return;
static uint32_t g_spy_register_calls;
config_service_err_t config_service_register_change_cb(config_change_cb_t cb)
{
    g_spy_register_calls++;
    g_spy_change_cb = cb;
    return g_spy_register_return;
}

/* TimeProvider and TimeService. */
static time_provider_ts_t g_spy_time_ts;
static time_provider_err_t g_spy_time_return;
time_provider_err_t time_provider_get(time_provider_ts_t *ts_out)
{
    *ts_out = g_spy_time_ts;
    return g_spy_time_return;
}
static uint32_t g_spy_retry_calls;
static void spy_trigger_retry(void *ctx)
{
    (void) ctx;
    g_spy_retry_calls++;
}
static time_service_sync_t spy_get_sync_state(void *ctx)
{
    (void) ctx;
    return TIME_SERVICE_SYNC_OK;
}
static const itime_service_t g_spy_time = {
    .ctx = NULL, .get_sync_state = spy_get_sync_state, .trigger_retry = spy_trigger_retry};

/* ILifecycle (get_state only) and the /control handler. */
static lifecycle_state_t g_spy_life_state;
static lifecycle_state_t spy_life_get_state(void)
{
    return g_spy_life_state;
}
static const ilifecycle_t g_spy_lifecycle = {.get_state = spy_life_get_state};

static uint32_t g_spy_control_calls;
static uint32_t g_spy_control_len;
static void spy_control_handler(const uint8_t *payload, uint32_t len)
{
    (void) payload;
    g_spy_control_calls++;
    g_spy_control_len = len;
}

static uint32_t g_spy_update_cmd_calls;
static us_ota_command_t g_spy_update_cmd;
static update_service_err_t g_spy_update_cmd_return;
static update_service_t *g_spy_update_self_seen;
static uint32_t g_spy_update_chunk_calls;
static uint32_t g_spy_chunk_job;
static uint32_t g_spy_chunk_offset;
static uint32_t g_spy_chunk_len;
static uint8_t g_spy_chunk_first_byte;
static update_service_err_t g_spy_update_chunk_return;

static update_service_err_t spy_update_handle_command(update_service_t *self,
                                                      const us_ota_command_t *cmd)
{
    g_spy_update_self_seen = self;
    g_spy_update_cmd_calls++;
    g_spy_update_cmd = *cmd;
    return g_spy_update_cmd_return;
}

static update_service_err_t spy_update_on_chunk(update_service_t *self, uint32_t job,
                                                uint32_t offset, const uint8_t *data, uint32_t len)
{
    g_spy_update_self_seen = self;
    g_spy_update_chunk_calls++;
    g_spy_chunk_job = job;
    g_spy_chunk_offset = offset;
    g_spy_chunk_len = len;
    g_spy_chunk_first_byte = data[0];
    return g_spy_update_chunk_return;
}

static const iupdate_service_t g_spy_update_ops = {.handle_command = spy_update_handle_command,
                                                   .on_chunk = spy_update_on_chunk};

static mqtt_client_err_t g_spy_mqtt_publish_return;
static uint32_t g_spy_mqtt_publish_calls;
/* Ticks each publish takes, added to the mock tick counter (the drain's time budget, CP-D32). */
static TickType_t g_spy_mqtt_publish_tick_step;
static char g_spy_mqtt_publish_last_topic[SAF_TOPIC_MAX];
static char g_spy_mqtt_publish_last_payload[TC_JSON_BUF_SIZE];
static uint32_t g_spy_mqtt_publish_last_len;
static mqtt_qos_t g_spy_mqtt_publish_last_qos;
mqtt_client_err_t mqtt_client_publish(mqtt_client_handle_t handle, const char *topic,
                                      const uint8_t *payload, uint32_t len, mqtt_qos_t qos)
{
    (void) handle;
    g_spy_mqtt_publish_calls++;
    g_mock_tick_count += g_spy_mqtt_publish_tick_step;
    (void) strncpy(g_spy_mqtt_publish_last_topic, topic, SAF_TOPIC_MAX - 1u);
    uint32_t copy_len = (len < (sizeof(g_spy_mqtt_publish_last_payload) - 1u))
                            ? len
                            : (sizeof(g_spy_mqtt_publish_last_payload) - 1u);
    (void) memcpy(g_spy_mqtt_publish_last_payload, payload, copy_len);
    g_spy_mqtt_publish_last_payload[copy_len] = '\0';
    g_spy_mqtt_publish_last_len = len;
    g_spy_mqtt_publish_last_qos = qos;
    return g_spy_mqtt_publish_return;
}

static uint32_t g_spy_mqtt_subscribe_calls;
static char g_spy_mqtt_subscribe_topic[4][SAF_TOPIC_MAX];
static mqtt_qos_t g_spy_mqtt_subscribe_qos[4];
static mqtt_client_err_t g_spy_mqtt_subscribe_return;
mqtt_client_err_t mqtt_client_subscribe(mqtt_client_handle_t handle, const char *topic,
                                        mqtt_qos_t qos)
{
    (void) handle;
    if (g_spy_mqtt_subscribe_calls < 4u)
    {
        (void) strncpy(g_spy_mqtt_subscribe_topic[g_spy_mqtt_subscribe_calls], topic,
                       SAF_TOPIC_MAX - 1u);
        g_spy_mqtt_subscribe_qos[g_spy_mqtt_subscribe_calls] = qos;
    }
    g_spy_mqtt_subscribe_calls++;
    return g_spy_mqtt_subscribe_return;
}

static mqtt_stats_t g_spy_mqtt_stats;
static uint32_t g_spy_mqtt_get_stats_calls;
mqtt_client_err_t mqtt_client_get_stats(mqtt_client_handle_t handle, mqtt_stats_t *stats_out)
{
    (void) handle;
    g_spy_mqtt_get_stats_calls++;
    *stats_out = g_spy_mqtt_stats;
    return MQTT_CLIENT_ERR_OK;
}

static uint32_t g_spy_mqtt_process_calls;
mqtt_client_err_t mqtt_client_process(mqtt_client_handle_t handle)
{
    (void) handle;
    g_spy_mqtt_process_calls++;
    return MQTT_CLIENT_ERR_OK;
}

static bool g_spy_mqtt_is_connected_return;
bool mqtt_client_is_connected(mqtt_client_handle_t handle)
{
    (void) handle;
    return g_spy_mqtt_is_connected_return;
}

static mqtt_client_err_t g_spy_mqtt_connect_return;
static uint32_t g_spy_mqtt_connect_calls;
static const char *g_spy_mqtt_connect_last_endpoint;
static const char *g_spy_mqtt_connect_last_client_id;
mqtt_client_err_t mqtt_client_connect_step(mqtt_client_handle_t handle,
                                           const mqtt_connect_cfg_t *cfg)
{
    (void) handle;
    g_spy_mqtt_connect_calls++;
    g_spy_mqtt_connect_last_endpoint = cfg->broker_endpoint;
    g_spy_mqtt_connect_last_client_id = cfg->client_id;
    return g_spy_mqtt_connect_return;
}

/* ======================================================================= */
/* Fixture                                                                 */
/* ======================================================================= */

static cloud_publisher_config_t g_cfg;
static cloud_publisher_handle_t g_handle;

void setUp(void)
{
    mock_freertos_reset();
    cloud_publisher_reset_for_test();
    g_spy_seq = 0u;

    (void) memset(&g_spy_snapshot, 0, sizeof(g_spy_snapshot));
    g_spy_snapshot_return = SENSOR_SERVICE_ERR_OK;
    g_spy_sensor_get_snapshot_calls = 0u;

    g_spy_alarm_limit = 0;
    g_spy_alarm_cb = NULL;
    g_spy_fd_alarm_cb = NULL;
    g_spy_alarm_subscribe_calls = 0u;

    (void) memset(&g_spy_fd_reading, 0, sizeof(g_spy_fd_reading));
    g_spy_fd_return = MODBUS_POLLER_ERR_OK;
    g_spy_fd_slave_seen = 0u;
    g_spy_fd_calls = 0u;

    g_spy_saf_enqueue_return = SAF_ERR_OK;
    g_spy_saf_enqueue_calls = 0u;
    g_spy_saf_enqueue_last_qos = 0u;
    g_spy_saf_enqueue_last_len = 0u;
    (void) memset(g_spy_saf_enqueue_last_topic, 0, sizeof(g_spy_saf_enqueue_last_topic));
    g_spy_saf_dequeue_return = SAF_ERR_EMPTY;
    (void) memset(&g_spy_saf_dequeue_entry, 0, sizeof(g_spy_saf_dequeue_entry));
    (void) memset(g_spy_saf_dequeue_payload, 0, sizeof(g_spy_saf_dequeue_payload));
    g_spy_saf_dequeue_calls = 0u;
    g_spy_saf_dequeue_available = 0u;
    g_spy_saf_confirm_calls = 0u;
    g_spy_saf_confirm_return = SAF_ERR_OK;

    (void) memset(&g_spy_health_snapshot, 0, sizeof(g_spy_health_snapshot));
    g_spy_health_get_return = HEALTH_MONITOR_ERR_OK;
    g_spy_health_poll_seq = 0u;
    g_spy_health_get_seq = 0u;
    g_spy_health_update_mqtt_calls = 0u;
    (void) memset(&g_spy_health_update_mqtt_stats, 0, sizeof(g_spy_health_update_mqtt_stats));
    g_spy_health_push_event_calls = 0u;
    (void) memset(g_spy_health_events, 0, sizeof(g_spy_health_events));

    (void) memset(&g_spy_params, 0, sizeof(g_spy_params));
    g_spy_params.telemetry_interval_ms = 60000u;
    g_spy_params.health_interval_ms = 600000u;
    g_spy_set_deferred_calls = 0u;
    g_spy_set_fail_index = 0u;
    g_spy_set_calls_blocking = 0u;
    g_spy_change_cb = NULL;
    g_spy_register_return = CONFIG_SERVICE_OK;
    g_spy_register_calls = 0u;

    g_spy_time_ts =
        (time_provider_ts_t){.epoch = 1793200000u, .sync_state = TIME_SYNC_SYNCHRONISED};
    g_spy_time_return = TIME_PROVIDER_ERR_OK;
    g_spy_retry_calls = 0u;
    g_spy_life_state = LIFECYCLE_STATE_OPERATIONAL;
    g_spy_control_calls = 0u;
    g_spy_control_len = 0u;

    g_spy_update_cmd_calls = 0u;
    g_spy_update_cmd_return = UPDATE_SERVICE_ERR_OK;
    g_spy_update_self_seen = NULL;
    g_spy_update_chunk_calls = 0u;
    g_spy_update_chunk_return = UPDATE_SERVICE_ERR_OK;

    g_spy_mqtt_subscribe_calls = 0u;
    g_spy_mqtt_subscribe_return = MQTT_CLIENT_ERR_OK;
    (void) memset(g_spy_mqtt_subscribe_topic, 0, sizeof(g_spy_mqtt_subscribe_topic));
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_OK;
    g_spy_mqtt_publish_calls = 0u;
    g_spy_mqtt_publish_tick_step = 0u;
    (void) memset(g_spy_mqtt_publish_last_topic, 0, sizeof(g_spy_mqtt_publish_last_topic));
    (void) memset(g_spy_mqtt_publish_last_payload, 0, sizeof(g_spy_mqtt_publish_last_payload));
    (void) memset(&g_spy_mqtt_stats, 0, sizeof(g_spy_mqtt_stats));
    g_spy_mqtt_get_stats_calls = 0u;
    g_spy_mqtt_process_calls = 0u;
    g_spy_mqtt_is_connected_return = true; /* most tests assume an already-connected broker */
    g_spy_mqtt_connect_return = MQTT_CLIENT_ERR_OK;
    g_spy_mqtt_connect_calls = 0u;
    g_spy_mqtt_connect_last_endpoint = NULL;
    g_spy_mqtt_connect_last_client_id = NULL;

    (void) memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.mqtt = (mqtt_client_handle_t) 0x1000;
    g_cfg.saf = (saf_handle_t) 0x1004;
    g_cfg.health = (health_monitor_handle_t) 0x1005;
    g_cfg.cfg_read = &g_spy_provider;
    g_cfg.cfg_write = &g_spy_manager;
    g_cfg.time = &g_spy_time;
    g_cfg.lifecycle = &g_spy_lifecycle;
    g_cfg.control_handler = spy_control_handler;
    g_cfg.fd_slave_addr = 1u;
    g_cfg.fw_version = 0x01020003u;
    g_cfg.update_ops = &g_spy_update_ops;
    g_cfg.update = (update_service_t *) 0x1009;
    g_handle = NULL;
}

void tearDown(void)
{
}

/* ======================================================================= */
/* CP-T01..T03 — create()                                                 */
/* ======================================================================= */

void test_CP_T01_create_happy_path(void)
{
    cloud_publisher_err_t rc = cloud_publisher_create(&g_cfg, &g_handle);

    TEST_ASSERT_EQUAL(CP_ERR_OK, rc);
    TEST_ASSERT_NOT_NULL(g_handle);
    TEST_ASSERT_EQUAL_UINT32(3u, g_mock_xQueueCreateStatic_call_count); /* alarm, command, OTA */
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xTaskCreateStatic_call_count);
    TEST_ASSERT_EQUAL_UINT32(3u, g_mock_xTimerCreateStatic_call_count);
    TEST_ASSERT_EQUAL_UINT32(3u, g_mock_xTimerStart_call_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_alarm_subscribe_calls);
}

void test_CP_T02_create_null_config(void)
{
    cloud_publisher_err_t rc = cloud_publisher_create(NULL, &g_handle);
    TEST_ASSERT_EQUAL(CP_ERR_NULL_PTR, rc);
}

void test_CP_T03_create_pool_exhaustion(void)
{
    cloud_publisher_handle_t h2 = NULL;
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    TEST_ASSERT_EQUAL(CP_ERR_NO_RESOURCE, cloud_publisher_create(&g_cfg, &h2));
}

/* ======================================================================= */
/* CP-T04..T05 — telemetry                                                */
/* ======================================================================= */

void test_CP_T04_telemetry_connected(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_OK;
    g_mock_xTaskNotifyWait_next_value = TC_TELEMETRY_TICK;

    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_sensor_get_snapshot_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_topic, "/telemetry"));
    TEST_ASSERT_EQUAL(MQTT_QOS_0, g_spy_mqtt_publish_last_qos);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_saf_enqueue_calls);
}

void test_CP_T05_telemetry_disconnected(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_mqtt_is_connected_return = false;
    g_mock_xTaskNotifyWait_next_value = TC_TELEMETRY_TICK;

    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_saf_enqueue_calls);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_saf_enqueue_last_topic, "/telemetry"));
}

/* ======================================================================= */
/* CP-T06 — health                                                        */
/* ======================================================================= */

void test_CP_T06_health_connected(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_OK;
    g_mock_xTaskNotifyWait_next_value = TC_HEALTH_TICK;

    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_topic, "/health"));
    TEST_ASSERT_EQUAL(MQTT_QOS_0, g_spy_mqtt_publish_last_qos);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_payload, "0011223344556677889900aa"));
}

/* ======================================================================= */
/* CP-T07..T08, T15 — alarms                                              */
/* ======================================================================= */

static void prv_load_alarm_event(cp_alarm_event_t *ev)
{
    (void) memset(ev, 0, sizeof(*ev));
    (void) strncpy(ev->sensor_name, "temperature", sizeof(ev->sensor_name) - 1u);
    ev->alarm_type = CP_ALARM_TYPE_HIGH;
    ev->measured_value_raw = 412;
    ev->threshold_value_raw = 400;
    ev->source = CP_ALARM_SOURCE_GATEWAY;
}

void test_CP_T07_alarm_connected(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_OK;

    cp_alarm_event_t ev;
    prv_load_alarm_event(&ev);
    g_mock_xQueueSend_last_item_size = sizeof(ev); /* enable xQueueSend capture */
    cloud_publisher_inject_alarm_for_test(g_handle, &ev);

    /* xQueueSend captured the event; deliver exactly that item once via
     * xQueueReceive, then report the queue empty. */
    (void) memcpy(g_mock_xQueueReceive_next_item, g_mock_xQueueSend_last_item, sizeof(ev));
    g_mock_xQueueReceive_next_item_size = sizeof(ev);
    g_mock_xQueueReceive_return = pdTRUE;
    g_mock_xQueueReceive_available = 1u;

    g_mock_xTaskNotifyWait_next_value = TC_ALARM_PENDING;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_topic, "/alarms"));
    TEST_ASSERT_EQUAL(MQTT_QOS_1, g_spy_mqtt_publish_last_qos);
    /* The outcome is a CloudPublisher-local seam now (CP-D19), not a health event. */
    TEST_ASSERT_EQUAL_UINT32(1u, cloud_publisher_alarm_published_for_test());
    TEST_ASSERT_EQUAL_UINT32(0u, cloud_publisher_alarm_buffered_for_test());
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_health_push_event_calls);
}

void test_CP_T08_alarm_disconnected(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_mqtt_is_connected_return = false;

    cp_alarm_event_t ev;
    prv_load_alarm_event(&ev);
    g_mock_xQueueSend_last_item_size = sizeof(ev); /* enable xQueueSend capture */
    cloud_publisher_inject_alarm_for_test(g_handle, &ev);

    (void) memcpy(g_mock_xQueueReceive_next_item, g_mock_xQueueSend_last_item, sizeof(ev));
    g_mock_xQueueReceive_next_item_size = sizeof(ev);
    g_mock_xQueueReceive_return = pdTRUE;
    g_mock_xQueueReceive_available = 1u;

    g_mock_xTaskNotifyWait_next_value = TC_ALARM_PENDING;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_saf_enqueue_calls);
    TEST_ASSERT_EQUAL(MQTT_QOS_1, g_spy_saf_enqueue_last_qos);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_saf_enqueue_last_topic, "/alarms"));
    TEST_ASSERT_EQUAL_UINT32(1u, cloud_publisher_alarm_buffered_for_test());
    TEST_ASSERT_EQUAL_UINT32(0u, cloud_publisher_alarm_published_for_test());
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_health_push_event_calls);
}

void test_CP_T15_alarm_queue_full(void)
{
    /* Overflow: xQueueSend reports the queue full (pdFALSE). The
     * subscriber callback must not crash and must still notify. */
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_mock_xQueueSend_return = pdFALSE;

    cp_alarm_event_t ev;
    prv_load_alarm_event(&ev);
    g_mock_xQueueSend_last_item_size = sizeof(ev); /* enable xQueueSend capture */
    cloud_publisher_inject_alarm_for_test(g_handle, &ev);

    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xQueueSend_call_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xTaskNotify_call_count);
    TEST_ASSERT_EQUAL_UINT32(TC_ALARM_PENDING, g_mock_xTaskNotify_last_value);
}

/* ======================================================================= */
/* CP-T09..T10 — StoreAndForward drain                                    */
/* ======================================================================= */

void test_CP_T09_saf_drain_on_reconnect(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));

    saf_entry_t entry;
    (void) memset(&entry, 0, sizeof(entry));
    (void) strncpy(entry.topic, "dt/iotmonitor/x/telemetry", SAF_TOPIC_MAX - 1u);
    entry.payload_len = 2u;
    g_spy_saf_dequeue_payload[0] = (uint8_t) 'a';
    g_spy_saf_dequeue_payload[1] = (uint8_t) 'b';
    entry.qos = MQTT_QOS_0;

    g_spy_saf_dequeue_entry = entry;
    g_spy_saf_dequeue_return = SAF_ERR_OK;
    g_spy_saf_dequeue_available = 1u; /* exactly one buffered entry, then empty */
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_OK;

    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1u, g_spy_saf_dequeue_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_saf_confirm_calls);
}

void test_CP_T10_saf_drain_stops_on_publish_failure(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));

    saf_entry_t entry;
    (void) memset(&entry, 0, sizeof(entry));
    (void) strncpy(entry.topic, "dt/iotmonitor/x/telemetry", SAF_TOPIC_MAX - 1u);
    entry.payload_len = 2u;
    entry.qos = MQTT_QOS_0;

    g_spy_saf_dequeue_entry = entry;
    g_spy_saf_dequeue_return = SAF_ERR_OK;
    g_spy_saf_dequeue_available = 1u;
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_PUBLISH_FAIL;

    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_saf_confirm_calls);
}

/* ======================================================================= */
/* CP-T11..T12 — inbound command routing                                 */
/* ======================================================================= */

void test_CP_T11_inbound_config_command(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_OK;

    const uint8_t payload[] = "{\"req\":5,\"telemetry_interval_ms\":30000}";
    /* command_queue is CloudPublisher's *second* xQueueCreateStatic() call
     * (alarm_queue is first) — the shared FreeRTOS mock distinguishes the
     * first two distinct queue handles a test touches, routing them to
     * separate tracking globals (see tests/support/freertos_mock.c). Must
     * use the "_2" globals here, not the plain ones. */
    g_mock_xQueueSend2_last_item_size = TC_COMMAND_ENTRY_SIZE; /* enable xQueueSend capture */
    cloud_publisher_inject_command_for_test(g_handle, "cmd/iotmonitor/x/config", payload,
                                            (uint32_t) (sizeof(payload) - 1u));

    (void) memcpy(g_mock_xQueueReceive2_next_item, g_mock_xQueueSend2_last_item,
                  g_mock_xQueueSend2_last_item_size);
    g_mock_xQueueReceive2_next_item_size = g_mock_xQueueSend2_last_item_size;
    g_mock_xQueueReceive2_return = pdTRUE;
    g_mock_xQueueReceive2_available = 1u;

    g_mock_xTaskNotifyWait_next_value = TC_COMMAND_PENDING;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_set_deferred_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_topic, "/config/result"));
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_payload, "\"result\":\"applied\""));
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_payload, "\"req\":5"));
}

void test_CP_T12_inbound_unknown_topic(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));

    const uint8_t payload[] = "x";
    /* command_queue is the second queue CloudPublisher creates — see the
     * matching comment in test_CP_T11 above. */
    g_mock_xQueueSend2_last_item_size = TC_COMMAND_ENTRY_SIZE; /* enable xQueueSend capture */
    cloud_publisher_inject_command_for_test(g_handle, "cmd/iotmonitor/x/bogus", payload,
                                            (uint32_t) sizeof(payload));

    (void) memcpy(g_mock_xQueueReceive2_next_item, g_mock_xQueueSend2_last_item,
                  g_mock_xQueueSend2_last_item_size);
    g_mock_xQueueReceive2_next_item_size = g_mock_xQueueSend2_last_item_size;
    g_mock_xQueueReceive2_return = pdTRUE;
    g_mock_xQueueReceive2_available = 1u;

    g_mock_xTaskNotifyWait_next_value = TC_COMMAND_PENDING;
    /* Must not crash; no handler should fire. */
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_set_deferred_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_update_cmd_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_update_chunk_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_control_calls);
}

/* ======================================================================= */
/* CP-T13 — stats polling                                                 */
/* ======================================================================= */

void test_CP_T13_stats_polling(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_saf_dequeue_return = SAF_ERR_EMPTY; /* nothing to drain */

    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_get_stats_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_health_update_mqtt_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_mqtt_connect_calls); /* already connected — no attempt */
}

/* ======================================================================= */
/* CP-T17..T18 (new, post-CP-D9) — connect/reconnect                      */
/* ======================================================================= */

void test_CP_T17_reconnect_attempted_when_disconnected(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_saf_dequeue_return = SAF_ERR_EMPTY;
    g_spy_mqtt_is_connected_return = false;
    g_spy_mqtt_connect_return = MQTT_CLIENT_ERR_OK;

    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_connect_calls);
}

void test_CP_T18_reconnect_backoff_does_not_hammer_every_tick(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_saf_dequeue_return = SAF_ERR_EMPTY;
    g_spy_mqtt_is_connected_return = false;
    g_spy_mqtt_connect_return = MQTT_CLIENT_ERR_CONNECT_FAIL;

    /* xTaskNotifyWait's mock clears consumed bits on exit (clear_on_exit),
     * so the tick must be re-armed before each simulated step — otherwise
     * steps 2/3 below would see no notification bits at all and this
     * assertion would pass regardless of whether backoff logic works. */
    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle); /* 1st attempt: fails, arms backoff */
    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle); /* still backing off: no retry yet */
    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_connect_calls);
}

void test_CP_T19_reconnect_in_progress_retries_every_tick_without_backoff(void)
{
    /* CP-D10: MQTT_CLIENT_ERR_IN_PROGRESS means connect_step() is mid
     * sequence (e.g. ticking the TLS handshake), not failed — unlike
     * CP-T18's terminal failure, this must NOT arm the backoff, so the
     * next stats tick retries immediately and lets the sequence advance. */
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_saf_dequeue_return = SAF_ERR_EMPTY;
    g_spy_mqtt_is_connected_return = false;
    g_spy_mqtt_connect_return = MQTT_CLIENT_ERR_IN_PROGRESS;

    /* xTaskNotifyWait's mock clears consumed bits on exit (clear_on_exit),
     * so the tick must be re-armed before each simulated step. */
    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(3u, g_spy_mqtt_connect_calls); /* every tick, no backoff */
}

void test_CP_T25_reconnect_backoff_is_exponential_and_resets(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_saf_dequeue_return = SAF_ERR_EMPTY;
    g_spy_mqtt_is_connected_return = false;
    g_spy_mqtt_connect_return = MQTT_CLIENT_ERR_CONNECT_FAIL;

    /* Each stats-tick step re-arms the tick (the notify mock clears consumed
     * bits on exit). A failed attempt arms a backoff of B seconds; since the
     * countdown ticks at 1 Hz that is B quiet ticks, then the next tick
     * retries. */
#define STEP_N(n)                                                                                  \
    do                                                                                             \
    {                                                                                              \
        for (uint32_t k = 0u; k < (n); k++)                                                        \
        {                                                                                          \
            g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;                                     \
            cloud_publisher_task_step_for_test(g_handle);                                          \
        }                                                                                          \
    } while (0)

    /* 1st attempt fails immediately (countdown started at 0), arming a 2 s backoff. */
    STEP_N(1);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_connect_calls);
    STEP_N(2); /* 2 quiet ticks of the 2 s backoff */
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_connect_calls);
    STEP_N(1); /* 2nd attempt; fails, backoff doubles to 4 s */
    TEST_ASSERT_EQUAL_UINT32(2u, g_spy_mqtt_connect_calls);

    STEP_N(4); /* 4 quiet ticks of the 4 s backoff — longer than before */
    TEST_ASSERT_EQUAL_UINT32(2u, g_spy_mqtt_connect_calls);
    STEP_N(1); /* 3rd attempt; confirms the curve doubled 2 -> 4 */
    TEST_ASSERT_EQUAL_UINT32(3u, g_spy_mqtt_connect_calls);

    /* A successful connect (is_connected true) resets the curve. */
    g_spy_mqtt_is_connected_return = true;
    STEP_N(1); /* connected: countdown -> 0, backoff -> MIN; no attempt */
    TEST_ASSERT_EQUAL_UINT32(3u, g_spy_mqtt_connect_calls);

    /* Drop again: retry is immediate (countdown was reset to 0)... */
    g_spy_mqtt_is_connected_return = false;
    g_spy_mqtt_connect_return = MQTT_CLIENT_ERR_CONNECT_FAIL;
    STEP_N(1);
    TEST_ASSERT_EQUAL_UINT32(4u, g_spy_mqtt_connect_calls);
    /* ...and the backoff is back to 2 s, not the grown value: 2 quiet, retry on 3rd. */
    STEP_N(2);
    TEST_ASSERT_EQUAL_UINT32(4u, g_spy_mqtt_connect_calls);
    STEP_N(1);
    TEST_ASSERT_EQUAL_UINT32(5u, g_spy_mqtt_connect_calls);

#undef STEP_N
}

/* ======================================================================= */
/* CP-T20..T21 (WIFITASK-O1 Phase 3) — wifi-recv-ready wake               */
/* ======================================================================= */

void test_CP_T20_wifi_recv_ready_triggers_stats_poll(void)
{
    /* MQTT_CLIENT_WIFI_RECV_READY_BIT (set by MqttClient via
     * wifitask_try_recv()) must re-drive prv_poll_stats() — and with it
     * prv_maybe_reconnect() — on its own, not just on CP_NOTIFY_STATS_TICK,
     * or a TLS handshake that just received data sits idle until the next
     * 1 Hz tick instead of being retried immediately. */
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_saf_dequeue_return = SAF_ERR_EMPTY;

    g_mock_xTaskNotifyWait_next_value = TC_WIFI_RECV_READY;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_get_stats_calls);
}

void test_CP_T21_wifi_recv_ready_bit_cleared_no_busy_loop(void)
{
    /* CP_NOTIFY_ALL_BITS (xTaskNotifyWait's clear-on-exit mask) must
     * include this bit. Any bit set via eSetBits wakes xTaskNotifyWait
     * regardless of the mask, but a bit left out of the mask is never
     * cleared from the notification word — every following wait call
     * would then return immediately with that bit still set, a busy
     * loop rather than a missed wake. Proven here by NOT re-arming
     * between the two steps: a second call must see nothing pending. */
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_saf_dequeue_return = SAF_ERR_EMPTY;

    g_mock_xTaskNotifyWait_next_value = TC_WIFI_RECV_READY;
    cloud_publisher_task_step_for_test(g_handle);
    g_spy_mqtt_get_stats_calls = 0u;
    cloud_publisher_task_step_for_test(g_handle); /* no re-arm: bit must be gone */

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_mqtt_get_stats_calls);
}

/* ======================================================================= */
/* CP-T20..T24 — real SensorService / AlarmService integration (SS-O8)    */
/* ======================================================================= */

static void prv_set_reading(sensor_id_t id, int32_t value)
{
    g_spy_snapshot.readings[id].value = value;
    g_spy_snapshot.readings[id].valid = true;
}

void test_CP_T20_telemetry_converts_fixed_point_to_wire_units(void)
{
    char buf[512];
    modbus_fd_reading_t fd;
    (void) memset(&fd, 0, sizeof(fd));
    prv_set_reading(SENSOR_ID_TEMPERATURE, 2345); /* 23.45 degC -> 235 deci-C (rounded) */
    prv_set_reading(SENSOR_ID_HUMIDITY, 5049);    /* 50.49 %RH  -> 50                   */
    prv_set_reading(SENSOR_ID_PRESSURE, 10132);   /* 1013.2 hPa -> 1013                 */
    prv_set_reading(SENSOR_ID_GYRO_X, -1500);     /* mdps, passed through               */
    g_spy_snapshot.readings[SENSOR_ID_TEMPERATURE].timestamp.sync_state = TIME_SYNC_SYNCHRONISED;

    TEST_ASSERT_GREATER_THAN_UINT32(
        0u, cp_serialise_telemetry(buf, sizeof(buf), &g_spy_snapshot, &fd, &g_spy_time_ts, "SER"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"temperature_deci_c\":235,"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"humidity_pct\":50,"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pressure_hpa\":1013,"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"gyro_x_mdps\":-1500,"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"sync_state\":\"synchronised\""));
}

void test_CP_T21_telemetry_rounds_negative_temperature_away_from_zero(void)
{
    char buf[512];
    modbus_fd_reading_t fd;
    (void) memset(&fd, 0, sizeof(fd));
    prv_set_reading(SENSOR_ID_TEMPERATURE, -1005); /* -10.05 degC -> -101 deci-C */
    g_spy_time_ts.sync_state = TIME_SYNC_UNSYNCHRONISED;

    (void) cp_serialise_telemetry(buf, sizeof(buf), &g_spy_snapshot, &fd, &g_spy_time_ts, "SER");

    TEST_ASSERT_NOT_NULL(strstr(buf, "\"temperature_deci_c\":-101,"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"sync_state\":\"unsynchronised\""));
}

void test_CP_T22_telemetry_reports_invalid_channel_as_null(void)
{
    char buf[512];
    modbus_fd_reading_t fd;
    (void) memset(&fd, 0, sizeof(fd));
    prv_set_reading(SENSOR_ID_ACCEL_X, 900);
    g_spy_snapshot.readings[SENSOR_ID_ACCEL_X].valid = false;

    (void) cp_serialise_telemetry(buf, sizeof(buf), &g_spy_snapshot, &fd, &g_spy_time_ts, "SER");

    TEST_ASSERT_NOT_NULL(strstr(buf, "\"accel_x_mg\":null,"));
    TEST_ASSERT_NULL(strstr(buf, "invalid_channels")); /* removed in schema 1.1 (CP-D22) */
}

void test_CP_T27_no_channel_is_null_when_all_valid(void)
{
    char buf[1024];
    modbus_fd_reading_t fd;
    (void) memset(&fd, 0, sizeof(fd));
    for (uint32_t i = 0u; i < (uint32_t) SENSOR_ID_COUNT; i++)
    {
        g_spy_snapshot.readings[i].valid = true;
    }

    TEST_ASSERT_GREATER_THAN_UINT32(
        0u, cp_serialise_telemetry(buf, sizeof(buf), &g_spy_snapshot, &fd, &g_spy_time_ts, "SER"));
    TEST_ASSERT_NULL(strstr(buf, "\"gyro_x_mdps\":null"));
    TEST_ASSERT_NULL(strstr(buf, "invalid_channels"));
}

void test_CP_T28_all_channels_invalid_still_fits_the_buffer(void)
{
    char buf[2048];
    modbus_fd_reading_t fd;
    (void) memset(&fd, 0, sizeof(fd));

    TEST_ASSERT_GREATER_THAN_UINT32(
        0u, cp_serialise_telemetry(buf, sizeof(buf), &g_spy_snapshot, &fd, &g_spy_time_ts, "SER"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mag_z_mgauss\":null,"));
}

void test_CP_T23_telemetry_skipped_when_snapshot_unavailable(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_snapshot_return = SENSOR_SERVICE_ERR_NOT_INIT;
    g_mock_xTaskNotifyWait_next_value = TC_TELEMETRY_TICK;

    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_sensor_get_snapshot_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_saf_enqueue_calls);
}

static cp_alarm_event_t prv_fire_alarm(sensor_id_t sensor, alarm_event_t event, int32_t value)
{
    cp_alarm_event_t out;
    sensor_reading_t reading;
    (void) memset(&reading, 0, sizeof(reading));
    reading.value = value;
    reading.valid = true;

    g_mock_xQueueSend_last_item_size = sizeof(out); /* enable xQueueSend capture */
    g_spy_alarm_cb(sensor, event, &reading);
    (void) memcpy(&out, g_mock_xQueueSend_last_item, sizeof(out));
    return out;
}

void test_CP_T24_alarm_callback_builds_payload_from_real_types(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    TEST_ASSERT_NOT_NULL(g_spy_alarm_cb);
    g_spy_alarm_limit = 3500;

    cp_alarm_event_t ev = prv_fire_alarm(SENSOR_ID_TEMPERATURE, ALARM_EVENT_RAISED_HIGH, 3612);

    TEST_ASSERT_EQUAL_STRING("temperature", ev.sensor_name);
    TEST_ASSERT_EQUAL(CP_ALARM_TYPE_HIGH, ev.alarm_type);
    TEST_ASSERT_EQUAL_INT32(3612, ev.measured_value_raw);
    TEST_ASSERT_EQUAL_INT32(3500, ev.threshold_value_raw);
    TEST_ASSERT_EQUAL(CP_ALARM_SOURCE_GATEWAY, ev.source);
    TEST_ASSERT_EQUAL(SENSOR_ID_TEMPERATURE, g_spy_alarm_limit_sensor);
    TEST_ASSERT_EQUAL(ALARM_EVENT_RAISED_HIGH, g_spy_alarm_limit_event);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xTaskNotify_call_count);
    TEST_ASSERT_EQUAL_UINT32(TC_ALARM_PENDING, g_mock_xTaskNotify_last_value);
}

void test_CP_T25_cleared_alarm_reports_the_limit_that_was_raised(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_alarm_limit = 9500;
    (void) prv_fire_alarm(SENSOR_ID_PRESSURE, ALARM_EVENT_RAISED_LOW, 9400);

    cp_alarm_event_t ev = prv_fire_alarm(SENSOR_ID_PRESSURE, ALARM_EVENT_CLEARED, 9600);

    TEST_ASSERT_EQUAL_STRING("pressure", ev.sensor_name);
    TEST_ASSERT_EQUAL(CP_ALARM_TYPE_CLEAR, ev.alarm_type);
    TEST_ASSERT_EQUAL(ALARM_EVENT_RAISED_LOW, g_spy_alarm_limit_event);
}

void test_CP_T27_fd_alarm_flags_become_field_device_alarms(void)
{
    g_cfg.fd_slave_addr = 1u;
    g_spy_fd_return = MODBUS_POLLER_ERR_OK;
    g_spy_fd_reading.humidity_centi_pct = 8123u;
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    TEST_ASSERT_NOT_NULL(g_spy_fd_alarm_cb);

    /* HUMIDITY_HIGH (bit 3) rises. */
    g_mock_xQueueSend_last_item_size = sizeof(cp_alarm_event_t);
    g_spy_fd_alarm_cb(1u, 0x0000u, 0x0008u);
    cp_alarm_event_t ev;
    (void) memcpy(&ev, g_mock_xQueueSend_last_item, sizeof(ev));

    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xQueueSend_call_count);
    TEST_ASSERT_EQUAL_STRING("humidity", ev.sensor_name);
    TEST_ASSERT_EQUAL(CP_ALARM_TYPE_HIGH, ev.alarm_type);
    TEST_ASSERT_EQUAL(CP_ALARM_SOURCE_FIELD_DEVICE, ev.source);
    TEST_ASSERT_EQUAL_INT32(8123, ev.measured_value_raw);

    /* The same bit falls, TEMP_LOW (bit 0) rises: two events, the clear first by bit order. */
    g_mock_xQueueSend_call_count = 0u;
    g_spy_fd_alarm_cb(1u, 0x0008u, 0x0001u);
    TEST_ASSERT_EQUAL_UINT32(2u, g_mock_xQueueSend_call_count);

    /* Another slave's flags are not ours. */
    g_mock_xQueueSend_call_count = 0u;
    g_spy_fd_alarm_cb(7u, 0x0000u, 0x0001u);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_xQueueSend_call_count);
}

void test_CP_T26_alarm_for_unthresholded_sensor_is_ignored(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    sensor_reading_t reading;
    (void) memset(&reading, 0, sizeof(reading));

    g_spy_alarm_cb(SENSOR_ID_ACCEL_X, ALARM_EVENT_RAISED_HIGH, &reading);

    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_xQueueSend_call_count);
}

/* ======================================================================= */
/* CP-T14 — JSON truncation                                               */
/* ======================================================================= */

void test_CP_T14_json_truncation(void)
{
    char tiny_buf[8];
    sensor_snapshot_t snap;
    modbus_fd_reading_t fd;
    (void) memset(&snap, 0, sizeof(snap));
    (void) memset(&fd, 0, sizeof(fd));

    uint32_t n = cp_serialise_telemetry(tiny_buf, (uint32_t) sizeof(tiny_buf), &snap, &fd,
                                        &g_spy_time_ts, "0011223344556677889900aa");

    TEST_ASSERT_EQUAL_UINT32(0u, n);
}

/* ======================================================================= */
/* CP-T16 — configurable intervals                                        */
/* ======================================================================= */

void test_CP_T16_a_changed_interval_retimes_the_timer(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));

    g_spy_params.telemetry_interval_ms = 30000u;
    g_spy_change_cb(CONFIG_PARAM_TELEMETRY_INTERVAL); /* ConfigService tells the task to look */
    g_mock_xTaskNotifyWait_next_value = TC_CONFIG_CHANGED;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xTimerChangePeriod_call_count);
    TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(30000u), g_mock_xTimerChangePeriod_last_period);
}

/* ======================================================================= */
/* CP-T30..T42 - LifecycleController operations (CP-D16)                   */
/* ======================================================================= */

#define TC_FLUSH_REQUEST (1u << 6)
#define TC_OTA_OUT (1u << 8)

static void prv_load_buffered_entry(void)
{
    saf_entry_t entry;

    (void) memset(&entry, 0, sizeof(entry));
    (void) strncpy(entry.topic, "dt/iotmonitor/x/telemetry", SAF_TOPIC_MAX - 1u);
    entry.payload_len = 2u;
    entry.qos = MQTT_QOS_0;
    g_spy_saf_dequeue_entry = entry;
    g_spy_saf_dequeue_return = SAF_ERR_OK;
    g_spy_saf_dequeue_available = 1u;
}

void test_CP_T30_is_ready_is_false_for_null_and_before_create(void)
{
    TEST_ASSERT_FALSE(cloud_publisher_is_ready(NULL));
}

void test_CP_T31_is_ready_is_true_after_create_even_when_the_broker_is_down(void)
{
    g_spy_mqtt_is_connected_return = false;
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    TEST_ASSERT_TRUE(cloud_publisher_is_ready(g_handle));
}

void test_CP_T32_flush_rejects_null(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_NULL_PTR, cloud_publisher_flush(NULL));
}

void test_CP_T33_flush_posts_the_flush_request_to_the_task(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_mock_xTaskNotify_call_count = 0u;

    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_flush(g_handle));
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xTaskNotify_call_count);
    TEST_ASSERT_EQUAL_UINT32(TC_FLUSH_REQUEST, g_mock_xTaskNotify_last_value);
    TEST_ASSERT_EQUAL(eSetBits, g_mock_xTaskNotify_last_action);
}

void test_CP_T34_flush_reports_a_notification_that_could_not_be_posted(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_mock_xTaskNotify_return = pdFALSE;
    TEST_ASSERT_EQUAL(CP_ERR_NO_RESOURCE, cloud_publisher_flush(g_handle));
}

void test_CP_T35_the_task_drains_the_offline_buffer_on_a_flush_request(void)
{
    g_mock_xQueueReceive_available = 0u; /* no pending alarms: the flush drains an empty queue */
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    prv_load_buffered_entry();
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_OK;

    g_mock_xTaskNotifyWait_next_value = TC_FLUSH_REQUEST;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_saf_confirm_calls);
}

void test_CP_T36_a_flush_request_with_the_broker_down_leaves_the_buffer_alone(void)
{
    g_mock_xQueueReceive_available = 0u; /* no pending alarms: the flush drains an empty queue */
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    prv_load_buffered_entry();
    g_spy_mqtt_is_connected_return = false;

    g_mock_xTaskNotifyWait_next_value = TC_FLUSH_REQUEST;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_saf_dequeue_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_mqtt_publish_calls);
}

void test_CP_T37_a_flush_request_publishes_pending_alarms_first(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_OK;

    cp_alarm_event_t ev;
    prv_load_alarm_event(&ev);
    g_mock_xQueueSend_last_item_size = sizeof(ev);
    cloud_publisher_inject_alarm_for_test(g_handle, &ev);
    (void) memcpy(g_mock_xQueueReceive_next_item, g_mock_xQueueSend_last_item, sizeof(ev));
    g_mock_xQueueReceive_next_item_size = sizeof(ev);
    g_mock_xQueueReceive_return = pdTRUE;
    g_mock_xQueueReceive_available = 1u;
    prv_load_buffered_entry();

    g_mock_xTaskNotifyWait_next_value = TC_FLUSH_REQUEST;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(2u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_saf_confirm_calls);
}

void test_CP_T42_the_notify_mask_clears_the_new_bits(void)
{
    g_mock_xQueueReceive_available = 0u; /* no pending alarms: the flush drains an empty queue */
    /* A bit missing from the clear-on-exit mask would wake the task forever (see the comment on
     * CP_NOTIFY_ALL_BITS). Both bits must be consumed by one step. */
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_mock_xTaskNotifyWait_next_value = TC_FLUSH_REQUEST | TC_OTA_OUT;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_xTaskNotifyWait_next_value);
}

/* ======================================================================= */
/* CP-T50..T69 - the OTA path (update-service-lld.md CPC-1..CPC-6)          */
/** The OTA payloads now start with the common block (schema 1.1); the rest is the old wire text. */
static void prv_assert_wire_tail(const char *old_wire, const char *actual)
{
    char tail[256];

    TEST_ASSERT_TRUE(strlen(old_wire) < (sizeof(tail) - 1u));
    (void) snprintf(tail, sizeof(tail), ",%s", &old_wire[1]); /* "{x}" -> ",x}" */
    TEST_ASSERT_EQUAL_STRING_LEN("{\"schema_version\":\"1.1\"", actual, 23u);
    TEST_ASSERT_TRUE(strlen(actual) > strlen(tail));
    TEST_ASSERT_EQUAL_STRING(tail, &actual[strlen(actual) - strlen(tail)]);
}

/* ======================================================================= */

#define TC_SERIAL_TOPIC "0011223344556677889900aa"

/** @brief Push a command through the command queue and run one task step. */
static void prv_deliver_command(const char *topic, const uint8_t *payload, uint32_t len)
{
    g_mock_xQueueSend2_last_item_size = TC_COMMAND_ENTRY_SIZE;
    cloud_publisher_inject_command_for_test(g_handle, topic, payload, len);

    (void) memcpy(g_mock_xQueueReceive2_next_item, g_mock_xQueueSend2_last_item,
                  g_mock_xQueueSend2_last_item_size);
    g_mock_xQueueReceive2_next_item_size = g_mock_xQueueSend2_last_item_size;
    g_mock_xQueueReceive2_return = pdTRUE;
    g_mock_xQueueReceive2_available = 1u;

    g_mock_xTaskNotifyWait_next_value = TC_COMMAND_PENDING;
    cloud_publisher_task_step_for_test(g_handle);
}

static void prv_deliver_ota_command(const char *json)
{
    prv_deliver_command("cmd/iotmonitor/" TC_SERIAL_TOPIC "/ota", (const uint8_t *) json,
                        (uint32_t) strlen(json));
}

void test_CP_T50_an_ota_command_is_parsed_and_handed_to_update_service(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));

    prv_deliver_ota_command("{\"job\":42,\"size_a\":491648,\"size_b\":491600}");

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_update_cmd_calls);
    TEST_ASSERT_EQUAL_UINT32(42u, g_spy_update_cmd.job);
    TEST_ASSERT_EQUAL_UINT32(491648u, g_spy_update_cmd.size_a);
    TEST_ASSERT_EQUAL_UINT32(491600u, g_spy_update_cmd.size_b);
    TEST_ASSERT_EQUAL_PTR(0x1009, g_spy_update_self_seen);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_mqtt_publish_calls); /* UpdateService reports, not us */
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_set_deferred_calls);
}

void test_CP_T50b_a_command_UpdateService_refuses_is_logged_and_not_reported_by_us(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_update_cmd_return = UPDATE_SERVICE_ERR_BUSY;

    prv_deliver_ota_command("{\"job\":1,\"size_a\":1000,\"size_b\":1000}");

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_update_cmd_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_mqtt_publish_calls);
}

void test_CP_T51_a_malformed_command_is_rejected_with_the_job_when_it_can_be_read(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));

    prv_deliver_ota_command("{\"job\":42,\"size_a\":491648}");

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_update_cmd_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL_STRING("dt/iotmonitor/" TC_SERIAL_TOPIC "/ota/result",
                             g_spy_mqtt_publish_last_topic);
    TEST_ASSERT_EQUAL(MQTT_QOS_1, g_spy_mqtt_publish_last_qos);
    prv_assert_wire_tail("{\"event\":\"ota\",\"job\":42,\"result\":\"rejected\","
                         "\"reason\":\"bad_command\"}",
                         g_spy_mqtt_publish_last_payload);
}

void test_CP_T52_a_command_with_no_readable_job_is_rejected_without_one(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));

    prv_deliver_ota_command("not json at all");

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_update_cmd_calls);
    prv_assert_wire_tail("{\"event\":\"ota\",\"result\":\"rejected\",\"reason\":\"bad_command\"}",
                         g_spy_mqtt_publish_last_payload);
}

void test_CP_T52b_a_rejection_is_buffered_while_the_broker_is_down(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_mqtt_is_connected_return = false;

    prv_deliver_ota_command("{\"job\":1}");

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_saf_enqueue_calls);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_saf_enqueue_last_topic, "/ota/result"));
}

void test_CP_T53_a_chunk_goes_straight_to_update_service_not_through_the_queue(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));

    uint8_t msg[8U + 5U] = {0x2A, 0x00, 0x00, 0x00, /* job 42 */
                            0x00, 0x10, 0x00, 0x00, /* offset 4096 */
                            0xAB, 1,    2,    3,    4};
    const uint32_t queued_before = g_mock_xQueueSend2_call_count;

    cloud_publisher_inject_command_for_test(g_handle, "cmd/iotmonitor/" TC_SERIAL_TOPIC "/ota/data",
                                            msg, (uint32_t) sizeof(msg));

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_update_chunk_calls);
    TEST_ASSERT_EQUAL_UINT32(42u, g_spy_chunk_job);
    TEST_ASSERT_EQUAL_UINT32(4096u, g_spy_chunk_offset);
    TEST_ASSERT_EQUAL_UINT32(5u, g_spy_chunk_len);
    TEST_ASSERT_EQUAL_UINT8(0xAB, g_spy_chunk_first_byte);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_update_cmd_calls); /* "/ota" must not capture "/ota/data" */
    TEST_ASSERT_EQUAL_UINT32(queued_before, g_mock_xQueueSend2_call_count);
}

void test_CP_T53b_a_chunk_of_a_full_kilobyte_is_not_truncated_to_the_command_size(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));

    static uint8_t msg[8U + 1024U];

    (void) memset(msg, 0x5A, sizeof(msg));
    cloud_publisher_inject_command_for_test(g_handle, "cmd/iotmonitor/" TC_SERIAL_TOPIC "/ota/data",
                                            msg, (uint32_t) sizeof(msg));

    TEST_ASSERT_EQUAL_UINT32(1024u, g_spy_chunk_len);
}

void test_CP_T54_a_chunk_without_data_or_for_a_gateway_without_ota_is_dropped(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));

    const uint8_t header_only[8] = {0};

    cloud_publisher_inject_command_for_test(g_handle, "cmd/iotmonitor/x/ota/data", header_only, 8u);
    cloud_publisher_inject_command_for_test(g_handle, "cmd/iotmonitor/x/ota/data", header_only, 3u);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_update_chunk_calls);

    cloud_publisher_reset_for_test();
    g_cfg.update_ops = NULL;
    g_cfg.update = NULL;
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    uint8_t msg[10] = {1, 0, 0, 0, 0, 0, 0, 0, 9, 9};

    cloud_publisher_inject_command_for_test(g_handle, "cmd/iotmonitor/x/ota/data", msg, 10u);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_update_chunk_calls);
}

void test_CP_T55_without_update_service_an_ota_command_is_ignored_and_nothing_is_subscribed(void)
{
    g_cfg.update_ops = NULL;
    g_cfg.update = NULL;
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));

    prv_deliver_ota_command("{\"job\":1,\"size_a\":1000,\"size_b\":1000}");
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_update_cmd_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_mqtt_publish_calls);

    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    /* Only the two non-OTA command topics: /config and /control (CP-D26). */
    TEST_ASSERT_EQUAL_UINT32(2u, g_spy_mqtt_subscribe_calls);
    TEST_ASSERT_EQUAL_STRING("cmd/iotmonitor/" TC_SERIAL_TOPIC "/config",
                             g_spy_mqtt_subscribe_topic[0]);
    TEST_ASSERT_EQUAL_STRING("cmd/iotmonitor/" TC_SERIAL_TOPIC "/control",
                             g_spy_mqtt_subscribe_topic[1]);
}

void test_CP_T56_the_command_topics_are_subscribed_once_the_broker_is_connected(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));

    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(4u, g_spy_mqtt_subscribe_calls);
    TEST_ASSERT_EQUAL_STRING("cmd/iotmonitor/" TC_SERIAL_TOPIC "/config",
                             g_spy_mqtt_subscribe_topic[0]);
    TEST_ASSERT_EQUAL(MQTT_QOS_1, g_spy_mqtt_subscribe_qos[0]);
    TEST_ASSERT_EQUAL_STRING("cmd/iotmonitor/" TC_SERIAL_TOPIC "/control",
                             g_spy_mqtt_subscribe_topic[1]);
    TEST_ASSERT_EQUAL(MQTT_QOS_1, g_spy_mqtt_subscribe_qos[1]);
    TEST_ASSERT_EQUAL_STRING("cmd/iotmonitor/" TC_SERIAL_TOPIC "/ota",
                             g_spy_mqtt_subscribe_topic[2]);
    TEST_ASSERT_EQUAL(MQTT_QOS_1, g_spy_mqtt_subscribe_qos[2]);
    TEST_ASSERT_EQUAL_STRING("cmd/iotmonitor/" TC_SERIAL_TOPIC "/ota/data",
                             g_spy_mqtt_subscribe_topic[3]);
    TEST_ASSERT_EQUAL(MQTT_QOS_0, g_spy_mqtt_subscribe_qos[3]);

    /* Not again on the next tick. */
    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_EQUAL_UINT32(4u, g_spy_mqtt_subscribe_calls);
}

void test_CP_T57_a_subscribe_that_fails_is_retried_on_the_next_tick(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_mqtt_subscribe_return = MQTT_CLIENT_ERR_SUBSCRIBE_FAIL;

    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_EQUAL_UINT32(4u, g_spy_mqtt_subscribe_calls);

    g_spy_mqtt_subscribe_return = MQTT_CLIENT_ERR_OK;
    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_EQUAL_UINT32(8u, g_spy_mqtt_subscribe_calls);

    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_EQUAL_UINT32(8u, g_spy_mqtt_subscribe_calls); /* done */
}

void test_CP_T58_every_new_connection_subscribes_again(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));

    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_EQUAL_UINT32(4u, g_spy_mqtt_subscribe_calls);

    g_spy_mqtt_is_connected_return = false; /* the link drops */
    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);

    g_spy_mqtt_is_connected_return = true; /* and comes back */
    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_EQUAL_UINT32(8u, g_spy_mqtt_subscribe_calls);
}

void test_CP_T59_the_message_callback_reaches_the_instance_and_drops_before_create(void)
{
    const uint8_t msg[10] = {1, 0, 0, 0, 0, 0, 0, 0, 7, 7};

    cloud_publisher_on_message("cmd/iotmonitor/x/ota/data", 25u, msg, 10u); /* no instance yet */
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_update_chunk_calls);

    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    cloud_publisher_on_message("cmd/iotmonitor/x/ota/data", 25u, msg, 10u);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_update_chunk_calls);

    cloud_publisher_on_message(NULL, 0u, msg, 10u);
    cloud_publisher_on_message("cmd/iotmonitor/x/ota/data", 25u, NULL, 0u);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_update_chunk_calls);
}

/* ---- IOtaTransport ---------------------------------------------------- */

void test_CP_T60_get_ota_transport_validates_its_arguments(void)
{
    iota_transport_t t;

    TEST_ASSERT_EQUAL(CP_ERR_NULL_PTR, cloud_publisher_get_ota_transport(NULL, &t));
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    TEST_ASSERT_EQUAL(CP_ERR_NULL_PTR, cloud_publisher_get_ota_transport(g_handle, NULL));
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_get_ota_transport(g_handle, &t));
    TEST_ASSERT_EQUAL_PTR(g_handle, t.ctx);
    TEST_ASSERT_NOT_NULL(t.is_connected);
    TEST_ASSERT_NOT_NULL(t.request_chunk);
    TEST_ASSERT_NOT_NULL(t.report);
}

/** @brief Queue a chunk request through the transport and load the entry for the task. */
static void prv_load_ota_entry(const iota_transport_t *t, bool is_report)
{
    const us_chunk_request_t req = {
        .job = 42u, .slot = FIRMWARE_SLOT_B, .offset = 4096u, .len = 1024u};
    const us_ota_report_t rep = {.job_known = true,
                                 .job = 42u,
                                 .result = US_RESULT_FAILED,
                                 .reason = US_REASON_HASH_MISMATCH,
                                 .version = 0x01020000UL,
                                 .rollback_count = 0u};

    g_mock_xQueueSend_last_item_size = cloud_publisher_ota_entry_size_for_test();
    g_mock_xTaskNotify_call_count = 0u;

    if (is_report)
    {
        TEST_ASSERT_TRUE(t->report(t->ctx, &rep));
    }
    else
    {
        TEST_ASSERT_TRUE(t->request_chunk(t->ctx, &req));
    }

    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xTaskNotify_call_count);
    TEST_ASSERT_EQUAL_UINT32(TC_OTA_OUT, g_mock_xTaskNotify_last_value);

    (void) memcpy(g_mock_xQueueReceive_next_item, g_mock_xQueueSend_last_item,
                  g_mock_xQueueSend_last_item_size);
    g_mock_xQueueReceive_next_item_size = g_mock_xQueueSend_last_item_size;
    g_mock_xQueueReceive_return = pdTRUE;
    g_mock_xQueueReceive_available = 1u;
}

void test_CP_T61_a_chunk_request_is_published_at_qos0_when_connected(void)
{
    iota_transport_t t;

    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_get_ota_transport(g_handle, &t));
    prv_load_ota_entry(&t, false);

    g_mock_xTaskNotifyWait_next_value = TC_OTA_OUT;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL_STRING("dt/iotmonitor/" TC_SERIAL_TOPIC "/ota/request",
                             g_spy_mqtt_publish_last_topic);
    TEST_ASSERT_EQUAL(MQTT_QOS_0, g_spy_mqtt_publish_last_qos);
    prv_assert_wire_tail("{\"job\":42,\"slot\":\"B\",\"offset\":4096,\"len\":1024}",
                         g_spy_mqtt_publish_last_payload);
}

void test_CP_T62_a_chunk_request_is_dropped_not_buffered_when_the_link_is_down(void)
{
    iota_transport_t t;

    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_get_ota_transport(g_handle, &t));
    prv_load_ota_entry(&t, false);
    g_spy_mqtt_is_connected_return = false;

    g_mock_xTaskNotifyWait_next_value = TC_OTA_OUT;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_saf_enqueue_calls);
}

void test_CP_T63_a_report_is_published_at_qos1_with_the_wire_payload(void)
{
    iota_transport_t t;

    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_get_ota_transport(g_handle, &t));
    prv_load_ota_entry(&t, true);

    g_mock_xTaskNotifyWait_next_value = TC_OTA_OUT;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL_STRING("dt/iotmonitor/" TC_SERIAL_TOPIC "/ota/result",
                             g_spy_mqtt_publish_last_topic);
    TEST_ASSERT_EQUAL(MQTT_QOS_1, g_spy_mqtt_publish_last_qos);
    prv_assert_wire_tail("{\"event\":\"ota\",\"job\":42,\"result\":\"failed\","
                         "\"reason\":\"hash_mismatch\",\"version\":\"1.2.0\",\"rollbacks\":0}",
                         g_spy_mqtt_publish_last_payload);
}

void test_CP_T64_a_report_is_buffered_through_store_and_forward_while_offline(void)
{
    iota_transport_t t;

    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_get_ota_transport(g_handle, &t));
    prv_load_ota_entry(&t, true);
    g_spy_mqtt_is_connected_return = false;

    g_mock_xTaskNotifyWait_next_value = TC_OTA_OUT;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_saf_enqueue_calls);
    TEST_ASSERT_EQUAL_STRING("dt/iotmonitor/" TC_SERIAL_TOPIC "/ota/result",
                             g_spy_saf_enqueue_last_topic);
    TEST_ASSERT_EQUAL(MQTT_QOS_1, g_spy_saf_enqueue_last_qos);
}

void test_CP_T65_a_full_outbound_queue_is_reported_and_posts_no_notification(void)
{
    iota_transport_t t;
    const us_chunk_request_t req = {.job = 1u, .slot = FIRMWARE_SLOT_A, .offset = 0u, .len = 1u};
    const us_ota_report_t rep = {0};

    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_get_ota_transport(g_handle, &t));
    g_mock_xQueueSend_return = pdFALSE;
    g_mock_xTaskNotify_call_count = 0u;

    TEST_ASSERT_FALSE(t.request_chunk(t.ctx, &req));
    TEST_ASSERT_FALSE(t.report(t.ctx, &rep));
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_xTaskNotify_call_count);
}

void test_CP_T66_is_connected_mirrors_the_broker_as_the_task_last_saw_it(void)
{
    iota_transport_t t;

    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_get_ota_transport(g_handle, &t));
    TEST_ASSERT_FALSE(t.is_connected(t.ctx)); /* nothing seen yet */

    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_TRUE(t.is_connected(t.ctx));

    g_spy_mqtt_is_connected_return = false;
    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_FALSE(t.is_connected(t.ctx));
}

void test_CP_T67_the_ota_notification_bit_is_consumed_by_one_step(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_mock_xQueueReceive_available = 0u;
    g_mock_xTaskNotifyWait_next_value = TC_OTA_OUT;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_xTaskNotifyWait_next_value);
}

/* ---- JSON ------------------------------------------------------------- */

void test_CP_T70_ota_report_variants_serialise_to_the_wire_contract(void)
{
    char buf[200];
    us_ota_report_t r = {.job_known = true,
                         .job = 42u,
                         .result = US_RESULT_ACCEPTED,
                         .reason = US_REASON_NONE,
                         .version = 0x01000000UL,
                         .rollback_count = 0u};

    TEST_ASSERT_TRUE(cp_serialise_ota_report(buf, sizeof(buf), &r, &g_spy_time_ts, "SER") > 0u);
    prv_assert_wire_tail("{\"event\":\"ota\",\"job\":42,\"result\":\"accepted\","
                         "\"version\":\"1.0.0\",\"rollbacks\":0}",
                         buf);

    r = (us_ota_report_t){.job_known = false,
                          .result = US_RESULT_SUCCESS,
                          .version = 0x02000000UL,
                          .rollback_count = 0u};
    TEST_ASSERT_TRUE(cp_serialise_ota_report(buf, sizeof(buf), &r, &g_spy_time_ts, "SER") > 0u);
    prv_assert_wire_tail("{\"event\":\"ota\",\"result\":\"success\","
                         "\"version\":\"2.0.0\",\"rollbacks\":0}",
                         buf);

    r = (us_ota_report_t){.job_known = false,
                          .result = US_RESULT_ROLLED_BACK,
                          .reason = US_REASON_ROLLBACK,
                          .version = 0x01000003UL,
                          .rollback_count = 2u};
    TEST_ASSERT_TRUE(cp_serialise_ota_report(buf, sizeof(buf), &r, &g_spy_time_ts, "SER") > 0u);
    prv_assert_wire_tail("{\"event\":\"ota\",\"result\":\"rolled_back\",\"reason\":\"rollback\","
                         "\"version\":\"1.0.3\",\"rollbacks\":2}",
                         buf);
}

void test_CP_T71_a_buffer_that_is_too_small_gives_zero_for_every_ota_serialiser(void)
{
    char buf[8];
    const us_ota_report_t r = {.job_known = true,
                               .job = 42u,
                               .result = US_RESULT_FAILED,
                               .reason = US_REASON_BAD_IMAGE,
                               .version = 0x01000000UL};
    const us_chunk_request_t q = {.job = 1u, .slot = FIRMWARE_SLOT_A, .offset = 0u, .len = 1u};

    TEST_ASSERT_EQUAL_UINT32(0u,
                             cp_serialise_ota_report(buf, sizeof(buf), &r, &g_spy_time_ts, "SER"));
    TEST_ASSERT_EQUAL_UINT32(0u,
                             cp_serialise_ota_request(buf, sizeof(buf), &q, &g_spy_time_ts, "SER"));

    /* Every length from zero up to the full text is refused or exact, never an overrun. */
    char exact[200];
    const uint32_t full = cp_serialise_ota_report(exact, sizeof(exact), &r, &g_spy_time_ts, "SER");

    TEST_ASSERT_TRUE(full > 0u);
    for (uint32_t size = 1u; size <= full; size++)
    {
        TEST_ASSERT_EQUAL_UINT32(0u,
                                 cp_serialise_ota_report(exact, size, &r, &g_spy_time_ts, "SER"));
    }
    TEST_ASSERT_EQUAL_UINT32(full,
                             cp_serialise_ota_report(exact, full + 1u, &r, &g_spy_time_ts, "SER"));
}

void test_CP_T72_the_ota_command_parser_accepts_the_contract_and_whitespace(void)
{
    us_ota_command_t c;
    uint32_t job = 0u;
    bool known = false;
    const char *ok = "{\"job\":42,\"size_a\":491648,\"size_b\":491600}";
    const char *spaced = "{ \"job\": 7 , \"size_a\": 1 , \"size_b\": 2 }";
    const char *reordered = "{\"size_b\":2,\"size_a\":1,\"job\":7}";

    TEST_ASSERT_TRUE(
        cp_parse_ota_command((const uint8_t *) ok, (uint32_t) strlen(ok), &c, &job, &known));
    TEST_ASSERT_TRUE(known);
    TEST_ASSERT_EQUAL_UINT32(42u, c.job);
    TEST_ASSERT_EQUAL_UINT32(491648u, c.size_a);
    TEST_ASSERT_EQUAL_UINT32(491600u, c.size_b);

    TEST_ASSERT_TRUE(
        cp_parse_ota_command((const uint8_t *) spaced, (uint32_t) strlen(spaced), &c, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(7u, c.job);
    TEST_ASSERT_TRUE(cp_parse_ota_command((const uint8_t *) reordered, (uint32_t) strlen(reordered),
                                          &c, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(2u, c.size_b);
}

void test_CP_T73_the_ota_command_parser_refuses_everything_else(void)
{
    us_ota_command_t c;
    const char *bad[] = {
        "{\"job\":42,\"size_a\":1}",                                /* size_b missing     */
        "{\"size_a\":1,\"size_b\":2}",                              /* job missing        */
        "{\"job\":\"42\",\"size_a\":1,\"size_b\":2}",               /* job is a string    */
        "{\"job\":-1,\"size_a\":1,\"size_b\":2}",                   /* negative           */
        "{\"job\":4294967296,\"size_a\":1,\"size_b\":2}",           /* 2^32               */
        "{\"job\":99999999999999999999,\"size_a\":1,\"size_b\":2}", /* far over           */
        "{\"job\":42x,\"size_a\":1,\"size_b\":2}",                  /* trailing garbage   */
        "{\"job\":,\"size_a\":1,\"size_b\":2}",                     /* empty value        */
        "",
        "not json",
    };

    for (uint32_t i = 0u; i < (sizeof(bad) / sizeof(bad[0])); i++)
    {
        TEST_ASSERT_FALSE_MESSAGE(cp_parse_ota_command((const uint8_t *) bad[i],
                                                       (uint32_t) strlen(bad[i]), &c, NULL, NULL),
                                  bad[i]);
    }

    TEST_ASSERT_FALSE(cp_parse_ota_command(NULL, 5u, &c, NULL, NULL));

    /* The largest 32-bit value is fine; a payload over the limit is not. */
    const char *max = "{\"job\":4294967295,\"size_a\":1,\"size_b\":2}";

    TEST_ASSERT_TRUE(
        cp_parse_ota_command((const uint8_t *) max, (uint32_t) strlen(max), &c, NULL, NULL));
    TEST_ASSERT_EQUAL_UINT32(0xFFFFFFFFUL, c.job);

    uint8_t big[200];

    (void) memset(big, ' ', sizeof(big));
    TEST_ASSERT_FALSE(cp_parse_ota_command(big, (uint32_t) sizeof(big), &c, NULL, NULL));
}

void test_CP_T74_a_bad_command_still_reports_a_job_it_could_read(void)
{
    us_ota_command_t c;
    uint32_t job = 0u;
    bool known = true;
    const char *no_sizes = "{\"job\":42}";
    const char *garbage = "{}";

    TEST_ASSERT_FALSE(cp_parse_ota_command((const uint8_t *) no_sizes, (uint32_t) strlen(no_sizes),
                                           &c, &job, &known));
    TEST_ASSERT_TRUE(known);
    TEST_ASSERT_EQUAL_UINT32(42u, job);

    TEST_ASSERT_FALSE(cp_parse_ota_command((const uint8_t *) garbage, (uint32_t) strlen(garbage),
                                           &c, &job, &known));
    TEST_ASSERT_FALSE(known);
}

/* ======================================================================= */
/* WP1 PR-D — the reconciliation with the real modules (CP-D18..D31)        */
/*                                                                          */
/* The reconciliation document numbers these CP-T50..T76; the OTA tests      */
/* already use T50..T74, so they are CP-T80..T106 here (same order).        */
/* ======================================================================= */

static void prv_deliver_config(const char *json)
{
    prv_deliver_command("cmd/iotmonitor/" TC_SERIAL_TOPIC "/config", (const uint8_t *) json,
                        (uint32_t) strlen(json));
}

static void prv_create_and_connect(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
}

static void prv_stats_tick(void)
{
    g_mock_xTaskNotifyWait_next_value = TC_STATS_TICK;
    cloud_publisher_task_step_for_test(g_handle);
}

/* CP-T80 (spec T50): create rejects each mandatory NULL member and accepts the optional ones. */
void test_CP_T80_create_rejects_each_mandatory_null_member(void)
{
    static const size_t k_offsets[] = {
        offsetof(cloud_publisher_config_t, mqtt),
        offsetof(cloud_publisher_config_t, saf),
        offsetof(cloud_publisher_config_t, health),
        offsetof(cloud_publisher_config_t, cfg_read),
        offsetof(cloud_publisher_config_t, cfg_write),
        offsetof(cloud_publisher_config_t, lifecycle),
    };

    for (uint32_t i = 0u; i < (sizeof(k_offsets) / sizeof(k_offsets[0])); i++)
    {
        cloud_publisher_config_t c = g_cfg;
        const void **member = (const void **) ((uint8_t *) &c + k_offsets[i]);

        *member = NULL;
        TEST_ASSERT_EQUAL_MESSAGE(CP_ERR_NULL_PTR, cloud_publisher_create(&c, &g_handle), "member");
    }

    /* Optional members: no time service, no control handler, no Field Device, no OTA. */
    g_cfg.time = NULL;
    g_cfg.control_handler = NULL;
    g_cfg.fd_slave_addr = 0u;
    g_cfg.update_ops = NULL;
    g_cfg.update = NULL;
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
}

void test_CP_T80b_update_ops_and_update_come_together_or_not_at_all(void)
{
    g_cfg.update = NULL; /* ops without an instance */
    TEST_ASSERT_EQUAL(CP_ERR_NULL_PTR, cloud_publisher_create(&g_cfg, &g_handle));
}

/* CP-T81: the change callback is registered; a refusal is NO_RESOURCE and takes no pool slot. */
void test_CP_T81_create_registers_the_change_callback_and_reports_a_refusal(void)
{
    g_spy_register_return = CONFIG_SERVICE_ERR_NOT_INIT;
    TEST_ASSERT_EQUAL(CP_ERR_NO_RESOURCE, cloud_publisher_create(&g_cfg, &g_handle));
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_register_calls);

    g_spy_register_return = CONFIG_SERVICE_OK;
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle)); /* slot not used up */
    TEST_ASSERT_NOT_NULL(g_spy_change_cb);
}

/* CP-T82: intervals are read in ms; only the two interval parameters wake the task. */
void test_CP_T82_intervals_come_from_get_params_in_ms_and_other_parameters_are_ignored(void)
{
    g_spy_params.telemetry_interval_ms = 5000u;
    g_spy_params.health_interval_ms = 20000u;
    prv_create_and_connect();
    g_mock_xTaskNotify_call_count = 0u;
    g_spy_change_cb(CONFIG_PARAM_TEMP_ALARM_HIGH);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_xTaskNotify_call_count);

    g_spy_change_cb(CONFIG_PARAM_HEALTH_INTERVAL);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xTaskNotify_call_count);
    TEST_ASSERT_EQUAL_UINT32(TC_CONFIG_CHANGED, g_mock_xTaskNotify_last_value);
}

/* CP-T83: the stack watermarks are polled before the snapshot is read. */
void test_CP_T83_health_tick_polls_then_reads_the_snapshot(void)
{
    prv_create_and_connect();
    g_mock_xTaskNotifyWait_next_value = TC_HEALTH_TICK;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_NOT_EQUAL_UINT32(0u, g_spy_health_poll_seq);
    TEST_ASSERT_LESS_THAN_UINT32(g_spy_health_get_seq, g_spy_health_poll_seq);
}

/* CP-T84: the absolute MQTT counters go to HealthMonitor. */
void test_CP_T84_stats_tick_hands_the_absolute_stats_to_health_monitor(void)
{
    prv_create_and_connect();
    g_spy_mqtt_stats.publishes_sent = 17u;
    g_spy_mqtt_stats.reconnect_count = 3u;

    prv_stats_tick();

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_health_update_mqtt_calls);
    TEST_ASSERT_EQUAL_UINT32(17u, g_spy_health_update_mqtt_stats.publishes_sent);
    TEST_ASSERT_EQUAL_UINT32(3u, g_spy_health_update_mqtt_stats.reconnect_count);
}

/* CP-T85: a connect is reported once, with one NTP retry; nothing repeats while connected. */
void test_CP_T85_disconnected_to_connected_reports_once_and_triggers_one_ntp_retry(void)
{
    g_spy_mqtt_is_connected_return = false;
    prv_create_and_connect();
    prv_stats_tick(); /* still down: nothing to report */
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_health_push_event_calls);

    g_spy_mqtt_is_connected_return = true;
    prv_stats_tick();
    prv_stats_tick();
    prv_stats_tick();

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_health_push_event_calls);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_CLOUD_CONNECTED, g_spy_health_events[0]);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_retry_calls);
}

/* CP-T86: the disconnect callback reports once and clears the link mirror. */
void test_CP_T86_on_disconnect_reports_once(void)
{
    prv_create_and_connect();
    prv_stats_tick(); /* connected */
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_health_push_event_calls);

    cloud_publisher_on_disconnect();
    cloud_publisher_on_disconnect(); /* a second report of the same drop adds nothing */

    TEST_ASSERT_EQUAL_UINT32(2u, g_spy_health_push_event_calls);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_CLOUD_DISCONNECTED, g_spy_health_events[1]);
}

void test_CP_T86b_on_disconnect_before_create_does_nothing(void)
{
    cloud_publisher_on_disconnect();
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_health_push_event_calls);
}

/* CP-T87: no TimeService is not an error. */
void test_CP_T87_without_a_time_service_no_retry_is_requested(void)
{
    g_cfg.time = NULL;
    prv_create_and_connect();
    prv_stats_tick();

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_retry_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_health_push_event_calls);
}

/* CP-T88: /control is skipped without a handler (the other three topics remain). */
void test_CP_T88_control_is_subscribed_only_with_a_handler(void)
{
    g_cfg.control_handler = NULL;
    prv_create_and_connect();
    prv_stats_tick();

    TEST_ASSERT_EQUAL_UINT32(3u, g_spy_mqtt_subscribe_calls);
    for (uint32_t i = 0u; i < 3u; i++)
    {
        TEST_ASSERT_NULL(strstr(g_spy_mqtt_subscribe_topic[i], "/control"));
    }
}

/* CP-T89: SAF accepted -> BUFFERED; a refusal is a lost message, logged, no health event. */
void test_CP_T89_a_saf_refusal_is_a_lost_message_not_a_health_event(void)
{
    prv_create_and_connect();
    g_spy_mqtt_is_connected_return = false;
    g_spy_saf_enqueue_return = SAF_ERR_FLASH;
    g_mock_xTaskNotifyWait_next_value = TC_TELEMETRY_TICK;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_saf_enqueue_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_health_push_event_calls);
    TEST_ASSERT_LESS_THAN_UINT16(2049u, g_spy_saf_enqueue_last_len);
}

/* CP-T90: the pointer entry is published, then confirmed; EMPTY ends; a failed confirm stops. */
void test_CP_T90_drain_publishes_the_entry_then_confirms_and_stops_on_a_failed_confirm(void)
{
    prv_create_and_connect();
    g_spy_saf_dequeue_entry.payload_len = 3u;
    (void) strncpy(g_spy_saf_dequeue_entry.topic, "dt/iotmonitor/x/health", SAF_TOPIC_MAX);
    g_spy_saf_dequeue_entry.qos = 1u;
    g_spy_saf_dequeue_return = SAF_ERR_OK;
    g_spy_saf_dequeue_available = 3u;
    g_spy_saf_confirm_return = SAF_ERR_FLASH;

    prv_stats_tick();

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL(MQTT_QOS_1, g_spy_mqtt_publish_last_qos);
    TEST_ASSERT_EQUAL_UINT32(3u, g_spy_mqtt_publish_last_len);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_saf_confirm_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_saf_dequeue_calls); /* the drain stopped for this tick */
}

/* CP-T91: the Field Device is converted from register units to the wire units. */
void test_CP_T91_telemetry_converts_the_field_device_to_wire_units(void)
{
    prv_create_and_connect();
    g_spy_fd_reading = (modbus_fd_reading_t){.temperature_centi_c = 2155,
                                             .humidity_centi_pct = 4049,
                                             .pressure_deci_hpa = 10135,
                                             .temperature_valid = true,
                                             .humidity_valid = true,
                                             .pressure_valid = true,
                                             .link_online = true,
                                             .has_data = true};
    g_mock_xTaskNotifyWait_next_value = TC_TELEMETRY_TICK;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT8(1u, g_spy_fd_slave_seen);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_payload,
                                "\"field_device\":{\"online\":true,\"temperature_deci_c\":216,"
                                "\"humidity_pct\":40,\"pressure_hpa\":1014}"));
}

/* CP-T92: offline, no data, or no Field Device configured. */
void test_CP_T92_an_offline_field_device_is_online_false_with_nulls_and_none_is_null(void)
{
    prv_create_and_connect();
    g_spy_fd_reading.has_data = true; /* data, but the link is down */
    g_spy_fd_reading.link_online = false;
    g_spy_fd_reading.temperature_valid = true;
    g_mock_xTaskNotifyWait_next_value = TC_TELEMETRY_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_payload,
                                "\"field_device\":{\"online\":false,\"temperature_deci_c\":null,"
                                "\"humidity_pct\":null,\"pressure_hpa\":null}"));

    cloud_publisher_reset_for_test();
    g_cfg.fd_slave_addr = 0u;
    g_spy_fd_calls = 0u;
    prv_create_and_connect();
    g_mock_xTaskNotifyWait_next_value = TC_TELEMETRY_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_payload, "\"field_device\":null"));
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_fd_calls);
}

/* CP-T93: an invalid onboard channel is null and there is no invalid_channels member. */
void test_CP_T93_an_invalid_onboard_channel_is_null_in_the_published_payload(void)
{
    prv_create_and_connect();
    g_spy_snapshot.readings[SENSOR_ID_TEMPERATURE].valid = false;
    g_mock_xTaskNotifyWait_next_value = TC_TELEMETRY_TICK;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_payload, "\"temperature_deci_c\":null"));
    TEST_ASSERT_NULL(strstr(g_spy_mqtt_publish_last_payload, "invalid_channels"));
}

/* CP-T94: the common block, synchronised, unsynchronised, and a TimeProvider error. */
void test_CP_T94_the_common_block_follows_time_provider(void)
{
    prv_create_and_connect();

    g_mock_xTaskNotifyWait_next_value = TC_TELEMETRY_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_payload,
                                "\"schema_version\":\"1.1\",\"device_id\":\"" TC_SERIAL_TOPIC
                                "\",\"timestamp\":1793200000,\"sync_state\":\"synchronised\""));

    g_spy_time_ts = (time_provider_ts_t){.epoch = 42u, .sync_state = TIME_SYNC_UNSYNCHRONISED};
    g_mock_xTaskNotifyWait_next_value = TC_TELEMETRY_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_payload,
                                "\"timestamp\":42,\"sync_state\":\"unsynchronised\""));

    g_spy_time_return = TIME_PROVIDER_ERR_RTC_FAIL;
    g_mock_xTaskNotifyWait_next_value = TC_TELEMETRY_TICK;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_NOT_NULL(
        strstr(g_spy_mqtt_publish_last_payload, "\"sync_state\":\"unsynchronised\""));
}

/* CP-T95: the health payload carries the snapshot, the firmware version and the stack headroom. */
void test_CP_T95_health_1_1_carries_every_field(void)
{
    prv_create_and_connect();
    g_spy_health_snapshot.uptime_s = 3600u;
    g_spy_health_snapshot.lifecycle_state = LIFECYCLE_STATE_OPERATIONAL;
    g_spy_health_snapshot.last_reset_cause = LIFECYCLE_RESET_SOFT;
    g_spy_health_snapshot.mqtt_reconnect_count = 2u;
    g_spy_health_snapshot.cloud_connected = true;
    g_spy_health_snapshot.wifi_rssi_dbm = -61;
    g_spy_health_snapshot.buffer_entry_count = 4u;
    g_spy_health_snapshot.stack_watermark_words[0] = 120u;
    g_spy_health_snapshot.stack_watermark_words[1] = 65u;
    g_spy_health_snapshot.stack_watermark_words[2] = 300u;

    g_mock_xTaskNotifyWait_next_value = TC_HEALTH_TICK;
    cloud_publisher_task_step_for_test(g_handle);

    const char *p = g_spy_mqtt_publish_last_payload;
    TEST_ASSERT_NOT_NULL(strstr(p, "\"fw_version\":\"1.2.3\""));
    TEST_ASSERT_NOT_NULL(strstr(p, "\"uptime_s\":3600"));
    TEST_ASSERT_NOT_NULL(strstr(p, "\"lifecycle_state\":\"operational\""));
    TEST_ASSERT_NOT_NULL(strstr(p, "\"last_reset_cause\":\"soft\""));
    TEST_ASSERT_NOT_NULL(strstr(p, "\"wifi_rssi_dbm\":-61"));
    TEST_ASSERT_NOT_NULL(strstr(p, "\"cloud_connected\":true"));
    TEST_ASSERT_NOT_NULL(strstr(p, "\"buffer_entry_count\":4"));
    TEST_ASSERT_NOT_NULL(
        strstr(p, "\"stack_free_words\":[120,65,300]")); /* not padded with zeros */
}

/* CP-T96: an alarm keeps the time of detection. */
void test_CP_T96_the_alarm_timestamp_is_the_reading_time_not_the_serialisation_time(void)
{
    prv_create_and_connect();
    g_spy_time_ts.epoch = 9999u; /* serialisation time: must not be used */
    cp_alarm_event_t ev;
    prv_load_alarm_event(&ev);
    ev.timestamp = (time_provider_ts_t){.epoch = 1234u, .sync_state = TIME_SYNC_SYNCHRONISED};
    char buf[512];

    TEST_ASSERT_GREATER_THAN_UINT32(0u, cp_serialise_alarm(buf, sizeof(buf), &ev, "SER"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"timestamp\":1234,\"sync_state\":\"synchronised\""));
}

/* CP-T97: a valid config is applied per key, in order, in RAM only. */
void test_CP_T97_config_is_applied_per_key_in_order_without_blocking_writes(void)
{
    prv_create_and_connect();
    prv_deliver_config("{\"req\":17,\"telemetry_interval_ms\":30000,\"temp_alarm_high\":4500}");

    TEST_ASSERT_EQUAL_UINT32(2u, g_spy_set_deferred_calls);
    TEST_ASSERT_EQUAL(CONFIG_PARAM_TELEMETRY_INTERVAL, g_spy_set_ids[0]);
    TEST_ASSERT_EQUAL_INT64(30000, g_spy_set_values[0]);
    TEST_ASSERT_EQUAL(CONFIG_PARAM_TEMP_ALARM_HIGH, g_spy_set_ids[1]);
    TEST_ASSERT_EQUAL_INT64(4500, g_spy_set_values[1]);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_set_calls_blocking); /* no set_param, no persist_pending */
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_payload, "\"event\":\"config\",\"req\":17,"
                                                                 "\"result\":\"applied\""));
    TEST_ASSERT_EQUAL(MQTT_QOS_1, g_spy_mqtt_publish_last_qos);
}

/* CP-T98: the second key is refused: the first is put back with its old value. */
void test_CP_T98_a_refused_key_reverts_the_earlier_ones(void)
{
    prv_create_and_connect();
    g_spy_params.telemetry_interval_ms = 60000u;
    g_spy_set_fail_index = 2u; /* the second set_param_deferred call fails */

    prv_deliver_config("{\"telemetry_interval_ms\":30000,\"temp_alarm_high\":4500}");

    TEST_ASSERT_EQUAL_UINT32(3u, g_spy_set_deferred_calls); /* two tries, one revert */
    TEST_ASSERT_EQUAL(CONFIG_PARAM_TELEMETRY_INTERVAL, g_spy_set_ids[2]);
    TEST_ASSERT_EQUAL_INT64(60000, g_spy_set_values[2]); /* the old value */
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_payload,
                                "\"result\":\"rejected\",\"reason\":\"invalid_value\","
                                "\"key\":\"temp_alarm_high\""));
}

/* CP-T99: Tier 1 and unknown keys, nothing applied. */
void test_CP_T99_local_only_and_unknown_keys_are_refused_with_nothing_applied(void)
{
    prv_create_and_connect();

    prv_deliver_config("{\"filter_alpha\":100}");
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_payload,
                                "\"reason\":\"not_remote\",\"key\":\"filter_alpha\""));

    prv_deliver_config("{\"telemetry_interval_ms\":30000,\"nonsense\":1}");
    TEST_ASSERT_NOT_NULL(
        strstr(g_spy_mqtt_publish_last_payload, "\"reason\":\"unknown_key\",\"key\":\"nonsense\""));

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_set_deferred_calls);
}

/* CP-T100: malformed inputs. */
void test_CP_T100_malformed_config_commands_are_rejected_with_nothing_applied(void)
{
    static const char *const k_bad[] = {
        "{\"telemetry_interval_ms\":\"30000\"}",                     /* string value       */
        "{\"telemetry_interval_ms\":30000.5}",                       /* float              */
        "{\"telemetry_interval_ms\":{\"a\":1}}",                     /* nested object      */
        "{\"telemetry_interval_ms\":1,\"telemetry_interval_ms\":2}", /* duplicate */
        "{\"telemetry_interval_ms\":30000} x",                       /* trailing data      */
        "{}",                                                        /* empty object       */
        "{\"req\":3}",                                               /* nothing to apply   */
        "not json",
    };

    prv_create_and_connect();
    for (uint32_t i = 0u; i < (sizeof(k_bad) / sizeof(k_bad[0])); i++)
    {
        prv_deliver_config(k_bad[i]);
        TEST_ASSERT_NOT_NULL_MESSAGE(
            strstr(g_spy_mqtt_publish_last_payload, "\"reason\":\"bad_json\""), k_bad[i]);
    }
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_set_deferred_calls);

    /* More than eight parameters: the rule is the parser's. Through the queue such a command
     * (about 280 bytes) is cut at the 128-byte command cap and rejected as malformed instead. */
    static const char k_nine[] = "{\"polling_interval_ms\":1000,\"modbus_poll_period_ms\":1000,"
                                 "\"telemetry_interval_ms\":1000,\"health_interval_ms\":10000,"
                                 "\"temp_alarm_high\":1,\"temp_alarm_low\":1,\"temp_hysteresis\":1,"
                                 "\"humidity_alarm_high\":1,\"humidity_alarm_low\":1}";
    cp_cfg_cmd_t cmd;

    TEST_ASSERT_EQUAL(CP_CFG_TOO_MANY_KEYS,
                      cp_parse_config_command((const uint8_t *) k_nine, sizeof(k_nine) - 1u, &cmd));
    prv_deliver_config(k_nine);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_payload, "\"reason\":\"bad_json\""));
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_set_deferred_calls);
}

/* CP-T101: a value outside the C type is refused before ConfigService is asked. */
void test_CP_T101_a_value_outside_the_c_type_is_refused_before_any_call(void)
{
    prv_create_and_connect();
    prv_deliver_config("{\"temp_alarm_high\":40000}"); /* int16_t tops out at 32767 */

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_set_deferred_calls);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_payload,
                                "\"reason\":\"invalid_value\",\"key\":\"temp_alarm_high\""));
}

/* CP-T102: busy outside Operational (a local edit is open). */
void test_CP_T102_config_is_refused_as_busy_unless_the_gateway_is_operational(void)
{
    prv_create_and_connect();
    g_spy_life_state = LIFECYCLE_STATE_EDITING_CONFIG;
    prv_deliver_config("{\"req\":9,\"telemetry_interval_ms\":30000}");

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_set_deferred_calls);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_payload,
                                "\"req\":9,\"result\":\"rejected\",\"reason\":\"busy\""));
}

/* CP-T103: the result goes to /config/result and never to /results. */
void test_CP_T103_the_config_result_topic(void)
{
    prv_create_and_connect();
    prv_deliver_config("{\"telemetry_interval_ms\":30000}");

    TEST_ASSERT_EQUAL_STRING("dt/iotmonitor/" TC_SERIAL_TOPIC "/config/result",
                             g_spy_mqtt_publish_last_topic);
}

/* CP-T104: /control reaches the handler; without one it is ignored. */
void test_CP_T104_control_reaches_the_handler_and_is_ignored_without_one(void)
{
    prv_create_and_connect();
    prv_deliver_command("cmd/iotmonitor/" TC_SERIAL_TOPIC "/control",
                        (const uint8_t *) "{\"command\":\"restart\"}", 21u);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_control_calls);
    TEST_ASSERT_EQUAL_UINT32(21u, g_spy_control_len);

    cloud_publisher_reset_for_test();
    g_cfg.control_handler = NULL;
    g_spy_control_calls = 0u;
    prv_create_and_connect();
    prv_deliver_command("cmd/iotmonitor/" TC_SERIAL_TOPIC "/control",
                        (const uint8_t *) "{\"command\":\"restart\"}", 21u);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_control_calls);
}

/* CP-T105: every serialiser output fits the buffer at the largest values. */
void test_CP_T105_the_largest_payloads_fit_the_buffer(void)
{
    char buf[TC_JSON_BUF_SIZE];
    sensor_snapshot_t snap;
    modbus_fd_reading_t fd = {.temperature_centi_c = -32768,
                              .humidity_centi_pct = 65535u,
                              .pressure_deci_hpa = 65535u,
                              .temperature_valid = true,
                              .humidity_valid = true,
                              .pressure_valid = true,
                              .link_online = true,
                              .has_data = true};
    health_snapshot_t hs;

    (void) memset(&snap, 0, sizeof(snap));
    for (uint32_t i = 0u; i < (uint32_t) SENSOR_ID_COUNT; i++)
    {
        snap.readings[i].valid = true;
        snap.readings[i].value = -2147483647 - 1;
    }
    TEST_ASSERT_GREATER_THAN_UINT32(
        0u, cp_serialise_telemetry(buf, sizeof(buf), &snap, &fd, &g_spy_time_ts, TC_SERIAL_TOPIC));

    (void) memset(&hs, 0xFF, sizeof(hs));
    hs.lifecycle_state = LIFECYCLE_STATE_UPDATING_FW;
    hs.last_reset_cause = LIFECYCLE_RESET_WATCHDOG;
    hs.wifi_rssi_dbm = -2147483647 - 1;
    for (uint32_t i = 0u; i < HEALTH_MONITOR_MAX_TASKS; i++)
    {
        hs.stack_watermark_words[i] = 65535u;
    }
    const uint32_t n =
        cp_serialise_health(buf, sizeof(buf), &hs, 0xFFFFFFFFu, &g_spy_time_ts, TC_SERIAL_TOPIC);
    TEST_ASSERT_GREATER_THAN_UINT32(0u, n);
    TEST_ASSERT_LESS_THAN_UINT32(TC_JSON_BUF_SIZE, n);
}

/* CP-T106: the module links with the real APIs only; the placeholder names are gone. */
void test_CP_T106_the_placeholder_symbols_are_not_part_of_the_build(void)
{
    /* This test file defines the real entry points only. If cloud_publisher.c still called one of
     * the ten placeholder functions, the test executable would not link at all, so reaching this
     * line is the proof; the done-when check on the ELF repeats it with nm (WP1 section 12). */
    prv_create_and_connect();
    TEST_ASSERT_TRUE(cloud_publisher_is_ready(g_handle));
}

/* ======================================================================= */
/* CP-T107..T112 - paced store-and-forward replay (CP-D32, GM-O20)           */
/* ======================================================================= */

/* Let the mock clock run, then take a stats tick: the drain's pacing is in ticks, not in calls. */
static void prv_stats_tick_after_ms(uint32_t ms)
{
    g_mock_tick_count += pdMS_TO_TICKS(ms);
    prv_stats_tick();
}

static void prv_load_backlog(uint32_t entries, uint32_t payload_len)
{
    saf_entry_t entry;

    (void) memset(&entry, 0, sizeof(entry));
    (void) strncpy(entry.topic, "dt/iotmonitor/x/telemetry", SAF_TOPIC_MAX - 1u);
    entry.payload_len = payload_len;
    entry.qos = MQTT_QOS_0;
    g_spy_saf_dequeue_entry = entry;
    g_spy_saf_dequeue_return = SAF_ERR_OK;
    g_spy_saf_dequeue_available = entries;
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_OK;
}

void test_CP_T107_a_replay_pass_stops_at_the_entry_limit(void)
{
    prv_create_and_connect();
    prv_load_backlog(10u, 2u);

    prv_stats_tick();

    TEST_ASSERT_EQUAL_UINT32(4u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL_UINT32(4u, g_spy_saf_confirm_calls);
    /* The link is served after each entry, not only at the end of the pass. */
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(4u, g_spy_mqtt_process_calls);
}

void test_CP_T108_a_replay_pass_stops_at_the_byte_limit(void)
{
    prv_create_and_connect();
    prv_load_backlog(10u, 4000u);

    prv_stats_tick();

    /* One 4000 B entry is over the 2048 B limit and ends the pass. */
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);
}

void test_CP_T109_a_replay_pass_stops_at_the_time_limit(void)
{
    prv_create_and_connect();
    prv_load_backlog(10u, 2u);
    g_spy_mqtt_publish_tick_step = pdMS_TO_TICKS(300u);

    prv_stats_tick();

    TEST_ASSERT_EQUAL_UINT32(2u, g_spy_mqtt_publish_calls);
}

void test_CP_T110_a_failed_replay_publish_backs_off_before_the_next_attempt(void)
{
    prv_create_and_connect();
    prv_load_backlog(40u, 2u);
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_PUBLISH_FAIL;

    prv_stats_tick();
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);

    prv_stats_tick_after_ms(4900u); /* CP_DRAIN_BACKOFF_MIN_S is 5 s */
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);

    prv_stats_tick_after_ms(200u);
    TEST_ASSERT_EQUAL_UINT32(2u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_saf_confirm_calls);
}

void test_CP_T111_an_entry_that_fails_three_times_in_a_row_is_dropped(void)
{
    prv_create_and_connect();
    prv_load_backlog(60u, 2u);
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_PUBLISH_FAIL;

    for (uint32_t i = 0u; (i < 40u) && (g_spy_mqtt_publish_calls < 3u); i++)
    {
        prv_stats_tick_after_ms(1000u);
    }

    TEST_ASSERT_EQUAL_UINT32(3u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_saf_confirm_calls); /* the drop */
    bool dropped_reported = false;
    for (uint32_t i = 0u; (i < g_spy_health_push_event_calls) && (i < TC_EVENT_LOG); i++)
    {
        dropped_reported |= (g_spy_health_events[i] == HEALTH_EVENT_BUFFER_OVERFLOW);
    }
    TEST_ASSERT_TRUE(dropped_reported);
}

void test_CP_T112_replay_is_not_attempted_while_the_link_is_down(void)
{
    prv_create_and_connect();
    prv_load_backlog(10u, 2u);
    g_spy_mqtt_is_connected_return = false;
    g_spy_saf_dequeue_calls = 0u;

    prv_stats_tick();

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_saf_dequeue_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_mqtt_publish_calls);
}

void test_CP_T113_a_second_pass_waits_for_the_pass_interval_however_often_it_is_woken(void)
{
    prv_create_and_connect();
    prv_load_backlog(40u, 2u);

    prv_stats_tick();
    TEST_ASSERT_EQUAL_UINT32(4u, g_spy_mqtt_publish_calls);

    /* Receive-ready wakes run the same code path as the 1 Hz tick. */
    prv_stats_tick_after_ms(100u);
    prv_stats_tick_after_ms(100u);
    TEST_ASSERT_EQUAL_UINT32(4u, g_spy_mqtt_publish_calls);

    prv_stats_tick_after_ms(900u); /* CP_DRAIN_PASS_INTERVAL_MS has run out */
    TEST_ASSERT_EQUAL_UINT32(8u, g_spy_mqtt_publish_calls);
}

/* ======================================================================= */
/* CP-T114..T118 - the link is made ready before a connect attempt         */
/* (CP-D33, CP-D34, GM-O17/O18/O19)                                         */
/* ======================================================================= */

static uint32_t g_spy_prepare_calls;
static const char *g_spy_prepare_result;
static const char *prv_spy_prepare(void)
{
    g_spy_prepare_calls++;
    return g_spy_prepare_result;
}

static void prv_create_disconnected_with_prepare(void)
{
    g_cfg.connect_prepare = prv_spy_prepare;
    g_spy_prepare_calls = 0u;
    g_spy_prepare_result = "13.60.114.42";
    g_spy_mqtt_is_connected_return = false;
    g_spy_mqtt_connect_return = MQTT_CLIENT_ERR_CONNECT_FAIL;
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
}

void test_CP_T114_the_address_from_the_hook_is_the_one_connected_to(void)
{
    prv_create_disconnected_with_prepare();

    prv_stats_tick();

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_prepare_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_connect_calls);
    TEST_ASSERT_EQUAL_STRING("13.60.114.42", g_spy_mqtt_connect_last_endpoint);
}

void test_CP_T115_a_hook_that_is_not_ready_counts_as_a_failed_attempt(void)
{
    prv_create_disconnected_with_prepare();
    g_spy_prepare_result = NULL;

    prv_stats_tick(); /* hook says no: no connect, back off */
    prv_stats_tick(); /* still backing off: the hook is not asked again */

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_prepare_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_mqtt_connect_calls);
}

void test_CP_T116_the_hook_is_asked_again_once_the_backoff_has_run_out(void)
{
    prv_create_disconnected_with_prepare();
    g_spy_prepare_result = NULL;

    prv_stats_tick();
    prv_stats_tick();
    prv_stats_tick();
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_prepare_calls); /* backoff 2 s: two ticks waiting */
    prv_stats_tick();
    TEST_ASSERT_EQUAL_UINT32(2u, g_spy_prepare_calls);

    g_spy_prepare_result = "13.60.114.43"; /* the network is back */
    for (uint32_t i = 0u; (i < 10u) && (g_spy_mqtt_connect_calls == 0u); i++)
    {
        prv_stats_tick();
    }
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_connect_calls);
    TEST_ASSERT_EQUAL_STRING("13.60.114.43", g_spy_mqtt_connect_last_endpoint);
}

void test_CP_T117_the_hook_is_not_asked_between_the_steps_of_one_attempt(void)
{
    prv_create_disconnected_with_prepare();
    g_spy_mqtt_connect_return = MQTT_CLIENT_ERR_IN_PROGRESS;

    prv_stats_tick();
    prv_stats_tick();
    prv_stats_tick();

    TEST_ASSERT_EQUAL_UINT32(3u, g_spy_mqtt_connect_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_prepare_calls);
}

void test_CP_T118_the_cpu_serial_is_the_client_id_unless_one_is_given(void)
{
    g_cfg.mqtt_connect_cfg.client_id = NULL;
    prv_create_disconnected_with_prepare();
    prv_stats_tick();
    TEST_ASSERT_NOT_NULL(g_spy_mqtt_connect_last_client_id);
    TEST_ASSERT_EQUAL_UINT32(24u, (uint32_t) strlen(g_spy_mqtt_connect_last_client_id));

    cloud_publisher_reset_for_test();
    g_cfg.mqtt_connect_cfg.client_id = "given-id";
    prv_create_disconnected_with_prepare();
    prv_stats_tick();
    TEST_ASSERT_EQUAL_STRING("given-id", g_spy_mqtt_connect_last_client_id);
}
