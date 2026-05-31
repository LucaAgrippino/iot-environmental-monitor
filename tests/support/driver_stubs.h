/**
 * @file driver_stubs.h
 * @brief Narrow stub declarations for Logger test build.
 *
 * Replaces #include "rtc_driver.h" and #include "debug_uart_driver.h" in
 * test_logger.c. Reason: those includes cause Ceedling to auto-link the
 * real driver .c files, which conflicts with the inline stub bodies in
 * test_logger.c. By including a header with no corresponding .c anywhere
 * in the source paths, Ceedling has nothing to auto-link, and the real
 * drivers stay out of the Logger test executable.
 *
 * Keep the declarations IDENTICAL in signature to the real headers — the
 * linker matches by name but the compiler must see consistent types in
 * both test_logger.c (this file) and logger.c (the real headers).
 *
 * Logger tests validate Logger logic against controllable stubs of its
 * driver dependencies. Driver behaviour is validated separately by the
 * driver tests, which run against the CMSIS mock. Real driver + real
 * Logger together is the job of on-target integration tests, not units.
 */

#ifndef DRIVER_STUBS_H
#define DRIVER_STUBS_H

#include <stdint.h>
#include <stdbool.h>

/* ---------------------------------------------------------------------- */
/* rtc_driver subset                                                       */
/* ---------------------------------------------------------------------- */

typedef struct {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
} rtc_datetime_t;

typedef enum {
    RTC_OK                 = 0,
    RTC_ERR_INIT_TIMEOUT   = 1,
    RTC_ERR_SYNC_TIMEOUT   = 2,
    RTC_ERR_NULL_ARG       = 3,
    RTC_ERR_BACKUP_BOUNDS  = 4,
    RTC_ERR_LSE_NOT_READY  = 5
} rtc_err_t;

rtc_err_t rtc_get_time(rtc_datetime_t *dt);

/* ---------------------------------------------------------------------- */
/* debug_uart_driver subset                                                */
/* ---------------------------------------------------------------------- */

typedef enum
{
    DEBUG_UART_OK = 0,                  /**< Success. */
    DEBUG_UART_ERR_NOT_INITIALISED = 1, /**< debug_uart_init() not yet called. */
    DEBUG_UART_ERR_NULL_POINTER = 2,    /**< Required pointer is NULL. */
    DEBUG_UART_ERR_INVALID_PARAM = 3,   /**< Out-of-range parameter. */
    DEBUG_UART_ERR_TX_TIMEOUT = 4,      /**< Peripheral TXE flag did not assert within timeout. */
    DEBUG_UART_ERR_NO_LINE_AVAILABLE =
        5, /**< debug_uart_read_line() called with nothing pending. */
    DEBUG_UART_ERR_RX_ALREADY_ATTACHED = 6 /**< debug_uart_attach_rx() called twice. */
} debug_uart_err_t;

debug_uart_err_t debug_uart_send(const uint8_t *data, size_t length, uint32_t timeout_ms);

#endif /* DRIVER_STUBS_H */
