/**
 * @file magnetometer_driver.h
 * @brief MagnetometerDriver (Gateway) — LIS3MDL 3-axis magnetometer over I2C2.
 *
 * Provides IMagnetometer (per components.md). Platform singleton with a vtable
 * (GPB-D8, consistent with I2cDriver LLD-D10): one LIS3MDL per board.
 *
 * Two-phase initialisation (GPB-D1):
 *   1. magnetometer_init()                  — before the scheduler: configures
 *      the sensor, the DRDY pin (PC8) and EXTI line 8, but leaves the interrupt masked.
 *   2. magnetometer_attach_drdy_callback()  — from SensorTask start-up: stores the
 *      callback and context, then unmasks EXTI line 8.
 *
 * Output is raw LSB (GPB-D4); sensitivity at +/-4 gauss is 6842 LSB/gauss.
 *
 * @note Companion: docs/lld/drivers/gw-sensor-drivers-group-b-lld.md
 */

#ifndef MAGNETOMETER_DRIVER_H
#define MAGNETOMETER_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "i2c/i2c_driver.h" /* ii2c_t, i2c_err_t (transitive — for error mapping) */

/** Sensitivity at +/-4 gauss full scale, LSB per gauss (LIS3MDL datasheet). */
#define MAGNETOMETER_LSB_PER_GAUSS (6842)

/**
 * @brief Error codes returned by all MagnetometerDriver operations.
 */
typedef enum
{
    MAGNETOMETER_ERR_OK = 0,          /**< Operation succeeded. */
    MAGNETOMETER_ERR_I2C = 1,         /**< I2C bus error (NACK, timeout, bus busy, bad argument). */
    MAGNETOMETER_ERR_NOT_READY = 2,   /**< ZYXDA not set, or init has not succeeded. */
    MAGNETOMETER_ERR_INVALID_ARG = 3, /**< NULL pointer passed as argument. */
    MAGNETOMETER_ERR_HW = 4           /**< GPIO or EXTI setup failed (GPB-D9). */
} magnetometer_err_t;

/**
 * @brief Raw 16-bit magnetic field vector from the LIS3MDL.
 */
typedef struct
{
    int16_t x; /**< X-axis magnetic field, raw LSB. */
    int16_t y; /**< Y-axis magnetic field, raw LSB. */
    int16_t z; /**< Z-axis magnetic field, raw LSB. */
} magnetometer_data_t;

/**
 * @brief DRDY callback, invoked from EXTI ISR context.
 *
 * Must be ISR-safe: no blocking calls, only *FromISR() FreeRTOS APIs.
 *
 * @param ctx  Opaque context registered with the callback (typically a TaskHandle_t).
 */
typedef void (*magnetometer_drdy_cb_t)(void *ctx);

/**
 * @brief Initialise the LIS3MDL (Phase 1 — pre-scheduler).
 *
 * Writes CTRL_REG1..5 (+/-4 gauss, ultra-high-performance XY and Z, 10 Hz, BDU,
 * continuous conversion), configures PC8 as a floating input and EXTI line 8 for
 * a rising edge. Does NOT unmask the interrupt. Safe to call again after a sensor
 * failure (REQ-NF-205): an already-configured EXTI line is accepted and the
 * registered callback is kept.
 *
 * @return MAGNETOMETER_ERR_OK on success;
 *         MAGNETOMETER_ERR_I2C if any I2C transaction fails;
 *         MAGNETOMETER_ERR_HW if the GPIO or EXTI configuration fails.
 * @note Threading: task-context only, non-blocking. Call before the scheduler starts,
 *       after i2c_init(), gpio_init().
 */
magnetometer_err_t magnetometer_init(void);

/**
 * @brief Register the DRDY callback and unmask EXTI line 8 (Phase 2 — post-scheduler).
 *
 * The LIS3MDL holds DRDY high until its output registers are read, so a conversion
 * that finished before the interrupt was unmasked would never produce an edge. If
 * DRDY is already high once the line is armed, the output registers are read once
 * to release it.
 *
 * @param[in] cb   Callback invoked in ISR context. Must not be NULL.
 * @param[in] ctx  Opaque context passed to cb.
 * @return MAGNETOMETER_ERR_OK on success;
 *         MAGNETOMETER_ERR_INVALID_ARG if cb is NULL;
 *         MAGNETOMETER_ERR_NOT_READY if magnetometer_init() has not succeeded;
 *         MAGNETOMETER_ERR_HW if the EXTI line cannot be enabled;
 *         MAGNETOMETER_ERR_I2C if releasing a stale DRDY fails.
 * @note Threading: task-context only. Call once from SensorTask before the first read.
 */
magnetometer_err_t magnetometer_attach_drdy_callback(magnetometer_drdy_cb_t cb, void *ctx);

/**
 * @brief Read the current magnetic field vector.
 *
 * Checks STATUS_REG (0x27) ZYXDA; if clear returns NOT_READY with no retry (the DRDY
 * notification has already arrived). Otherwise burst-reads OUT_X_L..OUT_Z_H (six
 * bytes, register address bit 7 set for auto-increment) and assembles three int16_t.
 *
 * @param[out] out  Receives the raw reading. Must not be NULL. Unchanged on error.
 * @return MAGNETOMETER_ERR_OK on success;
 *         MAGNETOMETER_ERR_INVALID_ARG if out is NULL;
 *         MAGNETOMETER_ERR_NOT_READY if init has not succeeded or ZYXDA is clear;
 *         MAGNETOMETER_ERR_I2C on an I2C bus error.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
magnetometer_err_t magnetometer_read(magnetometer_data_t *out);

/**
 * @brief Vtable interface for MagnetometerDriver (IMagnetometer).
 */
typedef struct
{
    /* cppcheck-suppress unusedStructMember; used via magnetometer_driver singleton */
    magnetometer_err_t (*init)(void);
    /* cppcheck-suppress unusedStructMember; used via magnetometer_driver singleton */
    magnetometer_err_t (*attach_drdy_callback)(magnetometer_drdy_cb_t cb, void *ctx);
    /* cppcheck-suppress unusedStructMember; used via magnetometer_driver singleton */
    magnetometer_err_t (*read)(magnetometer_data_t *out);
} imagnetometer_t;

/** Singleton pointer — defined in magnetometer_driver.c. */
extern const imagnetometer_t *const magnetometer_driver;

#ifdef TEST
/**
 * @brief Reset module-level state for unit tests (ready = false, callback cleared).
 *
 * Test-only; not compiled into firmware builds.
 */
void magnetometer_reset_for_test(void);
#endif

#endif /* MAGNETOMETER_DRIVER_H */
