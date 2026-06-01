/**
 * @file barometer_driver.h
 * @brief Simulated barometer driver — pressure readings via bounded random walk.
 *
 * Implements IBarometer for the STM32F469 Field Device. No hardware dependency;
 * the simulation is self-contained. Replacing this driver with a physical I2C
 * sensor requires only a driver-layer change (Vision §5.1.1).
 *
 * @note See docs/lld/drivers/simulated-sensor-drivers.md for the full specification.
 */

#ifndef BAROMETER_DRIVER_H
#define BAROMETER_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Constants                                                          */
/* ------------------------------------------------------------------ */

/** Minimum valid pressure × 10 (300.0 hPa). */
#define BARO_PRESSURE_MIN_X10 (3000)
/** Maximum valid pressure × 10 (1100.0 hPa). */
#define BARO_PRESSURE_MAX_X10 (11000)
/** Default pressure × 10 (1013.2 hPa — standard sea-level pressure). */
#define BARO_DEFAULT_PRESSURE_X10 (10132)

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Error codes for BarometerDriver.
 * Naming follows the cross-cutting convention in lld.md §3.2.
 */
typedef enum
{
    BARO_ERR_OK = 0,    /**< Reading produced successfully. */
    BARO_ERR_FAULT = 1, /**< Fault injection active; simulate sensor failure. */
} baro_err_t;

/* ------------------------------------------------------------------ */
/* Data types                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief A single pressure sample.
 *
 * pressure_x10: atmospheric pressure in units of 0.1 hPa.
 * Example: 10132 → 1013.2 hPa (standard sea-level pressure).
 * Valid range: 3000..11000 (300.0..1100.0 hPa).
 * Matches the PRESSURE register encoding in modbus-register-map.md §6.2.
 */
typedef struct
{
    // cppcheck-suppress unusedStructMember -- populated here, consumed by SensorService callers not
    // in cppcheck's file set
    int32_t pressure_x10;
} baro_reading_t;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise the barometer simulation module.
 *
 * Resets the simulation to the default pressure value and clears any
 * active fault injection (REQ-SA-030).
 * Called from SensorTask startup prologue before the scheduler starts.
 *
 * @return BARO_ERR_OK always.
 * @note Threading: task-context only, non-blocking. Must be called before
 *       the scheduler starts.
 */
baro_err_t barometer_init(void);

/**
 * @brief Read one pressure sample from the simulation.
 *
 * Advances the simulation by one step (bounded random walk, §3.2).
 * Returns BARO_ERR_FAULT if fault injection is active; in that case
 * @p reading is unchanged and SensorService marks the sample invalid
 * (REQ-SA-0E1). Returns BARO_ERR_FAULT if @p reading is NULL.
 *
 * @param[out] reading  Populated on BARO_ERR_OK. Must not be NULL.
 * @return BARO_ERR_OK or BARO_ERR_FAULT.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
baro_err_t barometer_read(baro_reading_t *reading);

/**
 * @brief Arm or disarm fault injection.
 *
 * When @p inject is true, all subsequent barometer_read() calls return
 * BARO_ERR_FAULT. When @p inject is false, normal simulation resumes.
 * Called from the CLI self-test command (REQ-LI-130) and from unit tests.
 * Not part of IBarometer.
 *
 * @param inject  true to inject faults; false to resume normal operation.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
void barometer_inject_fault(bool inject);

/* ------------------------------------------------------------------ */
/* Test visibility macro                                               */
/* ------------------------------------------------------------------ */

#ifdef TEST
#define BARO_TEST_VISIBLE
#else
#define BARO_TEST_VISIBLE static
#endif

/* ------------------------------------------------------------------ */
/* Test-only hooks                                                     */
/* ------------------------------------------------------------------ */

#ifdef TEST
/** @brief Reset module state to power-on defaults. */
void barometer_reset_for_test(void);

/** @brief Force internal pressure to @p pressure_x10 (boundary tests). */
void barometer_set_pressure_for_test(int32_t pressure_x10);

/** @brief Read back internal state (T-BARO-01 state verification). */
void barometer_get_state_for_test(int32_t *out_pressure, bool *out_fault);
#endif /* TEST */

#endif /* BAROMETER_DRIVER_H */
