/**
 * @file modbus_register_map.h
 * @brief ModbusRegisterMap — Application-layer Modbus register dispatcher.
 *
 * Implements the DIP interface imodbus_register_map_t consumed by ModbusSlave.
 * Acts as Mediator between ModbusSlave and all data providers (SensorService,
 * AlarmService, ConfigService, HealthMonitor, TimeProvider).
 *
 * @see docs/lld/application/modbus-register-map-lld.md
 * @see docs/hld/modbus-register-map.md (register data contract)
 */

#ifndef MODBUS_REGISTER_MAP_H
#define MODBUS_REGISTER_MAP_H

#include <stdint.h>
#include <stdbool.h>

/* ===================================================================== */
/* Modbus exception codes                                               */
/* ===================================================================== */

#ifndef MODBUS_EXCEPTION_DEFINED
#define MODBUS_EXCEPTION_DEFINED
typedef enum
{
    MB_EXC_NONE = 0x00,
    MB_EXC_ILLEGAL_FUNCTION = 0x01,
    MB_EXC_ILLEGAL_DATA_ADDR = 0x02,
    MB_EXC_ILLEGAL_DATA_VALUE = 0x03,
} modbus_exception_t;
#endif /* MODBUS_EXCEPTION_DEFINED */

/* ===================================================================== */
/* Error codes                                                          */
/* ===================================================================== */

typedef enum
{
    MRM_ERR_OK = 0,
    MRM_ERR_NULL_ARG = 1,
    MRM_ERR_NOT_INIT = 2,
    MRM_ERR_INVARIANT = 3,
    MRM_ERR_PROVIDER_FAIL = 4,
} modbus_register_map_err_t;

/* ===================================================================== */
/* Register address constants                                           */
/* ===================================================================== */

/* MODBUS_SLAVE_ADDR is placed at 0x0150 (first address of the reserved
 * configuration range) to support the Mediator role. The HLD data-spec
 * lists 0x0150–0x01FF as reserved; this is a known deviation.          */
#define MRM_REG_MODBUS_SLAVE_ADDR ((uint16_t) 0x0150U)

/* Max registers per single Modbus PDU (per protocol spec §4.3) */
#define MRM_MAX_REGS_PER_READ ((uint16_t) 125U)
#define MRM_MAX_REGS_PER_WRITE ((uint16_t) 123U)

/* ===================================================================== */
/* Opaque type — concrete definition in modbus_register_map.c           */
/* ===================================================================== */

typedef struct modbus_register_map modbus_register_map_t;

/* ===================================================================== */
/* DIP interface — imodbus_register_map_t (Application-owned)          */
/* ===================================================================== */

/* Companion §4: bulk read/write vtable consumed by ModbusSlave.
 * Named imodbus_register_map_t (P10 snake_case) to distinguish from the
 * per-register IModbusRegisterMap typedef in modbus_slave.h. Both will
 * be unified in a future ModbusSlave refactor (MRM-DEVIATION-001).     */

struct imodbus_register_map_s;
typedef struct imodbus_register_map_s
{
    void *ctx; /* concrete modbus_register_map_t * */

    /* cppcheck-suppress unusedStructMember -- called by ModbusSlave FC04 */
    modbus_exception_t (*read_input_regs)(void *ctx, uint16_t addr, uint16_t count,
                                          uint16_t *out_buf);

    /* cppcheck-suppress unusedStructMember -- called by ModbusSlave FC03 */
    modbus_exception_t (*read_holding_regs)(void *ctx, uint16_t addr, uint16_t count,
                                            uint16_t *out_buf);

    /* cppcheck-suppress unusedStructMember -- called by ModbusSlave FC06 */
    modbus_exception_t (*write_single_reg)(void *ctx, uint16_t addr, uint16_t value);

    /* cppcheck-suppress unusedStructMember -- called by ModbusSlave FC16 */
    modbus_exception_t (*write_multiple_regs)(void *ctx, uint16_t addr, uint16_t count,
                                              const uint16_t *values);
} imodbus_register_map_t;

/* ===================================================================== */
/* Remote command identifiers (for LifecycleController routing)         */
/* ===================================================================== */

#include "lifecycle_controller/ilifecycle.h"

/* ilifecycle.h already defined the enumerators — alias the type. */
typedef lifecycle_remote_cmd_t lifecycle_remote_cmd_t;

/* ===================================================================== */
/* Provider / dependency types pulled in for the init signature         */
/* ===================================================================== */

#ifndef TEST
#include "sensor_service/sensor_service.h"
#include "alarm_service/alarm_service.h"
#include "config_service/config_service.h"
#include "health_monitor/ihealth_report.h"
#include "health_monitor/ihealth_snapshot.h"
#include "time_provider/time_provider.h"
#else
#include "mrm_deps_stub.h"
#endif /* TEST */

/* ===================================================================== */
/* New vtable types (injected into MRM; not yet in modbus_slave.h)      */
/* ===================================================================== */

/* imodbus_slave_stats_t: snapshots ModbusSlave counters for Modbus regs */
typedef struct
{
    /* cppcheck-suppress unusedStructMember -- called by modbus_register_map_poll_stats */
    void (*snapshot)(modbus_slave_stats_t *out);
} imodbus_slave_stats_t;

/* imodbus_slave_t: leaf header — definition lives with the implementor */
#include "modbus_slave/imodbus_slave.h"

/* ===================================================================== */
/* Public API                                                           */
/* ===================================================================== */

/**
 * @brief Initialise ModbusRegisterMap.
 *
 * Stores all provider pointers; zeroes command-cell cache and stats snapshot.
 * Asserts the slot-table sort invariant in debug builds.
 *
 * @param  self          MRM instance (statically allocated by caller).
 * @param  sensors       ISensorService handle. Must not be NULL.
 * @param  alarms        IAlarmService handle. Must not be NULL.
 * @param  cfg_read      IConfigProvider handle. Must not be NULL.
 * @param  cfg_write     IConfigManager handle. Must not be NULL.
 * @param  health_read   IHealthSnapshot handle. Must not be NULL.
 * @param  health_write  IHealthReport handle. Must not be NULL.
 * @param  time          ITimeProvider handle. Must not be NULL.
 * @param  mb_stats      IModbusSlaveStats handle. Must not be NULL.
 * @param  mb_slave      IModbusSlave handle. Must not be NULL.
 * @param  lifecycle     ILifecycleController handle. Must not be NULL.
 * @return MRM_ERR_OK on success; MRM_ERR_NULL_ARG if any pointer is NULL;
 *         MRM_ERR_INVARIANT if slot-table order is broken (debug only).
 */
modbus_register_map_err_t
modbus_register_map_init(modbus_register_map_t *self, const isensor_service_t *sensors,
                         const ialarm_service_t *alarms, const iconfig_provider_t *cfg_read,
                         iconfig_manager_t *cfg_write, const ihealth_snapshot_t *health_read,
                         ihealth_report_t *health_write, const itime_provider_t *time,
                         const imodbus_slave_stats_t *mb_stats, imodbus_slave_t *mb_slave,
                         const ilifecycle_t *lifecycle);

/**
 * @brief Build the imodbus_register_map_t vtable for a given instance.
 *
 * Called after init to get the interface pointer to inject into ModbusSlave.
 *
 * @param  self   Initialised MRM instance.
 * @param  iface  Vtable struct to fill.
 */
void modbus_register_map_get_iface(modbus_register_map_t *self, imodbus_register_map_t *iface);

/**
 * @brief Poll ModbusSlave statistics and push to HealthMonitor.
 *
 * Called by ModbusSlaveTask at 1 Hz via a FreeRTOS software timer notification
 * (per LLD §9.1). Snapshots the current stats counters, calls
 * IHealthReport.update_modbus_slave_stats(), and caches the snapshot for
 * subsequent FC04 register reads (Metric Producer Pattern).
 *
 * @param  self  Initialised MRM instance.
 * @return MRM_ERR_OK or MRM_ERR_NOT_INIT.
 */
modbus_register_map_err_t modbus_register_map_poll_stats(modbus_register_map_t *self);

/**
 * @brief  Return the singleton ModbusRegisterMap instance.
 *
 * The struct is opaque to consumers; this accessor returns a pointer
 * for use as the first argument to modbus_register_map_init() and
 * the IModbusRegisterMap vtable handlers.
 *
 * @return Non-NULL pointer to the static singleton.
 */
modbus_register_map_t *modbus_register_map_instance(void);

/* ===================================================================== */
/* Test-only hooks                                                       */
/* ===================================================================== */

#ifdef TEST
/**
 * @brief Reset module state to post-BSS defaults (call from setUp()).
 */
void modbus_register_map_reset_for_test(void);

/**
 * @brief Return pointer to the module-internal static test instance.
 *
 * Allows test TUs to obtain a valid modbus_register_map_t * without
 * needing to know the struct size (which is opaque in the public header).
 */
modbus_register_map_t *modbus_register_map_get_test_instance(void);
#endif /* TEST */

#endif /* MODBUS_REGISTER_MAP_H */
