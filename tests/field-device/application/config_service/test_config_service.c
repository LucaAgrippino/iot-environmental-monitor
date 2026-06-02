/**
 * @file test_config_service.c
 * @brief Unit tests for ConfigService — TC-CSVC-001..012.
 *
 * Covers docs/lld/application/config-service.md §7.
 *
 * Mocking strategy:
 *   - IConfigStore → s_stub_store vtable with configurable return values
 *     (config_store_stub.h — no matching .c, so config_store.c is not linked)
 *   - FreeRTOS     → freertos_mock.h (auto-links freertos_mock.c)
 *   - Logger macros → no-ops via #ifdef TEST in config_service.c
 *
 * Build defines required: STM32F469xx, TEST, UNIT_TEST (project.yml
 * :test_config_service:).
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"
#include "freertos_mock.h"   /* causes Ceedling to auto-link freertos_mock.c */

/* config_store_stub.h is pulled in transitively via config_service.h when TEST
 * is defined; include it explicitly so the stub vtable is visible here. */
#include "config_store_stub.h"
#include "config_service/config_service.h"

/* ========================================================================= */
/* ConfigStore stub                                                          */
/* ========================================================================= */

static config_store_err_t g_load_ret  = CONFIG_STORE_ERR_NO_VALID_SLOT;
static config_store_err_t g_save_ret  = CONFIG_STORE_ERR_OK;
static uint32_t           g_save_call_count;
static config_blob_t      g_saved_blob;

static config_store_err_t stub_init(const void *health)
{
    (void) health;
    return CONFIG_STORE_ERR_OK;
}

static config_store_err_t stub_load(void *data_out, uint32_t *len_out, uint32_t max_len)
{
    (void) max_len;
    if (g_load_ret == CONFIG_STORE_ERR_OK)
    {
        (void) memcpy(data_out, &g_saved_blob, sizeof(config_blob_t));
        *len_out = (uint32_t) sizeof(config_blob_t);
    }
    return g_load_ret;
}

static config_store_err_t stub_save(const void *data, uint32_t len)
{
    g_save_call_count++;
    if (len >= (uint32_t) sizeof(config_blob_t))
    {
        (void) memcpy(&g_saved_blob, data, sizeof(config_blob_t));
    }
    return g_save_ret;
}

static config_store_err_t stub_check_integrity(void)
{
    return CONFIG_STORE_ERR_OK;
}

static config_store_err_t stub_erase(void)
{
    return CONFIG_STORE_ERR_OK;
}

static const iconfig_store_t s_stub_store = {
    .init             = stub_init,
    .load             = stub_load,
    .save             = stub_save,
    .check_integrity  = stub_check_integrity,
    .erase            = stub_erase,
};

/* ========================================================================= */
/* Change callback stub                                                      */
/* ========================================================================= */

static uint32_t          g_cb_call_count;
static config_param_id_t g_cb_last_param;

static void stub_change_cb(config_param_id_t param_id)
{
    g_cb_call_count++;
    g_cb_last_param = param_id;
}

/* ========================================================================= */
/* setUp / tearDown                                                          */
/* ========================================================================= */

void setUp(void)
{
    mock_freertos_reset();
    g_mock_xSemaphoreCreateMutexStatic_return = (SemaphoreHandle_t) 0x1234U;
    g_mock_xSemaphoreTake_return              = pdTRUE;
    g_mock_xSemaphoreGive_return              = pdTRUE;

    config_service_reset_for_test();

    g_load_ret       = CONFIG_STORE_ERR_NO_VALID_SLOT;
    g_save_ret       = CONFIG_STORE_ERR_OK;
    g_save_call_count = 0U;
    (void) memset(&g_saved_blob, 0, sizeof(g_saved_blob));

    g_cb_call_count = 0U;
    g_cb_last_param = CONFIG_PARAM_COUNT;
}

void tearDown(void) {}

/* ========================================================================= */
/* TC-CSVC-001: init with no stored data — defaults applied, all params valid */
/* ========================================================================= */

void test_TC_CSVC_001_init_no_stored_data_applies_defaults(void)
{
    config_service_err_t err = config_service_init(&s_stub_store);
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, err);

    const config_params_t *p = config_service_get_params();
    TEST_ASSERT_NOT_NULL(p);

    /* Verify representative default values are within their valid ranges. */
    TEST_ASSERT_GREATER_OR_EQUAL(100U,  p->polling_interval_ms);
    TEST_ASSERT_LESS_OR_EQUAL(60000U,   p->polling_interval_ms);
    TEST_ASSERT_TRUE(p->filter_alpha > 0.0f);
    TEST_ASSERT_TRUE(p->filter_alpha < 1.0f);
    TEST_ASSERT_EQUAL_UINT8(1U,         p->modbus_slave_addr);
    TEST_ASSERT_TRUE(p->temp_range_min + 1.0f < p->temp_range_max);
}

/* ========================================================================= */
/* TC-CSVC-002: init with NULL store — returns ERR_NULL_ARG                 */
/* ========================================================================= */

void test_TC_CSVC_002_init_null_store_returns_null_arg_error(void)
{
    config_service_err_t err = config_service_init(NULL);
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NULL_ARG, err);
}

/* ========================================================================= */
/* TC-CSVC-003: apply_loaded with valid blob — params match stored values   */
/* ========================================================================= */

void test_TC_CSVC_003_apply_loaded_valid_blob_restores_params(void)
{
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, config_service_init(&s_stub_store));

    config_blob_t blob;
    blob.schema_version              = CONFIG_SCHEMA_VERSION;
    blob.params.polling_interval_ms  = 5000U;
    blob.params.filter_alpha         = 0.3f;
    blob.params.temp_range_min       = -40.0f;
    blob.params.temp_range_max       = 85.0f;
    blob.params.humidity_range_min   = 0.0f;
    blob.params.humidity_range_max   = 100.0f;
    blob.params.pressure_range_min   = 300.0f;
    blob.params.pressure_range_max   = 1100.0f;
    blob.params.temp_alarm_high      = 40.0f;
    blob.params.temp_alarm_low       = 0.0f;
    blob.params.temp_hysteresis      = 1.0f;
    blob.params.humidity_alarm_high  = 80.0f;
    blob.params.humidity_alarm_low   = 20.0f;
    blob.params.humidity_hysteresis  = 2.0f;
    blob.params.pressure_alarm_high  = 1050.0f;
    blob.params.pressure_alarm_low   = 950.0f;
    blob.params.pressure_hysteresis  = 5.0f;
    blob.params.modbus_slave_addr    = 5U;
    blob.params.modbus_poll_period_ms = 2000U;

    config_service_err_t err = config_service_apply_loaded(&blob, sizeof(blob));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, err);

    const config_params_t *p = config_service_get_params();
    TEST_ASSERT_EQUAL_UINT32(5000U,  p->polling_interval_ms);
    TEST_ASSERT_EQUAL_FLOAT(0.3f,    p->filter_alpha);
    TEST_ASSERT_EQUAL_UINT8(5U,      p->modbus_slave_addr);
    TEST_ASSERT_EQUAL_UINT32(2000U,  p->modbus_poll_period_ms);
}

/* ========================================================================= */
/* TC-CSVC-004: apply_loaded with wrong schema_version — defaults applied   */
/* ========================================================================= */

void test_TC_CSVC_004_apply_loaded_wrong_schema_version_uses_defaults(void)
{
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, config_service_init(&s_stub_store));

    config_blob_t blob;
    (void) memset(&blob, 0, sizeof(blob));
    blob.schema_version             = CONFIG_SCHEMA_VERSION + 1U;
    blob.params.polling_interval_ms = 9999U;  /* would violate range if applied */

    config_service_err_t err = config_service_apply_loaded(&blob, sizeof(blob));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, err);

    const config_params_t *p = config_service_get_params();
    /* Polling interval must be the default, not the bogus stored value. */
    TEST_ASSERT_NOT_EQUAL(9999U, p->polling_interval_ms);
    TEST_ASSERT_GREATER_OR_EQUAL(100U,  p->polling_interval_ms);
    TEST_ASSERT_LESS_OR_EQUAL(60000U,   p->polling_interval_ms);
}

/* ========================================================================= */
/* TC-CSVC-005: set_param valid — param updated, ConfigStore write called   */
/* ========================================================================= */

void test_TC_CSVC_005_set_param_valid_updates_param_and_persists(void)
{
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, config_service_init(&s_stub_store));

    uint32_t interval = 500U;
    config_service_err_t err = config_service_set_param(CONFIG_PARAM_POLL_INTERVAL,
                                                         &interval);
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, err);

    const config_params_t *p = config_service_get_params();
    TEST_ASSERT_EQUAL_UINT32(500U, p->polling_interval_ms);

    TEST_ASSERT_GREATER_THAN(0U, g_save_call_count);
}

/* ========================================================================= */
/* TC-CSVC-006: set_param invalid — ERR_INVALID returned, param unchanged  */
/* ========================================================================= */

void test_TC_CSVC_006_set_param_invalid_returns_error_param_unchanged(void)
{
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, config_service_init(&s_stub_store));

    const config_params_t *p = config_service_get_params();
    uint32_t original = p->polling_interval_ms;

    uint32_t bad_interval = 50U;   /* below minimum 100 */
    /* Note: due to intentional bug in set_param, after this call the mutex
     * is not released on hardware.  Unit tests are unaffected because the
     * FreeRTOS mock does not enforce mutex balance. */
    config_service_err_t err = config_service_set_param(CONFIG_PARAM_POLL_INTERVAL,
                                                         &bad_interval);
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, err);

    p = config_service_get_params();
    TEST_ASSERT_EQUAL_UINT32(original, p->polling_interval_ms);

    TEST_ASSERT_EQUAL_UINT32(0U, g_save_call_count);
}

/* ========================================================================= */
/* TC-CSVC-007: cross-param — alarm_high below alarm_low+hysteresis invalid */
/* ========================================================================= */

void test_TC_CSVC_007_cross_param_alarm_high_below_low_plus_hyst_invalid(void)
{
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, config_service_init(&s_stub_store));

    /* Default: temp_alarm_low=0, temp_hysteresis=1 → alarm_high must be > 1.
     * Set alarm_high to 0.5 which is below alarm_low (0) + hysteresis (1) = 1. */
    float bad_high = 0.5f;
    config_service_err_t err = config_service_set_param(CONFIG_PARAM_TEMP_ALARM_HIGH,
                                                         &bad_high);
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, err);

    const config_params_t *p = config_service_get_params();
    TEST_ASSERT_EQUAL_FLOAT(40.0f, p->temp_alarm_high);  /* default unchanged */
}

/* ========================================================================= */
/* TC-CSVC-008: snapshot + change + restore — original values restored,    */
/*              ConfigStore write called                                     */
/* ========================================================================= */

void test_TC_CSVC_008_snapshot_change_restore_reverts_and_persists(void)
{
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, config_service_init(&s_stub_store));

    /* Save snapshot of defaults. */
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, config_service_snapshot());

    const config_params_t *p = config_service_get_params();
    uint32_t original_interval = p->polling_interval_ms;

    /* Change a parameter. */
    uint32_t new_interval = 3000U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK,
                      config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &new_interval));
    TEST_ASSERT_EQUAL_UINT32(3000U, config_service_get_params()->polling_interval_ms);

    uint32_t save_count_before_restore = g_save_call_count;

    /* Restore. */
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, config_service_restore_snapshot());

    p = config_service_get_params();
    TEST_ASSERT_EQUAL_UINT32(original_interval, p->polling_interval_ms);

    TEST_ASSERT_GREATER_THAN(save_count_before_restore, g_save_call_count);
}

/* ========================================================================= */
/* TC-CSVC-009: validate_param does not modify state                        */
/* ========================================================================= */

void test_TC_CSVC_009_validate_param_does_not_modify_state(void)
{
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, config_service_init(&s_stub_store));

    const config_params_t *p   = config_service_get_params();
    uint32_t original_interval = p->polling_interval_ms;

    uint32_t valid_val = 2000U;
    config_service_err_t err = config_service_validate_param(CONFIG_PARAM_POLL_INTERVAL,
                                                              &valid_val);
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, err);

    /* State must be unchanged — no write to ConfigStore either. */
    TEST_ASSERT_EQUAL_UINT32(original_interval,
                             config_service_get_params()->polling_interval_ms);
    TEST_ASSERT_EQUAL_UINT32(0U, g_save_call_count);

    /* Validate invalid value — also must not modify state. */
    uint32_t bad_val = 50U;
    err = config_service_validate_param(CONFIG_PARAM_POLL_INTERVAL, &bad_val);
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_INVALID, err);
    TEST_ASSERT_EQUAL_UINT32(original_interval,
                             config_service_get_params()->polling_interval_ms);
}

/* ========================================================================= */
/* TC-CSVC-010: change callback fires on success, not on validation failure */
/* ========================================================================= */

void test_TC_CSVC_010_callback_fires_on_success_not_on_failure(void)
{
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, config_service_init(&s_stub_store));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK,
                      config_service_register_change_callback(stub_change_cb));

    /* Trigger a validation failure — callback must NOT fire. */
    uint32_t bad_val = 99999U;
    (void) config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &bad_val);
    TEST_ASSERT_EQUAL_UINT32(0U, g_cb_call_count);

    /* setUp restores state between test functions; within this single function
     * the mock mutex state from the failed set_param is harmless because the
     * FreeRTOS mock does not track mutex ownership. */

    /* Trigger a valid change — callback MUST fire. */
    config_service_reset_for_test();
    g_save_call_count = 0U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, config_service_init(&s_stub_store));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK,
                      config_service_register_change_callback(stub_change_cb));

    uint32_t good_val = 2000U;
    config_service_err_t err = config_service_set_param(CONFIG_PARAM_POLL_INTERVAL,
                                                         &good_val);
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT32(1U, g_cb_call_count);
    TEST_ASSERT_EQUAL(CONFIG_PARAM_POLL_INTERVAL, g_cb_last_param);
}

/* ========================================================================= */
/* TC-CSVC-011: ConfigStore write failure — ERR_PERSIST returned,          */
/*              in-memory param already updated                              */
/* ========================================================================= */

void test_TC_CSVC_011_configstore_fail_returns_persist_error_param_updated(void)
{
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_OK, config_service_init(&s_stub_store));

    g_save_ret = CONFIG_STORE_ERR_FLASH_WRITE;

    uint32_t new_val = 4000U;
    config_service_err_t err = config_service_set_param(CONFIG_PARAM_POLL_INTERVAL,
                                                         &new_val);
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_PERSIST, err);

    /* In-memory value must already reflect the new value. */
    TEST_ASSERT_EQUAL_UINT32(4000U, config_service_get_params()->polling_interval_ms);
}

/* ========================================================================= */
/* TC-CSVC-012: functions before init — ERR_NOT_INIT returned              */
/* ========================================================================= */

void test_TC_CSVC_012_functions_before_init_return_not_init(void)
{
    /* config_service_reset_for_test() was called in setUp — s_cs.initialised is false. */

    TEST_ASSERT_NULL(config_service_get_params());

    uint32_t val = 1000U;
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NOT_INIT,
                      config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &val));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NOT_INIT,
                      config_service_validate_param(CONFIG_PARAM_POLL_INTERVAL, &val));
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NOT_INIT, config_service_snapshot());
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NOT_INIT, config_service_restore_snapshot());
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NOT_INIT, config_service_flush());

    const config_blob_t blob = { .schema_version = CONFIG_SCHEMA_VERSION };
    TEST_ASSERT_EQUAL(CONFIG_SERVICE_ERR_NOT_INIT,
                      config_service_apply_loaded(&blob, sizeof(blob)));
}
