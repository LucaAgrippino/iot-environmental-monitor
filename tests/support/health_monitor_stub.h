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
    HEALTH_EVENT_TIME_SYNC_ACQUIRED = 0,
    HEALTH_EVENT_TIME_SYNC_LOST     = 1,
    /* Remaining event constants exist in health_monitor.h but are not
     * called by TimeProvider — omitted to keep the stub minimal. */
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
    /* Remaining function pointers intentionally omitted — TimeProvider
     * only calls push_event. Callers must not access beyond push_event. */
};

#endif /* HEALTH_MONITOR_STUB_H */
