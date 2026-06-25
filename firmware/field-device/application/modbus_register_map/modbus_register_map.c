/**
 * @file modbus_register_map.c
 * @brief ModbusRegisterMap — Application-layer Modbus register dispatcher.
 *
 * Implements the slot-table dispatch for FC03, FC04, FC06, FC16.
 * Acts as Mediator between ModbusSlave, SensorService, AlarmService,
 * ConfigService, HealthMonitor, TimeProvider, and LifecycleController.
 *
 * Intentional bug for exercise: read_pressure() reads from SENSOR_ID_HUMIDITY
 * instead of SENSOR_ID_PRESSURE (copy-paste error). TC-MRM-007 will fail until
 * the bug is corrected. See docs/dev-tools/modbus_register_map/bug-log.md.
 *
 * @see docs/lld/application/modbus-register-map-lld.md
 */

#ifndef TEST
#include "sensor_service/sensor_service.h"
#include "alarm_service/alarm_service.h"
#include "config_service/config_service.h"
#include "health_monitor/health_monitor.h"
#include "time_provider/time_provider.h"
#define LOG_MODULE "MRM"
#include "logger/logger.h"
#else
#include "mrm_deps_stub.h"
#define LOG_INFO(mod, fmt, ...) ((void) 0)
#define LOG_WARN(mod, fmt, ...) ((void) 0)
#define LOG_ERROR(mod, fmt, ...) ((void) 0)
#endif /* TEST */

#include "modbus_register_map/modbus_register_map.h"
#include "firmware_version.h"
#include <string.h>
#include <stddef.h>

/* ===================================================================== */
/* Internal struct definition                                           */
/* ===================================================================== */

struct modbus_register_map
{
    bool initialised;
    const isensor_service_t *sensors;
    const ialarm_service_t *alarms;
    const iconfig_provider_t *cfg_read;
    iconfig_manager_t *cfg_write;
    const ihealth_snapshot_t *health_read;
    ihealth_report_t *health_write;
    const itime_provider_t *time;
    const imodbus_slave_stats_t *mb_stats;
    imodbus_slave_t *mb_slave;
    const ilifecycle_t *lifecycle;
    uint16_t cmd_ack_alarm_last;
    uint16_t cmd_reset_metrics_last;
    uint16_t cmd_soft_restart_last;
    modbus_slave_stats_t last_stats_snapshot;
};

static modbus_register_map_t s_mrm;

/* ===================================================================== */
/* Identity constants (compile-time; no DeviceProfileRegistry yet)      */
/* ===================================================================== */

#define MRM_MAP_VERSION ((uint16_t) 1U)
#define MRM_DEVICE_ID_HI ((uint16_t) 0U)
#define MRM_DEVICE_ID_LO ((uint16_t) 0U)
#define MRM_HARDWARE_REV ((uint16_t) 1U)
#define MRM_FW_VER_MAJOR ((uint16_t) FW_VERSION_MAJOR) /* firmware_version.h §0x0004 */
#define MRM_FW_VER_MINOR ((uint16_t) FW_VERSION_MINOR) /* firmware_version.h §0x0005 */
#define MRM_FW_VER_PATCH ((uint16_t) FW_VERSION_PATCH) /* firmware_version.h §0x0006 */
#define MRM_VENDOR_CODE ((uint16_t) 0x1A45U)

/* ===================================================================== */
/* Register value limits (per data-spec §6.4)                          */
/* ===================================================================== */

#define TEMP_FIXED_MIN ((int16_t) (-4000))
#define TEMP_FIXED_MAX ((int16_t) (8500))
#define TEMP_HYS_MAX ((uint16_t) 1000U)
#define HUM_FIXED_MAX ((uint16_t) 10000U)
#define HUM_HYS_MAX ((uint16_t) 1000U)
#define PRESS_FIXED_MIN ((uint16_t) 3000U)
#define PRESS_FIXED_MAX ((uint16_t) 11000U)
#define PRESS_HYS_MAX ((uint16_t) 100U)
#define SAMP_PERIOD_MIN ((uint16_t) 100U)
#define SAMP_PERIOD_MAX ((uint16_t) 60000U)
#define LCD_BRIGHT_MAX ((uint16_t) 100U)
#define LCD_TIMEOUT_MAX ((uint16_t) 3600U)
#define SLAVE_ADDR_MIN ((uint16_t) 1U)
#define SLAVE_ADDR_MAX ((uint16_t) 247U)

/* ===================================================================== */
/* Slot-table type definitions                                          */
/* ===================================================================== */

typedef enum
{
    REG_TYPE_UINT16,
    REG_TYPE_INT16,
    REG_TYPE_UINT32_HI,
    REG_TYPE_UINT32_LO,
    REG_TYPE_BITFIELD16,
    REG_TYPE_ENUM16,
} reg_type_t;

typedef enum
{
    REG_ACCESS_R,
    REG_ACCESS_RW,
} reg_access_t;

typedef modbus_exception_t (*reg_read_fn_t)(const modbus_register_map_t *self, uint16_t *out_value);
typedef modbus_exception_t (*reg_write_fn_t)(modbus_register_map_t *self, uint16_t value);
typedef modbus_exception_t (*reg_validate_fn_t)(const modbus_register_map_t *self, uint16_t value);

typedef struct
{
    uint16_t addr;
    reg_type_t type;
    reg_access_t access;
    reg_read_fn_t read_fn;
    reg_write_fn_t write_fn;
    reg_validate_fn_t validate_fn;
} reg_slot_t;

/* ===================================================================== */
/* Forward declarations of all slot handlers                            */
/* ===================================================================== */

/* Identity */
static modbus_exception_t read_map_version(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_device_id_hi(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_device_id_lo(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_hardware_rev(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_fw_ver_major(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_fw_ver_minor(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_fw_ver_patch(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_vendor_code(const modbus_register_map_t *s, uint16_t *o);
/* Sensors */
static modbus_exception_t read_temperature(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_humidity(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_pressure(const modbus_register_map_t *s, uint16_t *o);
/* Device state / metrics */
static modbus_exception_t read_device_state(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_alarm_flags(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_uptime_hi(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_uptime_lo(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_modbus_rx_ok_hi(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_modbus_rx_ok_lo(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_modbus_crc_err_hi(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_modbus_crc_err_lo(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_zero(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_sensor_err_hi(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_sensor_err_lo(const modbus_register_map_t *s, uint16_t *o);
/* Configuration reads */
static modbus_exception_t read_temp_alarm_low(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_temp_alarm_high(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_temp_hysteresis(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_hum_alarm_low(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_hum_alarm_high(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_hum_hysteresis(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_press_alarm_low(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_press_alarm_high(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_press_hysteresis(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_sampling_period(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_lcd_brightness(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_lcd_timeout(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t read_modbus_slave_addr(const modbus_register_map_t *s, uint16_t *o);
/* Configuration writes */
static modbus_exception_t write_temp_alarm_low(modbus_register_map_t *s, uint16_t v);
static modbus_exception_t write_temp_alarm_high(modbus_register_map_t *s, uint16_t v);
static modbus_exception_t write_temp_hysteresis(modbus_register_map_t *s, uint16_t v);
static modbus_exception_t write_hum_alarm_low(modbus_register_map_t *s, uint16_t v);
static modbus_exception_t write_hum_alarm_high(modbus_register_map_t *s, uint16_t v);
static modbus_exception_t write_hum_hysteresis(modbus_register_map_t *s, uint16_t v);
static modbus_exception_t write_press_alarm_low(modbus_register_map_t *s, uint16_t v);
static modbus_exception_t write_press_alarm_high(modbus_register_map_t *s, uint16_t v);
static modbus_exception_t write_press_hysteresis(modbus_register_map_t *s, uint16_t v);
static modbus_exception_t write_sampling_period(modbus_register_map_t *s, uint16_t v);
static modbus_exception_t write_lcd_brightness(modbus_register_map_t *s, uint16_t v);
static modbus_exception_t write_lcd_timeout(modbus_register_map_t *s, uint16_t v);
static modbus_exception_t write_modbus_slave_addr_reg(modbus_register_map_t *s, uint16_t v);
/* Configuration validators */
static modbus_exception_t validate_temp_alarm_low(const modbus_register_map_t *s, uint16_t v);
static modbus_exception_t validate_temp_alarm_high(const modbus_register_map_t *s, uint16_t v);
static modbus_exception_t validate_temp_hysteresis(const modbus_register_map_t *s, uint16_t v);
static modbus_exception_t validate_hum_alarm_low(const modbus_register_map_t *s, uint16_t v);
static modbus_exception_t validate_hum_alarm_high(const modbus_register_map_t *s, uint16_t v);
static modbus_exception_t validate_hum_hysteresis(const modbus_register_map_t *s, uint16_t v);
static modbus_exception_t validate_press_alarm_low(const modbus_register_map_t *s, uint16_t v);
static modbus_exception_t validate_press_alarm_high(const modbus_register_map_t *s, uint16_t v);
static modbus_exception_t validate_press_hysteresis(const modbus_register_map_t *s, uint16_t v);
static modbus_exception_t validate_sampling_period(const modbus_register_map_t *s, uint16_t v);
static modbus_exception_t validate_lcd_brightness(const modbus_register_map_t *s, uint16_t v);
static modbus_exception_t validate_lcd_timeout(const modbus_register_map_t *s, uint16_t v);
static modbus_exception_t validate_slave_addr(const modbus_register_map_t *s, uint16_t v);
/* Command handlers */
static modbus_exception_t read_cmd_ack_alarm(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t write_cmd_ack_alarm(modbus_register_map_t *s, uint16_t v);
static modbus_exception_t read_cmd_reset_metrics(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t write_cmd_reset_metrics(modbus_register_map_t *s, uint16_t v);
static modbus_exception_t read_cmd_soft_restart(const modbus_register_map_t *s, uint16_t *o);
static modbus_exception_t write_cmd_soft_restart(modbus_register_map_t *s, uint16_t v);

/* ===================================================================== */
/* Slot table — sorted ascending by address (invariant checked at init)  */
/* ===================================================================== */

static const reg_slot_t k_slots[] = {
    /* Identity and version (0x0000–0x0007) */
    {0x0000u, REG_TYPE_UINT16, REG_ACCESS_R, read_map_version, NULL, NULL},
    {0x0001u, REG_TYPE_UINT16, REG_ACCESS_R, read_device_id_hi, NULL, NULL},
    {0x0002u, REG_TYPE_UINT16, REG_ACCESS_R, read_device_id_lo, NULL, NULL},
    {0x0003u, REG_TYPE_UINT16, REG_ACCESS_R, read_hardware_rev, NULL, NULL},
    {0x0004u, REG_TYPE_UINT16, REG_ACCESS_R, read_fw_ver_major, NULL, NULL},
    {0x0005u, REG_TYPE_UINT16, REG_ACCESS_R, read_fw_ver_minor, NULL, NULL},
    {0x0006u, REG_TYPE_UINT16, REG_ACCESS_R, read_fw_ver_patch, NULL, NULL},
    {0x0007u, REG_TYPE_UINT16, REG_ACCESS_R, read_vendor_code, NULL, NULL},
    /* Sensor readings (0x0010–0x0012) */
    {0x0010u, REG_TYPE_INT16, REG_ACCESS_R, read_temperature, NULL, NULL},
    {0x0011u, REG_TYPE_UINT16, REG_ACCESS_R, read_humidity, NULL, NULL},
    {0x0012u, REG_TYPE_UINT16, REG_ACCESS_R, read_pressure, NULL, NULL},
    /* Device state and metrics (0x0030–0x003B) */
    {0x0030u, REG_TYPE_ENUM16, REG_ACCESS_R, read_device_state, NULL, NULL},
    {0x0031u, REG_TYPE_BITFIELD16, REG_ACCESS_R, read_alarm_flags, NULL, NULL},
    {0x0032u, REG_TYPE_UINT32_HI, REG_ACCESS_R, read_uptime_hi, NULL, NULL},
    {0x0033u, REG_TYPE_UINT32_LO, REG_ACCESS_R, read_uptime_lo, NULL, NULL},
    {0x0034u, REG_TYPE_UINT32_HI, REG_ACCESS_R, read_modbus_rx_ok_hi, NULL, NULL},
    {0x0035u, REG_TYPE_UINT32_LO, REG_ACCESS_R, read_modbus_rx_ok_lo, NULL, NULL},
    {0x0036u, REG_TYPE_UINT32_HI, REG_ACCESS_R, read_modbus_crc_err_hi, NULL, NULL},
    {0x0037u, REG_TYPE_UINT32_LO, REG_ACCESS_R, read_modbus_crc_err_lo, NULL, NULL},
    {0x0038u, REG_TYPE_UINT32_HI, REG_ACCESS_R, read_zero, NULL, NULL},
    {0x0039u, REG_TYPE_UINT32_LO, REG_ACCESS_R, read_zero, NULL, NULL},
    {0x003Au, REG_TYPE_UINT32_HI, REG_ACCESS_R, read_sensor_err_hi, NULL, NULL},
    {0x003Bu, REG_TYPE_UINT32_LO, REG_ACCESS_R, read_sensor_err_lo, NULL, NULL},
    /* Temperature thresholds (0x0100–0x0102) */
    {0x0100u, REG_TYPE_INT16, REG_ACCESS_RW, read_temp_alarm_low, write_temp_alarm_low,
     validate_temp_alarm_low},
    {0x0101u, REG_TYPE_INT16, REG_ACCESS_RW, read_temp_alarm_high, write_temp_alarm_high,
     validate_temp_alarm_high},
    {0x0102u, REG_TYPE_UINT16, REG_ACCESS_RW, read_temp_hysteresis, write_temp_hysteresis,
     validate_temp_hysteresis},
    /* Humidity thresholds (0x0110–0x0112) */
    {0x0110u, REG_TYPE_UINT16, REG_ACCESS_RW, read_hum_alarm_low, write_hum_alarm_low,
     validate_hum_alarm_low},
    {0x0111u, REG_TYPE_UINT16, REG_ACCESS_RW, read_hum_alarm_high, write_hum_alarm_high,
     validate_hum_alarm_high},
    {0x0112u, REG_TYPE_UINT16, REG_ACCESS_RW, read_hum_hysteresis, write_hum_hysteresis,
     validate_hum_hysteresis},
    /* Pressure thresholds (0x0120–0x0122) */
    {0x0120u, REG_TYPE_UINT16, REG_ACCESS_RW, read_press_alarm_low, write_press_alarm_low,
     validate_press_alarm_low},
    {0x0121u, REG_TYPE_UINT16, REG_ACCESS_RW, read_press_alarm_high, write_press_alarm_high,
     validate_press_alarm_high},
    {0x0122u, REG_TYPE_UINT16, REG_ACCESS_RW, read_press_hysteresis, write_press_hysteresis,
     validate_press_hysteresis},
    /* Acquisition parameters (0x0130) */
    {0x0130u, REG_TYPE_UINT16, REG_ACCESS_RW, read_sampling_period, write_sampling_period,
     validate_sampling_period},
    /* LCD parameters (0x0140–0x0141) — placeholder; LcdUi not yet integrated */
    {0x0140u, REG_TYPE_UINT16, REG_ACCESS_RW, read_lcd_brightness, write_lcd_brightness,
     validate_lcd_brightness},
    {0x0141u, REG_TYPE_UINT16, REG_ACCESS_RW, read_lcd_timeout, write_lcd_timeout,
     validate_lcd_timeout},
    /* Modbus slave address (0x0150) — deviation from data-spec §6.4 reserved range */
    {0x0150u, REG_TYPE_UINT16, REG_ACCESS_RW, read_modbus_slave_addr, write_modbus_slave_addr_reg,
     validate_slave_addr},
    /* Command registers (0x0200–0x0202) */
    {0x0200u, REG_TYPE_UINT16, REG_ACCESS_RW, read_cmd_ack_alarm, write_cmd_ack_alarm, NULL},
    {0x0201u, REG_TYPE_UINT16, REG_ACCESS_RW, read_cmd_reset_metrics, write_cmd_reset_metrics,
     NULL},
    {0x0202u, REG_TYPE_UINT16, REG_ACCESS_RW, read_cmd_soft_restart, write_cmd_soft_restart, NULL},
};

#define K_SLOTS_COUNT ((uint16_t) (sizeof k_slots / sizeof k_slots[0]))

/* ===================================================================== */
/* Slot lookup (linear scan — justified in LLD §5.4)                   */
/* ===================================================================== */

static const reg_slot_t *find_slot(uint16_t addr)
{
    for (uint16_t i = 0u; i < K_SLOTS_COUNT; i++)
    {
        if (k_slots[i].addr == addr)
        {
            return &k_slots[i];
        }
    }
    return NULL;
}

/* ===================================================================== */
/* Identity read handlers                                               */
/* ===================================================================== */

static modbus_exception_t read_map_version(const modbus_register_map_t *s, uint16_t *o)
{
    (void) s;
    *o = MRM_MAP_VERSION;
    return MB_EXC_NONE;
}
static modbus_exception_t read_device_id_hi(const modbus_register_map_t *s, uint16_t *o)
{
    (void) s;
    *o = MRM_DEVICE_ID_HI;
    return MB_EXC_NONE;
}
static modbus_exception_t read_device_id_lo(const modbus_register_map_t *s, uint16_t *o)
{
    (void) s;
    *o = MRM_DEVICE_ID_LO;
    return MB_EXC_NONE;
}
static modbus_exception_t read_hardware_rev(const modbus_register_map_t *s, uint16_t *o)
{
    (void) s;
    *o = MRM_HARDWARE_REV;
    return MB_EXC_NONE;
}
static modbus_exception_t read_fw_ver_major(const modbus_register_map_t *s, uint16_t *o)
{
    (void) s;
    *o = MRM_FW_VER_MAJOR;
    return MB_EXC_NONE;
}
static modbus_exception_t read_fw_ver_minor(const modbus_register_map_t *s, uint16_t *o)
{
    (void) s;
    *o = MRM_FW_VER_MINOR;
    return MB_EXC_NONE;
}
static modbus_exception_t read_fw_ver_patch(const modbus_register_map_t *s, uint16_t *o)
{
    (void) s;
    *o = MRM_FW_VER_PATCH;
    return MB_EXC_NONE;
}
static modbus_exception_t read_vendor_code(const modbus_register_map_t *s, uint16_t *o)
{
    (void) s;
    *o = MRM_VENDOR_CODE;
    return MB_EXC_NONE;
}

/* ===================================================================== */
/* Sensor read handlers                                                 */
/* ===================================================================== */

static modbus_exception_t read_temperature(const modbus_register_map_t *s, uint16_t *o)
{
    sensor_snapshot_t snap;
    if (s->sensors->get_snapshot(&snap) != SENSOR_SERVICE_ERR_OK)
    {
        *o = 0x8000u;
        return MB_EXC_NONE;
    }
    if (!snap.readings[SENSOR_ID_TEMPERATURE].valid)
    {
        *o = 0x8000u;
        return MB_EXC_NONE;
    }
    *o = (uint16_t) (int16_t) snap.readings[SENSOR_ID_TEMPERATURE].value;
    return MB_EXC_NONE;
}

static modbus_exception_t read_humidity(const modbus_register_map_t *s, uint16_t *o)
{
    sensor_snapshot_t snap;
    if (s->sensors->get_snapshot(&snap) != SENSOR_SERVICE_ERR_OK)
    {
        *o = 0xFFFFu;
        return MB_EXC_NONE;
    }
    if (!snap.readings[SENSOR_ID_HUMIDITY].valid)
    {
        *o = 0xFFFFu;
        return MB_EXC_NONE;
    }
    *o = (uint16_t) (uint32_t) snap.readings[SENSOR_ID_HUMIDITY].value;
    return MB_EXC_NONE;
}

/* INTENTIONAL BUG (MRM-BUG-001): reads from SENSOR_ID_HUMIDITY instead
 * of SENSOR_ID_PRESSURE. TC-MRM-007 detects this. Fix: replace both
 * SENSOR_ID_HUMIDITY occurrences with SENSOR_ID_PRESSURE.             */
static modbus_exception_t read_pressure(const modbus_register_map_t *s, uint16_t *o)
{
    sensor_snapshot_t snap;
    if (s->sensors->get_snapshot(&snap) != SENSOR_SERVICE_ERR_OK)
    {
        *o = 0xFFFFu;
        return MB_EXC_NONE;
    }
    if (!snap.readings[SENSOR_ID_PRESSURE].valid) /* BUG: should be SENSOR_ID_PRESSURE */
    {
        *o = 0xFFFFu;
        return MB_EXC_NONE;
    }
    *o = (uint16_t) (uint32_t) snap.readings[SENSOR_ID_PRESSURE].value; /* BUG */
    return MB_EXC_NONE;
}

/* ===================================================================== */
/* Device state and metrics read handlers                               */
/* ===================================================================== */

static modbus_exception_t read_device_state(const modbus_register_map_t *s, uint16_t *o)
{
    device_health_snapshot_t hsnap;
    s->health_read->get_snapshot(&hsnap);
    *o = (uint16_t) hsnap.lifecycle_state;
    return MB_EXC_NONE;
}

static modbus_exception_t read_alarm_flags(const modbus_register_map_t *s, uint16_t *o)
{
    alarm_state_t states[SENSOR_ID_COUNT];
    uint16_t flags = 0u;

    s->alarms->get_all_states(states);

    if (states[SENSOR_ID_TEMPERATURE] == ALARM_STATE_ACTIVE_LOW)
        flags |= (1u << 0u);
    if (states[SENSOR_ID_TEMPERATURE] == ALARM_STATE_ACTIVE_HIGH)
        flags |= (1u << 1u);
    if (states[SENSOR_ID_HUMIDITY] == ALARM_STATE_ACTIVE_LOW)
        flags |= (1u << 2u);
    if (states[SENSOR_ID_HUMIDITY] == ALARM_STATE_ACTIVE_HIGH)
        flags |= (1u << 3u);
    if (states[SENSOR_ID_PRESSURE] == ALARM_STATE_ACTIVE_LOW)
        flags |= (1u << 4u);
    if (states[SENSOR_ID_PRESSURE] == ALARM_STATE_ACTIVE_HIGH)
        flags |= (1u << 5u);

    /* Bit 6: SENSOR_FAULT — any of the three environmental sensors invalid */
    sensor_snapshot_t ssnap;
    if (s->sensors->get_snapshot(&ssnap) == SENSOR_SERVICE_ERR_OK)
    {
        for (uint8_t i = 0u; i < 3u; i++)
        {
            if (!ssnap.readings[i].valid)
            {
                flags |= (1u << 6u);
                break;
            }
        }
    }
    else
    {
        flags |= (1u << 6u); /* sensor service itself unavailable */
    }

    *o = flags;
    return MB_EXC_NONE;
}

static modbus_exception_t read_uptime_hi(const modbus_register_map_t *s, uint16_t *o)
{
    device_health_snapshot_t hsnap;
    s->health_read->get_snapshot(&hsnap);
    *o = (uint16_t) (hsnap.uptime_s >> 16u);
    return MB_EXC_NONE;
}

static modbus_exception_t read_uptime_lo(const modbus_register_map_t *s, uint16_t *o)
{
    device_health_snapshot_t hsnap;
    s->health_read->get_snapshot(&hsnap);
    *o = (uint16_t) (hsnap.uptime_s & 0xFFFFu);
    return MB_EXC_NONE;
}

static modbus_exception_t read_modbus_rx_ok_hi(const modbus_register_map_t *s, uint16_t *o)
{
    *o = (uint16_t) (s->last_stats_snapshot.valid_frames >> 16u);
    return MB_EXC_NONE;
}
static modbus_exception_t read_modbus_rx_ok_lo(const modbus_register_map_t *s, uint16_t *o)
{
    *o = (uint16_t) (s->last_stats_snapshot.valid_frames & 0xFFFFu);
    return MB_EXC_NONE;
}
static modbus_exception_t read_modbus_crc_err_hi(const modbus_register_map_t *s, uint16_t *o)
{
    *o = (uint16_t) (s->last_stats_snapshot.crc_errors >> 16u);
    return MB_EXC_NONE;
}
static modbus_exception_t read_modbus_crc_err_lo(const modbus_register_map_t *s, uint16_t *o)
{
    *o = (uint16_t) (s->last_stats_snapshot.crc_errors & 0xFFFFu);
    return MB_EXC_NONE;
}
static modbus_exception_t read_zero(const modbus_register_map_t *s, uint16_t *o)
{
    (void) s;
    *o = 0u;
    return MB_EXC_NONE;
}
static modbus_exception_t read_sensor_err_hi(const modbus_register_map_t *s, uint16_t *o)
{
    device_health_snapshot_t hsnap;
    s->health_read->get_snapshot(&hsnap);
    *o = (uint16_t) (hsnap.sensor_fail_count >> 16u);
    return MB_EXC_NONE;
}
static modbus_exception_t read_sensor_err_lo(const modbus_register_map_t *s, uint16_t *o)
{
    device_health_snapshot_t hsnap;
    s->health_read->get_snapshot(&hsnap);
    *o = (uint16_t) (hsnap.sensor_fail_count & 0xFFFFu);
    return MB_EXC_NONE;
}

/* ===================================================================== */
/* Configuration read handlers                                          */
/* ===================================================================== */

static modbus_exception_t read_temp_alarm_low(const modbus_register_map_t *s, uint16_t *o)
{
    *o = (uint16_t) (int16_t) s->cfg_read->get_params()->temp_alarm_low;
    return MB_EXC_NONE;
}
static modbus_exception_t read_temp_alarm_high(const modbus_register_map_t *s, uint16_t *o)
{
    *o = (uint16_t) (int16_t) s->cfg_read->get_params()->temp_alarm_high;
    return MB_EXC_NONE;
}
static modbus_exception_t read_temp_hysteresis(const modbus_register_map_t *s, uint16_t *o)
{
    *o = (uint16_t) (int16_t) s->cfg_read->get_params()->temp_hysteresis;
    return MB_EXC_NONE;
}
static modbus_exception_t read_hum_alarm_low(const modbus_register_map_t *s, uint16_t *o)
{
    *o = s->cfg_read->get_params()->humidity_alarm_low;
    return MB_EXC_NONE;
}
static modbus_exception_t read_hum_alarm_high(const modbus_register_map_t *s, uint16_t *o)
{
    *o = s->cfg_read->get_params()->humidity_alarm_high;
    return MB_EXC_NONE;
}
static modbus_exception_t read_hum_hysteresis(const modbus_register_map_t *s, uint16_t *o)
{
    *o = s->cfg_read->get_params()->humidity_hysteresis;
    return MB_EXC_NONE;
}
static modbus_exception_t read_press_alarm_low(const modbus_register_map_t *s, uint16_t *o)
{
    *o = s->cfg_read->get_params()->pressure_alarm_low;
    return MB_EXC_NONE;
}
static modbus_exception_t read_press_alarm_high(const modbus_register_map_t *s, uint16_t *o)
{
    *o = s->cfg_read->get_params()->pressure_alarm_high;
    return MB_EXC_NONE;
}
static modbus_exception_t read_press_hysteresis(const modbus_register_map_t *s, uint16_t *o)
{
    *o = s->cfg_read->get_params()->pressure_hysteresis;
    return MB_EXC_NONE;
}
static modbus_exception_t read_sampling_period(const modbus_register_map_t *s, uint16_t *o)
{
    *o = (uint16_t) s->cfg_read->get_params()->polling_interval_ms;
    return MB_EXC_NONE;
}
static modbus_exception_t read_lcd_brightness(const modbus_register_map_t *s, uint16_t *o)
{
    (void) s;
    *o = 80u; /* placeholder — LcdUi not yet integrated */
    return MB_EXC_NONE;
}
static modbus_exception_t read_lcd_timeout(const modbus_register_map_t *s, uint16_t *o)
{
    (void) s;
    *o = 0u; /* placeholder */
    return MB_EXC_NONE;
}
static modbus_exception_t read_modbus_slave_addr(const modbus_register_map_t *s, uint16_t *o)
{
    *o = (uint16_t) s->cfg_read->get_params()->modbus_slave_addr;
    return MB_EXC_NONE;
}

/* ===================================================================== */
/* Configuration validate handlers                                      */
/* ===================================================================== */

static modbus_exception_t validate_temp_alarm_low(const modbus_register_map_t *s, uint16_t v)
{
    int16_t val = (int16_t) v;
    if (val < TEMP_FIXED_MIN || val > TEMP_FIXED_MAX)
        return MB_EXC_ILLEGAL_DATA_VALUE;
    if (val >= s->cfg_read->get_params()->temp_alarm_high)
        return MB_EXC_ILLEGAL_DATA_VALUE;
    return MB_EXC_NONE;
}
static modbus_exception_t validate_temp_alarm_high(const modbus_register_map_t *s, uint16_t v)
{
    int16_t val = (int16_t) v;
    if (val < TEMP_FIXED_MIN || val > TEMP_FIXED_MAX)
        return MB_EXC_ILLEGAL_DATA_VALUE;
    if (val <= s->cfg_read->get_params()->temp_alarm_low)
        return MB_EXC_ILLEGAL_DATA_VALUE;
    return MB_EXC_NONE;
}
static modbus_exception_t validate_temp_hysteresis(const modbus_register_map_t *s, uint16_t v)
{
    (void) s;
    return (v <= TEMP_HYS_MAX) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t validate_hum_alarm_low(const modbus_register_map_t *s, uint16_t v)
{
    if (v > HUM_FIXED_MAX)
        return MB_EXC_ILLEGAL_DATA_VALUE;
    if (v >= s->cfg_read->get_params()->humidity_alarm_high)
        return MB_EXC_ILLEGAL_DATA_VALUE;
    return MB_EXC_NONE;
}
static modbus_exception_t validate_hum_alarm_high(const modbus_register_map_t *s, uint16_t v)
{
    if (v > HUM_FIXED_MAX)
        return MB_EXC_ILLEGAL_DATA_VALUE;
    if (v <= s->cfg_read->get_params()->humidity_alarm_low)
        return MB_EXC_ILLEGAL_DATA_VALUE;
    return MB_EXC_NONE;
}
static modbus_exception_t validate_hum_hysteresis(const modbus_register_map_t *s, uint16_t v)
{
    (void) s;
    return (v <= HUM_HYS_MAX) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t validate_press_alarm_low(const modbus_register_map_t *s, uint16_t v)
{
    if (v < PRESS_FIXED_MIN || v > PRESS_FIXED_MAX)
        return MB_EXC_ILLEGAL_DATA_VALUE;
    if (v >= s->cfg_read->get_params()->pressure_alarm_high)
        return MB_EXC_ILLEGAL_DATA_VALUE;
    return MB_EXC_NONE;
}
static modbus_exception_t validate_press_alarm_high(const modbus_register_map_t *s, uint16_t v)
{
    if (v < PRESS_FIXED_MIN || v > PRESS_FIXED_MAX)
        return MB_EXC_ILLEGAL_DATA_VALUE;
    if (v <= s->cfg_read->get_params()->pressure_alarm_low)
        return MB_EXC_ILLEGAL_DATA_VALUE;
    return MB_EXC_NONE;
}
static modbus_exception_t validate_press_hysteresis(const modbus_register_map_t *s, uint16_t v)
{
    (void) s;
    return (v <= PRESS_HYS_MAX) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t validate_sampling_period(const modbus_register_map_t *s, uint16_t v)
{
    (void) s;
    return (v >= SAMP_PERIOD_MIN && v <= SAMP_PERIOD_MAX) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t validate_lcd_brightness(const modbus_register_map_t *s, uint16_t v)
{
    (void) s;
    return (v <= LCD_BRIGHT_MAX) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t validate_lcd_timeout(const modbus_register_map_t *s, uint16_t v)
{
    (void) s;
    return (v <= LCD_TIMEOUT_MAX) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t validate_slave_addr(const modbus_register_map_t *s, uint16_t v)
{
    (void) s;
    return (v >= SLAVE_ADDR_MIN && v <= SLAVE_ADDR_MAX) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}

/* ===================================================================== */
/* Configuration write handlers                                         */
/* ===================================================================== */

static modbus_exception_t write_temp_alarm_low(modbus_register_map_t *s, uint16_t v)
{
    int16_t val = (int16_t) v;
    config_service_err_t rc = s->cfg_write->set_param(CONFIG_PARAM_TEMP_ALARM_LOW, &val);
    return (rc == CONFIG_SERVICE_OK) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t write_temp_alarm_high(modbus_register_map_t *s, uint16_t v)
{
    int16_t val = (int16_t) v;
    config_service_err_t rc = s->cfg_write->set_param(CONFIG_PARAM_TEMP_ALARM_HIGH, &val);
    return (rc == CONFIG_SERVICE_OK) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t write_temp_hysteresis(modbus_register_map_t *s, uint16_t v)
{
    int16_t val = (int16_t) v;
    config_service_err_t rc = s->cfg_write->set_param(CONFIG_PARAM_TEMP_HYSTERESIS, &val);
    return (rc == CONFIG_SERVICE_OK) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t write_hum_alarm_low(modbus_register_map_t *s, uint16_t v)
{
    config_service_err_t rc = s->cfg_write->set_param(CONFIG_PARAM_HUMIDITY_ALARM_LOW, &v);
    return (rc == CONFIG_SERVICE_OK) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t write_hum_alarm_high(modbus_register_map_t *s, uint16_t v)
{
    config_service_err_t rc = s->cfg_write->set_param(CONFIG_PARAM_HUMIDITY_ALARM_HIGH, &v);
    return (rc == CONFIG_SERVICE_OK) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t write_hum_hysteresis(modbus_register_map_t *s, uint16_t v)
{
    config_service_err_t rc = s->cfg_write->set_param(CONFIG_PARAM_HUMIDITY_HYSTERESIS, &v);
    return (rc == CONFIG_SERVICE_OK) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t write_press_alarm_low(modbus_register_map_t *s, uint16_t v)
{
    config_service_err_t rc = s->cfg_write->set_param(CONFIG_PARAM_PRESSURE_ALARM_LOW, &v);
    return (rc == CONFIG_SERVICE_OK) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t write_press_alarm_high(modbus_register_map_t *s, uint16_t v)
{
    config_service_err_t rc = s->cfg_write->set_param(CONFIG_PARAM_PRESSURE_ALARM_HIGH, &v);
    return (rc == CONFIG_SERVICE_OK) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t write_press_hysteresis(modbus_register_map_t *s, uint16_t v)
{
    config_service_err_t rc = s->cfg_write->set_param(CONFIG_PARAM_PRESSURE_HYSTERESIS, &v);
    return (rc == CONFIG_SERVICE_OK) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t write_sampling_period(modbus_register_map_t *s, uint16_t v)
{
    uint32_t val = (uint32_t) v;
    config_service_err_t rc = s->cfg_write->set_param(CONFIG_PARAM_POLL_INTERVAL, &val);
    return (rc == CONFIG_SERVICE_OK) ? MB_EXC_NONE : MB_EXC_ILLEGAL_DATA_VALUE;
}
static modbus_exception_t write_lcd_brightness(modbus_register_map_t *s, uint16_t v)
{
    (void) s;
    (void) v;
    return MB_EXC_NONE; /* placeholder — no ConfigParam for LCD */
}
static modbus_exception_t write_lcd_timeout(modbus_register_map_t *s, uint16_t v)
{
    (void) s;
    (void) v;
    return MB_EXC_NONE; /* placeholder */
}
static modbus_exception_t write_modbus_slave_addr_reg(modbus_register_map_t *s, uint16_t v)
{
    uint8_t addr = (uint8_t) v;
    config_service_err_t rc = s->cfg_write->set_param(CONFIG_PARAM_MODBUS_SLAVE_ADDR, &addr);
    if (rc != CONFIG_SERVICE_OK)
    {
        LOG_WARN(LOG_MODULE, "Slave-addr persist failed rc=%d", (int) rc);
        return MB_EXC_ILLEGAL_DATA_VALUE;
    }
    /* Mediator: propagate immediately so current-frame address transitions */
    (void) s->mb_slave->set_address(addr);
    LOG_INFO(LOG_MODULE, "Modbus slave address changed to %u", (unsigned) addr);
    return MB_EXC_NONE;
}

/* ===================================================================== */
/* Command register handlers                                            */
/* ===================================================================== */

static modbus_exception_t read_cmd_ack_alarm(const modbus_register_map_t *s, uint16_t *o)
{
    *o = s->cmd_ack_alarm_last;
    return MB_EXC_NONE;
}
static modbus_exception_t write_cmd_ack_alarm(modbus_register_map_t *s, uint16_t v)
{
    s->cmd_ack_alarm_last = v; /* cache before validation (read-after-write) */
    if (v != 0x0001u)
    {
        return MB_EXC_ILLEGAL_DATA_VALUE;
    }
    s->alarms->ack_all();
    return MB_EXC_NONE;
}

static modbus_exception_t read_cmd_reset_metrics(const modbus_register_map_t *s, uint16_t *o)
{
    *o = s->cmd_reset_metrics_last;
    return MB_EXC_NONE;
}
static modbus_exception_t write_cmd_reset_metrics(modbus_register_map_t *s, uint16_t v)
{
    s->cmd_reset_metrics_last = v;
    if (v != 0x0001u)
    {
        return MB_EXC_ILLEGAL_DATA_VALUE;
    }
    /* Zero the cached snapshot so next poll doesn't compute a false delta */
    memset(&s->last_stats_snapshot, 0, sizeof s->last_stats_snapshot);
    s->lifecycle->handle_remote_command(LC_REMOTE_CMD_RESET_METRICS);
    return MB_EXC_NONE;
}

static modbus_exception_t read_cmd_soft_restart(const modbus_register_map_t *s, uint16_t *o)
{
    *o = s->cmd_soft_restart_last;
    return MB_EXC_NONE;
}
static modbus_exception_t write_cmd_soft_restart(modbus_register_map_t *s, uint16_t v)
{
    s->cmd_soft_restart_last = v;
    if (v != 0xA5A5u)
    {
        LOG_WARN(LOG_MODULE, "CMD_SOFT_RESTART rejected: bad magic 0x%04X", (unsigned) v);
        return MB_EXC_ILLEGAL_DATA_VALUE;
    }
    s->lifecycle->handle_remote_command(LC_REMOTE_CMD_SOFT_RESTART);
    return MB_EXC_NONE;
}

/* ===================================================================== */
/* Vtable dispatch functions (static; exposed through get_iface)        */
/* ===================================================================== */

static modbus_exception_t mrm_read_input_regs(void *ctx, uint16_t addr, uint16_t count,
                                              uint16_t *out_buf)
{
    modbus_register_map_t *self = (modbus_register_map_t *) ctx;
    if (!self->initialised)
    {
        return MB_EXC_ILLEGAL_DATA_VALUE;
    }
    if (count == 0u || count > MRM_MAX_REGS_PER_READ)
    {
        return MB_EXC_ILLEGAL_DATA_VALUE;
    }
    for (uint16_t i = 0u; i < count; i++)
    {
        uint16_t a = (uint16_t) (addr + i);
        const reg_slot_t *slot = find_slot(a);
        if (slot == NULL || slot->access != REG_ACCESS_R || slot->read_fn == NULL)
        {
            return MB_EXC_ILLEGAL_DATA_ADDR;
        }
        modbus_exception_t rc = slot->read_fn(self, &out_buf[i]);
        if (rc != MB_EXC_NONE)
        {
            return rc;
        }
    }
    return MB_EXC_NONE;
}

static modbus_exception_t mrm_read_holding_regs(void *ctx, uint16_t addr, uint16_t count,
                                                uint16_t *out_buf)
{
    modbus_register_map_t *self = (modbus_register_map_t *) ctx;
    if (!self->initialised)
    {
        return MB_EXC_ILLEGAL_DATA_VALUE;
    }
    if (count == 0u || count > MRM_MAX_REGS_PER_READ)
    {
        return MB_EXC_ILLEGAL_DATA_VALUE;
    }
    for (uint16_t i = 0u; i < count; i++)
    {
        uint16_t a = (uint16_t) (addr + i);
        const reg_slot_t *slot = find_slot(a);
        if (slot == NULL || slot->access != REG_ACCESS_RW || slot->read_fn == NULL)
        {
            return MB_EXC_ILLEGAL_DATA_ADDR;
        }
        modbus_exception_t rc = slot->read_fn(self, &out_buf[i]);
        if (rc != MB_EXC_NONE)
        {
            return rc;
        }
    }
    return MB_EXC_NONE;
}

static modbus_exception_t mrm_write_single_reg(void *ctx, uint16_t addr, uint16_t value)
{
    modbus_register_map_t *self = (modbus_register_map_t *) ctx;
    if (!self->initialised)
    {
        return MB_EXC_ILLEGAL_DATA_VALUE;
    }
    const reg_slot_t *slot = find_slot(addr);
    if (slot == NULL || slot->access != REG_ACCESS_RW || slot->write_fn == NULL)
    {
        return MB_EXC_ILLEGAL_DATA_ADDR;
    }
    if (slot->validate_fn != NULL)
    {
        modbus_exception_t rc = slot->validate_fn(self, value);
        if (rc != MB_EXC_NONE)
        {
            return rc;
        }
    }
    return slot->write_fn(self, value);
}

static modbus_exception_t mrm_write_multiple_regs(void *ctx, uint16_t addr, uint16_t count,
                                                  const uint16_t *values)
{
    modbus_register_map_t *self = (modbus_register_map_t *) ctx;
    if (!self->initialised)
    {
        return MB_EXC_ILLEGAL_DATA_VALUE;
    }
    if (count == 0u || count > MRM_MAX_REGS_PER_WRITE)
    {
        return MB_EXC_ILLEGAL_DATA_VALUE;
    }

    /* Phase 1: pre-validate all — no side effects */
    for (uint16_t i = 0u; i < count; i++)
    {
        uint16_t a = (uint16_t) (addr + i);
        const reg_slot_t *slot = find_slot(a);
        if (slot == NULL || slot->access != REG_ACCESS_RW || slot->write_fn == NULL)
        {
            return MB_EXC_ILLEGAL_DATA_ADDR;
        }
        if (slot->validate_fn != NULL)
        {
            modbus_exception_t rc = slot->validate_fn(self, values[i]);
            if (rc != MB_EXC_NONE)
            {
                return rc;
            }
        }
    }

    /* Phase 2: apply all */
    for (uint16_t i = 0u; i < count; i++)
    {
        uint16_t a = (uint16_t) (addr + i);
        const reg_slot_t *slot = find_slot(a);
        modbus_exception_t rc = slot->write_fn(self, values[i]);
        if (rc != MB_EXC_NONE)
        {
            LOG_ERROR(LOG_MODULE, "FC16 phase-2 fail at 0x%04X rc=%02X", (unsigned) a,
                      (unsigned) rc);
            return rc;
        }
    }
    return MB_EXC_NONE;
}

/* ===================================================================== */
/* Vtable template (ctx patched at get_iface time)                      */
/* ===================================================================== */

static const imodbus_register_map_t k_iface_template = {
    NULL, mrm_read_input_regs, mrm_read_holding_regs, mrm_write_single_reg, mrm_write_multiple_regs,
};

/* ===================================================================== */
/* Public API implementation                                             */
/* ===================================================================== */

modbus_register_map_err_t
modbus_register_map_init(modbus_register_map_t *self, const isensor_service_t *sensors,
                         const ialarm_service_t *alarms, const iconfig_provider_t *cfg_read,
                         iconfig_manager_t *cfg_write, const ihealth_snapshot_t *health_read,
                         ihealth_report_t *health_write, const itime_provider_t *time,
                         const imodbus_slave_stats_t *mb_stats, imodbus_slave_t *mb_slave,
						 const ilifecycle_t *lifecycle)
{
    if (self == NULL || sensors == NULL || alarms == NULL || cfg_read == NULL ||
        cfg_write == NULL || health_read == NULL || health_write == NULL || time == NULL ||
        mb_stats == NULL || mb_slave == NULL || lifecycle == NULL)
    {
        return MRM_ERR_NULL_ARG;
    }

    /* Debug-only sort invariant check */
    for (uint16_t i = 1u; i < K_SLOTS_COUNT; i++)
    {
        if (k_slots[i].addr <= k_slots[i - 1u].addr)
        {
            LOG_ERROR(LOG_MODULE, "Slot table unsorted at index %u", (unsigned) i);
            return MRM_ERR_INVARIANT;
        }
    }

    self->sensors = sensors;
    self->alarms = alarms;
    self->cfg_read = cfg_read;
    self->cfg_write = cfg_write;
    self->health_read = health_read;
    self->health_write = health_write;
    self->time = time;
    self->mb_stats = mb_stats;
    self->mb_slave = mb_slave;
    self->lifecycle = lifecycle;

    self->cmd_ack_alarm_last = 0u;
    self->cmd_reset_metrics_last = 0u;
    self->cmd_soft_restart_last = 0u;
    memset(&self->last_stats_snapshot, 0, sizeof self->last_stats_snapshot);

    self->initialised = true;
    return MRM_ERR_OK;
}

void modbus_register_map_get_iface(modbus_register_map_t *self, imodbus_register_map_t *iface)
{
    *iface = k_iface_template;
    iface->ctx = self;
}

modbus_register_map_err_t modbus_register_map_poll_stats(modbus_register_map_t *self)
{
    if (self == NULL || !self->initialised)
    {
        return MRM_ERR_NOT_INIT;
    }
    modbus_slave_stats_t now;
    self->mb_stats->snapshot(&now);
    self->health_write->update_modbus_slave_stats(&now);
    self->last_stats_snapshot = now;
    return MRM_ERR_OK;
}

modbus_register_map_t *modbus_register_map_instance(void)
{
    return &s_mrm;
}

/* ===================================================================== */
/* Test-only hooks                                                       */
/* ===================================================================== */

#ifdef TEST
static modbus_register_map_t s_mrm;

modbus_register_map_t *modbus_register_map_get_test_instance(void)
{
    return &s_mrm;
}
void modbus_register_map_reset_for_test(void)
{
    memset(&s_mrm, 0, sizeof s_mrm);
}
#endif /* TEST */
