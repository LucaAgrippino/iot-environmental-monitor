/**
 * @file modbus_poller.h
 * @brief ModbusPoller (Gateway application) — polling cycle, link-state hysteresis, command
 * routing.
 *
 * Provides IModbusPoller.  Platform singleton (MBP-D1): plain free functions,
 * one owning task (ModbusPollerTask).  ModbusPoller is the single Modbus bus
 * gatekeeper: other components post writes through modbus_poller_send_command().
 *
 * @note Companion: docs/lld/application/modbus-poller-lld.md
 */

#ifndef MODBUS_POLLER_H
#define MODBUS_POLLER_H

#include <stdbool.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "queue.h"

#include "health_monitor/health_monitor.h"
#include "modbus_master/modbus_master.h"

/** @brief Error codes returned by ModbusPoller operations. */
typedef enum
{
    MODBUS_POLLER_ERR_OK = 0,          /**< Success. */
    MODBUS_POLLER_ERR_NOT_INIT = 1,    /**< init() not called. */
    MODBUS_POLLER_ERR_NULL_ARG = 2,    /**< NULL pointer argument. */
    MODBUS_POLLER_ERR_QUEUE_FULL = 3,  /**< Command queue full; command dropped. */
    MODBUS_POLLER_ERR_REJECTED = 4,    /**< Slave not in the allowlist. */
    MODBUS_POLLER_ERR_INVALID_ARG = 5, /**< Bad function code, register count or period (MBP-D6). */
} modbus_poller_err_t;

/** @brief Function code of Write Single Register. */
#define MODBUS_POLLER_FC_WRITE_SINGLE (0x06U)
/** @brief Function code of Write Multiple Registers. */
#define MODBUS_POLLER_FC_WRITE_MULTIPLE (0x10U)
/** @brief Register values a command can carry (FC16 limit is 123). */
#define MODBUS_POLLER_CMD_MAX_REGS (125U)
/** @brief Largest register count of an FC16 command. */
#define MODBUS_POLLER_FC16_MAX_COUNT (123U)

/**
 * @brief Command posted to ModbusPoller by other application components.
 *
 * Field order differs from the companion: response_queue comes first so the
 * pointer sits inside the first 256 bytes the host FreeRTOS mock captures
 * (MBP-D6).  Use designated initialisers.
 */
typedef struct
{
    QueueHandle_t response_queue;              /**< Receives a modbus_command_response_t. */
    uint8_t slave_addr;                        /**< Target slave; must be provisioned. */
    uint8_t function_code;                     /**< 0x06 or 0x10. */
    uint16_t start_reg;                        /**< First holding register. */
    uint16_t reg_count;                        /**< FC16: 1..123.  FC06: ignored. */
    uint16_t data[MODBUS_POLLER_CMD_MAX_REGS]; /**< Values; FC06 uses data[0]. */
} modbus_command_t;

/** @brief Result of a command, posted to the caller's response queue. */
typedef struct
{
    modbus_master_err_t status;                /**< Outcome of the transaction. */
    uint16_t data[MODBUS_POLLER_CMD_MAX_REGS]; /**< Response payload as big-endian words. */
    uint16_t data_len;                         /**< Valid words in data[]. */
} modbus_command_response_t;

/** @brief Whether a slave has proved its identity against its profile (REQ-MB-120). */
typedef enum
{
    MODBUS_SLAVE_UNVERIFIED = 0, /**< Not checked yet, or must be checked again.            */
    MODBUS_SLAVE_VERIFIED,       /**< Map version and vendor code match the profile.         */
    MODBUS_SLAVE_REJECTED        /**< Mismatch or unsupported map version; not polled for data. */
} modbus_slave_identity_t;

/** @brief Per-slave runtime state; exposed for test observability only. */
typedef struct
{
    uint8_t slave_addr;
    uint8_t consecutive_successes;
    uint8_t consecutive_failures;
    bool link_online;
    modbus_slave_identity_t identity;
    uint8_t recheck_cycles; /**< Cycles until a rejected slave is checked again. */
} modbus_slave_state_t;

/**
 * @brief Initialise ModbusPoller.
 *
 * Creates the command queue (static allocation).  Call after modbus_master_init()
 * and after DeviceProfileRegistry is loaded.
 *
 * @param[in] poll_period_ms  Poll period in ms, at least 1 (REQ-NF-110 default: 1000).
 * @param[in] health          HealthMonitor handle receiving stats and link events.
 * @return MODBUS_POLLER_ERR_OK on success; MODBUS_POLLER_ERR_NULL_ARG if @p health is NULL;
 *         MODBUS_POLLER_ERR_INVALID_ARG if the period is 0.
 * @note Threading: call before the scheduler starts.
 */
modbus_poller_err_t modbus_poller_init(uint32_t poll_period_ms, health_monitor_handle_t health);

/**
 * @brief Report whether modbus_poller_init() has succeeded.
 *
 * LifecycleController's SelfChecking probe (REQ-DM-040). "Ready" means initialised and able to
 * accept commands; it does not mean a Field Device answers (the link may legitimately be down at
 * boot) and it does not mean the task has started, because ModbusPollerTask waits on the start
 * gate that opens only after this probe passes.
 *
 * @return true after a successful modbus_poller_init(); false before it or after a failed one.
 * @note Threading: any task.
 */
bool modbus_poller_is_ready(void);

/**
 * @brief Post a command for ModbusPoller to execute on behalf of the caller.
 *
 * Non-blocking.  The command is copied into the queue and executed before the
 * next poll (MBP-D4).  The caller waits on cmd->response_queue for the result.
 *
 * @param[in] cmd  Command; copied by value.  response_queue must stay valid until
 *                 the response arrives.
 * @return MODBUS_POLLER_ERR_OK, MODBUS_POLLER_ERR_NOT_INIT, MODBUS_POLLER_ERR_NULL_ARG,
 *         MODBUS_POLLER_ERR_INVALID_ARG (function code or register count),
 *         MODBUS_POLLER_ERR_REJECTED (slave not provisioned) or
 *         MODBUS_POLLER_ERR_QUEUE_FULL.
 * @note Threading: callable from any task.
 */
modbus_poller_err_t modbus_poller_send_command(const modbus_command_t *cmd);

/**
 * @brief Latest decoded Field Device sensor registers (0x0010 to 0x0012), in register units.
 *
 * The values keep the units of the register map (and of SensorService and config_params_t), so
 * there is one unit system inside the Gateway; the consumer converts to its own wire units.
 */
typedef struct
{
    int16_t temperature_centi_c; /**< x0.01 degC; meaningful only if temperature_valid.  */
    uint16_t humidity_centi_pct; /**< x0.01 %RH; meaningful only if humidity_valid.      */
    uint16_t pressure_deci_hpa;  /**< x0.1 hPa; meaningful only if pressure_valid.       */
    bool temperature_valid;      /**< false: sentinel 0x8000, or never received.         */
    bool humidity_valid;         /**< false: sentinel 0xFFFF, or never received.         */
    bool pressure_valid;         /**< false: sentinel 0xFFFF, or never received.         */
    bool link_online;            /**< Hysteresis state (REQ-NF-103/-104/-215).           */
    bool has_data;               /**< At least one response decoded since boot.          */
    uint32_t updated_tick;       /**< xTaskGetTickCount() of the last decode.            */
} modbus_fd_reading_t;

/**
 * @brief Change the poll period while running (WP12 D-13); the next cycle uses it.
 * @return MODBUS_POLLER_ERR_OK; _NOT_INIT; _INVALID_ARG if @p poll_period_ms is 0.
 * @note Threading: any task; one aligned 32-bit store.
 */
modbus_poller_err_t modbus_poller_set_poll_period(uint32_t poll_period_ms);

/**
 * @brief Copy the latest decoded readings of one slave (CP-D18, closes MBP-O3).
 *
 * A poll failure does not clear the values; the consumer decides what "offline" means from
 * @c link_online (REQ-SA-160 semantics stay with the consumer).
 *
 * @param[in]  slave_addr  Provisioned slave address.
 * @param[out] out         Receives the copy.
 * @return MODBUS_POLLER_ERR_OK; MODBUS_POLLER_ERR_NOT_INIT before modbus_poller_init();
 *         MODBUS_POLLER_ERR_NULL_ARG if @p out is NULL; MODBUS_POLLER_ERR_REJECTED if the slave is
 *         not provisioned. A provisioned slave with no response yet returns OK with has_data
 *         false and every channel invalid.
 * @note Threading: any task. A short critical section guards the copy (about 20 bytes).
 */
modbus_poller_err_t modbus_poller_get_fd_reading(uint8_t slave_addr, modbus_fd_reading_t *out);

/**
 * @brief Make the next poll cycle check a slave's identity again (REQ-DM-101).
 *
 * Call after a profile for @p slave_addr was added or updated at runtime (the registry does not
 * call the poller). The slave is marked unverified, so before its next data poll the poller repeats
 * the identity check against the profile as it now is. A rejected slave is re-checked at once.
 *
 * @param[in] slave_addr  Provisioned slave address.
 * @return MODBUS_POLLER_ERR_OK; MODBUS_POLLER_ERR_NOT_INIT before modbus_poller_init();
 *         MODBUS_POLLER_ERR_REJECTED if the address is not provisioned.
 * @note Threading: any task.
 */
modbus_poller_err_t modbus_poller_reprobe(uint8_t slave_addr);

/**
 * @brief ModbusPoller main loop — the ModbusPollerTask entry point.
 *
 * Completes ModbusMaster's two-phase init with modbus_master_start() (MBP-D3), then
 * runs the drain / poll / report / delay cycle indefinitely.  Never returns.
 *
 * @note Threading: call from ModbusPollerTask only.
 */
void modbus_poller_run(void);

#ifdef TEST
/**
 * @brief Reset all module state (unit tests only).
 */
void modbus_poller_reset_for_test(void);

/**
 * @brief Run one drain / poll / report cycle without the delay (unit tests only).
 */
void modbus_poller_cycle_for_test(void);

/**
 * @brief Read one slot of the slave state table (unit tests only).
 *
 * @param[in] slot  Table index, 0..3.
 * @return The slot, or NULL if out of range.
 */
const modbus_slave_state_t *modbus_poller_slave_state_for_test(uint8_t slot);

/**
 * @brief Mark a slave verified without the identity read, claiming its slot (unit tests only).
 */
void modbus_poller_trust_slave_for_test(uint8_t slave_addr);
#endif /* TEST */

#endif /* MODBUS_POLLER_H */
