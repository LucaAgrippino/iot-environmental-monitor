/**
 * @file rtc_driver_stub.h
 * @brief Narrow stub for RtcDriver in middleware/application unit tests.
 *
 * Declares only the symbols that TimeProvider calls through the irtc_t vtable.
 * Including this header instead of rtc_driver.h prevents Ceedling from
 * auto-linking the real rtc_driver.c (which requires CMSIS register stubs).
 *
 * The rtc_driver singleton is declared here and must be DEFINED in the test TU:
 *   const irtc_t *rtc_driver;
 * Tests inject a mock vtable via:
 *   *(const irtc_t **)&rtc_driver = &s_mock_rtc;
 *
 * Basename: rtc_driver_stub — does NOT match rtc_driver.c, so no auto-link.
 */

#ifndef RTC_DRIVER_STUB_H
#define RTC_DRIVER_STUB_H

#include <stdint.h>
#include <stdbool.h>

/* --------------------------------------------------------------------- */
/* Calendar date/time (matches rtc_driver.h exactly)                     */
/* --------------------------------------------------------------------- */

typedef struct
{
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
} rtc_datetime_t;

/* --------------------------------------------------------------------- */
/* Error codes (matches rtc_driver.h exactly)                            */
/* --------------------------------------------------------------------- */

typedef enum
{
    RTC_OK                = 0,
    RTC_ERR_INIT_TIMEOUT  = 1,
    RTC_ERR_SYNC_TIMEOUT  = 2,
    RTC_ERR_NULL_ARG      = 3,
    RTC_ERR_BACKUP_BOUNDS = 4,
    RTC_ERR_LSE_NOT_READY = 5,
} rtc_err_t;

/* --------------------------------------------------------------------- */
/* Vtable (matches irtc_t in rtc_driver.h exactly)                       */
/* --------------------------------------------------------------------- */

typedef struct
{
    rtc_err_t (*init)(void);
    rtc_err_t (*get_time)(rtc_datetime_t *dt);
    rtc_err_t (*set_time)(const rtc_datetime_t *dt);
    bool      (*is_backup_valid)(void);
    rtc_err_t (*read_backup)(uint8_t idx, uint32_t *out);
    rtc_err_t (*write_backup)(uint8_t idx, uint32_t value);
} irtc_t;

/* --------------------------------------------------------------------- */
/* Singleton pointer — declared here, defined in the test TU             */
/* --------------------------------------------------------------------- */

extern const irtc_t *rtc_driver;

#endif /* RTC_DRIVER_STUB_H */
