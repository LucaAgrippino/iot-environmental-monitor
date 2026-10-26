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

#include <string.h>
#include <stdint.h>
#include <stdbool.h>

#include "freertos_mock.h"
#include "logger_stub.h"
#include "cloud_publisher/cloud_publisher.h"
#include "cloud_publisher/cloud_publisher_json.h"

/* Notification bits — mirrors companion §4 (private in cloud_publisher.c,
 * re-declared here since they are an internal implementation detail). */
#define TC_TELEMETRY_TICK (1u << 0)
#define TC_HEALTH_TICK (1u << 1)
#define TC_STATS_TICK (1u << 2)
#define TC_ALARM_PENDING (1u << 3)
#define TC_COMMAND_PENDING (1u << 4)
#define TC_WIFI_RECV_READY (1u << 5) /**< Mirrors MQTT_CLIENT_WIFI_RECV_READY_BIT. */

/* Mirrors CP_JSON_BUF_SIZE (private to cloud_publisher.c). */
#define TC_JSON_BUF_SIZE 4096u

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

static modbus_poller_fd_reading_t g_spy_fd_reading;
modbus_poller_fd_reading_t modbus_poller_get_latest_fd(modbus_poller_handle_t handle)
{
    (void) handle;
    return g_spy_fd_reading;
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

static saf_err_t g_spy_saf_enqueue_return;
static uint32_t g_spy_saf_enqueue_calls;
static char g_spy_saf_enqueue_last_topic[SAF_TOPIC_MAX_LEN];
static mqtt_qos_t g_spy_saf_enqueue_last_qos;
saf_err_t store_and_forward_enqueue(store_and_forward_handle_t handle, const char *topic,
                                    const uint8_t *buf, uint32_t len, mqtt_qos_t qos)
{
    (void) handle;
    (void) buf;
    (void) len;
    g_spy_saf_enqueue_calls++;
    (void) strncpy(g_spy_saf_enqueue_last_topic, topic, SAF_TOPIC_MAX_LEN - 1u);
    g_spy_saf_enqueue_last_qos = qos;
    return g_spy_saf_enqueue_return;
}

static saf_err_t g_spy_saf_dequeue_return;
static saf_entry_t g_spy_saf_dequeue_entry;
static uint32_t g_spy_saf_dequeue_calls;
/* Number of times to actually deliver g_spy_saf_dequeue_entry while
 * g_spy_saf_dequeue_return == SAF_ERR_OK before falling back to
 * SAF_ERR_EMPTY — without this, a permanently-OK return would spin
 * prv_drain_saf()'s "while (dequeue() == SAF_ERR_OK)" loop forever. */
static uint32_t g_spy_saf_dequeue_available;
saf_err_t store_and_forward_dequeue(store_and_forward_handle_t handle, saf_entry_t *entry_out)
{
    (void) handle;
    g_spy_saf_dequeue_calls++;
    if ((g_spy_saf_dequeue_return == SAF_ERR_OK) && (g_spy_saf_dequeue_available > 0u))
    {
        g_spy_saf_dequeue_available--;
        *entry_out = g_spy_saf_dequeue_entry;
        return SAF_ERR_OK;
    }
    return SAF_ERR_EMPTY;
}

static uint32_t g_spy_saf_confirm_calls;
saf_err_t store_and_forward_confirm(store_and_forward_handle_t handle)
{
    (void) handle;
    g_spy_saf_confirm_calls++;
    return SAF_ERR_OK;
}

static cp_health_snapshot_t g_spy_health_snapshot;
cp_health_snapshot_t health_snapshot_get(health_monitor_handle_t handle)
{
    (void) handle;
    return g_spy_health_snapshot;
}

static uint32_t g_spy_health_update_mqtt_calls;
void health_report_update_mqtt(health_monitor_handle_t handle, const mqtt_stats_t *stats,
                               const mqtt_stats_t *last_stats)
{
    (void) handle;
    (void) stats;
    (void) last_stats;
    g_spy_health_update_mqtt_calls++;
}

static uint32_t g_spy_health_push_event_calls;
static cp_health_event_t g_spy_health_push_event_last;
void health_report_push_event(health_monitor_handle_t handle, cp_health_event_t event)
{
    (void) handle;
    g_spy_health_push_event_calls++;
    g_spy_health_push_event_last = event;
}

static uint32_t g_spy_telemetry_interval_s;
uint32_t config_provider_get_telemetry_interval_s(config_service_handle_t handle)
{
    (void) handle;
    return g_spy_telemetry_interval_s;
}

static uint32_t g_spy_health_interval_s;
uint32_t config_provider_get_health_interval_s(config_service_handle_t handle)
{
    (void) handle;
    return g_spy_health_interval_s;
}

static bool g_spy_config_apply_return;
static uint32_t g_spy_config_apply_calls;
bool config_manager_apply(config_service_handle_t handle, const uint8_t *payload, uint32_t len)
{
    (void) handle;
    (void) payload;
    (void) len;
    g_spy_config_apply_calls++;
    return g_spy_config_apply_return;
}

static uint32_t g_spy_update_handle_calls;
void update_service_handle_command(update_service_handle_t handle, const uint8_t *payload,
                                   uint32_t len)
{
    (void) handle;
    (void) payload;
    (void) len;
    g_spy_update_handle_calls++;
}

static uint32_t g_spy_lifecycle_handle_calls;
void lifecycle_handle_remote(lifecycle_handle_t handle, const uint8_t *payload, uint32_t len)
{
    (void) handle;
    (void) payload;
    (void) len;
    g_spy_lifecycle_handle_calls++;
}

static mqtt_client_err_t g_spy_mqtt_publish_return;
static uint32_t g_spy_mqtt_publish_calls;
static char g_spy_mqtt_publish_last_topic[SAF_TOPIC_MAX_LEN];
static char g_spy_mqtt_publish_last_payload[TC_JSON_BUF_SIZE];
static uint32_t g_spy_mqtt_publish_last_len;
static mqtt_qos_t g_spy_mqtt_publish_last_qos;
mqtt_client_err_t mqtt_client_publish(mqtt_client_handle_t handle, const char *topic,
                                      const uint8_t *payload, uint32_t len, mqtt_qos_t qos)
{
    (void) handle;
    g_spy_mqtt_publish_calls++;
    (void) strncpy(g_spy_mqtt_publish_last_topic, topic, SAF_TOPIC_MAX_LEN - 1u);
    uint32_t copy_len = (len < (sizeof(g_spy_mqtt_publish_last_payload) - 1u))
                            ? len
                            : (sizeof(g_spy_mqtt_publish_last_payload) - 1u);
    (void) memcpy(g_spy_mqtt_publish_last_payload, payload, copy_len);
    g_spy_mqtt_publish_last_payload[copy_len] = '\0';
    g_spy_mqtt_publish_last_len = len;
    g_spy_mqtt_publish_last_qos = qos;
    return g_spy_mqtt_publish_return;
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
mqtt_client_err_t mqtt_client_connect_step(mqtt_client_handle_t handle,
                                           const mqtt_connect_cfg_t *cfg)
{
    (void) handle;
    (void) cfg;
    g_spy_mqtt_connect_calls++;
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

    (void) memset(&g_spy_snapshot, 0, sizeof(g_spy_snapshot));
    g_spy_snapshot_return = SENSOR_SERVICE_ERR_OK;
    g_spy_sensor_get_snapshot_calls = 0u;
    g_spy_alarm_limit = 0;
    (void) memset(&g_spy_fd_reading, 0, sizeof(g_spy_fd_reading));

    g_spy_alarm_cb = NULL;
    g_spy_alarm_subscribe_calls = 0u;

    g_spy_saf_enqueue_return = SAF_ERR_OK;
    g_spy_saf_enqueue_calls = 0u;
    (void) memset(g_spy_saf_enqueue_last_topic, 0, sizeof(g_spy_saf_enqueue_last_topic));

    g_spy_saf_dequeue_return = SAF_ERR_EMPTY;
    (void) memset(&g_spy_saf_dequeue_entry, 0, sizeof(g_spy_saf_dequeue_entry));
    g_spy_saf_dequeue_calls = 0u;
    g_spy_saf_dequeue_available = 0u;
    g_spy_saf_confirm_calls = 0u;

    (void) memset(&g_spy_health_snapshot, 0, sizeof(g_spy_health_snapshot));
    g_spy_health_update_mqtt_calls = 0u;
    g_spy_health_push_event_calls = 0u;

    g_spy_telemetry_interval_s = 60u;
    g_spy_health_interval_s = 600u;

    g_spy_config_apply_return = true;
    g_spy_config_apply_calls = 0u;

    g_spy_update_handle_calls = 0u;
    g_spy_lifecycle_handle_calls = 0u;

    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_OK;
    g_spy_mqtt_publish_calls = 0u;
    (void) memset(g_spy_mqtt_publish_last_topic, 0, sizeof(g_spy_mqtt_publish_last_topic));
    (void) memset(g_spy_mqtt_publish_last_payload, 0, sizeof(g_spy_mqtt_publish_last_payload));

    (void) memset(&g_spy_mqtt_stats, 0, sizeof(g_spy_mqtt_stats));
    g_spy_mqtt_get_stats_calls = 0u;
    g_spy_mqtt_process_calls = 0u;

    g_spy_mqtt_is_connected_return = true; /* most tests assume an already-connected broker */
    g_spy_mqtt_connect_return = MQTT_CLIENT_ERR_OK;
    g_spy_mqtt_connect_calls = 0u;

    (void) memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.mqtt = (mqtt_client_handle_t) 0x1000;
    g_cfg.poller = (modbus_poller_handle_t) 0x1003;
    g_cfg.saf = (store_and_forward_handle_t) 0x1004;
    g_cfg.health_read = (health_monitor_handle_t) 0x1005;
    g_cfg.health_write = (health_monitor_handle_t) 0x1006;
    g_cfg.cfg_read = (config_service_handle_t) 0x1007;
    g_cfg.cfg_write = (config_service_handle_t) 0x1008;
    g_cfg.update_svc = (update_service_handle_t) 0x1009;
    g_cfg.lifecycle = (lifecycle_handle_t) 0x100A;

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
    TEST_ASSERT_EQUAL_UINT32(2u, g_mock_xQueueCreateStatic_call_count);
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
    /* Outcome reported through IHealthReport (the seam TC-HW-CP-011 times). */
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_health_push_event_calls);
    TEST_ASSERT_EQUAL(CP_HEALTH_EVENT_ALARM_PUBLISHED, g_spy_health_push_event_last);
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
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_health_push_event_calls);
    TEST_ASSERT_EQUAL(CP_HEALTH_EVENT_ALARM_BUFFERED, g_spy_health_push_event_last);
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
    (void) strncpy(entry.topic, "dt/iotmonitor/x/telemetry", SAF_TOPIC_MAX_LEN - 1u);
    entry.len = 2u;
    entry.payload[0] = 'a';
    entry.payload[1] = 'b';
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
    (void) strncpy(entry.topic, "dt/iotmonitor/x/telemetry", SAF_TOPIC_MAX_LEN - 1u);
    entry.len = 2u;
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
    g_spy_config_apply_return = true;
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_OK;

    const uint8_t payload[] = "{\"telemetry_interval_s\":30}";
    /* command_queue is CloudPublisher's *second* xQueueCreateStatic() call
     * (alarm_queue is first) — the shared FreeRTOS mock distinguishes the
     * first two distinct queue handles a test touches, routing them to
     * separate tracking globals (see tests/support/freertos_mock.c). Must
     * use the "_2" globals here, not the plain ones. */
    g_mock_xQueueSend2_last_item_size = TC_COMMAND_ENTRY_SIZE; /* enable xQueueSend capture */
    cloud_publisher_inject_command_for_test(g_handle, "cmd/iotmonitor/x/config", payload,
                                            (uint32_t) sizeof(payload));

    (void) memcpy(g_mock_xQueueReceive2_next_item, g_mock_xQueueSend2_last_item,
                  g_mock_xQueueSend2_last_item_size);
    g_mock_xQueueReceive2_next_item_size = g_mock_xQueueSend2_last_item_size;
    g_mock_xQueueReceive2_return = pdTRUE;
    g_mock_xQueueReceive2_available = 1u;

    g_mock_xTaskNotifyWait_next_value = TC_COMMAND_PENDING;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_config_apply_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_topic, "/results"));
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

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_config_apply_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_update_handle_calls);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_lifecycle_handle_calls);
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
    modbus_poller_fd_reading_t fd;
    (void) memset(&fd, 0, sizeof(fd));
    prv_set_reading(SENSOR_ID_TEMPERATURE, 2345); /* 23.45 degC -> 235 deci-C (rounded) */
    prv_set_reading(SENSOR_ID_HUMIDITY, 5049);    /* 50.49 %RH  -> 50                   */
    prv_set_reading(SENSOR_ID_PRESSURE, 10132);   /* 1013.2 hPa -> 1013                 */
    prv_set_reading(SENSOR_ID_GYRO_X, -1500);     /* mdps, passed through               */
    g_spy_snapshot.readings[SENSOR_ID_TEMPERATURE].timestamp.sync_state = TIME_SYNC_SYNCHRONISED;

    TEST_ASSERT_GREATER_THAN_UINT32(
        0u, cp_serialise_telemetry(buf, sizeof(buf), &g_spy_snapshot, &fd, "SER"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"temperature_deci_c\":235,"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"humidity_pct\":50,"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"pressure_hpa\":1013,"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"gyro_x_mdps\":-1500,"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"sync_state\":\"synchronised\""));
}

void test_CP_T21_telemetry_rounds_negative_temperature_away_from_zero(void)
{
    char buf[512];
    modbus_poller_fd_reading_t fd;
    (void) memset(&fd, 0, sizeof(fd));
    prv_set_reading(SENSOR_ID_TEMPERATURE, -1005); /* -10.05 degC -> -101 deci-C */

    (void) cp_serialise_telemetry(buf, sizeof(buf), &g_spy_snapshot, &fd, "SER");

    TEST_ASSERT_NOT_NULL(strstr(buf, "\"temperature_deci_c\":-101,"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"sync_state\":\"unsynchronised\""));
}

void test_CP_T22_telemetry_reports_invalid_channel_as_zero(void)
{
    char buf[512];
    modbus_poller_fd_reading_t fd;
    (void) memset(&fd, 0, sizeof(fd));
    prv_set_reading(SENSOR_ID_ACCEL_X, 900);
    g_spy_snapshot.readings[SENSOR_ID_ACCEL_X].valid = false;

    (void) cp_serialise_telemetry(buf, sizeof(buf), &g_spy_snapshot, &fd, "SER");

    TEST_ASSERT_NOT_NULL(strstr(buf, "\"accel_x_mg\":0,"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"invalid_channels\":[\"temperature_deci_c\","));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"accel_x_mg\""));
}

void test_CP_T27_invalid_channels_is_empty_when_all_valid(void)
{
    char buf[1024];
    modbus_poller_fd_reading_t fd;
    (void) memset(&fd, 0, sizeof(fd));
    for (uint32_t i = 0u; i < (uint32_t) SENSOR_ID_COUNT; i++)
    {
        g_spy_snapshot.readings[i].valid = true;
    }

    TEST_ASSERT_GREATER_THAN_UINT32(
        0u, cp_serialise_telemetry(buf, sizeof(buf), &g_spy_snapshot, &fd, "SER"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"invalid_channels\":[],"));
}

void test_CP_T28_all_channels_invalid_fits_the_list_buffer(void)
{
    char buf[2048];
    modbus_poller_fd_reading_t fd;
    (void) memset(&fd, 0, sizeof(fd));

    TEST_ASSERT_GREATER_THAN_UINT32(
        0u, cp_serialise_telemetry(buf, sizeof(buf), &g_spy_snapshot, &fd, "SER"));
    TEST_ASSERT_NOT_NULL(strstr(buf, "\"mag_z_mgauss\"],"));
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
    modbus_poller_fd_reading_t fd;
    (void) memset(&snap, 0, sizeof(snap));
    (void) memset(&fd, 0, sizeof(fd));

    uint32_t n = cp_serialise_telemetry(tiny_buf, (uint32_t) sizeof(tiny_buf), &snap, &fd,
                                        "0011223344556677889900aa");

    TEST_ASSERT_EQUAL_UINT32(0u, n);
}

/* ======================================================================= */
/* CP-T16 — configurable intervals                                        */
/* ======================================================================= */

void test_CP_T16_configurable_intervals(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));

    g_spy_telemetry_interval_s = 30u;
    g_mock_xTaskNotifyWait_next_value = TC_TELEMETRY_TICK;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xTimerChangePeriod_call_count);
    TEST_ASSERT_EQUAL_UINT32(pdMS_TO_TICKS(30u * 1000u), g_mock_xTimerChangePeriod_last_period);
}

/* ======================================================================= */
/* CP-T30..T42 - LifecycleController operations (CP-D16)                   */
/* ======================================================================= */

#define TC_FLUSH_REQUEST (1u << 6)
#define TC_ROLLBACK_REPORT (1u << 7)

static void prv_load_buffered_entry(void)
{
    saf_entry_t entry;

    (void) memset(&entry, 0, sizeof(entry));
    (void) strncpy(entry.topic, "dt/iotmonitor/x/telemetry", SAF_TOPIC_MAX_LEN - 1u);
    entry.len = 2u;
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

void test_CP_T38_report_rollback_result_rejects_null(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_NULL_PTR, cloud_publisher_report_rollback_result(NULL));
}

void test_CP_T39_report_rollback_result_posts_the_request_to_the_task(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_mock_xTaskNotify_call_count = 0u;

    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_report_rollback_result(g_handle));
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xTaskNotify_call_count);
    TEST_ASSERT_EQUAL_UINT32(TC_ROLLBACK_REPORT, g_mock_xTaskNotify_last_value);
}

void test_CP_T40_the_task_publishes_the_rollback_result_at_qos1_when_connected(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_OK;

    g_mock_xTaskNotifyWait_next_value = TC_ROLLBACK_REPORT;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_topic, "/results"));
    TEST_ASSERT_EQUAL(MQTT_QOS_1, g_spy_mqtt_publish_last_qos);
    TEST_ASSERT_EQUAL_STRING("{\"event\":\"ota\",\"result\":\"rolled_back\"}",
                             g_spy_mqtt_publish_last_payload);
}

void test_CP_T41_the_rollback_result_is_buffered_when_the_broker_is_down(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_mqtt_is_connected_return = false;

    g_mock_xTaskNotifyWait_next_value = TC_ROLLBACK_REPORT;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_saf_enqueue_calls);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_saf_enqueue_last_topic, "/results"));
}

void test_CP_T42_the_notify_mask_clears_the_new_bits(void)
{
    g_mock_xQueueReceive_available = 0u; /* no pending alarms: the flush drains an empty queue */
    /* A bit missing from the clear-on-exit mask would wake the task forever (see the comment on
     * CP_NOTIFY_ALL_BITS). Both new bits must be consumed by one step. */
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_mock_xTaskNotifyWait_next_value = TC_FLUSH_REQUEST | TC_ROLLBACK_REPORT;
    cloud_publisher_task_step_for_test(g_handle);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_xTaskNotifyWait_next_value);
}
