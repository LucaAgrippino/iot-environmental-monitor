/**
 * @file health_monitor_stub.h
 * @brief Narrow stub for HealthMonitor in middleware unit tests.
 *
 * Provides ihealth_report_t with its push_event function pointer — the only
 * IHealthReport method TimeProvider calls. Including this header instead of
 * health_monitor.h prevents Ceedling from auto-linking health_monitor.c
 * (which would cascade to led_driver.c → gpio_driver.c → CMSIS stubs).
 *
 * The struct tag ihealth_report_s matches the definition in health_monitor.h,
 * so production code can include both headers without redefinition errors.
 *
 * Basename: health_monitor_stub — does NOT match health_monitor.c.
 */

#ifndef HEALTH_MONITOR_STUB_H
#define HEALTH_MONITOR_STUB_H

#include <stdint.h>

/* Forward typedef so ihealth_report_t can be used in the extern declaration
 * below even when time_provider.h has not yet been included.
 * Redefinition is valid in C99+ when both typedef refer to the same type. */
#ifndef IHEALTH_REPORT_T_DEFINED
#define IHEALTH_REPORT_T_DEFINED
struct ihealth_report_s;
typedef struct ihealth_report_s ihealth_report_t;
#endif /* IHEALTH_REPORT_T_DEFINED */

/* --------------------------------------------------------------------- */
/* Minimal types — only what TimeProvider needs from health_monitor.h    */
/* --------------------------------------------------------------------- */

typedef enum
{
    HEALTH_MONITOR_ERR_OK       = 0,
    HEALTH_MONITOR_ERR_NOT_INIT = 1,
    HEALTH_MONITOR_ERR_NULL_ARG = 2,
} health_monitor_err_t;

typedef enum
{
    HEALTH_EVENT_TIME_SYNC_ACQUIRED   = 0,
    HEALTH_EVENT_TIME_SYNC_LOST       = 1,
    HEALTH_EVENT_CONFIG_WRITE_FAIL    = 2,
    HEALTH_EVENT_CONFIG_READ_FAIL     = 3,
    HEALTH_EVENT_CONFIG_NO_VALID_SLOT = 4,
    HEALTH_EVENT_SENSOR_FAIL          = 5,
    HEALTH_EVENT_LCD_FAIL             = 6,
} health_event_t;

/* --------------------------------------------------------------------- */
/* IHealthReport vtable (struct tag matches health_monitor.h)            */
/* --------------------------------------------------------------------- */

/* Complete the struct body. The typedef name ihealth_report_t is declared in
 * time_provider.h (which the test TU always includes alongside this stub).
 * Including a struct body for a forward-declared tag is always valid in C. */
struct ihealth_report_s
{
    health_monitor_err_t (*init)(void);
    health_monitor_err_t (*push_event)(health_event_t event, uint32_t param);
    /* Remaining function pointers intentionally omitted — callers must not
     * access beyond push_event in stub builds. */
};

/* Singleton pointer — declared here, defined in the test TU that needs it. */
extern const ihealth_report_t *const health_report;

/* --------------------------------------------------------------------- */
/* device_health_snapshot_t — subset used by LcdUi StatusScreen         */
/* Fields match health_monitor.h exactly for binary compatibility.       */
/* --------------------------------------------------------------------- */

#define HEALTH_STUB_TASK_COUNT (7U)  /* mirrors HEALTH_TASK_COUNT */

typedef struct
{
    uint32_t uptime_s;
    uint32_t sensor_fail_count;
    uint32_t alarm_raise_count;
    bool     config_write_failed;
    uint32_t modbus_valid_frames;
    uint32_t modbus_crc_errors;
    uint32_t modbus_exception_responses;
    uint16_t stack_watermark_words[HEALTH_STUB_TASK_COUNT];
} device_health_snapshot_t;

/* --------------------------------------------------------------------- */
/* ihealth_snapshot_t — read-side vtable                                */
/* --------------------------------------------------------------------- */

typedef struct
{
    health_monitor_err_t (*get_snapshot)(device_health_snapshot_t *snap_out);
} ihealth_snapshot_t;

#endif /* HEALTH_MONITOR_STUB_H */
