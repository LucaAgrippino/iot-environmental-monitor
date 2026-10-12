/**
 * @file modbus_master.h
 * @brief ModbusMaster (Gateway middleware) — Modbus RTU master transactions with retry.
 *
 * Provides IModbusMaster (init / start / transact) and IModbusMasterStats
 * (get_stats / reset_stats).  Platform singleton (MBMP-D1): plain free
 * functions, one instance, one owning task (ModbusPollerTask).
 *
 * Two-phase initialisation (MBMP-D3):
 *   1. modbus_master_init()  — before the scheduler, after modbus_uart_init().
 *   2. modbus_master_start() — from the owning task; registers the UART RX callback.
 *
 * @note Companion: docs/lld/middleware/modbus-master-poller.md
 */

#ifndef MODBUS_MASTER_H
#define MODBUS_MASTER_H

#include <stdint.h>

#include "modbus_frame.h"

/** @brief Error codes returned by ModbusMaster operations. */
typedef enum
{
    MODBUS_MASTER_ERR_OK = 0,              /**< Success. */
    MODBUS_MASTER_ERR_NOT_INIT = 1,        /**< init() or start() not called. */
    MODBUS_MASTER_ERR_NULL_ARG = 2,        /**< NULL pointer argument. */
    MODBUS_MASTER_ERR_TIMEOUT = 3,         /**< No response after all retries. */
    MODBUS_MASTER_ERR_CRC = 4,             /**< Response CRC mismatch after all retries. */
    MODBUS_MASTER_ERR_BAD_RESPONSE = 5,    /**< Runt frame, or address / function mismatch. */
    MODBUS_MASTER_ERR_EXCEPTION = 6,       /**< Slave returned an exception PDU. */
    MODBUS_MASTER_ERR_RX_ERROR = 7,        /**< UART framing / overrun error after all retries. */
    MODBUS_MASTER_ERR_INVALID_ARG = 8,     /**< Value out of range (MBMP-D9). */
    MODBUS_MASTER_ERR_ALREADY_STARTED = 9, /**< start() called twice (MBMP-D9). */
} modbus_master_err_t;

/** @brief Transaction counters exposed through IModbusMasterStats. */
typedef struct
{
    uint32_t transactions_sent;   /**< Requests transmitted, one per attempt. */
    uint32_t transactions_ok;     /**< Valid responses received. */
    uint32_t timeouts;            /**< Per-attempt timeouts, including TX failures. */
    uint32_t crc_errors;          /**< Responses with a bad CRC. */
    uint32_t rx_errors;           /**< UART framing / overrun errors. */
    uint32_t exception_responses; /**< Exception PDUs received. */
    uint32_t bad_responses;       /**< Runt frames and address / function mismatches. */
} modbus_master_stats_t;

/**
 * @brief Initialise ModbusMaster (phase 1, pre-scheduler).
 *
 * Stores configuration and clears the statistics.  Does not touch the UART.
 * Call after modbus_uart_init().
 *
 * @param[in] response_timeout_ms  Response timeout per attempt, 1..; REQ-MB-050 uses 200.
 * @param[in] max_retries          Retries after the first attempt; REQ-MB-060 uses 3.
 * @return MODBUS_MASTER_ERR_OK, or MODBUS_MASTER_ERR_INVALID_ARG if the timeout is 0.
 * @note Threading: call before the scheduler starts.
 */
modbus_master_err_t modbus_master_init(uint32_t response_timeout_ms, uint8_t max_retries);

/**
 * @brief Start ModbusMaster (phase 2, from task context).
 *
 * Captures the calling task as the notification target and registers the UART
 * RX callback.  Call exactly once, from ModbusPollerTask.
 *
 * @return MODBUS_MASTER_ERR_OK on success.
 * @return MODBUS_MASTER_ERR_NOT_INIT if init() was not called.
 * @return MODBUS_MASTER_ERR_ALREADY_STARTED on a second call.
 * @note Threading: task-context only.
 */
modbus_master_err_t modbus_master_start(void);

/**
 * @brief Execute one Modbus transaction, with the retry loop.
 *
 * Builds the wire frame, transmits it, waits for the RX notification, validates
 * the response and retries on timeout, TX failure, UART RX error, runt frame or bad CRC.
 * An address or function-code mismatch and an exception response are returned
 * at once, without retry.
 *
 * @param[in]  request   Request PDU; data_len at most MODBUS_MAX_DATA_BYTES.
 * @param[out] response  Filled on success, and on an exception (exception code in data[0]).
 * @return MODBUS_MASTER_ERR_OK, or an error code.
 * @return MODBUS_MASTER_ERR_NOT_INIT before start().
 * @return MODBUS_MASTER_ERR_NULL_ARG if @p request or @p response is NULL.
 * @return MODBUS_MASTER_ERR_INVALID_ARG if request->data_len is too large.
 * @note Threading: task-context only, blocking.  Called only from the owning task.
 */
modbus_master_err_t modbus_master_transact(const modbus_frame_t *request, modbus_frame_t *response);

/**
 * @brief Copy the current statistics snapshot.
 *
 * @param[out] stats_out  Destination; not NULL.
 * @return MODBUS_MASTER_ERR_OK, MODBUS_MASTER_ERR_NULL_ARG or MODBUS_MASTER_ERR_NOT_INIT.
 * @note Threading: counters change only in the owning task; call from that task.
 */
modbus_master_err_t modbus_master_get_stats(modbus_master_stats_t *stats_out);

/**
 * @brief Reset all counters to zero (CMD_RESET_METRICS).
 *
 * @return MODBUS_MASTER_ERR_OK or MODBUS_MASTER_ERR_NOT_INIT.
 * @note Threading: task-context only, non-blocking.
 */
modbus_master_err_t modbus_master_reset_stats(void);

#ifdef TEST
/**
 * @brief Reset all module state to the power-on default (unit tests only).
 */
void modbus_master_reset_for_test(void);
#endif /* TEST */

#endif /* MODBUS_MASTER_H */
