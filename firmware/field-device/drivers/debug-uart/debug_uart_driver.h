/**
 * @file debug_uart_driver.h
 * @brief CMSIS-level debug-UART driver — line-buffered RX, blocking TX.
 *
 * Provides IDebugUart (per components.md): TX of arbitrary byte streams
 * and ISR-driven line-ready notification for RX. Used by Logger (TX)
 * and ConsoleService (TX + RX) on both boards.
 *
 * The driver depends only on CMSIS. It does NOT depend on FreeRTOS or
 * any other RTOS. The consumer wires the RX line-ready callback to its
 * own threading primitives (e.g., xTaskNotifyFromISR()).
 *
 * Thread safety: the driver is NOT internally serialised. Concurrent
 * calls to debug_uart_send() from multiple contexts will interleave
 * bytes on the wire. The caller (typically Logger) must serialise
 * itself if multiple producers exist.
 *
 * @note Realised on USART3 (Field Device) or USART1 (Gateway), each routed
 *       to the board's ST-LINK V2-1 virtual COM port.
 * @note See docs/lld/drivers/debug-uart-driver.md for the full design.
 */

#ifndef DEBUG_UART_DRIVER_H
#define DEBUG_UART_DRIVER_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Configuration constants                                             */
/* ------------------------------------------------------------------ */

/**
 * @brief Maximum length of a single received line, in bytes.
 *
 * Lines longer than this are truncated; the truncation is reported via
 * the line-flag output of debug_uart_read_line().
 */
#define DEBUG_UART_LINE_MAX_LEN  (128U)

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Debug-UART driver result codes.
 */
typedef enum
{
    DEBUG_UART_OK                      =  0, /**< Success. */
    DEBUG_UART_ERR_NOT_INITIALISED     =  1, /**< debug_uart_init() not yet called. */
    DEBUG_UART_ERR_NULL_POINTER        =  2, /**< Required pointer is NULL. */
    DEBUG_UART_ERR_INVALID_PARAM       =  3, /**< Out-of-range parameter. */
    DEBUG_UART_ERR_TX_TIMEOUT          =  4, /**< Peripheral TXE flag did not assert within timeout. */
    DEBUG_UART_ERR_NO_LINE_AVAILABLE   =  5, /**< debug_uart_read_line() called with nothing pending. */
    DEBUG_UART_ERR_RX_ALREADY_ATTACHED =  6  /**< debug_uart_attach_rx() called twice. */
} debug_uart_err_t;

/* ------------------------------------------------------------------ */
/* Line completion flag                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Reported alongside each line read.
 */
typedef enum
{
    DEBUG_UART_LINE_OK        = 0, /**< Line fitted within DEBUG_UART_LINE_MAX_LEN. */
    DEBUG_UART_LINE_TRUNCATED = 1  /**< Line exceeded DEBUG_UART_LINE_MAX_LEN; tail was dropped. */
} debug_uart_line_flag_t;

/* ------------------------------------------------------------------ */
/* RX callback                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Signature of the RX line-ready callback.
 *
 * Invoked from the USART RX ISR each time a complete line has been
 * accumulated. The callback runs in interrupt context with the USART
 * vector active; it must follow the ISR contract from
 * task-breakdown.md §6 (acknowledge, capture, notify, return).
 *
 * Typical FreeRTOS wiring:
 * @code
 *   static TaskHandle_t s_console_task;
 *   static void on_line_ready(void *ctx)
 *   {
 *       BaseType_t woken = pdFALSE;
 *       xTaskNotifyFromISR(s_console_task, (1U << CONSOLE_LINE_BIT),
 *                          eSetBits, &woken);
 *       portYIELD_FROM_ISR(woken);
 *   }
 * @endcode
 *
 * The driver does not call portYIELD_FROM_ISR() itself — that is a
 * FreeRTOS concern and belongs in the callback.
 *
 * @param[in] context Opaque pointer registered with debug_uart_attach_rx().
 */
typedef void (*debug_uart_line_callback_t)(void *context);

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise the driver (phase 1 — TX-ready).
 *
 * Enables peripheral clocks (USART and the GPIO port hosting the TX/RX
 * pins), configures the TX/RX pins for alternate-function mode at the
 * board-specific alternate-function number, sets the USART for 115200,
 * 8N1, no flow control (REQ-LI-000), and arms the peripheral for TX.
 * RX interrupts are not enabled yet.
 *
 * Must be called once before any other function. After this call returns
 * OK, debug_uart_send() is callable; debug_uart_read_line() is not.
 *
 * @return DEBUG_UART_OK on success.
 *
 * @note Threading: not internally serialised. Caller must ensure this
 *       function is called exactly once and not concurrently with any
 *       other API entry point.
 */
debug_uart_err_t debug_uart_init(void);

/**
 * @brief Attach an RX line-ready callback (phase 2).
 *
 * Stores the callback and context, enables the RX-not-empty interrupt,
 * and unmasks the USART NVIC vector. From this point on, each complete
 * line received causes @c callback to be invoked from ISR context, with
 * @c context as its argument.
 *
 * Calling twice returns DEBUG_UART_ERR_RX_ALREADY_ATTACHED.
 *
 * @param[in] callback Function invoked on each complete line. Must not be NULL.
 *                     Runs in ISR context — see debug_uart_line_callback_t.
 * @param[in] context  Opaque pointer passed back to @c callback. May be NULL.
 *
 * @return DEBUG_UART_OK on success; DEBUG_UART_ERR_NOT_INITIALISED,
 *         DEBUG_UART_ERR_NULL_POINTER, or DEBUG_UART_ERR_RX_ALREADY_ATTACHED.
 *
 * @note Threading: not internally serialised. Caller calls once.
 */
debug_uart_err_t debug_uart_attach_rx(debug_uart_line_callback_t callback,
                                      void *context);

/**
 * @brief Send a buffer of bytes synchronously.
 *
 * Writes each byte to the USART data register, polling the TX-empty flag
 * between bytes. Returns after the last byte has been pushed to the data
 * register (not after it has fully left the wire).
 *
 * NOT thread-safe. If multiple producers exist, the caller must serialise.
 * The typical case: Logger holds its own mutex and is the only caller in
 * the multi-task path; ConsoleService is single-task.
 *
 * @param[in] data       Buffer of bytes to send. Must be non-NULL when length > 0.
 * @param[in] length     Number of bytes; 0 is permitted and returns OK immediately.
 * @param[in] timeout_ms Maximum time to wait for the TXE flag, per byte,
 *                       in milliseconds. Used to detect a wedged peripheral.
 *
 * @return DEBUG_UART_OK on success; DEBUG_UART_ERR_NOT_INITIALISED,
 *         DEBUG_UART_ERR_NULL_POINTER, or DEBUG_UART_ERR_TX_TIMEOUT.
 *
 * @note Threading: task-context only. Blocks during the byte loop for the
 *       duration of the transmission (~87 µs per byte at 115200 bps).
 *       NOT internally serialised. NOT ISR-safe.
 */
debug_uart_err_t debug_uart_send(const uint8_t *data,
                                 size_t length,
                                 uint32_t timeout_ms);

/**
 * @brief Read the most recent complete line.
 *
 * Copies the latest accumulated line into the caller's buffer and
 * null-terminates it. The line terminator characters (\r and \n) are
 * stripped before copying. Resets the internal line-ready flag so that
 * the next callback invocation corresponds to a new line.
 *
 * @param[out] out_buf    Caller-provided buffer; receives the line.
 * @param[in]  buf_size   Size of @c out_buf in bytes. Must be at least
 *                        DEBUG_UART_LINE_MAX_LEN + 1 (for the null
 *                        terminator).
 * @param[out] out_length Number of bytes written, excluding the null.
 *                        May be 0 (an empty line).
 * @param[out] out_flag   DEBUG_UART_LINE_OK or DEBUG_UART_LINE_TRUNCATED.
 *
 * @return DEBUG_UART_OK on success; DEBUG_UART_ERR_NOT_INITIALISED,
 *         DEBUG_UART_ERR_NULL_POINTER, DEBUG_UART_ERR_INVALID_PARAM
 *         (buf_size too small), or DEBUG_UART_ERR_NO_LINE_AVAILABLE.
 *
 * @note Threading: task-context only. Typically called by the consumer
 *       task after its callback has notified it. Briefly disables the
 *       USART NVIC vector while copying.
 */
debug_uart_err_t debug_uart_read_line(uint8_t *out_buf,
                                      size_t buf_size,
                                      size_t *out_length,
                                      debug_uart_line_flag_t *out_flag);

#endif /* DEBUG_UART_DRIVER_H */
