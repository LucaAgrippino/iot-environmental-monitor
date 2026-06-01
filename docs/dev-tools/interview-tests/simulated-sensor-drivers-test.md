# Interview Test — Simulated Sensor Driver (20 minutes)

## Brief (read this first — 3 minutes)

This codebase is an IoT Environmental Monitoring Gateway built on two STM32 boards.
The field device uses a pure-software simulation of a pressure sensor: no I2C peripheral,
no hardware dependency. The simulation implements the same narrow interface (`IBarometer`)
as the real physical sensor drivers, proving that the `SensorService` layer above it
is hardware-agnostic. The simulation uses a bounded random walk so that pressure values
vary realistically from the first boot, and exposes a fault-injection mechanism so that
`SensorService`'s error-handling path can be exercised in unit tests and on-device
self-test commands — without needing a broken sensor.

Your task is to implement `barometer_read()`, the single entry point that advances the
simulation by one step and returns the new reading. The function must return an error
code on failure (NULL pointer or injected fault) rather than asserting or crashing,
because `SensorService` uses the error code to mark the sample invalid and push a health
event. No dynamic memory, no floating-point, and values must stay within the physical
range [3000, 11000] (units of 0.1 hPa, i.e. 300.0–1100.0 hPa).

## Files given to the candidate

### `barometer_driver_exercise.h`

```c
#ifndef BAROMETER_DRIVER_EXERCISE_H
#define BAROMETER_DRIVER_EXERCISE_H

#include <stdbool.h>
#include <stdint.h>

/** Minimum valid pressure × 10 (300.0 hPa). */
#define BARO_PRESSURE_MIN_X10     (3000)
/** Maximum valid pressure × 10 (1100.0 hPa). */
#define BARO_PRESSURE_MAX_X10     (11000)
/** Default initial pressure × 10 (1013.2 hPa — standard sea level). */
#define BARO_DEFAULT_PRESSURE_X10 (10132)

typedef enum
{
    BARO_ERR_OK    = 0, /**< Reading produced successfully. */
    BARO_ERR_FAULT = 1, /**< Fault injection active or NULL pointer. */
} baro_err_t;

typedef struct
{
    int32_t pressure_x10; /**< Pressure in 0.1 hPa units. Range: 3000..11000. */
} baro_reading_t;

/**
 * @brief Initialise the simulation. Resets pressure to default, clears fault flag.
 * @return BARO_ERR_OK always.
 */
baro_err_t barometer_init(void);

/**
 * @brief Read one pressure sample.
 *
 * Advances the simulation by one bounded random-walk step.
 * Returns BARO_ERR_FAULT (reading unchanged) if:
 *   - @p reading is NULL, or
 *   - fault injection is active (barometer_inject_fault(true) was called).
 *
 * @param[out] reading  Populated with the new pressure on BARO_ERR_OK.
 * @return BARO_ERR_OK or BARO_ERR_FAULT.
 */
baro_err_t barometer_read(baro_reading_t *reading);

/**
 * @brief Arm or disarm fault injection.
 * @param inject  true = next read returns BARO_ERR_FAULT; false = normal.
 */
void barometer_inject_fault(bool inject);

#endif /* BAROMETER_DRIVER_EXERCISE_H */
```

### `barometer_driver_exercise.c` (partial)

```c
#include "barometer_driver_exercise.h"
#include <stdlib.h>

static int32_t s_pressure_x10  = BARO_DEFAULT_PRESSURE_X10;
static bool    s_fault_injected = false;

/* Returns a delta in [-2, +2] for the random walk. Do not modify. */
static int32_t random_delta(void)
{
    return (int32_t)(rand() % 5) - 2;
}

baro_err_t barometer_init(void)
{
    s_pressure_x10  = BARO_DEFAULT_PRESSURE_X10;
    s_fault_injected = false;
    return BARO_ERR_OK;
}

void barometer_inject_fault(bool inject)
{
    s_fault_injected = inject;
}

baro_err_t barometer_read(baro_reading_t *reading)
{
    /* TODO: implement.
     *
     * Rules:
     *  1. Return BARO_ERR_FAULT if reading is NULL (do not dereference it).
     *  2. Return BARO_ERR_FAULT if fault injection is armed; leave reading unchanged.
     *  3. Advance s_pressure_x10 by random_delta().
     *  4. Clamp s_pressure_x10 to [BARO_PRESSURE_MIN_X10, BARO_PRESSURE_MAX_X10].
     *  5. Copy the clamped value into reading->pressure_x10.
     *  6. Return BARO_ERR_OK.
     */
    (void) reading;
    return BARO_ERR_OK;
}
```

## Follow-up questions

**Q1:** Why does the function return `BARO_ERR_FAULT` for a NULL pointer rather than
asserting or calling `configASSERT()`?

*Model answer:* `configASSERT()` triggers a hard fault handler on the target (often an
infinite loop), which loses all diagnostic state and resets the board. Returning an error
code lets `SensorService` log the event, push a health event (`HEALTH_EVENT_SENSOR_FAIL`),
and continue operating for other sensors. The BARR-C rule "no silent failures" means an
error must be propagated, not masked — but propagation through a return code is safer than
crashing firmware in a deployed device.

**Q2:** The state variable `s_pressure_x10` is updated in place before writing to
`reading->pressure_x10`. Why not compute the new value into a local, then assign both the
state and the output in one step?

*Model answer:* Either approach is correct for a single-reader, task-context-only driver.
Updating `s_pressure_x10` first is more readable because it mirrors the intended sequence:
advance → clamp → emit. The one-step alternative with a local is equally valid and avoids
a brief window where `s_pressure_x10` is unclamped — though since this driver is not ISR-safe
and has a single caller (`SensorTask`), that window is never observable. Either implementation
passes the unit tests; the important thing is that clamping happens before the output is written.

**Q3:** Why are there two separate `if` statements for the lower and upper clamp, rather
than `if / else if`?

*Model answer:* For `random_delta()` returning at most ±2 from an already-clamped value,
both forms are equivalent — a single step cannot simultaneously breach both bounds. However,
the two-`if` form is unconditionally correct for any delta magnitude: if the delta were large
enough to jump from below the minimum past the maximum in one step, the `else if` form would
silently miss the second clamp. The two-`if` form is the safer default in firmware code where
constants can be changed independently of the algorithm.

## Model solution

```c
baro_err_t barometer_read(baro_reading_t *reading)
{
    if (NULL == reading)
    {
        return BARO_ERR_FAULT;
    }

    if (s_fault_injected)
    {
        return BARO_ERR_FAULT;
    }

    s_pressure_x10 += random_delta();

    if (s_pressure_x10 < BARO_PRESSURE_MIN_X10)
    {
        s_pressure_x10 = BARO_PRESSURE_MIN_X10;
    }

    if (s_pressure_x10 > BARO_PRESSURE_MAX_X10)
    {
        s_pressure_x10 = BARO_PRESSURE_MAX_X10;
    }

    reading->pressure_x10 = s_pressure_x10;
    return BARO_ERR_OK;
}
```

## Marking guide

**Must have (pass/fail):**
- NULL check on `reading` before any dereference, returning `BARO_ERR_FAULT`
- `s_fault_injected` check returns `BARO_ERR_FAULT` and does NOT modify `*reading`
- `s_pressure_x10` (not `reading->pressure_x10`) is clamped — so state is correct on the next call
- Both the lower and upper bounds are clamped
- Returns `BARO_ERR_OK` on the happy path

**Nice to have (differentiates mid from senior):**
- Named constants for bounds (`BARO_PRESSURE_MIN_X10` / `BARO_PRESSURE_MAX_X10`) rather than literals
- Awareness that NULL vs fault check order is a design choice, not a correctness requirement
- Can articulate why `else if` works here but two `if` is safer generally

**Red flags (automatic fail or strong negative signal):**
- Dereferencing `reading` before the NULL check
- Clamping `reading->pressure_x10` instead of `s_pressure_x10` (breaks state continuity)
- Missing either clamping condition
- Using `float` or `double` for arithmetic
- Using `malloc()` or any dynamic allocation
- Returning `BARO_ERR_OK` on the NULL path
