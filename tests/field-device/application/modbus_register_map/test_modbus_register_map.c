/**
 * @file test_modbus_register_map.c
 * @brief Unity unit tests for ModbusRegisterMap — TC-MRM-001..027.
 *
 * All 11 provider vtables are stubbed via mrm_deps_stub.h (included
 * transitively through modbus_register_map.h when TEST is defined).
 * No real module .c files are linked.
 *
 * TC-MRM-009 INTENTIONALLY FAILS: read_pressure() contains MRM-BUG-001
 * (reads SENSOR_ID_HUMIDITY instead of SENSOR_ID_PRESSURE). Fix by
 * replacing both SENSOR_ID_HUMIDITY occurrences in read_pressure() with
 * SENSOR_ID_PRESSURE in modbus_register_map.c.
 */

#include "unity.h"
#include "modbus_register_map/modbus_register_map.h"
#include <string.h>

/* ===================================================================== */
/* Stub state                                                            */
/* ===================================================================== */

static config_params_t s_params;
static sensor_snapshot_t s_sensor_snap;
static sensor_service_err_t s_sensor_get_rc;
static alarm_state_t s_alarm_states[SENSOR_ID_COUNT];
static device_health_snapshot_t s_health_snap;
static modbus_slave_stats_t s_mb_stats;

/* Call tracking */
static bool s_ack_all_called;
static uint32_t s_set_param_call_count;
static config_param_id_t s_last_set_param_id;
static config_service_err_t s_set_param_rc;
static bool s_health_update_called;
static modbus_slave_stats_t s_last_health_stats;
static bool s_mb_stats_snapped;
static bool s_slave_addr_set_called;
static uint8_t s_slave_addr_set;
static bool s_lc_cmd_called;
static lc_remote_cmd_t s_last_lc_cmd;

/* ===================================================================== */
/* Stub functions                                                        */
/* ===================================================================== */

static sensor_service_err_t stub_sensor_get_snapshot(sensor_snapshot_t *snap)
{
    if (s_sensor_get_rc != SENSOR_SERVICE_ERR_OK)
    {
        return s_sensor_get_rc;
    }
    *snap = s_sensor_snap;
    return SENSOR_SERVICE_ERR_OK;
}

static alarm_service_err_t stub_alarm_get_all_states(alarm_state_t states[SENSOR_ID_COUNT])
{
    memcpy(states, s_alarm_states, SENSOR_ID_COUNT * sizeof(alarm_state_t));
    return ALARM_SERVICE_ERR_OK;
}

static alarm_service_err_t stub_alarm_ack_all(void)
{
    s_ack_all_called = true;
    return ALARM_SERVICE_ERR_OK;
}

static const config_params_t *stub_cfg_get_params(void)
{
    return &s_params;
}

static config_service_err_t stub_cfg_set_param(config_param_id_t id, const void *value)
{
    (void) value;
    s_set_param_call_count++;
    s_last_set_param_id = id;
    return s_set_param_rc;
}

static health_monitor_err_t stub_health_get_snapshot(device_health_snapshot_t *snap)
{
    *snap = s_health_snap;
    return HEALTH_MONITOR_ERR_OK;
}

static health_monitor_err_t stub_health_update_modbus_stats(const modbus_slave_stats_t *stats)
{
    s_health_update_called = true;
    s_last_health_stats = *stats;
    return HEALTH_MONITOR_ERR_OK;
}

static void stub_mb_stats_snapshot(modbus_slave_stats_t *out)
{
    s_mb_stats_snapped = true;
    *out = s_mb_stats;
}

static void stub_set_slave_address(uint8_t new_addr)
{
    s_slave_addr_set_called = true;
    s_slave_addr_set = new_addr;
}

static void stub_handle_remote_command(lc_remote_cmd_t cmd)
{
    s_lc_cmd_called = true;
    s_last_lc_cmd = cmd;
}

/* ===================================================================== */
/* Vtable instances                                                      */
/* ===================================================================== */

static isensor_service_t s_sensor_vtable;
static ialarm_service_t s_alarm_vtable;
static iconfig_provider_t s_cfg_read_vtable;
static iconfig_manager_t s_cfg_write_vtable;
static ihealth_snapshot_t s_health_read_vtable;
static ihealth_report_t s_health_report_vtable;
static itime_provider_t s_time_vtable;
static imodbus_slave_stats_t s_mb_stats_vtable;
static imodbus_slave_t s_mb_slave_vtable;
static ilifecycle_controller_t s_lifecycle_vtable;

/* MRM instance and interface */
static modbus_register_map_t *s_mrm;
static imodbus_register_map_t s_iface;

/* ===================================================================== */
/* Helpers                                                               */
/* ===================================================================== */

static void init_mrm(void)
{
    modbus_register_map_err_t rc = modbus_register_map_init(
        s_mrm, &s_sensor_vtable, &s_alarm_vtable, &s_cfg_read_vtable, &s_cfg_write_vtable,
        &s_health_read_vtable, &s_health_report_vtable, &s_time_vtable, &s_mb_stats_vtable,
        &s_mb_slave_vtable, &s_lifecycle_vtable);
    TEST_ASSERT_EQUAL_INT(MRM_ERR_OK, rc);
    modbus_register_map_get_iface(s_mrm, &s_iface);
}

/* ===================================================================== */
/* setUp / tearDown                                                      */
/* ===================================================================== */

void setUp(void)
{
    modbus_register_map_reset_for_test();
    s_mrm = modbus_register_map_get_test_instance();

    /* Wire vtables */
    memset(&s_sensor_vtable, 0, sizeof s_sensor_vtable);
    memset(&s_alarm_vtable, 0, sizeof s_alarm_vtable);
    memset(&s_cfg_read_vtable, 0, sizeof s_cfg_read_vtable);
    memset(&s_cfg_write_vtable, 0, sizeof s_cfg_write_vtable);
    memset(&s_health_read_vtable, 0, sizeof s_health_read_vtable);
    memset(&s_health_report_vtable, 0, sizeof s_health_report_vtable);
    memset(&s_time_vtable, 0, sizeof s_time_vtable);
    memset(&s_mb_stats_vtable, 0, sizeof s_mb_stats_vtable);
    memset(&s_mb_slave_vtable, 0, sizeof s_mb_slave_vtable);
    memset(&s_lifecycle_vtable, 0, sizeof s_lifecycle_vtable);

    s_sensor_vtable.get_snapshot = stub_sensor_get_snapshot;
    s_alarm_vtable.get_all_states = stub_alarm_get_all_states;
    s_alarm_vtable.ack_all = stub_alarm_ack_all;
    s_cfg_read_vtable.get_params = stub_cfg_get_params;
    s_cfg_write_vtable.set_param = stub_cfg_set_param;
    s_health_read_vtable.get_snapshot = stub_health_get_snapshot;
    s_health_report_vtable.update_modbus_slave_stats = stub_health_update_modbus_stats;
    s_mb_stats_vtable.snapshot = stub_mb_stats_snapshot;
    s_mb_slave_vtable.set_slave_address = stub_set_slave_address;
    s_lifecycle_vtable.handle_remote_command = stub_handle_remote_command;

    /* Default params */
    memset(&s_params, 0, sizeof s_params);
    s_params.temp_alarm_high = 4000;
    s_params.temp_alarm_low = 0;
    s_params.temp_hysteresis = 100;
    s_params.humidity_alarm_high = 8000;
    s_params.humidity_alarm_low = 2000;
    s_params.humidity_hysteresis = 200;
    s_params.pressure_alarm_high = 10500;
    s_params.pressure_alarm_low = 9500;
    s_params.pressure_hysteresis = 50;
    s_params.polling_interval_ms = 1000;
    s_params.modbus_slave_addr = 1;

    /* Default sensor snapshot — all three env sensors valid */
    memset(&s_sensor_snap, 0, sizeof s_sensor_snap);
    s_sensor_snap.readings[SENSOR_ID_TEMPERATURE].value = 2200;
    s_sensor_snap.readings[SENSOR_ID_TEMPERATURE].valid = true;
    s_sensor_snap.readings[SENSOR_ID_HUMIDITY].value = 5000;
    s_sensor_snap.readings[SENSOR_ID_HUMIDITY].valid = true;
    s_sensor_snap.readings[SENSOR_ID_PRESSURE].value = 10132;
    s_sensor_snap.readings[SENSOR_ID_PRESSURE].valid = true;
    s_sensor_get_rc = SENSOR_SERVICE_ERR_OK;

    /* Default alarm states — all clear */
    memset(s_alarm_states, 0, sizeof s_alarm_states);

    /* Default health snapshot */
    memset(&s_health_snap, 0, sizeof s_health_snap);
    s_health_snap.lifecycle_state = LIFECYCLE_STATE_OPERATIONAL;

    /* Default MB stats */
    memset(&s_mb_stats, 0, sizeof s_mb_stats);

    /* Clear call tracking */
    s_ack_all_called = false;
    s_set_param_call_count = 0;
    s_last_set_param_id = (config_param_id_t) 0;
    s_set_param_rc = CONFIG_SERVICE_OK;
    s_health_update_called = false;
    memset(&s_last_health_stats, 0, sizeof s_last_health_stats);
    s_mb_stats_snapped = false;
    s_slave_addr_set_called = false;
    s_slave_addr_set = 0;
    s_lc_cmd_called = false;
    s_last_lc_cmd = (lc_remote_cmd_t) 0;

    memset(&s_iface, 0, sizeof s_iface);
}

void tearDown(void)
{
}

/* ===================================================================== */
/* TC-MRM-001: init returns NULL_ARG when self is NULL                  */
/* ===================================================================== */
void test_TC_MRM_001_init_null_self(void)
{
    modbus_register_map_err_t rc = modbus_register_map_init(
        NULL, &s_sensor_vtable, &s_alarm_vtable, &s_cfg_read_vtable, &s_cfg_write_vtable,
        &s_health_read_vtable, &s_health_report_vtable, &s_time_vtable, &s_mb_stats_vtable,
        &s_mb_slave_vtable, &s_lifecycle_vtable);
    TEST_ASSERT_EQUAL_INT(MRM_ERR_NULL_ARG, rc);
}

/* ===================================================================== */
/* TC-MRM-002: init returns NULL_ARG when a dependency pointer is NULL  */
/* ===================================================================== */
void test_TC_MRM_002_init_null_dependency(void)
{
    /* Spot-check: sensors = NULL */
    modbus_register_map_err_t rc = modbus_register_map_init(
        s_mrm, NULL, &s_alarm_vtable, &s_cfg_read_vtable, &s_cfg_write_vtable,
        &s_health_read_vtable, &s_health_report_vtable, &s_time_vtable, &s_mb_stats_vtable,
        &s_mb_slave_vtable, &s_lifecycle_vtable);
    TEST_ASSERT_EQUAL_INT(MRM_ERR_NULL_ARG, rc);

    /* Spot-check: lifecycle = NULL */
    rc = modbus_register_map_init(s_mrm, &s_sensor_vtable, &s_alarm_vtable, &s_cfg_read_vtable,
                                  &s_cfg_write_vtable, &s_health_read_vtable,
                                  &s_health_report_vtable, &s_time_vtable, &s_mb_stats_vtable,
                                  &s_mb_slave_vtable, NULL);
    TEST_ASSERT_EQUAL_INT(MRM_ERR_NULL_ARG, rc);
}

/* ===================================================================== */
/* TC-MRM-003: init OK + get_iface builds a vtable with non-NULL ctx    */
/* ===================================================================== */
void test_TC_MRM_003_init_ok_iface_ctx(void)
{
    init_mrm();
    TEST_ASSERT_NOT_NULL(s_iface.ctx);
    TEST_ASSERT_NOT_NULL(s_iface.read_input_regs);
    TEST_ASSERT_NOT_NULL(s_iface.read_holding_regs);
    TEST_ASSERT_NOT_NULL(s_iface.write_single_reg);
    TEST_ASSERT_NOT_NULL(s_iface.write_multiple_regs);
}

/* ===================================================================== */
/* TC-MRM-004: FC04 before init returns ILLEGAL_DATA_VALUE              */
/* ===================================================================== */
void test_TC_MRM_004_fc04_before_init(void)
{
    /* get_iface before init — ctx is still NULL in template */
    modbus_register_map_get_iface(s_mrm, &s_iface);
    uint16_t buf = 0;
    modbus_exception_t exc = s_iface.read_input_regs(s_iface.ctx, 0x0000u, 1u, &buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_ILLEGAL_DATA_VALUE, exc);
}

/* ===================================================================== */
/* TC-MRM-005: FC04 identity registers (map version + vendor code)      */
/* ===================================================================== */
void test_TC_MRM_005_fc04_identity_regs(void)
{
    init_mrm();
    uint16_t buf[8];
    modbus_exception_t exc = s_iface.read_input_regs(s_iface.ctx, 0x0000u, 8u, buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_UINT16(1u, buf[0]);     /* MAP_VERSION  */
    TEST_ASSERT_EQUAL_UINT16(0u, buf[1]);     /* DEVICE_ID_HI */
    TEST_ASSERT_EQUAL_UINT16(0u, buf[2]);     /* DEVICE_ID_LO */
    TEST_ASSERT_EQUAL_UINT16(1u, buf[3]);     /* HARDWARE_REV */
    TEST_ASSERT_EQUAL_UINT16(1u, buf[4]);     /* FW_VER_MAJOR */
    TEST_ASSERT_EQUAL_UINT16(0u, buf[5]);     /* FW_VER_MINOR */
    TEST_ASSERT_EQUAL_UINT16(0u, buf[6]);     /* FW_VER_PATCH */
    TEST_ASSERT_EQUAL_HEX16(0x1A45u, buf[7]); /* VENDOR_CODE  */
}

/* ===================================================================== */
/* TC-MRM-006: FC04 temperature — valid reading                         */
/* ===================================================================== */
void test_TC_MRM_006_fc04_temperature_valid(void)
{
    init_mrm();
    s_sensor_snap.readings[SENSOR_ID_TEMPERATURE].value = 2200; /* 22.00 °C */
    s_sensor_snap.readings[SENSOR_ID_TEMPERATURE].valid = true;

    uint16_t buf = 0;
    modbus_exception_t exc = s_iface.read_input_regs(s_iface.ctx, 0x0010u, 1u, &buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_UINT16(2200u, buf);
}

/* ===================================================================== */
/* TC-MRM-007: FC04 temperature — sensor error returns sentinel 0x8000  */
/* ===================================================================== */
void test_TC_MRM_007_fc04_temperature_sensor_error(void)
{
    init_mrm();
    s_sensor_get_rc = SENSOR_SERVICE_ERR_NOT_INIT;

    uint16_t buf = 0;
    modbus_exception_t exc = s_iface.read_input_regs(s_iface.ctx, 0x0010u, 1u, &buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_HEX16(0x8000u, buf);

    s_sensor_get_rc = SENSOR_SERVICE_ERR_OK;
}

/* ===================================================================== */
/* TC-MRM-008: FC04 humidity — valid reading                            */
/* ===================================================================== */
void test_TC_MRM_008_fc04_humidity_valid(void)
{
    init_mrm();
    s_sensor_snap.readings[SENSOR_ID_HUMIDITY].value = 5000; /* 50.00 %RH */
    s_sensor_snap.readings[SENSOR_ID_HUMIDITY].valid = true;

    uint16_t buf = 0;
    modbus_exception_t exc = s_iface.read_input_regs(s_iface.ctx, 0x0011u, 1u, &buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_UINT16(5000u, buf);
}

/* ===================================================================== */
/* TC-MRM-009: FC04 pressure — INTENTIONAL BUG (MRM-BUG-001)           */
/*                                                                       */
/* read_pressure() reads SENSOR_ID_HUMIDITY instead of SENSOR_ID_       */
/* PRESSURE. This test FAILS until the bug is fixed.                    */
/* Expected: 10132 (pressure)   Actual: 5000 (humidity)                 */
/* ===================================================================== */
void test_TC_MRM_009_fc04_pressure_bug_detected(void)
{
    init_mrm();
    s_sensor_snap.readings[SENSOR_ID_HUMIDITY].value = 5000; /* 50.00 %RH */
    s_sensor_snap.readings[SENSOR_ID_HUMIDITY].valid = true;
    s_sensor_snap.readings[SENSOR_ID_PRESSURE].value = 10132; /* 1013.2 hPa */
    s_sensor_snap.readings[SENSOR_ID_PRESSURE].valid = true;

    uint16_t buf = 0;
    modbus_exception_t exc = s_iface.read_input_regs(s_iface.ctx, 0x0012u, 1u, &buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    /* FAILS: bug returns 5000 (humidity) instead of 10132 (pressure) */
    TEST_ASSERT_EQUAL_UINT16(10132u, buf);
}

/* ===================================================================== */
/* TC-MRM-010: FC04 alarm flags — temperature HIGH alarm active (bit 1) */
/* ===================================================================== */
void test_TC_MRM_010_fc04_alarm_flags_temp_high(void)
{
    init_mrm();
    s_alarm_states[SENSOR_ID_TEMPERATURE] = ALARM_STATE_ACTIVE_HIGH;
    /* All env sensors valid → SENSOR_FAULT bit 6 stays clear */

    uint16_t buf = 0;
    modbus_exception_t exc = s_iface.read_input_regs(s_iface.ctx, 0x0031u, 1u, &buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_HEX16(0x0002u, buf); /* bit 1 = TEMP_HIGH_ALARM */
}

/* ===================================================================== */
/* TC-MRM-011: FC04 device state — LIFECYCLE_STATE_OPERATIONAL = 1      */
/* ===================================================================== */
void test_TC_MRM_011_fc04_device_state(void)
{
    init_mrm();
    s_health_snap.lifecycle_state = LIFECYCLE_STATE_OPERATIONAL;

    uint16_t buf = 0;
    modbus_exception_t exc = s_iface.read_input_regs(s_iface.ctx, 0x0030u, 1u, &buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_UINT16(1u, buf);
}

/* ===================================================================== */
/* TC-MRM-012: FC04 uptime hi/lo pair                                   */
/* ===================================================================== */
void test_TC_MRM_012_fc04_uptime_hi_lo(void)
{
    init_mrm();
    s_health_snap.uptime_s = 0x00010000u; /* = 65536 s */

    uint16_t buf[2];
    modbus_exception_t exc = s_iface.read_input_regs(s_iface.ctx, 0x0032u, 2u, buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_UINT16(1u, buf[0]); /* UPTIME_HI */
    TEST_ASSERT_EQUAL_UINT16(0u, buf[1]); /* UPTIME_LO */
}

/* ===================================================================== */
/* TC-MRM-013: FC04 modbus stats regs after poll_stats                  */
/* ===================================================================== */
void test_TC_MRM_013_fc04_modbus_stats_after_poll(void)
{
    init_mrm();
    s_mb_stats.valid_frames = 0x00020001u; /* hi=2, lo=1 */
    s_mb_stats.crc_errors = 0x00000007u;

    modbus_register_map_err_t rc = modbus_register_map_poll_stats(s_mrm);
    TEST_ASSERT_EQUAL_INT(MRM_ERR_OK, rc);
    TEST_ASSERT_TRUE(s_mb_stats_snapped);
    TEST_ASSERT_TRUE(s_health_update_called);
    TEST_ASSERT_EQUAL_UINT32(0x00020001u, s_last_health_stats.valid_frames);

    /* Read MODBUS_RX_OK_HI (0x0034) and MODBUS_RX_OK_LO (0x0035) */
    uint16_t buf[2];
    modbus_exception_t exc = s_iface.read_input_regs(s_iface.ctx, 0x0034u, 2u, buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_UINT16(2u, buf[0]); /* hi */
    TEST_ASSERT_EQUAL_UINT16(1u, buf[1]); /* lo */

    /* Read MODBUS_CRC_ERR_HI (0x0036) and LO (0x0037) */
    modbus_exception_t exc2 = s_iface.read_input_regs(s_iface.ctx, 0x0036u, 2u, buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc2);
    TEST_ASSERT_EQUAL_UINT16(0u, buf[0]); /* hi */
    TEST_ASSERT_EQUAL_UINT16(7u, buf[1]); /* lo */
}

/* ===================================================================== */
/* TC-MRM-014: FC04 unknown input address → ILLEGAL_DATA_ADDR           */
/* ===================================================================== */
void test_TC_MRM_014_fc04_unknown_addr(void)
{
    init_mrm();
    uint16_t buf = 0;
    /* 0x0008 — gap between identity and sensor regs */
    modbus_exception_t exc = s_iface.read_input_regs(s_iface.ctx, 0x0008u, 1u, &buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_ILLEGAL_DATA_ADDR, exc);
}

/* ===================================================================== */
/* TC-MRM-015: FC04 on a holding-register address → ILLEGAL_DATA_ADDR   */
/* ===================================================================== */
void test_TC_MRM_015_fc04_on_holding_addr(void)
{
    init_mrm();
    uint16_t buf = 0;
    /* 0x0100 = temp_alarm_low — REG_ACCESS_RW, not visible via FC04 */
    modbus_exception_t exc = s_iface.read_input_regs(s_iface.ctx, 0x0100u, 1u, &buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_ILLEGAL_DATA_ADDR, exc);
}

/* ===================================================================== */
/* TC-MRM-016: FC03 reads holding registers (config params)             */
/* ===================================================================== */
void test_TC_MRM_016_fc03_read_config_holding_regs(void)
{
    init_mrm();
    s_params.temp_alarm_high = 4000;
    s_params.polling_interval_ms = 1000;

    /* Read temp_alarm_high at 0x0101 */
    uint16_t buf = 0;
    modbus_exception_t exc = s_iface.read_holding_regs(s_iface.ctx, 0x0101u, 1u, &buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_UINT16(4000u, buf);

    /* Read sampling_period at 0x0130 */
    exc = s_iface.read_holding_regs(s_iface.ctx, 0x0130u, 1u, &buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_UINT16(1000u, buf);
}

/* ===================================================================== */
/* TC-MRM-017: FC03 unknown holding address → ILLEGAL_DATA_ADDR         */
/* ===================================================================== */
void test_TC_MRM_017_fc03_unknown_holding_addr(void)
{
    init_mrm();
    uint16_t buf = 0;
    /* 0x01FF — in reserved range, no slot */
    modbus_exception_t exc = s_iface.read_holding_regs(s_iface.ctx, 0x01FFu, 1u, &buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_ILLEGAL_DATA_ADDR, exc);
}

/* ===================================================================== */
/* TC-MRM-018: FC03 on input-only address → ILLEGAL_DATA_ADDR           */
/* ===================================================================== */
void test_TC_MRM_018_fc03_on_input_only_addr(void)
{
    init_mrm();
    uint16_t buf = 0;
    /* 0x0010 = temperature — REG_ACCESS_R, not visible via FC03 */
    modbus_exception_t exc = s_iface.read_holding_regs(s_iface.ctx, 0x0010u, 1u, &buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_ILLEGAL_DATA_ADDR, exc);
}

/* ===================================================================== */
/* TC-MRM-019: FC06 write temp_alarm_high — valid value, set_param called*/
/* ===================================================================== */
void test_TC_MRM_019_fc06_write_temp_alarm_high_valid(void)
{
    init_mrm();
    /* 3000 > temp_alarm_low(0) and <= TEMP_FIXED_MAX(8500) → OK */
    modbus_exception_t exc = s_iface.write_single_reg(s_iface.ctx, 0x0101u, 3000u);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_UINT32(1u, s_set_param_call_count);
    TEST_ASSERT_EQUAL_INT(CONFIG_PARAM_TEMP_ALARM_HIGH, s_last_set_param_id);
}

/* ===================================================================== */
/* TC-MRM-020: FC06 write temp_alarm_high — value exceeds max            */
/* ===================================================================== */
void test_TC_MRM_020_fc06_write_temp_alarm_high_invalid(void)
{
    init_mrm();
    /* 9000 > TEMP_FIXED_MAX(8500) → validation rejects */
    modbus_exception_t exc = s_iface.write_single_reg(s_iface.ctx, 0x0101u, 9000u);
    TEST_ASSERT_EQUAL_INT(MB_EXC_ILLEGAL_DATA_VALUE, exc);
    TEST_ASSERT_EQUAL_UINT32(0u, s_set_param_call_count);
}

/* ===================================================================== */
/* TC-MRM-021: FC06 write sampling_period — valid value                 */
/* ===================================================================== */
void test_TC_MRM_021_fc06_write_sampling_period_valid(void)
{
    init_mrm();
    /* 2000 is in [100, 60000] → OK */
    modbus_exception_t exc = s_iface.write_single_reg(s_iface.ctx, 0x0130u, 2000u);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_UINT32(1u, s_set_param_call_count);
    TEST_ASSERT_EQUAL_INT(CONFIG_PARAM_POLL_INTERVAL, s_last_set_param_id);
}

/* ===================================================================== */
/* TC-MRM-022: FC06 write slave addr — Mediator calls both set_param    */
/*             and mb_slave->set_slave_address                          */
/* ===================================================================== */
void test_TC_MRM_022_fc06_write_slave_addr_mediator(void)
{
    init_mrm();
    /* Address 5: in range [1..247] */
    modbus_exception_t exc = s_iface.write_single_reg(s_iface.ctx, 0x0150u, 5u);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_UINT32(1u, s_set_param_call_count);
    TEST_ASSERT_EQUAL_INT(CONFIG_PARAM_MODBUS_SLAVE_ADDR, s_last_set_param_id);
    TEST_ASSERT_TRUE(s_slave_addr_set_called);
    TEST_ASSERT_EQUAL_UINT8(5u, s_slave_addr_set);
}

/* ===================================================================== */
/* TC-MRM-023: FC06 write to a read-only (input) address → ILLEGAL_DATA_ADDR */
/* ===================================================================== */
void test_TC_MRM_023_fc06_write_read_only_addr(void)
{
    init_mrm();
    /* 0x0010 = temperature register — REG_ACCESS_R */
    modbus_exception_t exc = s_iface.write_single_reg(s_iface.ctx, 0x0010u, 100u);
    TEST_ASSERT_EQUAL_INT(MB_EXC_ILLEGAL_DATA_ADDR, exc);
    TEST_ASSERT_EQUAL_UINT32(0u, s_set_param_call_count);
}

/* ===================================================================== */
/* TC-MRM-024: FC06 write to unknown address → ILLEGAL_DATA_ADDR        */
/* ===================================================================== */
void test_TC_MRM_024_fc06_write_unknown_addr(void)
{
    init_mrm();
    modbus_exception_t exc = s_iface.write_single_reg(s_iface.ctx, 0x00FFu, 0u);
    TEST_ASSERT_EQUAL_INT(MB_EXC_ILLEGAL_DATA_ADDR, exc);
}

/* ===================================================================== */
/* TC-MRM-025: FC16 multi-write — valid values, both set_param called   */
/* ===================================================================== */
void test_TC_MRM_025_fc16_multi_write_valid(void)
{
    init_mrm();
    /* humidity_alarm_low(0x0110)=3000, humidity_alarm_high(0x0111)=7000
     * Preconditions: low(2000) < 3000 < high(8000) and 2000 < 7000 < 10000 */
    const uint16_t values[2] = {3000u, 7000u};
    modbus_exception_t exc = s_iface.write_multiple_regs(s_iface.ctx, 0x0110u, 2u, values);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_UINT32(2u, s_set_param_call_count);
}

/* ===================================================================== */
/* TC-MRM-026: FC16 multi-write — phase-1 rejects bad second value,     */
/*             no set_param called (atomicity preserved)                */
/* ===================================================================== */
void test_TC_MRM_026_fc16_multi_write_phase1_reject(void)
{
    init_mrm();
    /* First reg (humidity_alarm_low=3000): valid
     * Second reg (humidity_alarm_high=1000): 1000 <= humidity_alarm_low(2000) → reject */
    const uint16_t values[2] = {3000u, 1000u};
    modbus_exception_t exc = s_iface.write_multiple_regs(s_iface.ctx, 0x0110u, 2u, values);
    TEST_ASSERT_EQUAL_INT(MB_EXC_ILLEGAL_DATA_VALUE, exc);
    TEST_ASSERT_EQUAL_UINT32(0u, s_set_param_call_count); /* no side effects */
}

/* ===================================================================== */
/* TC-MRM-027: Command registers — ack_alarm, reset_metrics,            */
/*             soft_restart (read-after-write + lifecycle routing)      */
/* ===================================================================== */
void test_TC_MRM_027_command_registers(void)
{
    init_mrm();

    /* --- ack_alarm (0x0200) --- */
    /* Good magic → ack_all called */
    modbus_exception_t exc = s_iface.write_single_reg(s_iface.ctx, 0x0200u, 0x0001u);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_TRUE(s_ack_all_called);

    /* Read back last written value via FC03 */
    uint16_t buf = 0;
    exc = s_iface.read_holding_regs(s_iface.ctx, 0x0200u, 1u, &buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_HEX16(0x0001u, buf);

    /* Bad magic — still cached, but ack_all NOT called again */
    s_ack_all_called = false;
    exc = s_iface.write_single_reg(s_iface.ctx, 0x0200u, 0x0000u);
    TEST_ASSERT_EQUAL_INT(MB_EXC_ILLEGAL_DATA_VALUE, exc);
    TEST_ASSERT_FALSE(s_ack_all_called);
    /* Read-after-write: bad value still cached */
    exc = s_iface.read_holding_regs(s_iface.ctx, 0x0200u, 1u, &buf);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_EQUAL_HEX16(0x0000u, buf);

    /* --- reset_metrics (0x0201) --- */
    exc = s_iface.write_single_reg(s_iface.ctx, 0x0201u, 0x0001u);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_TRUE(s_lc_cmd_called);
    TEST_ASSERT_EQUAL_INT(LC_REMOTE_CMD_RESET_METRICS, s_last_lc_cmd);

    /* --- soft_restart (0x0202) --- */
    /* Bad magic — lifecycle NOT called */
    s_lc_cmd_called = false;
    exc = s_iface.write_single_reg(s_iface.ctx, 0x0202u, 0x1234u);
    TEST_ASSERT_EQUAL_INT(MB_EXC_ILLEGAL_DATA_VALUE, exc);
    TEST_ASSERT_FALSE(s_lc_cmd_called);

    /* Good magic 0xA5A5 — lifecycle called with SOFT_RESTART */
    exc = s_iface.write_single_reg(s_iface.ctx, 0x0202u, 0xA5A5u);
    TEST_ASSERT_EQUAL_INT(MB_EXC_NONE, exc);
    TEST_ASSERT_TRUE(s_lc_cmd_called);
    TEST_ASSERT_EQUAL_INT(LC_REMOTE_CMD_SOFT_RESTART, s_last_lc_cmd);
}
