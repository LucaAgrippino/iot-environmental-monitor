/**
 * @file test_cloud_publisher.c
 * @brief Unit tests for CloudPublisher (Gateway).
 *
 * Covers CP-T01..CP-T16 as defined in
 * docs/lld/application/cloud-publisher-lld.md §13.
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
 *  - CP-D7: no mqtt_client_is_connected() exists on the real MqttClient
 *    API, so the connectivity gate is collapsed into mqtt_client_publish()
 *    itself. CP-T05 asserts publish() IS called (returning
 *    MQTT_CLIENT_ERR_NOT_CONNECTED) and the message still lands in SAF,
 *    rather than asserting publish() is skipped entirely.
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

static sensor_reading_t g_spy_sensor_reading;
static uint32_t g_spy_sensor_get_latest_calls;
sensor_reading_t sensor_service_get_latest(sensor_service_handle_t handle)
{
    (void) handle;
    g_spy_sensor_get_latest_calls++;
    return g_spy_sensor_reading;
}

static modbus_poller_fd_reading_t g_spy_fd_reading;
modbus_poller_fd_reading_t modbus_poller_get_latest_fd(modbus_poller_handle_t handle)
{
    (void) handle;
    return g_spy_fd_reading;
}

static alarm_event_cb_t g_spy_alarm_cb;
static void *g_spy_alarm_cb_ctx;
static uint32_t g_spy_alarm_subscribe_calls;
void alarm_service_subscribe(alarm_service_handle_t handle, alarm_event_cb_t cb, void *ctx)
{
    (void) handle;
    g_spy_alarm_subscribe_calls++;
    g_spy_alarm_cb = cb;
    g_spy_alarm_cb_ctx = ctx;
}

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

/* ======================================================================= */
/* Fixture                                                                 */
/* ======================================================================= */

static cloud_publisher_config_t g_cfg;
static cloud_publisher_handle_t g_handle;

void setUp(void)
{
    mock_freertos_reset();
    cloud_publisher_reset_for_test();

    (void) memset(&g_spy_sensor_reading, 0, sizeof(g_spy_sensor_reading));
    g_spy_sensor_get_latest_calls = 0u;
    (void) memset(&g_spy_fd_reading, 0, sizeof(g_spy_fd_reading));

    g_spy_alarm_cb = NULL;
    g_spy_alarm_cb_ctx = NULL;
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

    (void) memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.mqtt = (mqtt_client_handle_t) 0x1000;
    g_cfg.sensors = (sensor_service_handle_t) 0x1001;
    g_cfg.alarms = (alarm_service_handle_t) 0x1002;
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

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_sensor_get_latest_calls);
    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_mqtt_publish_last_topic, "/telemetry"));
    TEST_ASSERT_EQUAL(MQTT_QOS_0, g_spy_mqtt_publish_last_qos);
    TEST_ASSERT_EQUAL_UINT32(0u, g_spy_saf_enqueue_calls);
}

void test_CP_T05_telemetry_disconnected(void)
{
    /* CP-D7: no is_connected() query exists on the real MqttClient API,
     * so the gate is collapsed into mqtt_client_publish() itself —
     * publish() IS called and reports NOT_CONNECTED; the message still
     * lands in StoreAndForward. */
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_NOT_CONNECTED;
    g_mock_xTaskNotifyWait_next_value = TC_TELEMETRY_TICK;

    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_mqtt_publish_calls);
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

static void prv_load_alarm_event(alarm_event_t *ev)
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

    alarm_event_t ev;
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
}

void test_CP_T08_alarm_disconnected(void)
{
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_spy_mqtt_publish_return = MQTT_CLIENT_ERR_NOT_CONNECTED;

    alarm_event_t ev;
    prv_load_alarm_event(&ev);
    g_mock_xQueueSend_last_item_size = sizeof(ev); /* enable xQueueSend capture */
    cloud_publisher_inject_alarm_for_test(g_handle, &ev);

    (void) memcpy(g_mock_xQueueReceive_next_item, g_mock_xQueueSend_last_item, sizeof(ev));
    g_mock_xQueueReceive_next_item_size = sizeof(ev);
    g_mock_xQueueReceive_return = pdTRUE;
    g_mock_xQueueReceive_available = 1u;

    g_mock_xTaskNotifyWait_next_value = TC_ALARM_PENDING;
    cloud_publisher_task_step_for_test(g_handle);

    TEST_ASSERT_EQUAL_UINT32(1u, g_spy_saf_enqueue_calls);
    TEST_ASSERT_EQUAL(MQTT_QOS_1, g_spy_saf_enqueue_last_qos);
    TEST_ASSERT_NOT_NULL(strstr(g_spy_saf_enqueue_last_topic, "/alarms"));
}

void test_CP_T15_alarm_queue_full(void)
{
    /* Overflow: xQueueSend reports the queue full (pdFALSE). The
     * subscriber callback must not crash and must still notify. */
    TEST_ASSERT_EQUAL(CP_ERR_OK, cloud_publisher_create(&g_cfg, &g_handle));
    g_mock_xQueueSend_return = pdFALSE;

    alarm_event_t ev;
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
    g_mock_xQueueSend_last_item_size = TC_COMMAND_ENTRY_SIZE; /* enable xQueueSend capture */
    cloud_publisher_inject_command_for_test(g_handle, "cmd/iotmonitor/x/config", payload,
                                            (uint32_t) sizeof(payload));

    (void) memcpy(g_mock_xQueueReceive_next_item, g_mock_xQueueSend_last_item,
                  g_mock_xQueueSend_last_item_size);
    g_mock_xQueueReceive_next_item_size = g_mock_xQueueSend_last_item_size;
    g_mock_xQueueReceive_return = pdTRUE;
    g_mock_xQueueReceive_available = 1u;

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
    g_mock_xQueueSend_last_item_size = TC_COMMAND_ENTRY_SIZE; /* enable xQueueSend capture */
    cloud_publisher_inject_command_for_test(g_handle, "cmd/iotmonitor/x/bogus", payload,
                                            (uint32_t) sizeof(payload));

    (void) memcpy(g_mock_xQueueReceive_next_item, g_mock_xQueueSend_last_item,
                  g_mock_xQueueSend_last_item_size);
    g_mock_xQueueReceive_next_item_size = g_mock_xQueueSend_last_item_size;
    g_mock_xQueueReceive_return = pdTRUE;
    g_mock_xQueueReceive_available = 1u;

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
}

/* ======================================================================= */
/* CP-T14 — JSON truncation                                               */
/* ======================================================================= */

void test_CP_T14_json_truncation(void)
{
    char tiny_buf[8];
    sensor_reading_t reading;
    modbus_poller_fd_reading_t fd;
    (void) memset(&reading, 0, sizeof(reading));
    (void) memset(&fd, 0, sizeof(fd));

    uint32_t n = cp_serialise_telemetry(tiny_buf, (uint32_t) sizeof(tiny_buf), &reading, &fd,
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
