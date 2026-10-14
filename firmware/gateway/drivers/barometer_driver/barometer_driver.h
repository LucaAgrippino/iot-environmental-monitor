/**
 * @file barometer_driver.h
 * @brief BarometerDriver (Gateway) — LPS22HB pressure and temperature over I2C2.
 *
 * Provides IBarometer (per components.md). Platform singleton with a vtable
 * (GPA-D6, consistent with I2cDriver LLD-D10): one LPS22HB per board, so the
 * Gateway ADT pattern (opaque handle + pool) is not applied.
 *
 * The LPS22HB outputs compensated values natively; the driver only
 * reconstructs the 24-bit pressure word and rescales it to hPa x 10.
 *
 * The Field Device has a different, simulated driver with the same file name
 * (firmware/field-device/drivers/barometer_driver/). The two are built into
 * separate images and never share a link.
 *
 * @note Companion: docs/lld/drivers/gw-sensor-drivers-group-a-lld.md
 */

#ifndef BAROMETER_DRIVER_H
#define BAROMETER_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "i2c/i2c_driver.h" /* ii2c_t, i2c_err_t (transitive — for error mapping) */

/**
 * @brief Error codes returned by all BarometerDriver operations.
 */
typedef enum
{
    BAROMETER_ERR_OK = 0,         /**< Operation succeeded. */
    BAROMETER_ERR_I2C = 1,        /**< I2C bus error (NACK, timeout, or bus busy). */
    BAROMETER_ERR_NOT_READY = 2,  /**< Data not ready after retry, or init not done. */
    BAROMETER_ERR_INVALID_ARG = 3 /**< NULL pointer passed as output argument. */
} barometer_err_t;

/**
 * @brief Pressure and temperature reading from the LPS22HB.
 *
 * Pressure in hPa x 10 (matches the Modbus register map scaling).
 * Temperature in degC x 100 (native LPS22HB output units).
 */
typedef struct
{
    int32_t pressure_x10;     /**< hPa x 10; e.g. 10132 = 1013.2 hPa. */
    int32_t temperature_x100; /**< degC x 100; e.g. 2350 = 23.50 degC. */
} barometer_data_t;

/**
 * @brief Initialise the LPS22HB barometric pressure sensor.
 *
 * Writes CTRL_REG1 (0x10) = 0x12 (ODR 1 Hz, BDU) and CTRL_REG2 (0x11) = 0x10
 * (IF_ADD_INC set, normal mode). No calibration is needed — the LPS22HB
 * outputs compensated values natively.
 *
 * Must be called once before the scheduler starts, after i2c_init(). May be
 * called in any order relative to humidity_temp_init().
 *
 * @return BAROMETER_ERR_OK on success;
 *         BAROMETER_ERR_I2C if any I2C transaction fails.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
barometer_err_t barometer_init(void);

/**
 * @brief Read pressure and temperature from the LPS22HB.
 *
 * Checks STATUS_REG (0x27) bits P_DA and T_DA. If not both set, retries once
 * after a 2 ms vTaskDelay. On success burst-reads PRESS_OUT_XL/L/H and
 * TEMP_OUT_L/H (five bytes), sign-extends the 24-bit pressure and converts it
 * from hPa x 4096 to hPa x 10.
 *
 * @param[out] out  Receives the reading. Must not be NULL.
 *                  Left unchanged on any error.
 * @return BAROMETER_ERR_OK on success;
 *         BAROMETER_ERR_INVALID_ARG if out is NULL;
 *         BAROMETER_ERR_NOT_READY if init has not succeeded, or data is still
 *         not ready after the retry;
 *         BAROMETER_ERR_I2C on an I2C bus error.
 * @note Threading: task-context only, may block (2 ms retry). Not ISR-safe.
 */
barometer_err_t barometer_read(barometer_data_t *out);

/**
 * @brief Vtable interface for BarometerDriver (IBarometer).
 *
 * SensorService depends on this interface, not on the concrete driver.
 */
typedef struct
{
    /* cppcheck-suppress unusedStructMember; used via barometer_driver singleton */
    barometer_err_t (*init)(void);
    /* cppcheck-suppress unusedStructMember; used via barometer_driver singleton */
    barometer_err_t (*read)(barometer_data_t *out);
} ibarometer_t;

/** Singleton pointer — defined in barometer_driver.c. */
extern const ibarometer_t *const barometer_driver;

#ifdef TEST
/**
 * @brief Reset module-level state for unit tests.
 *
 * Clears the private state (ready = false) so each Unity test case starts
 * from a clean slate. Test-only; not compiled into firmware builds.
 */
void barometer_reset_for_test(void);
#endif

#endif /* BAROMETER_DRIVER_H */
