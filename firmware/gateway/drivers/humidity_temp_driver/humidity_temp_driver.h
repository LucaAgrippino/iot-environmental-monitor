/**
 * @file humidity_temp_driver.h
 * @brief HumidityTempDriver (Gateway) — HTS221 humidity and temperature over I2C2.
 *
 * Provides IHumidityTemp (per components.md). Platform singleton with a vtable
 * (GPA-D6, consistent with I2cDriver LLD-D10): one HTS221 per board, so the
 * Gateway ADT pattern (opaque handle + pool) is not applied.
 *
 * All register access goes through the II2c vtable singleton `i2c_driver`.
 * Compensation is integer-only; the driver outputs fixed-point values that
 * match the Modbus register map scaling (temperature and humidity x 100).
 *
 * The Field Device has a different, simulated driver with the same file name
 * (firmware/field-device/drivers/humidity_temp_driver/). The two are built
 * into separate images and never share a link.
 *
 * @note Companion: docs/lld/drivers/gw-sensor-drivers-group-a-lld.md
 */

#ifndef HUMIDITY_TEMP_DRIVER_H
#define HUMIDITY_TEMP_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "i2c/i2c_driver.h" /* ii2c_t, i2c_err_t (transitive — for error mapping) */

/**
 * @brief Error codes returned by all HumidityTempDriver operations.
 */
typedef enum
{
    HUMIDITY_TEMP_ERR_OK = 0,         /**< Operation succeeded. */
    HUMIDITY_TEMP_ERR_I2C = 1,        /**< I2C bus error (NACK, timeout, or bus busy). */
    HUMIDITY_TEMP_ERR_NOT_READY = 2,  /**< Data not ready after retry, or init not done. */
    HUMIDITY_TEMP_ERR_INVALID_ARG = 3 /**< NULL pointer passed as output argument. */
} humidity_temp_err_t;

/**
 * @brief Compensated humidity and temperature reading from the HTS221.
 *
 * Fixed-point integers matching the Modbus register map scaling:
 * temperature in degC x 100, humidity in %RH x 100.
 */
typedef struct
{
    int32_t temperature_x100; /**< degC x 100; e.g. 2350 = 23.50 degC. */
    int32_t humidity_x100;    /**< %RH x 100; e.g. 5500 = 55.00 %RH. */
} humidity_temp_data_t;

/**
 * @brief Initialise the HTS221 humidity and temperature sensor.
 *
 * Writes CTRL_REG1 (0x20) = 0x85 (active, BDU, ODR 1 Hz) and AV_CONF (0x10)
 * = 0x1B (default averaging). Burst-reads calibration registers 0x30..0x3F via
 * auto-increment and stores the coefficients for later compensation.
 *
 * Must be called once before the scheduler starts, after i2c_init(). May be
 * called in any order relative to barometer_init().
 *
 * @return HUMIDITY_TEMP_ERR_OK on success;
 *         HUMIDITY_TEMP_ERR_I2C if any I2C transaction fails.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
humidity_temp_err_t humidity_temp_init(void);

/**
 * @brief Read compensated humidity and temperature from the HTS221.
 *
 * Checks STATUS_REG (0x27) bits H_DA and T_DA. If not both set, retries once
 * after a 2 ms vTaskDelay. On success burst-reads HUMIDITY_OUT and TEMP_OUT
 * (four bytes) and applies integer linear interpolation against the stored
 * factory calibration.
 *
 * @param[out] out  Receives the compensated reading. Must not be NULL.
 *                  Left unchanged on any error.
 * @return HUMIDITY_TEMP_ERR_OK on success;
 *         HUMIDITY_TEMP_ERR_INVALID_ARG if out is NULL;
 *         HUMIDITY_TEMP_ERR_NOT_READY if init has not succeeded, or data is
 *         still not ready after the retry;
 *         HUMIDITY_TEMP_ERR_I2C on an I2C bus error.
 * @note Threading: task-context only, may block (2 ms retry). Not ISR-safe.
 */
humidity_temp_err_t humidity_temp_read(humidity_temp_data_t *out);

/**
 * @brief Vtable interface for HumidityTempDriver (IHumidityTemp).
 *
 * SensorService depends on this interface, not on the concrete driver.
 */
typedef struct
{
    /* cppcheck-suppress unusedStructMember; used via humidity_temp_driver singleton */
    humidity_temp_err_t (*init)(void);
    /* cppcheck-suppress unusedStructMember; used via humidity_temp_driver singleton */
    humidity_temp_err_t (*read)(humidity_temp_data_t *out);
} ihumidity_temp_t;

/** Singleton pointer — defined in humidity_temp_driver.c. */
extern const ihumidity_temp_t *const humidity_temp_driver;

#ifdef TEST
/**
 * @brief Reset module-level state for unit tests.
 *
 * Clears the private state (ready = false, calibration zeroed) so each Unity
 * test case starts from a clean slate. Test-only; not compiled into firmware.
 */
void humidity_temp_reset_for_test(void);
#endif

#endif /* HUMIDITY_TEMP_DRIVER_H */
