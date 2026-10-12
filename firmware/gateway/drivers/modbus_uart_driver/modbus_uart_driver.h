/**
 * @file modbus_uart_driver.h
 * @brief ModbusUartDriver (Gateway) — RS-485 byte-stream transport over UART4.
 *
 * Synchronous polled TX and IDLE-line-triggered asynchronous RX over the UART
 * wired to the RS-485 transceiver.  Platform singleton (MBUART-D8): one
 * Modbus UART per board, plain free functions with the `modbus_uart_` prefix.
 *
 * Board mapping (companion §4.1, Gateway):
 *   UART4  PA0 TX / PA1 RX (AF8), RTS_DE on PA15 (AF8), APB1 80 MHz.
 *
 * Two-phase initialisation (companion §2.3):
 *   1. modbus_uart_init()      — before the scheduler starts.
 *   2. modbus_uart_attach_rx() — from the consuming task's start-up prologue.
 *
 * Hardware RS-485 DE mode (MBUART-D3): CR3.DEM drives the transceiver DE pin
 * through the RTS alternate function.  No GpioDriver dependency.
 *
 * @note Companion: docs/lld/drivers/modbus-uart-driver.md
 */

#ifndef MODBUS_UART_DRIVER_H
#define MODBUS_UART_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

/* ===================================================================== */
/* Configuration constants                                               */
/* ===================================================================== */

/** Maximum Modbus RTU ADU length in bytes (REQ-NF-408). */
#define MODBUS_UART_BUF_SIZE (256U)

/** Baud rate: 9600 bps (REQ-MB-030). */
#define MODBUS_UART_BAUD (9600U)

/** Per-byte TXE polling timeout in ms (companion §3.5). */
#define MODBUS_UART_TXE_TIMEOUT_MS (5U)

/** TC polling timeout in ms, applied after the last byte (companion §3.5). */
#define MODBUS_UART_TC_TIMEOUT_MS (10U)

/* ===================================================================== */
/* Types                                                                 */
/* ===================================================================== */

/**
 * @brief Error codes returned by ModbusUartDriver operations.
 */
typedef enum
{
    MODBUS_UART_OK = 0,             /**< Operation succeeded. */
    MODBUS_UART_ERR_TIMEOUT = 1,    /**< TXE or TC flag did not assert within timeout. */
    MODBUS_UART_ERR_BUSY = 2,       /**< Transmit called while a TX is already in progress. */
    MODBUS_UART_ERR_INVALID_ARG = 3 /**< NULL pointer, or length outside 1..256. */
} modbus_uart_err_t;

/**
 * @brief Events delivered to the RX callback from the ISR.
 */
typedef enum
{
    MODBUS_UART_EVENT_RX_DONE = 0,  /**< IDLE detected; complete frame in buffer. */
    MODBUS_UART_EVENT_RX_ERROR = 1, /**< Overrun, framing or noise error, or buffer overflow. */
} modbus_uart_event_t;

/**
 * @brief RX callback registered by the consumer.
 *
 * Called from ISR context.  The consumer must only call ISR-safe APIs
 * (typically xTaskNotifyFromISR()) and then read the frame at task level via
 * modbus_uart_get_rx_frame().
 *
 * @param event    RX_DONE or RX_ERROR.
 * @param context  Opaque pointer registered at attach time.
 */
typedef void (*modbus_uart_rx_cb_t)(modbus_uart_event_t event, void *context);

/* ===================================================================== */
/* Public API                                                            */
/* ===================================================================== */

/**
 * @brief Initialise the Modbus UART peripheral.
 *
 * Enables the GPIOA and UART4 clocks, routes PA0/PA1/PA15 to AF8, configures
 * 9600 8N1 and hardware RS-485 DE mode (CR3.DEM), and enables TX and the
 * UART.  Does NOT enable RX or its interrupt (see modbus_uart_attach_rx()).
 *
 * Call once, before the FreeRTOS scheduler starts.
 *
 * @return MODBUS_UART_OK always.
 * @note Threading: task-context only, non-blocking.
 */
modbus_uart_err_t modbus_uart_init(void);

/**
 * @brief Register the RX callback, then enable the receiver and its interrupts.
 *
 * Call exactly once, from the consuming task's start-up prologue.  A NULL
 * callback is ignored and leaves the receiver disabled.
 *
 * @param[in] callback  Called from the ISR on RX_DONE or RX_ERROR; not NULL.
 * @param[in] context   Passed unchanged to the callback.
 * @note Threading: task-context only, non-blocking.  The callback runs in ISR
 *       context.
 */
void modbus_uart_attach_rx(modbus_uart_rx_cb_t callback, void *context);

/**
 * @brief Transmit a Modbus RTU frame over RS-485.
 *
 * Writes every byte once TXE is set, then waits for TC so the caller may
 * safely turn the bus around.  DE is asserted and released by hardware.
 *
 * @param[in] frame  Frame bytes; not NULL.
 * @param[in] len    Number of bytes, 1..MODBUS_UART_BUF_SIZE.
 * @return MODBUS_UART_OK on success.
 * @return MODBUS_UART_ERR_INVALID_ARG if @p frame is NULL or @p len is out of range.
 * @return MODBUS_UART_ERR_BUSY if a prior transmit is still in progress.
 * @return MODBUS_UART_ERR_TIMEOUT if TXE or TC does not assert in time.
 * @note Threading: task-context only, may block.  Not ISR-safe.
 */
modbus_uart_err_t modbus_uart_transmit(const uint8_t *frame, uint16_t len);

/**
 * @brief Copy the most recently received frame out of the internal buffer.
 *
 * Call from task context after the RX_DONE callback.  Resets the internal
 * length so the buffer is ready for the next frame (MBUART-D7).
 *
 * @param[out] buf  Destination; at least MODBUS_UART_BUF_SIZE bytes.
 * @param[out] len  Number of bytes copied.
 * @return MODBUS_UART_OK on success.
 * @return MODBUS_UART_ERR_INVALID_ARG if @p buf or @p len is NULL.
 * @note Threading: task-context only.  Single-consumer model.
 */
modbus_uart_err_t modbus_uart_get_rx_frame(uint8_t *buf, uint16_t *len);

/**
 * @brief Inject the millisecond tick source that bounds transmit waits.
 *
 * The driver is RTOS-free (P1) and host-testable, so the tick source is
 * injected.  With no source (or NULL) the transmit loops are unbounded.
 * Production code must inject one before the first transmit.
 *
 * @param[in] get_ms  Monotonic millisecond counter; 32-bit wrap is handled.
 * @note Threading: set once during start-up before any transmit.
 */
void modbus_uart_set_tick_source(uint32_t (*get_ms)(void));

#ifdef TEST
/**
 * @brief Reset all module state to the power-on default (unit tests only).
 */
void modbus_uart_reset_for_test(void);
#endif /* TEST */

#endif /* MODBUS_UART_DRIVER_H */
