/**
 * @file imu_driver.h
 * @brief ImuDriver (Gateway) — LSM6DSL accelerometer + gyroscope over I2C2.
 *
 * Provides IImu (per components.md). Platform singleton with a vtable
 * (GPB-D8, consistent with I2cDriver LLD-D10): one LSM6DSL per board.
 *
 * Two-phase initialisation (GPB-D1):
 *   1. imu_init()                 — before the scheduler: configures the sensor, the
 *      INT1 pin (PD11) and EXTI line 11, but leaves the interrupt masked.
 *   2. imu_attach_int1_callback() — from SensorTask start-up: stores the callback and
 *      context, then unmasks EXTI line 11.
 *
 * Output is raw LSB (GPB-D4). At +/-2 g the accelerometer resolves 0.061 mg/LSB
 * (about 16384 LSB/g); at +/-250 dps the gyroscope resolves 8.75 mdps/LSB (about
 * 114.3 LSB/dps).
 *
 * @note Companion: docs/lld/drivers/gw-sensor-drivers-group-b-lld.md
 */

#ifndef IMU_DRIVER_H
#define IMU_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "i2c/i2c_driver.h" /* ii2c_t, i2c_err_t (transitive — for error mapping) */

/**
 * @brief Error codes returned by all ImuDriver operations.
 */
typedef enum
{
    IMU_ERR_OK = 0,          /**< Operation succeeded. */
    IMU_ERR_I2C = 1,         /**< I2C bus error (NACK, timeout, bus busy, bad argument). */
    IMU_ERR_NOT_READY = 2,   /**< XLDA or GDA not set, or init has not succeeded. */
    IMU_ERR_INVALID_ARG = 3, /**< NULL pointer passed as argument. */
    IMU_ERR_HW = 4           /**< GPIO or EXTI setup failed (GPB-D9). */
} imu_err_t;

/**
 * @brief Raw 16-bit accelerometer and gyroscope reading from the LSM6DSL.
 */
typedef struct
{
    int16_t accel_x; /**< X-axis acceleration, raw LSB. */
    int16_t accel_y; /**< Y-axis acceleration, raw LSB. */
    int16_t accel_z; /**< Z-axis acceleration, raw LSB. */
    int16_t gyro_x;  /**< X-axis angular rate, raw LSB. */
    int16_t gyro_y;  /**< Y-axis angular rate, raw LSB. */
    int16_t gyro_z;  /**< Z-axis angular rate, raw LSB. */
} imu_data_t;

/**
 * @brief INT1 callback, invoked from EXTI ISR context.
 *
 * Must be ISR-safe: no blocking calls, only *FromISR() FreeRTOS APIs.
 *
 * @param ctx  Opaque context registered with the callback (typically a TaskHandle_t).
 */
typedef void (*imu_int1_cb_t)(void *ctx);

/**
 * @brief Initialise the LSM6DSL (Phase 1 — pre-scheduler).
 *
 * Writes CTRL3_C (BDU, IF_INC), INT1_CTRL (DRDY_G + DRDY_XL on INT1), CTRL1_XL
 * (+/-2 g, 104 Hz) and CTRL2_G (+/-250 dps, 104 Hz); configures PD11 as a floating
 * input and EXTI line 11 for a rising edge. Does NOT unmask the interrupt. Safe to
 * call again after a sensor failure (REQ-NF-205): an already-configured EXTI line is
 * accepted and the registered callback is kept.
 *
 * @return IMU_ERR_OK on success;
 *         IMU_ERR_I2C if any I2C transaction fails;
 *         IMU_ERR_HW if the GPIO or EXTI configuration fails.
 * @note Threading: task-context only, non-blocking. Call before the scheduler starts,
 *       after i2c_init(), gpio_init().
 */
imu_err_t imu_init(void);

/**
 * @brief Register the INT1 callback and unmask EXTI line 11 (Phase 2 — post-scheduler).
 *
 * The LSM6DSL holds INT1 high until its output registers are read, so a conversion
 * that finished before the interrupt was unmasked would never produce an edge. If INT1
 * is already high once the line is armed, the output registers are read once to
 * release it.
 *
 * @param[in] cb   Callback invoked in ISR context. Must not be NULL.
 * @param[in] ctx  Opaque context passed to cb.
 * @return IMU_ERR_OK on success;
 *         IMU_ERR_INVALID_ARG if cb is NULL;
 *         IMU_ERR_NOT_READY if imu_init() has not succeeded;
 *         IMU_ERR_HW if the EXTI line cannot be enabled;
 *         IMU_ERR_I2C if releasing a stale INT1 fails.
 * @note Threading: task-context only. Call once from SensorTask before the first read.
 */
imu_err_t imu_attach_int1_callback(imu_int1_cb_t cb, void *ctx);

/**
 * @brief Read the current acceleration and angular rate.
 *
 * Checks STATUS_REG (0x1E) XLDA and GDA; if either is clear returns NOT_READY with no
 * retry (the INT1 notification has already arrived). Otherwise burst-reads OUTX_L_G..
 * OUTZ_H_XL (twelve bytes, auto-increment via CTRL3_C.IF_INC) and assembles six int16_t.
 *
 * @param[out] out  Receives the raw reading. Must not be NULL. Unchanged on error.
 * @return IMU_ERR_OK on success;
 *         IMU_ERR_INVALID_ARG if out is NULL;
 *         IMU_ERR_NOT_READY if init has not succeeded or XLDA / GDA is clear;
 *         IMU_ERR_I2C on an I2C bus error.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
imu_err_t imu_read(imu_data_t *out);

/**
 * @brief Vtable interface for ImuDriver (IImu).
 */
typedef struct
{
    /* cppcheck-suppress unusedStructMember; used via imu_driver singleton */
    imu_err_t (*init)(void);
    /* cppcheck-suppress unusedStructMember; used via imu_driver singleton */
    imu_err_t (*attach_int1_callback)(imu_int1_cb_t cb, void *ctx);
    /* cppcheck-suppress unusedStructMember; used via imu_driver singleton */
    imu_err_t (*read)(imu_data_t *out);
} iimu_t;

/** Singleton pointer — defined in imu_driver.c. */
extern const iimu_t *const imu_driver;

#ifdef TEST
/**
 * @brief Reset module-level state for unit tests (ready = false, callback cleared).
 *
 * Test-only; not compiled into firmware builds.
 */
void imu_reset_for_test(void);
#endif

#endif /* IMU_DRIVER_H */
