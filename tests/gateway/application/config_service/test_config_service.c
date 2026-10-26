/**
 * @file test_config_service.c
 * @brief Unity tests for ConfigService (Gateway).
 *
 * ConfigStore is replaced by a spy defined in this file; the test reaches its types only
 * through config_service.h, so Ceedling links no ConfigStore .c file. FreeRTOS comes from
 * freertos_mock. Test IDs follow companion §12 (TC-CFG-001..024); TC-CFG-025.. are additions.
 */

#include <stddef.h>
#include <string.h>

#include "config_service.h"
#include "freertos_mock.h"
#include "unity.h"

#define STORE_HANDLE ((config_store_handle_t) 0x1234)

/* ======================================================================= */
/* ConfigStore spy                                                          */
/* ======================================================================= */

static config_store_err_t s_save_result;
static uint32_t s_save_calls;
static uint32_t s_save_len;
static config_blob_t s_saved;
static int32_t s_save_locks_held; /* mutexes taken and not yet given while the save runs */

config_store_err_t config_store_save(config_store_handle_t self, const void *data, uint32_t len)
{
    TEST_ASSERT_EQUAL_PTR(STORE_HANDLE, self);
    s_save_calls++;
    s_save_locks_held = (int32_t) g_mock_xSemaphoreTake_call_count -
                        (int32_t) g_mock_xSemaphoreGive_call_count;
    s_save_len = len;
    if (len == sizeof(s_saved))
    {
        (void) memcpy(&s_saved, data, sizeof(s_saved));
    }
    return s_save_result;
}

/* ======================================================================= */
/* Change callback spy                                                      */
/* ======================================================================= */

static uint32_t s_cb_calls;
static config_param_id_t s_cb_last;

static void on_change(config_param_id_t id)
{
    s_cb_calls++;
    s_cb_last = id;
}

/* ======================================================================= */
/* Fixture                                                                  */
/* ======================================================================= */

void setUp(void)
{
    mock_freertos_reset();
    config_service_reset_for_test();
    s_save_result = CONFIG_STORE_ERR_OK;
    s_save_calls = 0U;
    s_save_len = 0U;
    s_save_locks_held = -1;
    (void) memset(&s_saved, 0, sizeof(s_saved));
    s_cb_calls = 0U;
    s_cb_last = CONFIG_PARAM_COUNT;
}

void tearDown(void)
{
}

static void init_service(void)
{
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_init(STORE_HANDLE));
}

static config_blob_t blob_of_defaults(void)
{
    config_blob_t blob;
    (void) memset(&blob, 0, sizeof(blob));
    blob.schema_version = CONFIG_SCHEMA_VERSION;
    blob.params = *config_service_get_params();
    return blob;
}

static const config_params_t *params(void)
{
    return config_service_get_params();
}

/* ======================================================================= */
/* Init and defaults                                                        */
/* ======================================================================= */

/* TC-CFG-001 */
void test_init_applies_the_defaults(void)
{
    init_service();
    const config_params_t *p = params();

    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT32(1000U, p->polling_interval_ms);
    TEST_ASSERT_EQUAL_UINT16(100U, p->filter_alpha);
    TEST_ASSERT_EQUAL_INT16(-4000, p->temp_range_min);
    TEST_ASSERT_EQUAL_INT16(8500, p->temp_range_max);
    TEST_ASSERT_EQUAL_UINT16(10000U, p->humidity_range_max);
    TEST_ASSERT_EQUAL_UINT16(3000U, p->pressure_range_min);
    TEST_ASSERT_EQUAL_INT16(4000, p->temp_alarm_high);
    TEST_ASSERT_EQUAL_UINT16(9500U, p->pressure_alarm_low);
    TEST_ASSERT_EQUAL_UINT8(1U, p->modbus_slave_addr);
    TEST_ASSERT_EQUAL_UINT16(8883U, p->mqtt_port);
    TEST_ASSERT_EQUAL_UINT32(60000U, p->telemetry_interval_ms);
    TEST_ASSERT_EQUAL_UINT32(600000U, p->health_interval_ms);
    TEST_ASSERT_EQUAL_UINT8(0U, p->ntp_server_count);
    TEST_ASSERT_EQUAL_CHAR('\0', p->mqtt_broker[0]);
    TEST_ASSERT_EQUAL_UINT32(0U, s_save_calls);
}

/* TC-CFG-030 */
void test_init_rejects_a_null_store_and_is_idempotent(void)
{
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NULL_ARG, config_service_init(NULL));
    TEST_ASSERT_NULL(config_service_get_params());

    init_service();
    const uint32_t polling = 500U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &polling));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_init(STORE_HANDLE));
    TEST_ASSERT_EQUAL_UINT32(500U, params()->polling_interval_ms);
}

/* ======================================================================= */
/* apply_loaded                                                             */
/* ======================================================================= */

/* TC-CFG-002 */
void test_apply_loaded_takes_the_stored_values(void)
{
    init_service();
    config_blob_t blob = blob_of_defaults();
    blob.params.polling_interval_ms = 500U;
    blob.params.filter_alpha = 250U;
    (void) strcpy(blob.params.mqtt_broker, "broker.example");

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_apply_loaded(&blob, sizeof(blob)));

    TEST_ASSERT_EQUAL_UINT32(500U, params()->polling_interval_ms);
    TEST_ASSERT_EQUAL_UINT16(250U, params()->filter_alpha);
    TEST_ASSERT_EQUAL_STRING("broker.example", params()->mqtt_broker);
    TEST_ASSERT_EQUAL_UINT32(0U, s_save_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_cb_calls);
}

/* TC-CFG-003 */
void test_apply_loaded_with_a_wrong_schema_version_applies_defaults(void)
{
    init_service();
    config_blob_t blob = blob_of_defaults();
    blob.schema_version = CONFIG_SCHEMA_VERSION + 1U;
    blob.params.polling_interval_ms = 500U;

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_apply_loaded(&blob, sizeof(blob)));

    TEST_ASSERT_EQUAL_UINT32(1000U, params()->polling_interval_ms);
}

/* TC-CFG-031 */
void test_apply_loaded_with_a_wrong_length_applies_defaults(void)
{
    init_service();
    config_blob_t blob = blob_of_defaults();

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID,
                      config_service_apply_loaded(&blob, (uint32_t) sizeof(blob) - 1U));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NULL_ARG,
                      config_service_apply_loaded(NULL, (uint32_t) sizeof(blob)));
}

/* TC-CFG-026: a corrupt stored field falls back to its default, the rest is kept */
void test_apply_loaded_repairs_only_the_bad_field(void)
{
    init_service();
    config_blob_t blob = blob_of_defaults();
    blob.params.polling_interval_ms = 5U; /* below the minimum */
    blob.params.filter_alpha = 200U;

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_apply_loaded(&blob, sizeof(blob)));

    TEST_ASSERT_EQUAL_UINT32(1000U, params()->polling_interval_ms);
    TEST_ASSERT_EQUAL_UINT16(200U, params()->filter_alpha);
}

/* TC-CFG-027: an unprovisioned (empty) broker and no NTP servers must not discard the rest */
void test_apply_loaded_keeps_values_when_the_device_is_unprovisioned(void)
{
    init_service();
    config_blob_t blob = blob_of_defaults();
    blob.params.telemetry_interval_ms = 30000U;

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_apply_loaded(&blob, sizeof(blob)));

    TEST_ASSERT_EQUAL_UINT32(30000U, params()->telemetry_interval_ms);
}

/* TC-CFG-028: a stored alarm band that is inconsistent is repaired back to a valid one */
void test_apply_loaded_repairs_an_inconsistent_alarm_band(void)
{
    init_service();
    config_blob_t blob = blob_of_defaults();
    blob.params.temp_alarm_high = 100; /* band 100 is not wider than 2 * hysteresis 100 */

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_apply_loaded(&blob, sizeof(blob)));

    TEST_ASSERT_EQUAL_INT16(4000, params()->temp_alarm_high);
    TEST_ASSERT_EQUAL_INT16(0, params()->temp_alarm_low);
}

/* ======================================================================= */
/* set_param and validation                                                 */
/* ======================================================================= */

/* TC-CFG-004 */
void test_set_valid_poll_interval_applies_and_persists(void)
{
    init_service();
    const uint32_t value = 500U;

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &value));

    TEST_ASSERT_EQUAL_UINT32(500U, params()->polling_interval_ms);
    TEST_ASSERT_EQUAL_UINT32(1U, s_save_calls);
    TEST_ASSERT_EQUAL_UINT32((uint32_t) sizeof(config_blob_t), s_save_len);
    TEST_ASSERT_EQUAL_UINT32(CONFIG_SCHEMA_VERSION, s_saved.schema_version);
    TEST_ASSERT_EQUAL_UINT32(500U, s_saved.params.polling_interval_ms);
}

/* TC-CFG-005 */
void test_set_poll_interval_below_the_minimum_is_rejected(void)
{
    init_service();
    const uint32_t value = 50U;

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &value));

    TEST_ASSERT_EQUAL_UINT32(1000U, params()->polling_interval_ms);
    TEST_ASSERT_EQUAL_UINT32(0U, s_save_calls);
}

/* TC-CFG-006, TC-CFG-007 */
void test_poll_interval_boundaries_are_inclusive(void)
{
    init_service();
    uint32_t value = 100U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &value));
    value = 60000U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &value));
    value = 60001U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &value));
    TEST_ASSERT_EQUAL_UINT32(60000U, params()->polling_interval_ms);
}

/* TC-CFG-008 */
void test_alarm_high_too_close_to_low_is_rejected(void)
{
    init_service();
    int16_t value = 150; /* low 0, hysteresis 100: band 150 <= 200 */

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_TEMP_ALARM_HIGH, &value));
    TEST_ASSERT_EQUAL_INT16(4000, params()->temp_alarm_high);

    value = 201;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_TEMP_ALARM_HIGH, &value));
}

/* TC-CFG-009 */
void test_hysteresis_that_is_too_large_is_rejected(void)
{
    init_service();
    int16_t value = 2000; /* band 4000 is not wider than 2 * 2000 */

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_TEMP_HYSTERESIS, &value));
    TEST_ASSERT_EQUAL_INT16(100, params()->temp_hysteresis);

    value = 1999;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_TEMP_HYSTERESIS, &value));

    value = 0;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_TEMP_HYSTERESIS, &value));
}

/* TC-CFG-032: the humidity and pressure channels follow the same rules */
void test_humidity_and_pressure_use_the_same_band_rule(void)
{
    init_service();
    uint16_t value = 2300U; /* humidity: low 2000, hysteresis 200: band 300 <= 400 */

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_HUMIDITY_ALARM_HIGH, &value));
    value = 9700U; /* pressure: high 10500, hysteresis 50: band 800 > 100 */
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_PRESSURE_ALARM_LOW, &value));
    value = 10450U; /* band 10500 - 10450 = 50 <= 100 */
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_PRESSURE_ALARM_LOW, &value));
}

/* TC-CFG-033: sensor range limits */
void test_range_limits_and_minimum_gap(void)
{
    init_service();
    int16_t value = -5500;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_TEMP_RANGE_MIN, &value));
    value = -5600;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_TEMP_RANGE_MIN, &value));
    value = -5500 + 99; /* gap to range_min below 1.00 degC */
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_TEMP_RANGE_MAX, &value));
    value = 12501;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_TEMP_RANGE_MAX, &value));
}

/* TC-CFG-034: scalar rules */
void test_scalar_rules(void)
{
    init_service();
    uint16_t alpha = 0U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_FILTER_ALPHA, &alpha));
    alpha = 1000U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_FILTER_ALPHA, &alpha));
    alpha = 999U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_FILTER_ALPHA, &alpha));

    uint8_t addr = 248U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_MODBUS_SLAVE_ADDR, &addr));
    addr = 247U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_MODBUS_SLAVE_ADDR, &addr));

    uint16_t port = 0U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_MQTT_PORT, &port));

    uint32_t ms = 999U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_TELEMETRY_INTERVAL, &ms));
    ms = 3600000U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_TELEMETRY_INTERVAL, &ms));
    ms = 9999U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_HEALTH_INTERVAL, &ms));
    ms = 86400001U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_HEALTH_INTERVAL, &ms));
    ms = 100U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_MODBUS_POLL_PERIOD, &ms));
}

/* TC-CFG-012 */
void test_validate_does_not_change_anything(void)
{
    init_service();
    const uint32_t value = 500U;

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_validate_param(CONFIG_PARAM_POLL_INTERVAL, &value));

    TEST_ASSERT_EQUAL_UINT32(1000U, params()->polling_interval_ms);
    TEST_ASSERT_EQUAL_UINT32(0U, s_save_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_cb_calls);
}

/* ======================================================================= */
/* Strings                                                                  */
/* ======================================================================= */

/* TC-CFG-023 */
void test_set_mqtt_broker_stores_the_string_and_notifies(void)
{
    init_service();
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_register_change_cb(on_change));

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_MQTT_BROKER, "test.broker.io"));

    TEST_ASSERT_EQUAL_STRING("test.broker.io", params()->mqtt_broker);
    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_calls);
    TEST_ASSERT_EQUAL(CONFIG_PARAM_MQTT_BROKER, s_cb_last);
}

/* TC-CFG-024 */
void test_mqtt_broker_that_is_empty_or_too_long_is_rejected(void)
{
    init_service();
    char too_long[CONFIG_MAX_BROKER_LEN + 1U];
    (void) memset(too_long, 'a', sizeof(too_long));
    too_long[CONFIG_MAX_BROKER_LEN] = '\0';

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_MQTT_BROKER, too_long));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_MQTT_BROKER, ""));

    too_long[CONFIG_MAX_BROKER_LEN - 1U] = '\0'; /* 127 characters: the longest that fits */
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_MQTT_BROKER, too_long));
}

/* TC-CFG-025: NTP servers are set as a block, the count must refer to filled slots */
void test_ntp_servers_and_count(void)
{
    init_service();
    uint8_t count = 1U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_NTP_SERVER_COUNT, &count));

    char servers[CONFIG_MAX_NTP_SERVERS][CONFIG_MAX_NTP_HOST_LEN];
    (void) memset(servers, 0, sizeof(servers));
    (void) strcpy(servers[0], "162.159.200.1");
    (void) strcpy(servers[1], "216.239.35.0");
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_NTP_SERVERS, servers));

    count = 2U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param(CONFIG_PARAM_NTP_SERVER_COUNT, &count));
    count = 3U; /* third slot empty */
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_NTP_SERVER_COUNT, &count));
    count = 5U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_NTP_SERVER_COUNT, &count));
    TEST_ASSERT_EQUAL_STRING("216.239.35.0", params()->ntp_servers[1]);
}

/* TC-CFG-035: a host string with no terminator is rejected */
void test_ntp_block_with_an_unterminated_entry_is_rejected(void)
{
    init_service();
    char servers[CONFIG_MAX_NTP_SERVERS][CONFIG_MAX_NTP_HOST_LEN];
    (void) memset(servers, 'x', sizeof(servers));

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_NTP_SERVERS, servers));
}

/* ======================================================================= */
/* Callbacks                                                                */
/* ======================================================================= */

/* TC-CFG-013 */
void test_callback_fires_after_a_successful_write(void)
{
    init_service();
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_register_change_cb(on_change));
    const uint32_t value = 500U;

    (void) config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &value);

    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_calls);
    TEST_ASSERT_EQUAL(CONFIG_PARAM_POLL_INTERVAL, s_cb_last);
}

/* TC-CFG-014 */
void test_callback_does_not_fire_after_a_rejected_write(void)
{
    init_service();
    (void) config_service_register_change_cb(on_change);
    const uint32_t value = 50U;

    (void) config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &value);

    TEST_ASSERT_EQUAL_UINT32(0U, s_cb_calls);
}

/* TC-CFG-015 */
void test_callback_table_is_bounded(void)
{
    init_service();
    for (uint32_t i = 0U; i < CONFIG_MAX_CHANGE_CALLBACKS; i++)
    {
        TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_register_change_cb(on_change));
    }
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_register_change_cb(on_change));
}

/* TC-CFG-036: every mutex taken is given back (apply, snapshot copy, flash save) */
void test_set_param_takes_and_gives_every_mutex(void)
{
    init_service();
    const uint32_t value = 500U;

    (void) config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &value);

    TEST_ASSERT_EQUAL_UINT32(3U, g_mock_xSemaphoreTake_call_count);
    TEST_ASSERT_EQUAL_UINT32(3U, g_mock_xSemaphoreGive_call_count);
}

/* TC-CFG-041 (CFG-D15): the flash save runs holding only the persist mutex, never the
 * data mutex, so readers are not blocked for the length of a flash write */
void test_flash_save_holds_only_the_persist_mutex(void)
{
    init_service();
    const uint32_t value = 500U;

    (void) config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &value);
    TEST_ASSERT_EQUAL_INT32(1, s_save_locks_held);

    s_save_locks_held = -1;
    (void) config_service_flush();
    TEST_ASSERT_EQUAL_INT32(1, s_save_locks_held);

    (void) config_service_snapshot();
    s_save_locks_held = -1;
    (void) config_service_restore_snapshot();
    TEST_ASSERT_EQUAL_INT32(1, s_save_locks_held);
}

/* ======================================================================= */
/* Persistence                                                              */
/* ======================================================================= */

/* TC-CFG-018 */
void test_set_param_with_a_store_failure_keeps_the_value_and_reports_persist(void)
{
    init_service();
    (void) config_service_register_change_cb(on_change);
    s_save_result = CONFIG_STORE_ERR_FLASH_WRITE;
    const uint32_t value = 500U;

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_PERSIST, config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &value));

    TEST_ASSERT_EQUAL_UINT32(500U, params()->polling_interval_ms);
    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_calls);
}

/* TC-CFG-016 */
void test_flush_saves_the_blob(void)
{
    init_service();

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_flush());

    TEST_ASSERT_EQUAL_UINT32(1U, s_save_calls);
    TEST_ASSERT_EQUAL_UINT32((uint32_t) sizeof(config_blob_t), s_save_len);
}

/* TC-CFG-017 */
void test_flush_reports_a_store_failure(void)
{
    init_service();
    s_save_result = CONFIG_STORE_ERR_FLASH_ERASE;

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_PERSIST, config_service_flush());
}

/* ======================================================================= */
/* Deferred persistence (CFG-D16)                                           */
/* ======================================================================= */

/* TC-CFG-042 */
void test_deferred_set_applies_and_notifies_without_touching_flash(void)
{
    init_service();
    (void) config_service_register_change_cb(on_change);
    const uint32_t value = 500U;

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_set_param_deferred(CONFIG_PARAM_POLL_INTERVAL, &value));

    TEST_ASSERT_EQUAL_UINT32(500U, params()->polling_interval_ms);
    TEST_ASSERT_EQUAL_UINT32(1U, s_cb_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_save_calls);
    TEST_ASSERT_TRUE(config_service_has_pending());
}

/* TC-CFG-043 */
void test_persist_pending_writes_once_and_clears_the_flag(void)
{
    init_service();
    const uint32_t value = 500U;
    (void) config_service_set_param_deferred(CONFIG_PARAM_POLL_INTERVAL, &value);

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_persist_pending());

    TEST_ASSERT_EQUAL_UINT32(1U, s_save_calls);
    TEST_ASSERT_EQUAL_UINT32(500U, s_saved.params.polling_interval_ms);
    TEST_ASSERT_FALSE(config_service_has_pending());

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_persist_pending());
    TEST_ASSERT_EQUAL_UINT32(1U, s_save_calls); /* nothing pending: no second write */
}

/* TC-CFG-044: several deferred changes need one write */
void test_several_deferred_changes_share_one_write(void)
{
    init_service();
    uint32_t poll = 500U;
    uint16_t alpha = 200U;
    (void) config_service_set_param_deferred(CONFIG_PARAM_POLL_INTERVAL, &poll);
    (void) config_service_set_param_deferred(CONFIG_PARAM_FILTER_ALPHA, &alpha);

    (void) config_service_persist_pending();

    TEST_ASSERT_EQUAL_UINT32(1U, s_save_calls);
    TEST_ASSERT_EQUAL_UINT32(500U, s_saved.params.polling_interval_ms);
    TEST_ASSERT_EQUAL_UINT16(200U, s_saved.params.filter_alpha);
}

/* TC-CFG-045: a failed write leaves the change pending for the next attempt */
void test_persist_failure_keeps_the_change_pending(void)
{
    init_service();
    const uint32_t value = 500U;
    (void) config_service_set_param_deferred(CONFIG_PARAM_POLL_INTERVAL, &value);
    s_save_result = CONFIG_STORE_ERR_FLASH_WRITE;

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_PERSIST, config_service_persist_pending());
    TEST_ASSERT_TRUE(config_service_has_pending());

    s_save_result = CONFIG_STORE_ERR_OK;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_persist_pending());
    TEST_ASSERT_FALSE(config_service_has_pending());
}

/* TC-CFG-046 */
void test_deferred_set_rejects_what_set_param_rejects(void)
{
    init_service();
    const uint32_t bad = 50U;

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param_deferred(CONFIG_PARAM_POLL_INTERVAL, &bad));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NULL_ARG, config_service_set_param_deferred(CONFIG_PARAM_POLL_INTERVAL, NULL));
    TEST_ASSERT_FALSE(config_service_has_pending());
}

/* TC-CFG-047: a durable write also flushes whatever was deferred before it */
void test_durable_set_param_flushes_earlier_deferred_changes(void)
{
    init_service();
    uint16_t alpha = 200U;
    uint32_t poll = 500U;
    (void) config_service_set_param_deferred(CONFIG_PARAM_FILTER_ALPHA, &alpha);

    (void) config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &poll);

    TEST_ASSERT_EQUAL_UINT16(200U, s_saved.params.filter_alpha);
    TEST_ASSERT_FALSE(config_service_has_pending());
}

/* TC-CFG-048: guards, and the manager interface exposes the deferred setter */
void test_deferred_api_guards_and_vtable(void)
{
    const uint32_t value = 500U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NOT_INIT, config_service_set_param_deferred(CONFIG_PARAM_POLL_INTERVAL, &value));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NOT_INIT, config_service_persist_pending());
    TEST_ASSERT_FALSE(config_service_has_pending());

    init_service();
    const iconfig_manager_t *manager = config_service_get_manager();
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, manager->set_param_deferred(manager->ctx, CONFIG_PARAM_POLL_INTERVAL, &value));
    TEST_ASSERT_TRUE(config_service_has_pending());
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, manager->persist_pending(manager->ctx));
    TEST_ASSERT_FALSE(config_service_has_pending());
    TEST_ASSERT_EQUAL_UINT32(1U, s_save_calls);
}

/* ======================================================================= */
/* Snapshot and restore                                                     */
/* ======================================================================= */

/* TC-CFG-010 */
void test_snapshot_and_restore_roll_the_change_back(void)
{
    init_service();
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_snapshot());
    const uint32_t value = 500U;
    (void) config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &value);
    s_save_calls = 0U;

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, config_service_restore_snapshot());

    TEST_ASSERT_EQUAL_UINT32(1000U, params()->polling_interval_ms);
    TEST_ASSERT_EQUAL_UINT32(1U, s_save_calls);
    TEST_ASSERT_EQUAL_UINT32(1000U, s_saved.params.polling_interval_ms);
}

/* TC-CFG-011 */
void test_restore_without_a_snapshot_is_rejected(void)
{
    init_service();
    const uint32_t value = 500U;
    (void) config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &value);

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_restore_snapshot());

    TEST_ASSERT_EQUAL_UINT32(500U, params()->polling_interval_ms);
}

/* TC-CFG-037: restore keeps the restored values in memory even if persisting fails */
void test_restore_with_a_store_failure_reports_persist(void)
{
    init_service();
    (void) config_service_snapshot();
    const uint32_t value = 500U;
    (void) config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &value);
    s_save_result = CONFIG_STORE_ERR_FLASH_WRITE;

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_PERSIST, config_service_restore_snapshot());

    TEST_ASSERT_EQUAL_UINT32(1000U, params()->polling_interval_ms);
}

/* ======================================================================= */
/* Guards and interfaces                                                    */
/* ======================================================================= */

/* TC-CFG-019 */
void test_every_call_before_init_reports_not_init(void)
{
    const uint32_t value = 500U;
    config_blob_t blob;
    (void) memset(&blob, 0, sizeof(blob));

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NOT_INIT, config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &value));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NOT_INIT, config_service_validate_param(CONFIG_PARAM_POLL_INTERVAL, &value));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NOT_INIT, config_service_apply_loaded(&blob, sizeof(blob)));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NOT_INIT, config_service_snapshot());
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NOT_INIT, config_service_restore_snapshot());
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NOT_INIT, config_service_flush());
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NOT_INIT, config_service_register_change_cb(on_change));
    TEST_ASSERT_NULL(config_service_get_params());
    TEST_ASSERT_NULL(config_service_get_provider());
    TEST_ASSERT_NULL(config_service_get_manager());
}

/* TC-CFG-020 */
void test_null_arguments_are_rejected(void)
{
    init_service();

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NULL_ARG, config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, NULL));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NULL_ARG, config_service_validate_param(CONFIG_PARAM_POLL_INTERVAL, NULL));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NULL_ARG, config_service_register_change_cb(NULL));
}

/* TC-CFG-038 */
void test_an_unknown_parameter_id_is_rejected(void)
{
    init_service();
    const uint32_t value = 500U;

    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_set_param(CONFIG_PARAM_COUNT, &value));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, config_service_validate_param((config_param_id_t) -1, &value));
}

/* TC-CFG-021 */
void test_provider_vtable_returns_the_live_parameters(void)
{
    init_service();
    const iconfig_provider_t *provider = config_service_get_provider();

    TEST_ASSERT_NOT_NULL(provider);
    TEST_ASSERT_EQUAL_PTR(config_service_get_params(), provider->get_params(provider->ctx));
}

/* TC-CFG-022 */
void test_manager_vtable_forwards_set_and_validate(void)
{
    init_service();
    const iconfig_manager_t *manager = config_service_get_manager();
    const uint32_t value = 500U;

    TEST_ASSERT_NOT_NULL(manager);
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, manager->validate_param(manager->ctx, CONFIG_PARAM_POLL_INTERVAL, &value));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_OK, manager->set_param(manager->ctx, CONFIG_PARAM_POLL_INTERVAL, &value));
    TEST_ASSERT_EQUAL_UINT32(500U, params()->polling_interval_ms);
}

/* TC-CFG-039 */
void test_lock_and_unlock_take_and_give_the_mutex(void)
{
    init_service();

    config_service_lock();
    config_service_unlock();

    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xSemaphoreTake_call_count);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xSemaphoreGive_call_count);
}

/* TC-CFG-040: the blob fits the store with room to spare */
void test_blob_size_is_recorded_and_fits_the_store(void)
{
    TEST_ASSERT_LESS_THAN_UINT32(CONFIG_STORE_MAX_DATA_BYTES, (uint32_t) sizeof(config_blob_t));
    TEST_ASSERT_LESS_THAN_UINT32(1024U, (uint32_t) sizeof(config_blob_t));
}

/* TC-CFG-049 (CFG-D3): every integer field is naturally aligned, so a read cannot tear */
void test_every_integer_field_is_naturally_aligned(void)
{
    TEST_ASSERT_EQUAL_UINT32(0U, offsetof(config_params_t, polling_interval_ms) % 4U);
    TEST_ASSERT_EQUAL_UINT32(0U, offsetof(config_params_t, modbus_poll_period_ms) % 4U);
    TEST_ASSERT_EQUAL_UINT32(0U, offsetof(config_params_t, mqtt_port) % 2U);
    TEST_ASSERT_EQUAL_UINT32(0U, offsetof(config_params_t, telemetry_interval_ms) % 4U);
    TEST_ASSERT_EQUAL_UINT32(0U, offsetof(config_params_t, health_interval_ms) % 4U);
    TEST_ASSERT_EQUAL_UINT32(0U, offsetof(config_params_t, filter_alpha) % 2U);
    TEST_ASSERT_EQUAL_UINT32(0U, offsetof(config_params_t, temp_alarm_high) % 2U);
    TEST_ASSERT_EQUAL_UINT32(0U, sizeof(config_params_t) % 4U);
    TEST_ASSERT_EQUAL_UINT32(sizeof(uint32_t) + sizeof(config_params_t), sizeof(config_blob_t));
}
