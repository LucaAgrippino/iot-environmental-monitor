# Interview Test — TimeProvider (20–25 minutes)

## Brief (read this first — 3 minutes)

TimeProvider is a passive middleware singleton that wraps a battery-backed RTC driver and supplies a unified timestamp to all consumers in an IoT gateway system. It returns either a Unix epoch (when synchronised to an NTP or Modbus-sourced time) or an uptime counter (when not yet synchronised). Consumers always receive a `time_provider_ts_t` struct with both the epoch value and a `sync_state` flag; they are required to check the flag before interpreting the epoch as wall-clock time.

The candidate must implement `time_provider_set_time()`. This function accepts a new Unix epoch, converts it to a calendar datetime for the RTC, and transitions the module from UNSYNCHRONISED to SYNCHRONISED — pushing a health event on the first successful sync. A sanity check guards against unreasonably large time jumps when the device is already synchronised. The implementation must be thread-safe using an injected FreeRTOS mutex, and must propagate all error codes without silent failures.

## Files given to the candidate

### `time_provider_exercise.h`

```c
#ifndef TIME_PROVIDER_EXERCISE_H
#define TIME_PROVIDER_EXERCISE_H

#include <stdint.h>
#include <stdbool.h>

/* ---- Error codes ---- */
typedef enum {
    TIME_PROVIDER_ERR_OK       = 0,
    TIME_PROVIDER_ERR_NOT_INIT = 1,
    TIME_PROVIDER_ERR_RTC_FAIL = 2,
} time_provider_err_t;

/* ---- Sync state ---- */
typedef enum {
    TIME_SYNC_UNSYNCHRONISED = 0,
    TIME_SYNC_SYNCHRONISED   = 1,
} time_sync_state_t;

/* ---- Health events ---- */
typedef enum {
    HEALTH_EVENT_TIME_SYNC_ACQUIRED = 0,
} health_event_t;

/* ---- RTC interface ---- */
typedef enum { RTC_OK = 0, RTC_ERR_FAIL = 1 } rtc_err_t;
typedef struct { uint16_t year; uint8_t month, day, hour, minute, second; } rtc_datetime_t;
typedef struct {
    rtc_err_t (*get_time)(rtc_datetime_t *dt);
    rtc_err_t (*set_time)(const rtc_datetime_t *dt);
    rtc_err_t (*write_backup)(uint8_t idx, uint32_t value);
} irtc_t;
extern const irtc_t *rtc_driver;

/* ---- Health interface ---- */
typedef int health_monitor_err_t;
typedef struct { health_monitor_err_t (*push_event)(health_event_t e, uint32_t p); } ihealth_t;

/* ---- Module state (provided, do not modify) ---- */
typedef struct {
    bool              initialised;
    time_sync_state_t sync_state;
    ihealth_t        *health;
    void             *mutex;    /* opaque FreeRTOS mutex handle */
} tp_state_t;
extern tp_state_t s_tp;

/* ---- FreeRTOS helpers (stubs provided) ---- */
int  mutex_take(void *m);   /* returns 1 on success */
void mutex_give(void *m);

/* ---- Epoch helpers (provided, do not modify) ---- */
uint32_t datetime_to_epoch(const rtc_datetime_t *dt);
void     epoch_to_datetime(uint32_t epoch, rtc_datetime_t *dt);

/* ---- Sanity threshold ---- */
#define SANITY_DELTA_S       (86400UL)
#define SYNC_MAGIC           (0xA5A55A5AUL)
#define BKUP_REG             (0U)

/**
 * @brief Set the current time and mark the module as synchronised.
 *
 * Pre-conditions:
 *   - s_tp.initialised must be true (return ERR_NOT_INIT otherwise).
 *   - If s_tp.sync_state == SYNCHRONISED, apply a sanity check:
 *     read current RTC time, compute delta = |new_epoch - current_epoch|.
 *     If delta > SANITY_DELTA_S, reject with ERR_RTC_FAIL.
 *   - If s_tp.sync_state == UNSYNCHRONISED, skip the sanity check.
 *
 * On success:
 *   - Convert new_epoch to rtc_datetime_t and write to RTC via set_time().
 *   - Write SYNC_MAGIC to backup register BKUP_REG via write_backup().
 *   - Set s_tp.sync_state = SYNCHRONISED.
 *   - If the previous state was UNSYNCHRONISED, push
 *     HEALTH_EVENT_TIME_SYNC_ACQUIRED through s_tp.health->push_event().
 *
 * Thread-safety: acquire s_tp.mutex before reading/writing state.
 *   Release on ALL return paths (including error paths).
 *   Push the health event AFTER releasing the mutex.
 *
 * @param new_epoch  Unix epoch seconds to set.
 * @return TIME_PROVIDER_ERR_OK or TIME_PROVIDER_ERR_RTC_FAIL.
 */
time_provider_err_t time_provider_set_time(uint32_t new_epoch);

#endif /* TIME_PROVIDER_EXERCISE_H */
```

### `time_provider_exercise.c` (partial)

```c
#include "time_provider_exercise.h"

/* ---- Provided: module state (do not modify) ---- */
tp_state_t       s_tp;
const irtc_t    *rtc_driver;

/* ---- Provided: epoch helpers (already implemented) ---- */
uint32_t datetime_to_epoch(const rtc_datetime_t *dt)
{
    /* ... correct implementation provided ... */
    (void)dt; return 0; /* placeholder */
}

void epoch_to_datetime(uint32_t epoch, rtc_datetime_t *dt)
{
    /* ... correct implementation provided ... */
    (void)epoch; (void)dt; /* placeholder */
}

/* ---- Provided: mutex stubs ---- */
int  mutex_take(void *m) { (void)m; return 1; }
void mutex_give(void *m) { (void)m; }

/* ==================================================================
 * TODO: implement time_provider_set_time() according to the Doxygen
 * spec in time_provider_exercise.h.
 * ================================================================== */
time_provider_err_t time_provider_set_time(uint32_t new_epoch)
{
    /* YOUR CODE HERE */
    (void)new_epoch;
    return TIME_PROVIDER_ERR_OK;
}
```

## Follow-up questions

**Q1:** The spec says to push the health event *after* releasing the mutex. Why not push it while holding the mutex?

*Model answer:* `s_tp.health->push_event()` acquires its own mutex inside HealthMonitor. Calling it while holding TimeProvider's mutex creates a potential priority-inversion or deadlock if another task holds the HealthMonitor mutex and then tries to call `time_provider_get()` (which would try to acquire TimeProvider's mutex). Releasing TimeProvider's mutex first avoids any nested-lock ordering dependency between the two modules.

**Q2:** The sanity check is only applied when the state is already SYNCHRONISED. What happens on the very first sync call, when the RTC holds its default date (e.g. 2000-01-01)?

*Model answer:* If the sanity check were applied unconditionally, the delta between the default RTC date and the real wall-clock time (24+ years) would always exceed the threshold, making first-sync impossible after a cold boot. Skipping the check when UNSYNCHRONISED allows any epoch to be accepted on first sync, which is safe because the caller (TimeService / ModbusRegisterMap) has already validated the value from the external source.

**Q3:** If `rtc_driver->get_time()` fails during the sanity check, what must you do before returning the error?

*Model answer:* Release the mutex with `mutex_give()` before returning. Failing to do so leaves the mutex permanently held by the current task, deadlocking any future caller of `set_time()`, `get()`, `mark_unsynchronised()`, or `get_sync_state()`. This is a common error-path bug — the "happy path" always releases the mutex, but an early return in an inner branch is easy to miss.

## Model solution

```c
time_provider_err_t time_provider_set_time(uint32_t new_epoch)
{
    rtc_datetime_t dt;
    rtc_datetime_t current_dt;
    uint32_t       current_epoch;
    uint32_t       delta;
    bool           was_unsynchronised;

    if (!s_tp.initialised)
    {
        return TIME_PROVIDER_ERR_NOT_INIT;
    }

    if (!mutex_take(s_tp.mutex))
    {
        return TIME_PROVIDER_ERR_NOT_INIT;
    }

    if (s_tp.sync_state == TIME_SYNC_SYNCHRONISED)
    {
        if (rtc_driver->get_time(&current_dt) != RTC_OK)
        {
            mutex_give(s_tp.mutex);         /* must release before return */
            return TIME_PROVIDER_ERR_RTC_FAIL;
        }
        current_epoch = datetime_to_epoch(&current_dt);
        delta = (new_epoch > current_epoch) ? (new_epoch - current_epoch)
                                            : (current_epoch - new_epoch);
        if (delta > SANITY_DELTA_S)
        {
            mutex_give(s_tp.mutex);
            return TIME_PROVIDER_ERR_RTC_FAIL;
        }
    }

    epoch_to_datetime(new_epoch, &dt);
    if (rtc_driver->set_time(&dt) != RTC_OK)
    {
        mutex_give(s_tp.mutex);
        return TIME_PROVIDER_ERR_RTC_FAIL;
    }

    (void)rtc_driver->write_backup(BKUP_REG, SYNC_MAGIC);

    was_unsynchronised = (s_tp.sync_state == TIME_SYNC_UNSYNCHRONISED);
    s_tp.sync_state    = TIME_SYNC_SYNCHRONISED;

    mutex_give(s_tp.mutex);   /* release before calling external code */

    if (was_unsynchronised && s_tp.health != NULL)
    {
        (void)s_tp.health->push_event(HEALTH_EVENT_TIME_SYNC_ACQUIRED, 0U);
    }

    return TIME_PROVIDER_ERR_OK;
}
```

## Marking guide

**Must have (pass/fail):**
- NOT_INIT guard before acquiring the mutex
- Mutex acquired before reading `sync_state`
- Mutex released on EVERY error return path (including the RTC-get-time failure)
- Mutex released before calling `push_event()` (or at least before returning)
- `was_unsynchronised` captured inside the mutex, used outside
- `sync_state` set to SYNCHRONISED unconditionally on success
- Health event pushed only when previous state was UNSYNCHRONISED
- Correct uint32_t delta computation (handles new < current without signed overflow)

**Nice to have (differentiates mid from senior):**
- Explicit comment explaining why event is pushed outside the mutex
- Checking the return value of `write_backup()` (even if not required by spec)
- Recognition that the sanity-skip-when-UNSYNCHRONISED is necessary for cold-boot

**Red flags (automatic fail or strong negative signal):**
- Missing mutex release on any error path
- Pushing the health event inside the mutex
- Calling `push_event()` unconditionally (even when already SYNCHRONISED)
- Using signed arithmetic for the delta (wraps for large uint32_t values)
- Mutex acquired after checking sync_state (race condition)
