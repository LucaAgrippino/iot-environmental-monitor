/**
 * @file test_lifecycle_wiring.c
 * @brief Unity tests for the LifecycleController wiring (Gateway).
 *
 * Every real module the adapters call is replaced by a spy defined in this file. The real
 * LifecycleController is linked (it is included below), so the last group of tests boots the
 * actual state machine through the adapters. FreeRTOS comes from freertos_mock.
 * Test IDs are TC-LW-xxx.
 */

#include <string.h>

#include "device_profile_registry_stub.h"
#include "freertos_mock.h"
#include "lifecycle_controller.h"
#include "lifecycle_wiring.h"
#include "lifecycle_wiring_internal.h"
#include "stm32l475_cmsis_mock.h"
#include "unity.h"

#define HEALTH ((health_monitor_handle_t) 0x1001)
#define STORE ((config_store_handle_t) 0x1002)
#define CLOUD ((cloud_publisher_handle_t) 0x1003)
#define NTP ((ntp_client_handle_t) 0x1004)
#define SENSOR_TASK ((TaskHandle_t) 0x1005)
#define POLL_PERIOD_MS (750U)

/* ===================================================================== */
/* Spies for the real modules                                            */
/* ===================================================================== */

static config_store_err_t s_cs_check_result;
static config_store_err_t s_cs_load_result;
static uint32_t s_cs_load_calls;
static config_store_handle_t s_cs_handle_seen;
static uint32_t s_cs_max_seen;

config_store_err_t config_store_check_integrity(config_store_handle_t self)
{
    s_cs_handle_seen = self;
    return s_cs_check_result;
}

config_store_err_t config_store_load(config_store_handle_t self, void *data_out, uint32_t *len_out,
                                     uint32_t max_len)
{
    s_cs_load_calls++;
    s_cs_handle_seen = self;
    s_cs_max_seen = max_len;
    (void) memset(data_out, 0, 16U);
    *len_out = 16U;
    return s_cs_load_result;
}

static config_service_err_t s_cfg_apply_result;
static uint32_t s_cfg_apply_calls;
static uint32_t s_cfg_apply_len_seen;
static config_service_err_t s_cfg_snapshot_result;
static uint32_t s_cfg_snapshot_calls;
static config_service_err_t s_cfg_restore_result;
static uint32_t s_cfg_restore_calls;
static config_service_err_t s_cfg_flush_result;
static uint32_t s_cfg_flush_calls;
static config_service_err_t s_cfg_persist_result;
static uint32_t s_cfg_persist_calls;
static config_params_t s_params;
static bool s_params_null;

config_service_err_t config_service_apply_loaded(const void *blob, uint32_t len)
{
    (void) blob;
    s_cfg_apply_calls++;
    s_cfg_apply_len_seen = len;
    return s_cfg_apply_result;
}

config_service_err_t config_service_snapshot(void)
{
    s_cfg_snapshot_calls++;
    return s_cfg_snapshot_result;
}

config_service_err_t config_service_restore_snapshot(void)
{
    s_cfg_restore_calls++;
    return s_cfg_restore_result;
}

config_service_err_t config_service_flush(void)
{
    s_cfg_flush_calls++;
    return s_cfg_flush_result;
}

config_service_err_t config_service_persist_pending(void)
{
    s_cfg_persist_calls++;
    return s_cfg_persist_result;
}

bool config_service_get_profiles(device_profile_t *out, uint8_t max, uint8_t *count_out)
{
    (void) out;
    (void) max;
    (void) count_out;
    return false;
}

bool config_service_set_profiles(const device_profile_t *in, uint8_t count)
{
    (void) in;
    (void) count;
    return false;
}

const config_params_t *config_service_get_params(void)
{
    return s_params_null ? NULL : &s_params;
}

static TaskHandle_t s_sensor_task;
static sensor_service_err_t s_reconf_result;
static uint32_t s_reconf_calls;
static const config_params_t *s_reconf_params_seen;
static bool s_sensor_ready;

TaskHandle_t sensor_task_get_handle(void)
{
    return s_sensor_task;
}

sensor_service_err_t sensor_service_reconfigure(const config_params_t *params)
{
    s_reconf_calls++;
    s_reconf_params_seen = params;
    return s_reconf_result;
}

bool sensor_service_is_ready(void)
{
    return s_sensor_ready;
}

static alarm_service_err_t s_alarm_state_result;

static alarm_service_err_t spy_alarm_get_state(sensor_id_t sensor, alarm_state_t *state_out)
{
    (void) sensor;
    *state_out = ALARM_STATE_CLEAR;
    return s_alarm_state_result;
}

static alarm_service_err_t s_alarm_reconfigure_result;
static uint32_t s_alarm_reconfigure_calls;

alarm_service_err_t alarm_service_reconfigure(const config_params_t *params)
{
    (void) params;
    s_alarm_reconfigure_calls++;
    return s_alarm_reconfigure_result;
}

static const ialarm_service_t s_alarm_vtable = {.get_state = spy_alarm_get_state};
const ialarm_service_t *const alarm_service = &s_alarm_vtable;

static bool s_cloud_ready;
static cloud_publisher_err_t s_cloud_flush_result;
static uint32_t s_cloud_flush_calls;
static cloud_publisher_handle_t s_cloud_handle_seen;

bool cloud_publisher_is_ready(cloud_publisher_handle_t handle)
{
    s_cloud_handle_seen = handle;
    return s_cloud_ready;
}

cloud_publisher_err_t cloud_publisher_flush(cloud_publisher_handle_t handle)
{
    s_cloud_handle_seen = handle;
    s_cloud_flush_calls++;
    return s_cloud_flush_result;
}

static modbus_poller_err_t s_poller_init_result;
static uint32_t s_poller_init_calls;
static uint32_t s_poller_period_seen;
static health_monitor_handle_t s_poller_health_seen;
static bool s_poller_ready;
static uint32_t s_poller_run_calls;

static device_profile_registry_err_t s_registry_init_result;
static device_profile_registry_err_t s_registry_load_result;
static uint32_t s_registry_init_calls;

device_profile_registry_err_t device_profile_registry_init(const device_profile_storage_t *storage)
{
    (void) storage;
    s_registry_init_calls++;
    return s_registry_init_result;
}

device_profile_registry_err_t device_profile_registry_load(void)
{
    return s_registry_load_result;
}

modbus_poller_err_t modbus_poller_init(uint32_t poll_period_ms, health_monitor_handle_t health)
{
    s_poller_init_calls++;
    s_poller_period_seen = poll_period_ms;
    s_poller_health_seen = health;
    return s_poller_init_result;
}

bool modbus_poller_is_ready(void)
{
    return s_poller_ready;
}

void modbus_poller_run(void)
{
    s_poller_run_calls++;
}

static time_service_err_t s_time_result;
static ntp_client_handle_t s_time_ntp_seen;

time_service_err_t time_service_init(ntp_client_handle_t ntp)
{
    s_time_ntp_seen = ntp;
    return s_time_result;
}

static uint32_t s_reset_calls;

void reset_trigger(void)
{
    s_reset_calls++;
}

static health_monitor_err_t s_hm_result;
static health_event_t s_hm_event_seen;
static uint32_t s_hm_param_seen;
static lifecycle_state_t s_hm_state_seen;
static uint32_t s_hm_reset_calls;
static uint32_t s_hm_led_calls;
static health_monitor_handle_t s_hm_handle_seen;

health_monitor_err_t health_monitor_push_event(health_monitor_handle_t handle, health_event_t event,
                                               uint32_t param)
{
    s_hm_handle_seen = handle;
    s_hm_event_seen = event;
    s_hm_param_seen = param;
    return s_hm_result;
}

health_monitor_err_t health_monitor_update_lifecycle(health_monitor_handle_t handle,
                                                     lifecycle_state_t state)
{
    s_hm_handle_seen = handle;
    s_hm_state_seen = state;
    return s_hm_result;
}

health_monitor_err_t health_monitor_reset_metrics(health_monitor_handle_t handle)
{
    s_hm_handle_seen = handle;
    s_hm_reset_calls++;
    return s_hm_result;
}

health_monitor_err_t health_monitor_set_led_fault(health_monitor_handle_t handle)
{
    s_hm_handle_seen = handle;
    s_hm_led_calls++;
    return s_hm_result;
}

/* ===================================================================== */
/* Fixture                                                               */
/* ===================================================================== */

static lifecycle_wiring_config_t s_config;
static lifecycle_deps_t s_deps;

/* ===================================================================== */
/* OTA spies: the UpdateService and FirmwareStore vtables                */
/* ===================================================================== */

#define OTA_FS_HANDLE ((firmware_store_t *) 0x3000)
#define OTA_US_HANDLE ((update_service_t *) 0x3100)

static bool s_ota_created;
static update_service_err_t s_ota_start_result;
static update_service_err_t s_ota_selfcheck_result;
static update_service_err_t s_ota_rollback_result;
static update_service_err_t s_ota_after_rollback_result;
static uint32_t s_ota_start_calls;
static uint32_t s_ota_job_seen;
static uint32_t s_ota_selfcheck_calls;
static uint32_t s_ota_rollback_calls;
static uint32_t s_ota_after_rollback_calls;
static const update_service_t *s_ota_us_seen;

static firmware_store_err_t s_fw_info_result;
static firmware_boot_info_t s_fw_info;
static firmware_store_err_t s_fw_confirm_result;
static firmware_store_err_t s_fw_rollback_result;
static uint32_t s_fw_confirm_calls;
static uint32_t s_fw_rollback_calls;
static const firmware_store_t *s_fw_seen;

static bool ota_is_created(const update_service_t *self)
{
    s_ota_us_seen = self;
    return s_ota_created;
}

static update_service_err_t ota_start(update_service_t *self, uint32_t job)
{
    s_ota_us_seen = self;
    s_ota_start_calls++;
    s_ota_job_seen = job;
    return s_ota_start_result;
}

static update_service_err_t ota_selfcheck(update_service_t *self)
{
    s_ota_us_seen = self;
    s_ota_selfcheck_calls++;
    return s_ota_selfcheck_result;
}

static update_service_err_t ota_rollback(update_service_t *self)
{
    s_ota_us_seen = self;
    s_ota_rollback_calls++;
    return s_ota_rollback_result;
}

static update_service_err_t ota_after_rollback(update_service_t *self)
{
    s_ota_us_seen = self;
    s_ota_after_rollback_calls++;
    return s_ota_after_rollback_result;
}

static const iupdate_control_t k_spy_update = {.is_created = ota_is_created,
                                               .start = ota_start,
                                               .resume_self_checking = ota_selfcheck,
                                               .resume_rollback = ota_rollback,
                                               .resume_after_rollback = ota_after_rollback};

static firmware_store_err_t fw_get_info(const firmware_store_t *self, firmware_boot_info_t *out)
{
    s_fw_seen = self;
    *out = s_fw_info;
    return s_fw_info_result;
}

static firmware_store_err_t fw_confirm(firmware_store_t *self)
{
    s_fw_seen = self;
    s_fw_confirm_calls++;
    return s_fw_confirm_result;
}

static firmware_store_err_t fw_rollback(firmware_store_t *self)
{
    s_fw_seen = self;
    s_fw_rollback_calls++;
    return s_fw_rollback_result;
}

static const ifirmware_boot_control_t k_spy_firmware = {
    .get_boot_info = fw_get_info, .confirm = fw_confirm, .rollback = fw_rollback};

static void ota_spies_reset(void)
{
    s_ota_created = true;
    s_ota_start_result = UPDATE_SERVICE_ERR_OK;
    s_ota_selfcheck_result = UPDATE_SERVICE_ERR_OK;
    s_ota_rollback_result = UPDATE_SERVICE_ERR_OK;
    s_ota_after_rollback_result = UPDATE_SERVICE_ERR_OK;
    s_ota_start_calls = 0U;
    s_ota_job_seen = 0U;
    s_ota_selfcheck_calls = 0U;
    s_ota_rollback_calls = 0U;
    s_ota_after_rollback_calls = 0U;
    s_ota_us_seen = NULL;

    s_fw_info_result = FIRMWARE_STORE_ERR_OK;
    s_fw_info = (firmware_boot_info_t){
        .ota_capable = true, .running_slot = FIRMWARE_SLOT_A, .state = FIRMWARE_BOOT_CONFIRMED};
    s_fw_confirm_result = FIRMWARE_STORE_ERR_OK;
    s_fw_rollback_result = FIRMWARE_STORE_ERR_OK;
    s_fw_confirm_calls = 0U;
    s_fw_rollback_calls = 0U;
    s_fw_seen = NULL;
}

/** @brief The configuration with the OTA members set. */
static void use_ota(void)
{
    s_config.update_ops = &k_spy_update;
    s_config.update = OTA_US_HANDLE;
    s_config.firmware_ops = &k_spy_firmware;
    s_config.firmware = OTA_FS_HANDLE;
}

void setUp(void)
{
    stm32l475_cmsis_mock_reset();
    mock_freertos_reset();
    g_mock_xQueueReceive_available = 0U;
    g_mock_xQueueSend_last_item_size = sizeof(lifecycle_event_t);
    lifecycle_controller_reset_for_test();
    lifecycle_wiring_reset_for_test();

    s_cs_check_result = CONFIG_STORE_ERR_OK;
    s_cs_load_result = CONFIG_STORE_ERR_OK;
    s_cs_load_calls = 0U;
    s_cs_handle_seen = NULL;
    s_cs_max_seen = 0U;
    s_cfg_apply_result = CONFIG_SERVICE_OK;
    s_cfg_apply_calls = 0U;
    s_cfg_apply_len_seen = 0U;
    s_cfg_snapshot_result = CONFIG_SERVICE_OK;
    s_cfg_snapshot_calls = 0U;
    s_cfg_restore_result = CONFIG_SERVICE_OK;
    s_cfg_restore_calls = 0U;
    s_cfg_flush_result = CONFIG_SERVICE_OK;
    s_cfg_flush_calls = 0U;
    s_cfg_persist_result = CONFIG_SERVICE_OK;
    s_cfg_persist_calls = 0U;
    (void) memset(&s_params, 0, sizeof(s_params));
    s_params.modbus_poll_period_ms = POLL_PERIOD_MS;
    s_params_null = false;
    s_sensor_task = SENSOR_TASK;
    s_reconf_result = SENSOR_SERVICE_ERR_OK;
    s_reconf_calls = 0U;
    s_reconf_params_seen = NULL;
    s_sensor_ready = true;
    s_alarm_state_result = ALARM_SERVICE_ERR_OK;
    s_alarm_reconfigure_result = ALARM_SERVICE_ERR_OK;
    s_alarm_reconfigure_calls = 0U;
    s_cloud_ready = true;
    s_cloud_flush_result = CP_ERR_OK;
    s_cloud_flush_calls = 0U;
    ota_spies_reset();
    s_cloud_handle_seen = NULL;
    s_poller_init_result = MODBUS_POLLER_ERR_OK;
    s_poller_init_calls = 0U;
    s_registry_init_result = DEVICE_PROFILE_REGISTRY_OK;
    s_registry_load_result = DEVICE_PROFILE_REGISTRY_OK;
    s_registry_init_calls = 0U;
    s_poller_period_seen = 0U;
    s_poller_health_seen = NULL;
    s_poller_ready = true;
    s_poller_run_calls = 0U;
    s_time_result = TIME_SERVICE_OK;
    s_time_ntp_seen = NULL;
    s_reset_calls = 0U;
    s_hm_result = HEALTH_MONITOR_ERR_OK;
    s_hm_event_seen = HEALTH_EVENT_COUNT;
    s_hm_param_seen = 0U;
    s_hm_state_seen = LIFECYCLE_STATE_INIT;
    s_hm_reset_calls = 0U;
    s_hm_led_calls = 0U;
    s_hm_handle_seen = NULL;

    s_config = (lifecycle_wiring_config_t){
        .health = HEALTH, .config_store = STORE, .cloud = CLOUD, .ntp = NTP};
    (void) memset(&s_deps, 0, sizeof(s_deps));
}

void tearDown(void)
{
}

static void bind_ok(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK, lifecycle_wiring_bind(&s_config, &s_deps));
}

/* ===================================================================== */
/* bind                                                                  */
/* ===================================================================== */

void test_LW_001_bind_rejects_null_arguments(void)
{
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG, lifecycle_wiring_bind(NULL, &s_deps));
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG, lifecycle_wiring_bind(&s_config, NULL));
}

void test_LW_002_bind_rejects_each_missing_handle(void)
{
    lifecycle_wiring_config_t c = s_config;

    c.health = NULL;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG, lifecycle_wiring_bind(&c, &s_deps));
    c = s_config;
    c.config_store = NULL;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG, lifecycle_wiring_bind(&c, &s_deps));
    c = s_config;
    c.cloud = NULL;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG, lifecycle_wiring_bind(&c, &s_deps));
    c = s_config;
    c.ntp = NULL;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG, lifecycle_wiring_bind(&c, &s_deps));
}

void test_LW_003_bind_fills_every_member_and_leaves_the_watchdog_empty(void)
{
    bind_ok();
    TEST_ASSERT_NOT_NULL(s_deps.config_store);
    TEST_ASSERT_NOT_NULL(s_deps.config_manager);
    TEST_ASSERT_NOT_NULL(s_deps.sensors);
    TEST_ASSERT_NOT_NULL(s_deps.alarms);
    TEST_ASSERT_NOT_NULL(s_deps.console);
    TEST_ASSERT_NOT_NULL(s_deps.health_report);
    TEST_ASSERT_NOT_NULL(s_deps.health_admin);
    TEST_ASSERT_NOT_NULL(s_deps.cloud);
    TEST_ASSERT_NOT_NULL(s_deps.modbus_poller);
    TEST_ASSERT_NOT_NULL(s_deps.update_service);
    TEST_ASSERT_NOT_NULL(s_deps.time_service);
    TEST_ASSERT_NOT_NULL(s_deps.firmware_store);
    TEST_ASSERT_NOT_NULL(s_deps.reset_driver);
    TEST_ASSERT_NULL(s_deps.watchdog);
}

/* ===================================================================== */
/* IConfigStore                                                          */
/* ===================================================================== */

void test_LW_010_integrity_passes_through_and_the_store_handle_is_used(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.config_store->check_integrity(NULL));
    TEST_ASSERT_EQUAL_PTR(STORE, s_cs_handle_seen);
}

void test_LW_011_no_valid_slot_is_a_first_boot_by_default(void)
{
    s_cs_check_result = CONFIG_STORE_ERR_NO_VALID_SLOT;
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.config_store->check_integrity(NULL));
}

void test_LW_012_no_valid_slot_is_fatal_when_asked(void)
{
    s_cs_check_result = CONFIG_STORE_ERR_NO_VALID_SLOT;
    s_config.fault_on_no_config = true;
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.config_store->check_integrity(NULL));
}

void test_LW_013_a_flash_read_error_is_always_fatal(void)
{
    s_cs_check_result = CONFIG_STORE_ERR_FLASH_READ;
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.config_store->check_integrity(NULL));
}

void test_LW_014_load_passes_the_buffer_size_and_returns_the_length(void)
{
    uint8_t buf[32];
    uint32_t len = 0U;

    bind_ok();
    (void) s_deps.config_store->check_integrity(NULL);
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.config_store->load(NULL, buf, &len, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT32(1U, s_cs_load_calls);
    TEST_ASSERT_EQUAL_UINT32(sizeof(buf), s_cs_max_seen);
    TEST_ASSERT_EQUAL_UINT32(16U, len);
}

void test_LW_015_load_failure_is_reported(void)
{
    uint8_t buf[32];
    uint32_t len = 0U;

    s_cs_load_result = CONFIG_STORE_ERR_TOO_LARGE;
    bind_ok();
    (void) s_deps.config_store->check_integrity(NULL);
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.config_store->load(NULL, buf, &len, sizeof(buf)));
}

void test_LW_016_on_a_first_boot_load_returns_an_empty_blob_without_touching_flash(void)
{
    uint8_t buf[32];
    uint32_t len = 99U;

    s_cs_check_result = CONFIG_STORE_ERR_NO_VALID_SLOT;
    bind_ok();
    (void) s_deps.config_store->check_integrity(NULL);
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.config_store->load(NULL, buf, &len, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT32(0U, len);
    TEST_ASSERT_EQUAL_UINT32(0U, s_cs_load_calls);
}

/* ===================================================================== */
/* IConfigManager                                                        */
/* ===================================================================== */

void test_LW_020_applying_an_empty_blob_keeps_the_defaults_without_calling_the_service(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.config_manager->apply_loaded(NULL, NULL, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, s_cfg_apply_calls);
}

void test_LW_021_apply_loaded_passes_the_blob_length(void)
{
    uint8_t blob[16] = {0};

    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.config_manager->apply_loaded(NULL, blob, sizeof(blob)));
    TEST_ASSERT_EQUAL_UINT32(sizeof(blob), s_cfg_apply_len_seen);
}

void test_LW_022_a_rejected_blob_is_not_fatal_because_the_defaults_apply(void)
{
    uint8_t blob[16] = {0};

    s_cfg_apply_result = CONFIG_SERVICE_ERR_INVALID;
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.config_manager->apply_loaded(NULL, blob, sizeof(blob)));
}

void test_LW_023_apply_loaded_before_the_service_is_initialised_is_fatal(void)
{
    uint8_t blob[16] = {0};

    s_cfg_apply_result = CONFIG_SERVICE_ERR_NOT_INIT;
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.config_manager->apply_loaded(NULL, blob, sizeof(blob)));
}

void test_LW_024_snapshot_is_kept_by_the_service_so_no_bytes_cross(void)
{
    uint8_t buf[8];
    uint32_t len = 99U;

    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.config_manager->snapshot(NULL, buf, &len, sizeof(buf)));
    TEST_ASSERT_EQUAL_UINT32(1U, s_cfg_snapshot_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, len);
}

void test_LW_025_snapshot_failure_is_reported(void)
{
    uint8_t buf[8];
    uint32_t len = 0U;

    s_cfg_snapshot_result = CONFIG_SERVICE_ERR_NOT_INIT;
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.config_manager->snapshot(NULL, buf, &len, sizeof(buf)));
}

void test_LW_026_restore_calls_the_service_and_reports_a_persist_failure(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.config_manager->restore_snapshot(NULL, NULL, 0U));
    TEST_ASSERT_EQUAL_UINT32(1U, s_cfg_restore_calls);

    s_cfg_restore_result = CONFIG_SERVICE_ERR_PERSIST;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.config_manager->restore_snapshot(NULL, NULL, 0U));
}

void test_LW_027_commit_is_a_flush(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.config_manager->commit(NULL));
    TEST_ASSERT_EQUAL_UINT32(1U, s_cfg_flush_calls);

    s_cfg_flush_result = CONFIG_SERVICE_ERR_PERSIST;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.config_manager->commit(NULL));
}

void test_LW_027b_persist_pending_reaches_ConfigService_and_is_not_a_flush(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.config_manager->persist_pending(NULL));
    TEST_ASSERT_EQUAL_UINT32(1U, s_cfg_persist_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_cfg_flush_calls);

    s_cfg_persist_result = CONFIG_SERVICE_ERR_PERSIST;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.config_manager->persist_pending(NULL));
}

/* ===================================================================== */
/* Sensors, alarms, console                                              */
/* ===================================================================== */

void test_LW_030_sensors_init_applies_the_loaded_configuration(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.sensors->init(NULL));
    TEST_ASSERT_EQUAL_UINT32(1U, s_reconf_calls);
    TEST_ASSERT_EQUAL_PTR(&s_params, s_reconf_params_seen);
}

void test_LW_031_sensors_init_fails_when_sensor_task_was_never_created(void)
{
    s_sensor_task = NULL;
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.sensors->init(NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, s_reconf_calls);
}

void test_LW_032_a_rejected_sensor_configuration_fails_init(void)
{
    s_reconf_result = SENSOR_SERVICE_ERR_INVALID_ARG;
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.sensors->init(NULL));
}

void test_LW_033_sensors_reconfigure_passes_the_current_parameters(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.sensors->reconfigure(NULL));
    TEST_ASSERT_EQUAL_PTR(&s_params, s_reconf_params_seen);

    s_reconf_result = SENSOR_SERVICE_ERR_NOT_INIT;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.sensors->reconfigure(NULL));
}

void test_LW_034_sensors_is_ready_passes_through(void)
{
    bind_ok();
    TEST_ASSERT_TRUE(s_deps.sensors->is_ready(NULL));
    s_sensor_ready = false;
    TEST_ASSERT_FALSE(s_deps.sensors->is_ready(NULL));
}

void test_LW_035_alarms_init_verifies_the_service_is_up(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.alarms->init(NULL));

    s_alarm_state_result = ALARM_SERVICE_ERR_NOT_INIT;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.alarms->init(NULL));

    s_alarm_state_result = ALARM_SERVICE_ERR_OK;
    s_alarm_reconfigure_calls = 0U;
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.alarms->init(NULL));
    TEST_ASSERT_EQUAL_UINT32(1U, s_alarm_reconfigure_calls); /* thresholds from the config */
    s_alarm_reconfigure_result = ALARM_SERVICE_ERR_INVALID_ARG;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.alarms->init(NULL));
}

void test_LW_036_console_has_nothing_left_to_finish(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.console->init_finalise(NULL));
}

/* ===================================================================== */
/* Cloud, poller, time                                                   */
/* ===================================================================== */

void test_LW_040_cloud_init_verifies_the_instance_and_is_ready_passes_through(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.cloud->init(NULL));
    TEST_ASSERT_TRUE(s_deps.cloud->is_ready(NULL));
    TEST_ASSERT_EQUAL_PTR(CLOUD, s_cloud_handle_seen);

    s_cloud_ready = false;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.cloud->init(NULL));
    TEST_ASSERT_FALSE(s_deps.cloud->is_ready(NULL));
}

void test_LW_041_cloud_flush_passes_through(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.cloud->flush(NULL));
    TEST_ASSERT_EQUAL_UINT32(1U, s_cloud_flush_calls);

    s_cloud_flush_result = CP_ERR_NO_RESOURCE;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.cloud->flush(NULL));
}

void test_LW_042_poller_init_uses_the_configured_period_and_the_health_handle(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.modbus_poller->init(NULL));
    TEST_ASSERT_EQUAL_UINT32(POLL_PERIOD_MS, s_poller_period_seen);
    TEST_ASSERT_EQUAL_PTR(HEALTH, s_poller_health_seen);
}

void test_LW_042b_the_profile_registry_is_initialised_and_loaded_before_the_poller(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.modbus_poller->init(NULL));
    TEST_ASSERT_EQUAL_UINT32(1U, s_registry_init_calls);
}

void test_LW_042c_a_registry_failure_stops_the_poller_from_starting(void)
{
    bind_ok();
    s_registry_init_result = DEVICE_PROFILE_REGISTRY_ERR_NO_RESOURCE;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.modbus_poller->init(NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, s_poller_init_calls);

    s_registry_init_result = DEVICE_PROFILE_REGISTRY_OK;
    s_registry_load_result = DEVICE_PROFILE_REGISTRY_ERR_NOT_INIT;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.modbus_poller->init(NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, s_poller_init_calls);
}

void test_LW_043_poller_init_failures_are_reported(void)
{
    bind_ok();
    s_poller_init_result = MODBUS_POLLER_ERR_INVALID_ARG;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.modbus_poller->init(NULL));

    s_params_null = true;
    s_poller_init_calls = 0U;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.modbus_poller->init(NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, s_poller_init_calls);
}

void test_LW_044_poller_is_ready_passes_through(void)
{
    bind_ok();
    TEST_ASSERT_TRUE(s_deps.modbus_poller->is_ready(NULL));
    s_poller_ready = false;
    TEST_ASSERT_FALSE(s_deps.modbus_poller->is_ready(NULL));
}

void test_LW_045_time_service_init_gets_the_ntp_handle(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.time_service->init(NULL));
    TEST_ASSERT_EQUAL_PTR(NTP, s_time_ntp_seen);

    s_time_result = TIME_SERVICE_ERR_NULL_ARG;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.time_service->init(NULL));
}

/* ===================================================================== */
/* Stand-ins, reset, health                                              */
/* ===================================================================== */

void test_LW_050_without_an_update_service_nothing_is_pending_and_an_update_is_refused(void)
{
    bool self_check = true;
    bool rollback = true;

    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.update_service->init(NULL));
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.update_service->start(NULL, 4096U));
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.update_service->resume_self_checking(NULL));
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.update_service->resume_rollback(NULL));
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.update_service->resume_after_rollback(NULL));

    TEST_ASSERT_EQUAL(LC_DEP_OK,
                      s_deps.firmware_store->get_pending_flags(NULL, &self_check, &rollback));
    TEST_ASSERT_FALSE(self_check);
    TEST_ASSERT_FALSE(rollback);
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.firmware_store->confirm_self_check(NULL));
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.firmware_store->rollback(NULL));
}

void test_LW_051_soft_reset_calls_the_reset_driver(void)
{
    bind_ok();
    s_deps.reset_driver->soft_reset(NULL);
    TEST_ASSERT_EQUAL_UINT32(1U, s_reset_calls);
}

void test_LW_052_push_fault_raises_a_fault_event_with_the_code(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.health_report->push_fault(NULL, 0xABU));
    TEST_ASSERT_EQUAL(HEALTH_EVENT_FAULT, s_hm_event_seen);
    TEST_ASSERT_EQUAL_UINT32(0xABU, s_hm_param_seen);
    TEST_ASSERT_EQUAL_PTR(HEALTH, s_hm_handle_seen);
}

void test_LW_053_update_lifecycle_passes_the_state(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK,
                      s_deps.health_report->update_lifecycle(NULL, LIFECYCLE_STATE_EDITING_CONFIG));
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_EDITING_CONFIG, s_hm_state_seen);
}

void test_LW_054_health_errors_become_failures(void)
{
    bind_ok();
    s_hm_result = HEALTH_MONITOR_ERR_NOT_INIT;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.health_report->push_fault(NULL, 1U));
    TEST_ASSERT_EQUAL(LC_DEP_FAIL,
                      s_deps.health_report->update_lifecycle(NULL, LIFECYCLE_STATE_OPERATIONAL));
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.health_admin->reset_metrics(NULL));
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.health_admin->set_led_fault(NULL));
}

void test_LW_055_health_admin_calls_reach_the_monitor_and_init_is_a_no_op(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.health_report->init(NULL));
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.health_admin->reset_metrics(NULL));
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.health_admin->set_led_fault(NULL));
    TEST_ASSERT_EQUAL_UINT32(1U, s_hm_reset_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_hm_led_calls);
}

void test_LW_056_the_poller_task_runs_the_poller_once_the_gate_is_open(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK,
                      lifecycle_controller_init(LIFECYCLE_RESET_POWER_ON, &s_deps));
    lifecycle_wiring_poller_task(NULL);
    TEST_ASSERT_EQUAL_UINT32(1U, s_poller_run_calls);
}

/* ===================================================================== */
/* The real OTA adapters (LCC-4)                                         */
/* ===================================================================== */

void test_LW_057_bind_requires_the_four_ota_members_together(void)
{
    lifecycle_wiring_config_t c;

    use_ota();
    c = s_config;
    c.update_ops = NULL;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG, lifecycle_wiring_bind(&c, &s_deps));
    c = s_config;
    c.update = NULL;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG, lifecycle_wiring_bind(&c, &s_deps));
    c = s_config;
    c.firmware_ops = NULL;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG, lifecycle_wiring_bind(&c, &s_deps));
    c = s_config;
    c.firmware = NULL;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG, lifecycle_wiring_bind(&c, &s_deps));

    /* All four together, or none, is fine. */
    bind_ok();
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK,
                      lifecycle_wiring_bind(
                          &(lifecycle_wiring_config_t){
                              .health = HEALTH, .config_store = STORE, .cloud = CLOUD, .ntp = NTP},
                          &s_deps));
}

void test_LW_057b_bind_rejects_an_ops_table_with_a_missing_function(void)
{
    iupdate_control_t update = k_spy_update;
    ifirmware_boot_control_t firmware = k_spy_firmware;

    use_ota();
    update.start = NULL;
    s_config.update_ops = &update;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG, lifecycle_wiring_bind(&s_config, &s_deps));

    use_ota();
    firmware.confirm = NULL;
    s_config.firmware_ops = &firmware;
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_NULL_ARG, lifecycle_wiring_bind(&s_config, &s_deps));
}

void test_LW_058_the_update_adapter_passes_the_job_and_maps_the_results(void)
{
    use_ota();
    bind_ok();

    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.update_service->init(NULL));
    TEST_ASSERT_EQUAL_PTR(OTA_US_HANDLE, s_ota_us_seen);
    s_ota_created = false;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.update_service->init(NULL));

    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.update_service->start(NULL, 4242U));
    TEST_ASSERT_EQUAL_UINT32(4242U, s_ota_job_seen);
    s_ota_start_result = UPDATE_SERVICE_ERR_BAD_STATE;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.update_service->start(NULL, 1U));

    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.update_service->resume_self_checking(NULL));
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.update_service->resume_rollback(NULL));
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.update_service->resume_after_rollback(NULL));
    TEST_ASSERT_EQUAL_UINT32(1U, s_ota_selfcheck_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_ota_rollback_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_ota_after_rollback_calls);

    s_ota_selfcheck_result = UPDATE_SERVICE_ERR_BAD_STATE;
    s_ota_rollback_result = UPDATE_SERVICE_ERR_BAD_STATE;
    s_ota_after_rollback_result = UPDATE_SERVICE_ERR_BAD_STATE;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.update_service->resume_self_checking(NULL));
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.update_service->resume_rollback(NULL));
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.update_service->resume_after_rollback(NULL));
}

void test_LW_059_the_pending_flags_come_from_the_boot_state(void)
{
    const struct
    {
        firmware_boot_state_t state;
        bool ota_capable;
        bool self_check;
        bool rollback;
    } cases[] = {
        {FIRMWARE_BOOT_CONFIRMED, true, false, false},
        {FIRMWARE_BOOT_TRIAL_PENDING, true, false, false},
        {FIRMWARE_BOOT_TRIAL_RUNNING, true, true, false},
        {FIRMWARE_BOOT_ROLLED_BACK, true, false, true},
        {FIRMWARE_BOOT_TRIAL_RUNNING, false, false, false},
        {FIRMWARE_BOOT_ROLLED_BACK, false, false, false},
    };

    use_ota();
    bind_ok();

    for (uint32_t i = 0U; i < (sizeof(cases) / sizeof(cases[0])); i++)
    {
        bool self_check = !cases[i].self_check;
        bool rollback = !cases[i].rollback;

        s_fw_info.state = cases[i].state;
        s_fw_info.ota_capable = cases[i].ota_capable;

        TEST_ASSERT_EQUAL(LC_DEP_OK,
                          s_deps.firmware_store->get_pending_flags(NULL, &self_check, &rollback));
        TEST_ASSERT_EQUAL(cases[i].self_check, self_check);
        TEST_ASSERT_EQUAL(cases[i].rollback, rollback);
    }

    TEST_ASSERT_EQUAL_PTR(OTA_FS_HANDLE, s_fw_seen);
}

void test_LW_059b_a_failing_get_boot_info_is_a_failure(void)
{
    bool self_check = true;
    bool rollback = true;

    use_ota();
    bind_ok();
    s_fw_info_result = FIRMWARE_STORE_ERR_FLASH_READ;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL,
                      s_deps.firmware_store->get_pending_flags(NULL, &self_check, &rollback));
}

void test_LW_059c_confirm_and_rollback_reach_FirmwareStore(void)
{
    use_ota();
    bind_ok();

    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.firmware_store->confirm_self_check(NULL));
    TEST_ASSERT_EQUAL(LC_DEP_OK, s_deps.firmware_store->rollback(NULL));
    TEST_ASSERT_EQUAL_UINT32(1U, s_fw_confirm_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_fw_rollback_calls);
    TEST_ASSERT_EQUAL_PTR(OTA_FS_HANDLE, s_fw_seen);

    s_fw_confirm_result = FIRMWARE_STORE_ERR_FLASH_WRITE;
    s_fw_rollback_result = FIRMWARE_STORE_ERR_FLASH_WRITE;
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.firmware_store->confirm_self_check(NULL));
    TEST_ASSERT_EQUAL(LC_DEP_FAIL, s_deps.firmware_store->rollback(NULL));
}

/* ===================================================================== */
/* The real LifecycleController through the adapters                     */
/* ===================================================================== */

static void boot_controller(void)
{
    bind_ok();
    TEST_ASSERT_EQUAL(LIFECYCLE_ERR_OK,
                      lifecycle_controller_init(LIFECYCLE_RESET_POWER_ON, &s_deps));
    lifecycle_task_body(NULL);
}

void test_LW_060_a_normal_boot_reaches_OPERATIONAL(void)
{
    boot_controller();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(1U, s_cfg_apply_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_reconf_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_poller_init_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xEventGroupSetBits_call_count);
}

void test_LW_061_a_blank_flash_first_boot_reaches_OPERATIONAL_on_the_defaults(void)
{
    s_cs_check_result = CONFIG_STORE_ERR_NO_VALID_SLOT;
    boot_controller();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(0U, s_cs_load_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_cfg_apply_calls);
}

void test_LW_062_a_blank_flash_is_fatal_when_the_wiring_is_told_so(void)
{
    s_cs_check_result = CONFIG_STORE_ERR_NO_VALID_SLOT;
    s_config.fault_on_no_config = true;
    boot_controller();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL(HEALTH_EVENT_FAULT, s_hm_event_seen);
    TEST_ASSERT_EQUAL_UINT32(LC_FAULT_INTEGRITY, s_hm_param_seen);
    TEST_ASSERT_EQUAL_UINT32(1U, s_hm_led_calls);
}

void test_LW_063_a_missing_sensor_task_faults_in_BringingUpSensors(void)
{
    s_sensor_task = NULL;
    boot_controller();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_FAULTED, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(LC_FAULT_SENSORS_INIT, s_hm_param_seen);
}

void test_LW_064_a_stale_schema_blob_boots_on_the_defaults(void)
{
    s_cfg_apply_result = CONFIG_SERVICE_ERR_INVALID;
    boot_controller();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LW_065_an_offline_start_still_reaches_OPERATIONAL(void)
{
    /* CloudPublisher is "ready" when it exists, not when the broker is up (LC-O7); the wiring
     * does not ask for more. */
    boot_controller();
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LW_066_a_restart_request_flushes_the_cloud_and_a_confirmation_resets(void)
{
    const lifecycle_event_t req = {.type = LC_EVENT_RESTART_REQUESTED, .param = 0U};
    const lifecycle_event_t ack = {.type = LC_EVENT_RESTART_CONFIRMED, .param = 0U};

    boot_controller();

    g_mock_xQueueReceive_return = pdTRUE;
    (void) memcpy(g_mock_xQueueReceive_next_item, &req, sizeof(req));
    g_mock_xQueueReceive_next_item_size = sizeof(req);
    g_mock_xQueueReceive_available = 1U;
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_RESTARTING, lifecycle_controller->get_state());
    TEST_ASSERT_EQUAL_UINT32(1U, s_cloud_flush_calls);

    (void) memcpy(g_mock_xQueueReceive_next_item, &ack, sizeof(ack));
    g_mock_xQueueReceive_available = 1U;
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL_UINT32(1U, s_reset_calls);
}

void test_LW_068_a_trial_boot_resumes_the_self_check_and_waits_for_the_verdict(void)
{
    use_ota();
    s_fw_info.state = FIRMWARE_BOOT_TRIAL_RUNNING;
    boot_controller();

    TEST_ASSERT_EQUAL_UINT32(1U, s_ota_selfcheck_calls);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_INIT, lifecycle_controller->get_state());
}

void test_LW_068b_a_rolled_back_boot_resumes_the_report_and_reaches_OPERATIONAL(void)
{
    use_ota();
    s_fw_info.state = FIRMWARE_BOOT_ROLLED_BACK;
    boot_controller();

    TEST_ASSERT_EQUAL_UINT32(1U, s_ota_after_rollback_calls);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

static void deliver(lifecycle_event_type_t type, uint32_t param)
{
    const lifecycle_event_t ev = {.type = type, .param = param};

    g_mock_xQueueReceive_return = pdTRUE;
    (void) memcpy(g_mock_xQueueReceive_next_item, &ev, sizeof(ev));
    g_mock_xQueueReceive_next_item_size = sizeof(ev);
    g_mock_xQueueReceive_available = 1U;
    lifecycle_task_body(NULL);
}

void test_LW_069_an_update_starts_through_UpdateService_and_ends_on_OTA_FINISHED(void)
{
    use_ota();
    boot_controller();

    deliver(LC_EVENT_OTA_REQUESTED, 77U);
    TEST_ASSERT_EQUAL_UINT32(1U, s_ota_start_calls);
    TEST_ASSERT_EQUAL_UINT32(77U, s_ota_job_seen);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_UPDATING_FW, lifecycle_controller->get_state());

    deliver(LC_EVENT_OTA_FINISHED, 77U);
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

void test_LW_069b_an_activated_update_flushes_and_resets_through_the_adapters(void)
{
    use_ota();
    boot_controller();

    deliver(LC_EVENT_OTA_REQUESTED, 77U);
    deliver(LC_EVENT_OTA_RESET_REQUESTED, 77U);

    TEST_ASSERT_EQUAL_UINT32(1U, s_cloud_flush_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_reset_calls);
}

void test_LW_069c_a_failed_self_check_rolls_back_through_both_adapters_and_resets(void)
{
    use_ota();
    boot_controller();

    deliver(LC_EVENT_OTA_REQUESTED, 77U);
    deliver(LC_EVENT_SELF_CHECK_FAIL, 0U);

    TEST_ASSERT_EQUAL_UINT32(1U, s_ota_rollback_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_fw_rollback_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_reset_calls);
}

void test_LW_067_an_edit_session_snapshots_and_commits_through_ConfigService(void)
{
    const lifecycle_event_t enter = {.type = LC_EVENT_CONFIG_EDIT_ENTER, .param = 0U};
    const lifecycle_event_t apply = {.type = LC_EVENT_CONFIG_EDIT_APPLY, .param = 0U};

    boot_controller();
    g_mock_xQueueReceive_return = pdTRUE;
    g_mock_xQueueReceive_next_item_size = sizeof(enter);

    (void) memcpy(g_mock_xQueueReceive_next_item, &enter, sizeof(enter));
    g_mock_xQueueReceive_available = 1U;
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL_UINT32(1U, s_cfg_snapshot_calls);

    (void) memcpy(g_mock_xQueueReceive_next_item, &apply, sizeof(apply));
    g_mock_xQueueReceive_available = 1U;
    lifecycle_task_body(NULL);
    TEST_ASSERT_EQUAL_UINT32(1U, s_cfg_flush_calls);
    TEST_ASSERT_EQUAL_UINT32(2U, s_reconf_calls); /* once at boot, once after the edit */
    TEST_ASSERT_EQUAL(LIFECYCLE_STATE_OPERATIONAL, lifecycle_controller->get_state());
}

/* ===================================================================== */
/* /control commands                                                     */
/* ===================================================================== */

static void remote(const char *json)
{
    lifecycle_handle_remote((const uint8_t *) json, (uint32_t) strlen(json));
}

static lifecycle_event_t last_sent(void)
{
    lifecycle_event_t ev;

    (void) memcpy(&ev, g_mock_xQueueSend_last_item, sizeof(ev));
    return ev;
}

void test_LW_070_a_restart_command_requests_a_restart(void)
{
    boot_controller();
    remote("{\"command\":\"restart\"}");
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xQueueSend_call_count);
    TEST_ASSERT_EQUAL(LC_EVENT_RESTART_REQUESTED, last_sent().type);
}

void test_LW_071_a_restart_confirm_command_posts_the_confirmation(void)
{
    boot_controller();
    remote("{\"command\":\"restart_confirm\"}");
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xQueueSend_call_count);
    TEST_ASSERT_EQUAL(LC_EVENT_RESTART_CONFIRMED, last_sent().type);
}

void test_LW_072_a_reset_metrics_command_resets_the_health_counters_directly(void)
{
    boot_controller();
    remote("{\"command\":\"reset_metrics\"}");
    TEST_ASSERT_EQUAL_UINT32(1U, s_hm_reset_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend_call_count);
}

void test_LW_073_an_unknown_command_does_nothing(void)
{
    boot_controller();
    remote("{\"command\":\"format_everything\"}");
    remote("{\"cmd\":\"restart\"}");
    remote("not json");
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend_call_count);
    TEST_ASSERT_EQUAL_UINT32(0U, s_hm_reset_calls);
}

void test_LW_074_null_and_empty_payloads_are_ignored(void)
{
    boot_controller();
    lifecycle_handle_remote(NULL, 5U);
    lifecycle_handle_remote((const uint8_t *) "x", 0U);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend_call_count);
}

void test_LW_075_whitespace_in_the_payload_is_not_a_command(void)
{
    /* Strict match: the exact "command":"name" form, as the publisher side produces it. */
    boot_controller();
    remote("{\"command\": \"restart\"}");
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend_call_count);
}

void test_LW_076_an_oversize_payload_is_truncated_not_overrun(void)
{
    uint8_t big[300];

    boot_controller();
    (void) memset(big, 'x', sizeof(big));
    (void) memcpy(&big[250], "\"command\":\"restart\"", 19U);
    lifecycle_handle_remote(big, sizeof(big));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend_call_count); /* the command is past the limit */
}

void test_LW_077_a_full_queue_does_not_crash_the_command_path(void)
{
    boot_controller();
    g_mock_xQueueSend_return = pdFALSE;
    remote("{\"command\":\"restart\"}");
    remote("{\"command\":\"restart_confirm\"}");
    TEST_ASSERT_EQUAL_UINT32(2U, g_mock_xQueueSend_call_count);
}

void test_LW_078_a_command_before_the_controller_is_initialised_is_harmless(void)
{
    bind_ok();
    remote("{\"command\":\"restart\"}");
    remote("{\"command\":\"reset_metrics\"}");
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend_call_count);
}
