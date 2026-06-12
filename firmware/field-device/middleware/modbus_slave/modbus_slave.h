/**
 * @file modbus_slave.h
 * @brief ModbusSlave — Modbus RTU slave protocol for the Field Device.
 *
 * Implements IModbusSlave and IModbusSlaveStats (components.md middleware
 * layer). Receives frames from ModbusUartDriver, validates address and CRC,
 * dispatches FC03/04/06/16 to the injected IModbusRegisterMap vtable, and
 * transmits the response.
 *
 * Design decisions:
 *   - Reactive singleton — no thread ownership, no timers, no retries.
 *   - Two-phase coupling: modbus_slave_init() registers callback on
 *     ModbusUartDriver; ModbusTask calls modbus_slave_process() each time
 *     the callback fires.
 *   - IModbusRegisterMap is injected at init (P2 DIP); the interface is
 *     owned by the Application layer but declared here alongside the error
 *     type it references.
 *
 * @note See docs/lld/middleware/modbus-slave.md for the full design.
 */

#ifndef MODBUS_SLAVE_H
#define MODBUS_SLAVE_H

#include <stdint.h>
#include <stdbool.h>

#include "FreeRTOS.h"
#include "task.h"
/* ===================================================================== */
/* Test-visibility macro                                                  */
/* ===================================================================== */

#ifdef TEST
#define MODBUS_SLAVE_TEST_VISIBLE
#else
#define MODBUS_SLAVE_TEST_VISIBLE static
#endif

/* ===================================================================== */
/* Error codes                                                           */
/* ===================================================================== */

/**
 * @brief Error codes returned by ModbusSlave operations.
 *
 * Internal Modbus protocol errors (invalid address, value range) are mapped
 * to Modbus exception codes in the response PDU rather than surfaced here —
 * except to the internal helpers that build the response.
 */
typedef enum
{
    MODBUS_SLAVE_ERR_OK = 0,       /**< Operation succeeded. */
    MODBUS_SLAVE_ERR_NOT_INIT = 1, /**< Called before modbus_slave_init(). */
    MODBUS_SLAVE_ERR_NULL_ARG = 2, /**< NULL pointer argument. */
    MODBUS_SLAVE_ERR_INVALID_ADDR =
        3, /**< Register address out of range; maps to exception 0x02. */
    MODBUS_SLAVE_ERR_INVALID_VALUE = 4, /**< Register value out of range; maps to exception 0x03. */
    MODBUS_SLAVE_ERR_DEVICE_FAIL =
        5, /**< Register map returned device failure; maps to exception 0x04. */
} modbus_slave_err_t;

/* ===================================================================== */
/* Statistics                                                            */
/* ===================================================================== */

/**
 * @brief Cumulative protocol statistics counters.
 *
 * Polled by ModbusRegisterMap to expose reliability counters via Modbus
 * (Metric Producer Pattern — components.md §Metric Producer Pattern).
 */
typedef struct
{
    uint32_t valid_frames;         /**< Frames received with correct address + CRC. */
    uint32_t crc_errors;           /**< Frames discarded due to CRC mismatch. */
    uint32_t address_mismatches;   /**< Frames discarded — not our address. */
    uint32_t exception_responses;  /**< Responses sent carrying an exception code. */
    uint32_t unsupported_fc;       /**< Exception 0x01 triggered (unknown FC). */
    uint32_t successful_responses; /**< Normal (non-exception) responses sent. */
} modbus_slave_stats_t;

/* ===================================================================== */
/* IModbusRegisterMap — injected vtable (P2 DIP)                        */
/* ===================================================================== */

/**
 * @brief Register-read/write callbacks supplied by the Application layer.
 *
 * Owned conceptually by the Application layer (ModbusRegisterMap); declared
 * here because the function-pointer types reference modbus_slave_err_t.
 * The concrete binding is established in LifecycleController at init.
 */
typedef modbus_slave_err_t (*fn_read_input_reg_t)(uint16_t addr, uint16_t *value_out);
typedef modbus_slave_err_t (*fn_read_holding_reg_t)(uint16_t addr, uint16_t *value_out);
typedef modbus_slave_err_t (*fn_write_holding_reg_t)(uint16_t addr, uint16_t value);

typedef struct
{
    fn_read_input_reg_t read_input;       /**< FC04 dispatch. */
    fn_read_holding_reg_t read_holding;   /**< FC03 dispatch. */
    fn_write_holding_reg_t write_holding; /**< FC06 / FC16 dispatch. */
} IModbusRegisterMap;

/* ===================================================================== */
/* IModbusSlave — protocol execution                                     */
/* ===================================================================== */

/**
 * @brief Initialise ModbusSlave.
 *
 * Validates arguments, stores the injected vtable and task handle, registers
 * the frame-complete callback with ModbusUartDriver, and sets initialised.
 * ModbusTask must be created before this call so the callback has a valid
 * handle to notify.
 *
 * @param  reg_map     Injected IModbusRegisterMap vtable (must not be NULL).
 * @param  slave_addr  Initial slave address (1..247, REQ-MB-100).
 * @param  task_handle FreeRTOS task handle for direct-to-task notification.
 * @return MODBUS_SLAVE_ERR_OK or MODBUS_SLAVE_ERR_NULL_ARG /
 *         MODBUS_SLAVE_ERR_INVALID_ADDR.
 * @note   Threading: task-context only, non-blocking.
 */
modbus_slave_err_t modbus_slave_init(const IModbusRegisterMap *reg_map, uint8_t slave_addr,
                                     TaskHandle_t task_handle);

/**
 * @brief Process one received frame.
 *
 * Called from ModbusTask after receiving a direct-to-task notification from
 * the frame-complete callback. Executes the full state machine cycle:
 * address filter → CRC check → FC dispatch → build response → transmit.
 *
 * @return MODBUS_SLAVE_ERR_OK (silent drop is not an error at this level).
 * @note   Threading: task-context only. Not ISR-safe.
 */
modbus_slave_err_t modbus_slave_process(void);

/**
 * @brief Update the slave address filter at runtime.
 *
 * Called when the Modbus address is changed via ConfigService. Updates
 * s_slave.slave_addr under a critical section to guarantee atomicity.
 *
 * @param  new_addr  New slave address (1..247).
 * @return MODBUS_SLAVE_ERR_OK or MODBUS_SLAVE_ERR_INVALID_ADDR.
 */
modbus_slave_err_t modbus_slave_set_address(uint8_t new_addr);

/* ===================================================================== */
/* IModbusSlaveStats — statistics read-back                             */
/* ===================================================================== */

/**
 * @brief Copy the current statistics snapshot.
 *
 * Thread-safe: copy is performed under a critical section.
 *
 * @param[out] stats_out  Filled with counters snapshot (must not be NULL).
 * @return MODBUS_SLAVE_ERR_OK or MODBUS_SLAVE_ERR_NULL_ARG.
 */
modbus_slave_err_t modbus_slave_get_stats(modbus_slave_stats_t *stats_out);

/**
 * @brief Reset all statistics counters to zero.
 *
 * Thread-safe: reset is performed under a critical section.
 *
 * @return MODBUS_SLAVE_ERR_OK on success.
 */
modbus_slave_err_t modbus_slave_reset_stats(void);

/* ===================================================================== */
/* Test-only hooks                                                       */
/* ===================================================================== */

#ifdef TEST
/**
 * @brief Reset all module state to the power-on default.
 *
 * Call from setUp() in unit tests. Not present in production builds.
 */
void modbus_slave_reset_for_test(void);
#endif /* TEST */

#endif /* MODBUS_SLAVE_H */
