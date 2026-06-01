# Technical Exercise — Simulated Sensor Driver

## Brief (3 minutes)

The IoT Environmental Monitoring Gateway project uses a V-Model methodology. At the
driver layer, sensor hardware is abstracted behind narrow interfaces (`IBarometer`,
`IHumidityTemp`). For the STM32F469 Field Device, both interfaces are satisfied by
pure-software simulations — no I2C peripheral, no physical sensor. The simulation
uses a bounded random walk so that `SensorService` sees realistic, time-varying values
from the first boot, even on a bare board.

Implement `barometer_read()` — the single entry point that advances the pressure
simulation by one step and returns the new reading. Constraints: no dynamic memory,
no floating-point, values must remain within the physical range [3000, 11000] (units
of 0.1 hPa), and the function must propagate the fault-injection flag as an error
code so `SensorService` can exercise its error-handling path without hardware.

## Given files

### `barometer_driver_exercise.h`

```c
#ifndef BAROMETER_DRIVER_EXERCISE_H
#define BAROMETER_DRIVER_EXERCISE_H

#include <stdbool.h>
#include <stdint.h>

#define BARO_PRESSURE_MIN_X10     (3000)
#define BARO_PRESSURE_MAX_X10     (11000)
#define BARO_DEFAULT_PRESSURE_X10 (10132)

typedef enum
{
    BARO_ERR_OK    = 0,
    BARO_ERR_FAULT = 1,
} baro_err_t;

typedef struct
{
    int32_t pressure_x10;
} baro_reading_t;

/**
 * @brief Initialise the barometer simulation.
 * Resets pressure to BARO_DEFAULT_PRESSURE_X10 and clears fault injection.
 * @return BARO_ERR_OK always.
 */
baro_err_t barometer_init(void);

/**
 * @brief Read one pressure sample.
 *
 * Advances the simulation by one step using a bounded random walk.
 * Returns BARO_ERR_FAULT if fault injection is active (reading unchanged)
 * or if @p reading is NULL.
 *
 * @param[out] reading  Populated with the new pressure on BARO_ERR_OK.
 * @return BARO_ERR_OK or BARO_ERR_FAULT.
 */
baro_err_t barometer_read(baro_reading_t *reading);

/**
 * @brief Arm or disarm fault injection.
 * @param inject  true = inject faults on next read; false = resume normal.
 */
void barometer_inject_fault(bool inject);

#endif /* BAROMETER_DRIVER_EXERCISE_H */
```

### `barometer_driver_exercise.c` (partial)

```c
#include "barometer_driver_exercise.h"
#include <stdlib.h>

/* Module-level state — do not change these declarations. */
static int32_t s_pressure_x10  = BARO_DEFAULT_PRESSURE_X10;
static bool    s_fault_injected = false;

/* Returns a random delta in [-2, +2] for the pressure random walk. */
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
    /* TODO: implement this function.
     *
     * Requirements:
     *   1. Return BARO_ERR_FAULT if reading is NULL (do not dereference).
     *   2. Return BARO_ERR_FAULT if fault injection is armed; leave reading unchanged.
     *   3. Advance s_pressure_x10 by one random_delta() step.
     *   4. Clamp s_pressure_x10 to [BARO_PRESSURE_MIN_X10, BARO_PRESSURE_MAX_X10].
     *   5. Write the new pressure into reading->pressure_x10.
     *   6. Return BARO_ERR_OK.
     */
    (void) reading;
    return BARO_ERR_OK;
}
```

## Questions

**Q1:** Why must `barometer_read()` check for a NULL `reading` pointer rather than
asserting or relying on caller discipline?

*Answer:* SensorService calls `barometer_read()` with a stack-allocated struct and
will never pass NULL in production code, but defensive NULL checks follow the project's
no-silent-failures rule (P8) and are consistent with the GPIO driver precedent. They
also make the function safe to call from test harnesses that may pass NULL deliberately
(T-BARO-05), and ensure a meaningful error code is returned rather than a hard fault
that would reset the board and lose diagnostic information.

**Q2:** The companion specifies physical pressure ranges of 300.0–1100.0 hPa, stored
as integers 3000–11000 (× 10). Why this fixed-point encoding rather than `float`?

*Answer:* Fixed-point integers that directly match the Modbus register scale (§6.2)
eliminate a float-to-int conversion step in SensorService and avoid rounding errors.
The BARR-C:2018 subset used on this project discourages floating-point in firmware code
(REQ-NF) because the Cortex-M4 FPU requires explicit initialisation and floating-point
comparisons require care around NaN/infinity. Fixed-point with a documented scale factor
achieves the required resolution (0.1 hPa) without those risks.

**Q3:** The clamping after `random_delta()` uses two separate `if` statements rather
than `if/else if`. Is this correct? What would break if you changed the second `if`
to `else if`?

*Answer:* The two-`if` form is correct and the `else if` form would also be correct
here, because `random_delta()` returns at most ±2, and starting from a clamped value
(already in [3000, 11000]), a single step of ±2 cannot simultaneously exceed both
the lower and upper bound. However, the two-`if` form is safer as a general pattern:
if the delta were large enough to jump from below the minimum to above the maximum in
one step, the `else if` form would miss the second clamp. The two-`if` form is
unconditionally correct regardless of delta magnitude.

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

**Must have:**
- NULL check before dereferencing `reading`, returning `BARO_ERR_FAULT`
- `s_fault_injected` check returns `BARO_ERR_FAULT` without modifying `reading`
- Advance `s_pressure_x10` before clamping (not after reading assignment)
- Both bounds clamped (not just one)
- Returns `BARO_ERR_OK` on success

**Good to have:**
- Checks NULL before fault injection (or either order — both are defensible)
- Named constants `BARO_PRESSURE_MIN_X10` / `BARO_PRESSURE_MAX_X10` rather than magic numbers
- `const` not applicable here (state is mutated), but correct `const` usage elsewhere in discussion

**Red flags:**
- Checking `reading` after the fault path (allows dereference on fault if NULL)
- Clamping `reading->pressure_x10` instead of `s_pressure_x10` (breaks state across calls)
- Missing one of the two clamp conditions
- Dynamic memory allocation or use of `float`/`double`
- Ignoring the return value of `random_delta()` or recalculating it twice
