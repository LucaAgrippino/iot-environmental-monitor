/**
 * @file humidity_temp_driver.h
 * @brief Simulated temperature and humidity driver — readings via bounded random walk.
 *
 * Implements IHumidityTemp for the STM32F469 Field Device. No hardware dependency;
 * the simulation is self-contained. Replacing this driver with a physical I2C
 * sensor requires only a driver-layer change (Vision §5.1.1).
 *
 * @note See docs/lld/drivers/simulated-sensor-drivers.md for the full specification.
 */

#ifndef HUMIDITY_TEMP_DRIVER_H
#define HUMIDITY_TEMP_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Constants                                                           */
/* ------------------------------------------------------------------ */

/** Minimum valid temperature × 100 (−40.00 °C). */
#define HT_TEMPERATURE_MIN_X100 (-4000)
/** Maximum valid temperature × 100 (+85.00 °C). */
#define HT_TEMPERATURE_MAX_X100 (8500)
/** Minimum valid humidity × 100 (0.00 %RH). */
#define HT_HUMIDITY_MIN_X100 (0U)
/** Maximum valid humidity × 100 (100.00 %RH). */
#define HT_HUMIDITY_MAX_X100 (10000U)
/** Default temperature × 100 (22.00 °C). */
#define HT_DEFAULT_TEMPERATURE_X100 (2200)
/** Default humidity × 100 (50.00 %RH). */
#define HT_DEFAULT_HUMIDITY_X100 (5000U)

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Error codes for HumidityTempDriver.
 */
typedef enum
{
    HT_ERR_OK = 0,    /**< Reading produced successfully. */
    HT_ERR_FAULT = 1, /**< Fault injection active. */
} ht_err_t;

/* ------------------------------------------------------------------ */
/* Data types                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief A single combined temperature and humidity sample.
 *
 * temperature_x100: temperature in units of 0.01 °C.
 *   Example: 2350 → 23.50 °C. Valid range: −4000..8500.
 *   Matches TEMPERATURE register encoding (modbus-register-map.md §6.2).
 *
 * humidity_x100: relative humidity in units of 0.01 %RH.
 *   Example: 5500 → 55.00 %RH. Valid range: 0..10000.
 *   Matches HUMIDITY register encoding.
 */
typedef struct
{
    // cppcheck-suppress unusedStructMember -- populated here, consumed by SensorService callers not
    // in cppcheck's file set
    int32_t temperature_x100;
    // cppcheck-suppress unusedStructMember -- populated here, consumed by SensorService callers not
    // in cppcheck's file set
    uint32_t humidity_x100;
} ht_reading_t;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise the temperature and humidity simulation module.
 *
 * Resets both simulations to default values and clears fault injection
 * (REQ-SA-030).
 *
 * @return HT_ERR_OK always.
 * @note Threading: task-context only, non-blocking. Must be called before
 *       the scheduler starts.
 */
ht_err_t humidity_temp_init(void);

/**
 * @brief Read one combined temperature and humidity sample.
 *
 * Advances both simulations by one step (bounded random walk, §3.2).
 * Returns HT_ERR_FAULT if fault injection is active or if @p reading is NULL;
 * in the fault case @p reading is unchanged.
 *
 * @param[out] reading  Populated on HT_ERR_OK. Must not be NULL.
 * @return HT_ERR_OK or HT_ERR_FAULT.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
ht_err_t humidity_temp_read(ht_reading_t *reading);

/**
 * @brief Arm or disarm fault injection (same semantics as barometer_inject_fault).
 *
 * When @p inject is true, all subsequent humidity_temp_read() calls return
 * HT_ERR_FAULT. When @p inject is false, normal simulation resumes.
 *
 * @param inject  true to inject faults; false to resume normal operation.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
void humidity_temp_inject_fault(bool inject);

/* ------------------------------------------------------------------ */
/* Test visibility macro                                               */
/* ------------------------------------------------------------------ */

#ifdef TEST
#define HT_TEST_VISIBLE
#else
#define HT_TEST_VISIBLE static
#endif

/* ------------------------------------------------------------------ */
/* Test-only hooks                                                     */
/* ------------------------------------------------------------------ */

#ifdef TEST
/** @brief Reset module state to power-on defaults. */
void humidity_temp_reset_for_test(void);

/** @brief Force internal temperature to @p temperature_x100 (boundary tests). */
void humidity_temp_set_temp_for_test(int32_t temperature_x100);

/** @brief Force internal humidity to @p humidity_x100 (boundary tests). */
void humidity_temp_set_humidity_for_test(uint32_t humidity_x100);

/** @brief Read back internal state (T-HT-01 state verification). */
void humidity_temp_get_state_for_test(int32_t *out_temp, uint32_t *out_humidity, bool *out_fault);
#endif /* TEST */

#endif /* HUMIDITY_TEMP_DRIVER_H */
