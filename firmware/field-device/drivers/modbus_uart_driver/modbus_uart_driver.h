/**
 * @file modbus_uart_driver.h
 * @brief Modbus UART driver — byte-stream transport over RS-485 half-duplex.
 *
 * Provides IModbusUart (per components.md): synchronous TX and IDLE-line-
 * triggered asynchronous RX over the UART peripheral wired to the RS-485
 * transceiver on each board.
 *
 * Board mapping (MBUART-O1 resolved):
 *   Field Device  (STM32F469) — USART6, PG14 TX / PG9 RX, AF8, APB2 90 MHz
 *   Gateway       (STM32L475) — UART4,  PA0  TX / PA1 RX, AF8, APB1 80 MHz
 *
 * Two-phase initialisation (MBUART-D1):
 *   1. modbus_uart_init()      — call from main() before scheduler.
 *   2. modbus_uart_attach_rx() — call from consuming task after scheduler.
 *
 * Hardware RS-485 DE mode (MBUART-D3): CR3.DEM = 1 drives the transceiver
 * DE pin automatically via the RTS alternate function. No GpioDriver needed.
 *
 * @note See docs/lld/drivers/modbus-uart-driver.md for the full design.
 */

#ifndef MODBUS_UART_DRIVER_H
#define MODBUS_UART_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

/* ===================================================================== */
/* Test-visibility macro                                                  */
/* ===================================================================== */

#ifdef TEST
#define MODBUS_UART_TEST_VISIBLE
#else
#define MODBUS_UART_TEST_VISIBLE static
#endif

/* ===================================================================== */
/* Configuration constants                                               */
/* ===================================================================== */

/** Maximum Modbus RTU ADU length in bytes (REQ-NF-408). */
#define MODBUS_UART_BUF_SIZE (256U)

/** Baud rate: 9600 bps (REQ-MB-030). */
#define MODBUS_UART_BAUD (9600U)

/** Per-byte TXE polling timeout in ms. Allows 5× one character time at
 *  9600 baud (≈ 1.04 ms) to tolerate clock inaccuracies. */
#define MODBUS_UART_TXE_TIMEOUT_MS (5U)

/** TC polling timeout in ms, applied after the last byte is written.
 *  One character time ≈ 1.04 ms; 10 ms gives comfortable margin. */
#define MODBUS_UART_TC_TIMEOUT_MS (10U)

/* ===================================================================== */
/* Error codes                                                           */
/* ===================================================================== */

/**
 * @brief Error codes returned by ModbusUartDriver operations.
 *
 * Naming follows the cross-cutting convention in lld.md §3.2.
 */
typedef enum
{
    MODBUS_UART_ERR_OK = 0,      /**< Operation succeeded. */
    MODBUS_UART_ERR_TIMEOUT = 1, /**< TXE or TC flag did not assert within timeout. */
    MODBUS_UART_ERR_BUSY = 2,    /**< Transmit called while a TX is already in progress. */
} modbus_uart_err_t;

/* ===================================================================== */
/* Event type                                                            */
/* ===================================================================== */

/**
 * @brief Events delivered to the RX callback from the ISR.
 *
 * Two distinct events correspond to the two notification bits documented
 * in task-breakdown.md §5.4.
 */
typedef enum
{
    MODBUS_UART_EVENT_RX_DONE = 0,  /**< IDLE detected; complete frame in buffer. */
    MODBUS_UART_EVENT_RX_ERROR = 1, /**< Overrun, framing error, noise error, or buffer overflow. */
} modbus_uart_event_t;

/* ===================================================================== */
/* Callback type                                                         */
/* ===================================================================== */

/**
 * @brief RX callback type registered by the consumer.
 *
 * Called from ISR context. The consumer must not call any FreeRTOS API
 * that is not ISR-safe. Typically the consumer calls xTaskNotifyFromISR()
 * to wake its owning task, then reads the frame from the buffer at task
 * level via modbus_uart_get_rx_frame().
 *
 * @param event    What triggered the callback (RX_DONE or RX_ERROR).
 * @param context  Opaque pointer registered at attach time (typically the
 *                 task handle or a middleware context struct).
 */
typedef void (*modbus_uart_rx_cb_t)(modbus_uart_event_t event, void *context);

/* ===================================================================== */
/* Public API                                                            */
/* ===================================================================== */

/**
 * @brief Initialise the Modbus UART peripheral.
 *
 * Configures TX and RX pins as alternate-function outputs.
 * Configures the UART for 9600 8N1 (REQ-MB-030).
 * Enables hardware RS-485 DE mode on the RTS pin (MBUART-D3).
 * Enables TX and the UART peripheral. Does NOT enable the RX interrupt
 * (that is done in modbus_uart_attach_rx, after the consumer task exists).
 *
 * Must be called once from main() before the FreeRTOS scheduler starts.
 *
 * @return MODBUS_UART_ERR_OK on success.
 * @note Threading: task-context only, non-blocking. Must be called before
 *       the scheduler starts.
 */
modbus_uart_err_t modbus_uart_init(void);

/**
 * @brief Register the RX callback and enable the RX interrupt.
 *
 * Must be called from the consuming task's startup prologue
 * (after the scheduler has started and the task exists).
 * Must be called exactly once. Calling again overwrites the prior
 * registration without disabling the interrupt first — do not call
 * more than once in normal operation.
 *
 * @param callback  Function to call from the ISR on RX_DONE or RX_ERROR.
 *                  Must not be NULL.
 * @param context   Opaque pointer passed unchanged to the callback.
 * @note Threading: task-context only, non-blocking. Callback executes in
 *       ISR context.
 */
void modbus_uart_attach_rx(modbus_uart_rx_cb_t callback, void *context);

/**
 * @brief Transmit a Modbus RTU frame over RS-485.
 *
 * Asserts DE via hardware RS-485 mode, transmits all bytes, waits for
 * Transmission Complete (TC) before returning so the caller may safely
 * turn around to receive. De-assertion of DE is handled automatically
 * by the hardware after TC (MBUART-D4).
 *
 * Blocks in the calling task until transmission is complete or timeout.
 *
 * @param frame  Pointer to frame bytes (must not be NULL).
 * @param len    Number of bytes to transmit (1..256).
 * @return MODBUS_UART_ERR_OK on success; MODBUS_UART_ERR_TIMEOUT if TXE
 *         or TC does not assert within timeout; MODBUS_UART_ERR_BUSY if a
 *         prior transmit is still in progress.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
modbus_uart_err_t modbus_uart_transmit(const uint8_t *frame, uint16_t len);

/**
 * @brief Copy the most recently received frame from the internal buffer.
 *
 * Must be called from task context after the RX_DONE callback has fired.
 * Resets the internal receive length so the buffer is ready for the next
 * incoming frame.
 *
 * @param[out] buf  Destination buffer (must be at least MODBUS_UART_BUF_SIZE
 *                  bytes).
 * @param[out] len  Number of bytes in the received frame.
 * @return MODBUS_UART_ERR_OK on success.
 * @note Threading: task-context only. Single-consumer model — call only
 *       after RX_DONE callback and only from the owning task.
 */
modbus_uart_err_t modbus_uart_get_rx_frame(uint8_t *buf, uint16_t *len);

/**
 * @brief Inject the millisecond tick source used by modbus_uart_transmit().
 *
 * The driver bounds each byte's TXE wait and the final TC wait against a
 * monotonic millisecond counter. Because the driver is RTOS-free (P1) and
 * host-testable, it cannot call into any specific RTOS tick API directly.
 * The tick source is injected: production wires it to a real SysTick or
 * DWT reader; tests wire it to a controllable counter.
 *
 * If never called (or called with NULL), the transmit loops run without a
 * timeout bound. Production code must inject a real tick source during
 * system startup, before the first transmit call.
 *
 * @param get_ms Function returning monotonic milliseconds. May be NULL to
 *               disable the timeout mechanism. 32-bit wrap is handled via
 *               unsigned subtraction.
 * @note Threading: set once during startup before any transmit calls.
 */
void modbus_uart_set_tick_source(uint32_t (*get_ms)(void));

/* ===================================================================== */
/* Test-only hooks                                                        */
/* ===================================================================== */

#ifdef TEST
/**
 * @brief Reset all module state to the power-on default.
 *
 * Call from setUp() in unit tests. Not present in production builds.
 */
void modbus_uart_reset_for_test(void);
#endif /* TEST */

#endif /* MODBUS_UART_DRIVER_H */
