# LLD Companion — Gateway Sensor Drivers Group B

## MagnetometerDriver (LIS3MDL) + ImuDriver (LSM6DSL)

**Board:** Gateway (B-L475E-IOT01A)
**Layer:** Driver
**Provides:** `IMagnetometer`, `IImu`
**Consumes:** `II2c` (I2cDriver), `GpioDriver`, `ExtiDriver`
**SRS traces:** REQ-SA-031, REQ-SA-071, REQ-SA-040, REQ-SA-060,
REQ-SA-080
**HLD ref:** `components.md` — MagnetometerDriver, ImuDriver
(GW driver layer);
`sequence-diagrams.md` SD-01;
`task-breakdown.md` §5.2 SensorTask, §6.1 ISR inventory
**Version:** 1.1
**Date:** October 2026
**Status:** Pass H — Implemented (host-tested; hardware validation pending, see §14 GPB-O7)

**HLD anchor:** MagnetometerDriver + ImuDriver in
`components.md` (GW driver layer)

---

## 1. Purpose and scope

### 1.1 In scope

- The public interfaces `IMagnetometer` (`magnetometer_init`,
  `magnetometer_attach_drdy_callback`, `magnetometer_read`,
  `magnetometer_reset_for_test`) and `IImu` (`imu_init`,
  `imu_attach_int1_callback`, `imu_read`,
  `imu_reset_for_test`).
- Vtable singleton pattern for both interfaces (consistent with
  I2cDriver LLD-D10).
- Two-phase init pattern: Phase 1 (pre-scheduler) configures
  sensors and EXTI GPIO; Phase 2 (post-scheduler) registers
  callbacks and enables interrupts.
- DRDY/INT1 ISR handler design and integration with
  `stm32l4xx_it.c`.
- LIS3MDL 6-byte burst read and int16_t assembly (magnetometer
  x/y/z).
- LSM6DSL 12-byte burst read and int16_t assembly (gyro x/y/z +
  accel x/y/z).
- STATUS_REG polling as defence-in-depth before output register
  read.
- Hardware contract: I2C addresses, init register writes,
  EXTI pin assignments.
- Error codes, error table, and propagation strategy.
- Memory sizing, file layout, and unit-test plan.

### 1.2 Out of scope

- **I2C bus initialisation, pin configuration, bus recovery** —
  I2cDriver companion.
- **SensorService aggregation, IIR filtering, alarm evaluation** —
  SensorService + AlarmService companion (Application layer).
- **SensorTask scheduling, stack sizing** —
  `task-breakdown.md` §5.2.
- **Modbus register encoding** —
  `docs/modbus-register-map.md`. Note: magnetometer and IMU data
  are not mapped to Modbus registers; they are gateway-local,
  forwarded to cloud telemetry by CloudPublisher.
- **Physical-unit conversion** — application-layer concern (P2).
  Sensitivity constants are documented in headers for
  SensorService to apply.

### 1.3 Terminology

- **DRDY** — Data Ready; hardware signal asserted by the sensor
  when new data is available in output registers.
- **INT1** — Interrupt 1; LSM6DSL interrupt output pin routed to
  the MCU via EXTI.
- **EXTI** — External Interrupt controller; MCU peripheral that
  generates interrupts from GPIO edges.
- **Two-phase init** — Pattern where sensor configuration occurs
  before the scheduler starts (Phase 1), and interrupt callback
  registration occurs after (Phase 2). Required because callbacks
  store FreeRTOS task handles that only exist post-scheduler.
- **BDU** — Block Data Update; prevents reading a mix of old high
  byte and new low byte during a conversion cycle.

---

## 2. Sources

This companion covers two Gateway driver components that share
I2C2 and use DRDY/INT GPIO interrupt lines to notify SensorTask
when data is ready. This distinguishes them from Group A drivers,
which use STATUS_REG polling only.

| Component | Device | I2C address (7-bit) | PROVIDES | Root req |
|-----------|--------|---------------------|----------|----------|
| MagnetometerDriver | LIS3MDL | 0x1E | IMagnetometer | REQ-SA-031 |
| ImuDriver | LSM6DSL | 0x6A | IImu | REQ-SA-071 |

Both sensors are Gateway-only. No Field Device variant exists. No
platform-split `.c` files are required.

**Key design difference from Group A:** these drivers use a
two-phase init pattern (GPB-D1). Phase 1 configures the sensor
and the EXTI GPIO pin but does not enable the interrupt. Phase 2
(post-scheduler) registers a callback and enables the EXTI
interrupt. This is required because the callback stores a task
handle, which only exists after `xTaskCreate()`.

### 2.1 Source references

| Source | Relevant section |
|--------|-----------------|
| `components.md` — Gateway driver layer | MagnetometerDriver, ImuDriver responsibility sentences |
| `SRS.md` | REQ-SA-031 (magnetometer init/read), REQ-SA-071 (IMU init/read), REQ-SA-040/060/080 (error handling) |
| `task-breakdown.md` §5.2, §6.1 | SensorTask priority 5, 512 words; ISR inventory (GPB-O1: EXTI entries to be added) |
| `sequence-diagrams.md` SD-01 | Sensor read nominal flow |
| UM2153 §7.12.3 | LIS3MDL features, I2C2 |
| UM2153 §7.12.4 | LSM6DSL features, I2C2 |
| UM2153 Table 3 | I2C addresses confirmed |
| UM2153 Appendix B, Fig. 27 | DRDY/INT GPIO connections to EXTI lines |
| UM2153 Appendix B, Fig. 23 | MCU pin labels for EXTI lines |
| LIS3MDL datasheet | CTRL_REG registers, DRDY pin config, output registers, sensitivity |
| LSM6DSL datasheet | CTRL_REG registers, INT1_CTRL, output registers, sensitivity |

### 2.2 Consumed interfaces

MagnetometerDriver and ImuDriver call the following external
functions. Full API documentation is in the corresponding
companion documents.

**I2cDriver** (`i2c_driver.h`) — functions called by both drivers:

```c
i2c_err_t i2c_write(uint8_t dev_addr, const uint8_t *data, uint16_t len);
i2c_err_t i2c_write_read(uint8_t dev_addr,
                          const uint8_t *tx_data, uint16_t tx_len,
                          uint8_t       *rx_buf,  uint16_t rx_len);
```

Types consumed from `i2c_driver.h`:

```c
typedef enum {
    I2C_ERR_OK       = 0,
    I2C_ERR_NACK     = 1,
    I2C_ERR_TIMEOUT  = 2,
    I2C_ERR_BUS_BUSY = 3,
} i2c_err_t;
```

Access path: both drivers call `i2c_driver->write()` and
`i2c_driver->write_read()` through the `II2c` vtable singleton
pointer.

**GpioDriver** (`gpio_driver.h`) — Phase 1 configures the pin, Phase 2 samples it:

```c
gpio_err_t gpio_configure_pin(const gpio_pin_config_t *config);
gpio_err_t gpio_read_pin(gpio_port_t port, uint8_t pin, gpio_level_t *out_level);
```

The DRDY / INT1 pin is configured as an input with no pull (the sensor drives
the line actively). v1.0 named a `gpio_configure_input()` that does not exist
(GPB-D9).

**ExtiDriver** (`exti_driver.h`):

```c
exti_err_t exti_configure(uint8_t line, exti_port_t port, exti_edge_t edge);
exti_err_t exti_enable(uint8_t line, uint32_t nvic_priority);
void       exti_clear_pending(uint8_t line);   /* called by the ISR glue */
```

Phase 1 calls `exti_configure()` for a rising edge (an `EXTI_ERR_CONFLICT`
from a repeated init is accepted). Phase 2 calls `exti_enable()`. The ISR glue
calls `exti_clear_pending()`. The port argument is `exti_port_t`, not
`gpio_port_t`.

### 2.3 Dependency-conformance check

| Dependency | In `components.md` | Actual usage | Action |
|------------|-------------------|--------------|--------|
| I2cDriver | Yes | Yes — all register accesses via `i2c_driver->write()` / `i2c_driver->write_read()` | Correct |
| GpioDriver | Yes | Yes — DRDY/INT1 pin configured as input in Phase 1 | Correct |

---

## 3. Traceability

| Concern | SRS requirements | Use cases |
|---------|-----------------|-----------|
| Sensor initialisation | REQ-SA-031 | UC-01 |
| Sensor data read | REQ-SA-071 | UC-01, UC-02 |
| Sensor error detection | REQ-SA-040 | UC-01 |
| Continue with available sensors | REQ-SA-060 | UC-01 |
| Error code propagation | REQ-SA-080 | UC-01 |

---

## 4. Public API

### 4.1 MagnetometerDriver — data types

```c
/* magnetometer_driver.h */

#ifndef MAGNETOMETER_DRIVER_H
#define MAGNETOMETER_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include "i2c/i2c_driver.h"

/**
 * @brief Error codes returned by all MagnetometerDriver operations.
 */
typedef enum {
    MAGNETOMETER_ERR_OK          = 0, /**< Operation succeeded. */
    MAGNETOMETER_ERR_I2C         = 1, /**< I2C bus error (NACK, timeout,
                                           or bus busy). */
    MAGNETOMETER_ERR_NOT_READY   = 2, /**< STATUS_REG ZYXDA not set. */
    MAGNETOMETER_ERR_INVALID_ARG = 3, /**< NULL pointer passed as
                                           argument. */
    MAGNETOMETER_ERR_HW          = 4  /**< GPIO or EXTI setup failed
                                           (GPB-D9). */
} magnetometer_err_t;

/**
 * @brief Raw 16-bit magnetic field vector from LIS3MDL.
 *
 * Values are raw signed 16-bit LSB counts. Sensitivity at ±4 gauss
 * full scale: 6842 LSB/gauss. SensorService applies the sensitivity
 * factor when building cloud telemetry.
 */
typedef struct {
    int16_t x;   /**< X-axis magnetic field, raw LSB. */
    int16_t y;   /**< Y-axis magnetic field, raw LSB. */
    int16_t z;   /**< Z-axis magnetic field, raw LSB. */
} magnetometer_data_t;

/**
 * @brief DRDY callback type invoked from EXTI ISR context.
 *
 * The callback must be ISR-safe: no blocking calls, no FreeRTOS
 * API calls except `*FromISR()` variants.
 *
 * @param ctx  Opaque context pointer registered alongside the
 *             callback — typically a FreeRTOS TaskHandle_t cast
 *             to void*.
 */
typedef void (*magnetometer_drdy_cb_t)(void *ctx);
```

### 4.2 MagnetometerDriver — IMagnetometer

```c
/**
 * @brief Initialise the LIS3MDL magnetometer (Phase 1 — pre-scheduler).
 *
 * Writes CTRL_REG1–5 to configure ±4 gauss full scale,
 * ultra-high-performance mode, 10 Hz ODR, BDU enabled, continuous
 * conversion. Configures the DRDY GPIO pin as a rising-edge EXTI
 * input but does NOT enable the EXTI interrupt (Phase 2 does that).
 *
 * Must be called once from the board init function before
 * `vTaskStartScheduler()`.
 *
 * @return MAGNETOMETER_ERR_OK on success;
 *         MAGNETOMETER_ERR_I2C if any I2C transaction fails during
 *         register configuration.
 * @note   Threading: task-context only, non-blocking.
 *         Must be called before the scheduler starts.
 */
magnetometer_err_t magnetometer_init(void);

/**
 * @brief Register DRDY callback and enable EXTI interrupt
 *        (Phase 2 — post-scheduler).
 *
 * Stores the callback pointer and context in the module state,
 * then enables the LIS3MDL DRDY EXTI interrupt line. Must be
 * called from SensorTask startup, after `vTaskStartScheduler()`.
 *
 * @param[in] cb   Callback function invoked in ISR context when
 *                 DRDY fires. Must not be NULL.
 * @param[in] ctx  Opaque context passed to cb — typically
 *                 `xTaskGetCurrentTaskHandle()` cast to void*.
 * @return MAGNETOMETER_ERR_OK on success;
 *         MAGNETOMETER_ERR_INVALID_ARG if cb is NULL.
 * @note   Threading: task-context only. Call once from SensorTask
 *         before the first read cycle.
 */
magnetometer_err_t magnetometer_attach_drdy_callback(
    magnetometer_drdy_cb_t cb,
    void *ctx);

/**
 * @brief Read the current magnetic field vector from LIS3MDL.
 *
 * Checks STATUS_REG (0x27) bit 3 (ZYXDA). If not set, returns
 * MAGNETOMETER_ERR_NOT_READY (no retry — DRDY ISR has already
 * notified the caller). On success, burst-reads six output bytes
 * (OUT_X_L through OUT_Z_H) via auto-increment and assembles
 * three signed int16_t axis values.
 *
 * @param[out] out  Pointer to struct receiving the raw reading.
 *                  Must not be NULL.
 * @return MAGNETOMETER_ERR_OK on success;
 *         MAGNETOMETER_ERR_INVALID_ARG if out is NULL;
 *         MAGNETOMETER_ERR_I2C on I2C bus error;
 *         MAGNETOMETER_ERR_NOT_READY if ZYXDA not set.
 * @note   Threading: task-context only, non-blocking. Not ISR-safe.
 */
magnetometer_err_t magnetometer_read(magnetometer_data_t *out);

#ifdef TEST
/**
 * @brief Reset module-level state for unit tests.
 *
 * Clears s_mag to its post-bss-init value (ready = false,
 * callbacks = NULL) so each Unity test case starts from a clean
 * slate. Test-only; not compiled into firmware builds.
 */
void magnetometer_reset_for_test(void);
#endif
```

### 4.3 MagnetometerDriver — vtable interface

```c
/**
 * @brief Vtable interface for MagnetometerDriver (singleton pattern,
 *        consistent with I2cDriver LLD-D10).
 *
 * SensorService depends on this interface, not on the concrete
 * driver. The implementation file defines the singleton instance.
 */
typedef struct {
    magnetometer_err_t (*init)(void);
    magnetometer_err_t (*attach_drdy_callback)(
        magnetometer_drdy_cb_t cb, void *ctx);
    magnetometer_err_t (*read)(magnetometer_data_t *out);
} imagnetometer_t;

/** Singleton pointer — defined in magnetometer_driver.c. */
extern const imagnetometer_t * const magnetometer_driver;

#endif /* MAGNETOMETER_DRIVER_H */
```

**Vtable instance** (in `magnetometer_driver.c`):

```c
static const imagnetometer_t s_magnetometer_vtable = {
    .init                  = magnetometer_init,
    .attach_drdy_callback  = magnetometer_attach_drdy_callback,
    .read                  = magnetometer_read,
};

const imagnetometer_t * const magnetometer_driver =
    &s_magnetometer_vtable;
```

### 4.4 ImuDriver — data types

```c
/* imu_driver.h */

#ifndef IMU_DRIVER_H
#define IMU_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include "i2c/i2c_driver.h"

/**
 * @brief Error codes returned by all ImuDriver operations.
 */
typedef enum {
    IMU_ERR_OK          = 0, /**< Operation succeeded. */
    IMU_ERR_I2C         = 1, /**< I2C bus error (NACK, timeout,
                                  or bus busy). */
    IMU_ERR_NOT_READY   = 2, /**< STATUS_REG accel or gyro data
                                  not ready. */
    IMU_ERR_INVALID_ARG = 3, /**< NULL pointer passed as argument. */
    IMU_ERR_HW          = 4  /**< GPIO or EXTI setup failed (GPB-D9). */
} imu_err_t;

/**
 * @brief Raw 16-bit accelerometer and gyroscope reading from LSM6DSL.
 *
 * Values are raw signed 16-bit LSB counts.
 * Accelerometer sensitivity at ±2g: 16384 LSB/g.
 * Gyroscope sensitivity at ±250 dps: 8.75 mdps/LSB (≈ 114.3 LSB/dps).
 * SensorService applies the sensitivity factors when building
 * cloud telemetry.
 */
typedef struct {
    int16_t accel_x;   /**< X-axis acceleration, raw LSB. */
    int16_t accel_y;   /**< Y-axis acceleration, raw LSB. */
    int16_t accel_z;   /**< Z-axis acceleration, raw LSB. */
    int16_t gyro_x;    /**< X-axis angular rate, raw LSB. */
    int16_t gyro_y;    /**< Y-axis angular rate, raw LSB. */
    int16_t gyro_z;    /**< Z-axis angular rate, raw LSB. */
} imu_data_t;

/**
 * @brief INT1 callback type invoked from EXTI ISR context.
 *
 * The callback must be ISR-safe: no blocking calls, no FreeRTOS
 * API calls except `*FromISR()` variants.
 *
 * @param ctx  Opaque context pointer registered alongside the
 *             callback — typically a FreeRTOS TaskHandle_t cast
 *             to void*.
 */
typedef void (*imu_int1_cb_t)(void *ctx);
```

### 4.5 ImuDriver — IImu

```c
/**
 * @brief Initialise the LSM6DSL IMU (Phase 1 — pre-scheduler).
 *
 * Writes CTRL1_XL (±2g, 104 Hz), CTRL2_G (±250 dps, 104 Hz),
 * CTRL3_C (BDU, IF_INC), and INT1_CTRL (DRDY_G + DRDY_XL routed
 * to INT1). Configures the INT1 GPIO pin as a rising-edge EXTI
 * input but does NOT enable the EXTI interrupt (Phase 2 does that).
 *
 * Must be called once from the board init function before
 * `vTaskStartScheduler()`.
 *
 * @return IMU_ERR_OK on success;
 *         IMU_ERR_I2C if any I2C transaction fails during
 *         register configuration.
 * @note   Threading: task-context only, non-blocking.
 *         Must be called before the scheduler starts.
 */
imu_err_t imu_init(void);

/**
 * @brief Register INT1 callback and enable EXTI interrupt
 *        (Phase 2 — post-scheduler).
 *
 * Stores the callback pointer and context in the module state,
 * then enables the LSM6DSL INT1 EXTI interrupt line. Must be
 * called from SensorTask startup, after `vTaskStartScheduler()`.
 *
 * @param[in] cb   Callback function invoked in ISR context when
 *                 INT1 fires. Must not be NULL.
 * @param[in] ctx  Opaque context passed to cb — typically
 *                 `xTaskGetCurrentTaskHandle()` cast to void*.
 * @return IMU_ERR_OK on success;
 *         IMU_ERR_INVALID_ARG if cb is NULL.
 * @note   Threading: task-context only. Call once from SensorTask
 *         before the first read cycle.
 */
imu_err_t imu_attach_int1_callback(imu_int1_cb_t cb, void *ctx);

/**
 * @brief Read the current acceleration and gyroscope data from
 *        LSM6DSL.
 *
 * Checks STATUS_REG (0x1E) bits XLDA (bit 0) and GDA (bit 1).
 * If either is not set, returns IMU_ERR_NOT_READY (no retry —
 * INT1 ISR has already notified the caller). On success,
 * burst-reads twelve output bytes (OUTX_L_G through OUTZ_H_XL)
 * via auto-increment and assembles six signed int16_t axis values.
 *
 * @param[out] out  Pointer to struct receiving the raw reading.
 *                  Must not be NULL.
 * @return IMU_ERR_OK on success;
 *         IMU_ERR_INVALID_ARG if out is NULL;
 *         IMU_ERR_I2C on I2C bus error;
 *         IMU_ERR_NOT_READY if XLDA or GDA not set.
 * @note   Threading: task-context only, non-blocking. Not ISR-safe.
 */
imu_err_t imu_read(imu_data_t *out);

#ifdef TEST
/**
 * @brief Reset module-level state for unit tests.
 *
 * Clears s_imu to its post-bss-init value (ready = false,
 * callbacks = NULL) so each Unity test case starts from a clean
 * slate. Test-only; not compiled into firmware builds.
 */
void imu_reset_for_test(void);
#endif
```

### 4.6 ImuDriver — vtable interface

```c
/**
 * @brief Vtable interface for ImuDriver (singleton pattern,
 *        consistent with I2cDriver LLD-D10).
 *
 * SensorService depends on this interface, not on the concrete
 * driver. The implementation file defines the singleton instance.
 */
typedef struct {
    imu_err_t (*init)(void);
    imu_err_t (*attach_int1_callback)(imu_int1_cb_t cb, void *ctx);
    imu_err_t (*read)(imu_data_t *out);
} iimu_t;

/** Singleton pointer — defined in imu_driver.c. */
extern const iimu_t * const imu_driver;

#endif /* IMU_DRIVER_H */
```

**Vtable instance** (in `imu_driver.c`):

```c
static const iimu_t s_imu_vtable = {
    .init                 = imu_init,
    .attach_int1_callback = imu_attach_int1_callback,
    .read                 = imu_read,
};

const iimu_t * const imu_driver = &s_imu_vtable;
```

### 4.7 Output scale rationale

Neither LIS3MDL nor LSM6DSL data feeds a Modbus register (those
registers carry only temperature, humidity, and pressure). IMU and
magnetometer data is gateway-local, forwarded to cloud telemetry
by CloudPublisher. Raw LSB output with documented sensitivity is
the correct driver-layer contract; physical-unit conversion is an
application concern (P2 — layering).

---

## 5. Internal design

### 5.1 Module structure

Each driver is a singleton with a single private state struct,
a vtable pointer, and an ISR handler exposed via an internal
header.

```
magnetometer_driver.h       — public API + IMagnetometer vtable
magnetometer_driver.c       — singleton state, vtable instance,
                              two-phase init, ISR handler, read,
                              reset_for_test
magnetometer_driver_internal.h — extern declaration of
                              magnetometer_drdy_irq_handler()

imu_driver.h                — public API + IImu vtable
imu_driver.c                — singleton state, vtable instance,
                              two-phase init, ISR handler, read,
                              reset_for_test
imu_driver_internal.h       — extern declaration of
                              imu_int1_irq_handler()
```

### 5.2 Private state structs

Static, no dynamic allocation:

```c
/* magnetometer_driver.c */
typedef struct {
    magnetometer_drdy_cb_t drdy_cb;    /**< Registered DRDY callback. */
    void                  *drdy_ctx;   /**< Opaque context for callback. */
    bool                   ready;      /**< True after successful Phase 1
                                            init. */
} magnetometer_state_t;

static magnetometer_state_t s_mag;
```

```c
/* imu_driver.c */
typedef struct {
    imu_int1_cb_t int1_cb;    /**< Registered INT1 callback. */
    void         *int1_ctx;   /**< Opaque context for callback. */
    bool          ready;      /**< True after successful Phase 1
                                   init. */
} imu_state_t;

static imu_state_t s_imu;
```

### 5.3 Two-phase init — detailed flow

**Phase 1 (`magnetometer_init()` / `imu_init()`) — pre-scheduler:**

1. Write sensor configuration registers (ODR, full scale, output
   mode, interrupt routing) via `i2c_driver->write()`.
2. Configure DRDY/INT GPIO pin as input via GpioDriver (no pull —
   sensor drives the line actively).
3. Call `exti_configure(line, port, EXTI_EDGE_RISING)` to set up
   the EXTI trigger.
4. Do NOT call `exti_enable()` — no task handle exists yet.
5. Set `s_xxx.ready = true` on success.

**Phase 2 (`magnetometer_attach_drdy_callback()` /
`imu_attach_int1_callback()`) — post-scheduler:**

1. Validate `cb != NULL` — return `ERR_INVALID_ARG` if NULL.
2. Store callback pointer and context in module state.
3. Call `exti_enable(line, priority)` to unmask the EXTI interrupt
   in NVIC.
4. Return `ERR_OK`.

**Why two phases (not one)?** The callback stores a FreeRTOS task
handle as `ctx`. Task handles are only valid after
`xTaskCreate()`, which runs before `vTaskStartScheduler()`.
However, EXTI interrupts must not fire until the handler is
registered — hence the split. This is the same rationale as
TouchscreenDriver and ModbusUartDriver (GPB-D1).

### 5.4 ISR design

Each driver provides a non-static ISR handler function. These
are declared `extern` in internal headers so the EXTI vector glue
can call them:

**`magnetometer_driver_internal.h`:**

```c
#ifndef MAGNETOMETER_DRIVER_INTERNAL_H
#define MAGNETOMETER_DRIVER_INTERNAL_H

/**
 * @brief DRDY ISR handler — called from EXTI9_5_IRQHandler.
 *
 * Invokes the registered DRDY callback if non-NULL. Must only
 * be called from ISR context after EXTI pending bit is cleared.
 */
void magnetometer_drdy_irq_handler(void);

#endif /* MAGNETOMETER_DRIVER_INTERNAL_H */
```

**`imu_driver_internal.h`:**

```c
#ifndef IMU_DRIVER_INTERNAL_H
#define IMU_DRIVER_INTERNAL_H

/**
 * @brief INT1 ISR handler — called from EXTI15_10_IRQHandler.
 *
 * Invokes the registered INT1 callback if non-NULL. Must only
 * be called from ISR context after EXTI pending bit is cleared.
 */
void imu_int1_irq_handler(void);

#endif /* IMU_DRIVER_INTERNAL_H */
```

**ISR handler implementations** (in respective `.c` files, NOT
declared `static` — they have external linkage via the internal
headers):

```c
/* magnetometer_driver.c */
void magnetometer_drdy_irq_handler(void)
{
    if (s_mag.drdy_cb != NULL)
    {
        s_mag.drdy_cb(s_mag.drdy_ctx);
    }
}
```

```c
/* imu_driver.c */
void imu_int1_irq_handler(void)
{
    if (s_imu.int1_cb != NULL)
    {
        s_imu.int1_cb(s_imu.int1_ctx);
    }
}
```

**Integration with the EXTI vectors.** The vectors live in
`firmware/gateway/Src/stm32l4xx_it.c` (GPB-O8, closed): weak handlers that test
their pending bits, clear them through ExtiDriver and call the entry points
above; a bring-up harness may still override them with a strong definition.
The code is equivalent to:

```c
#include "magnetometer_driver_internal.h"
#include "imu_driver_internal.h"

void EXTI9_5_IRQHandler(void)
{
    if (EXTI->PR1 & (1U << 8U))       /* LIS3MDL DRDY — EXTI8 */
    {
        exti_clear_pending(8U);
        magnetometer_drdy_irq_handler();
    }
}

void EXTI15_10_IRQHandler(void)
{
    if (EXTI->PR1 & (1U << 11U))      /* LSM6DSL INT1 — EXTI11 */
    {
        exti_clear_pending(11U);
        imu_int1_irq_handler();
    }
}
```

**Callback registration example** (SensorTask startup):

```c
static void prv_mag_drdy_cb(void *ctx)
{
    TaskHandle_t task = (TaskHandle_t)ctx;
    BaseType_t yield = pdFALSE;
    xTaskNotifyFromISR(task, SENSOR_TASK_MAG_DRDY_BIT,
                       eSetBits, &yield);
    portYIELD_FROM_ISR(yield);
}

static void prv_imu_int1_cb(void *ctx)
{
    TaskHandle_t task = (TaskHandle_t)ctx;
    BaseType_t yield = pdFALSE;
    xTaskNotifyFromISR(task, SENSOR_TASK_IMU_INT1_BIT,
                       eSetBits, &yield);
    portYIELD_FROM_ISR(yield);
}
```

### 5.5 `_read()` function — STATUS_REG gate

`magnetometer_read()` and `imu_read()` check STATUS_REG before
reading output registers. This provides defence-in-depth: the
DRDY/INT1 ISR has already notified SensorTask, but STATUS_REG is
the authoritative "output registers are stable and complete"
signal (GPB-D6).

**LIS3MDL STATUS_REG (0x27):**
- Bit 3: ZYXDA — all axes data available. Read condition: bit 3
  = 1.

**LSM6DSL STATUS_REG (0x1E):**
- Bit 1: GDA — gyroscope data available.
- Bit 0: XLDA — accelerometer data available.
- Read condition: both bits = 1.

If STATUS_REG is not set, the driver returns `ERR_NOT_READY`.
No retry inside the driver — the caller (SensorTask) already
received the ISR notification and the ODR guarantees data
availability. A STATUS_REG miss at this point indicates a genuine
fault.

### 5.6 LIS3MDL output register read

Six bytes via I2C auto-increment burst starting at OUT_X_L
(0x28):

```
OUT_X_L (0x28), OUT_X_H (0x29),
OUT_Y_L (0x2A), OUT_Y_H (0x2B),
OUT_Z_L (0x2C), OUT_Z_H (0x2D)
```

Assembly (LSB-first, signed):

```c
uint8_t buf[6];
/* After i2c_driver->write_read(0x1E, {0x28 | 0x80}, 1, buf, 6): */
out->x = (int16_t)((uint16_t)buf[1] << 8 | (uint16_t)buf[0]);
out->y = (int16_t)((uint16_t)buf[3] << 8 | (uint16_t)buf[2]);
out->z = (int16_t)((uint16_t)buf[5] << 8 | (uint16_t)buf[4]);
```

Auto-increment on LIS3MDL I2C: **requires bit 7 of the register
address to be set** — the burst starts at `0x28 | 0x80 = 0xA8`
(ST BSP `lis3mdl.c`; GPB-D10). Without it the part returns the same
register repeatedly. v1.0 said no special bit was needed.

### 5.7 LSM6DSL output register read

Twelve bytes via I2C auto-increment burst starting at OUTX_L_G
(0x22):

```
OUTX_L_G  (0x22), OUTX_H_G  (0x23),   /* gyro X  */
OUTY_L_G  (0x24), OUTY_H_G  (0x25),   /* gyro Y  */
OUTZ_L_G  (0x26), OUTZ_H_G  (0x27),   /* gyro Z  */
OUTX_L_XL (0x28), OUTX_H_XL (0x29),   /* accel X */
OUTY_L_XL (0x2A), OUTY_H_XL (0x2B),   /* accel Y */
OUTZ_L_XL (0x2C), OUTZ_H_XL (0x2D)    /* accel Z */
```

Assembly (LSB-first, signed):

```c
uint8_t buf[12];
/* After i2c_driver->write_read(0x6A, {0x22}, 1, buf, 12): */
out->gyro_x  = (int16_t)((uint16_t)buf[1]  << 8 | (uint16_t)buf[0]);
out->gyro_y  = (int16_t)((uint16_t)buf[3]  << 8 | (uint16_t)buf[2]);
out->gyro_z  = (int16_t)((uint16_t)buf[5]  << 8 | (uint16_t)buf[4]);
out->accel_x = (int16_t)((uint16_t)buf[7]  << 8 | (uint16_t)buf[6]);
out->accel_y = (int16_t)((uint16_t)buf[9]  << 8 | (uint16_t)buf[8]);
out->accel_z = (int16_t)((uint16_t)buf[11] << 8 | (uint16_t)buf[10]);
```

Auto-increment on LSM6DSL I2C: enabled via CTRL3_C IF_INC bit
(set during init).

### 5.8 `magnetometer_reset_for_test()` implementation

```c
#ifdef TEST
void magnetometer_reset_for_test(void)
{
    memset(&s_mag, 0, sizeof(s_mag));
}
#endif
```

### 5.9 `imu_reset_for_test()` implementation

```c
#ifdef TEST
void imu_reset_for_test(void)
{
    memset(&s_imu, 0, sizeof(s_imu));
}
#endif
```

---

## 6. Synchronisation

Caller serialises. Neither driver holds FreeRTOS synchronisation
primitives. All entry points (`init`, `attach_*_callback`,
`read`) are intended to be called from a single task context
(`SensorTask`) or from `main()` before the scheduler starts.

Concurrent access from multiple tasks is not safe unless the
caller provides a mutex.

**ISR handlers:** `magnetometer_drdy_irq_handler()` and
`imu_int1_irq_handler()` access only the callback pointer and
context — both written once during Phase 2 and never modified
after. No data race exists between ISR and task contexts.

**I2C bus serialisation:** all four Group A + Group B sensor
drivers share I2C2 via I2cDriver. I2cDriver has no internal
mutex (established convention). SensorTask calls all sensor
reads sequentially within a single task period; no concurrent
I2C access occurs.

---

## 7. Init ordering

### 7.1 Pre-conditions

| Dependency | Pre-condition |
|------------|---------------|
| I2cDriver | `i2c_init()` must have completed successfully |
| GpioDriver | `gpio_init()` must have completed (for EXTI pin config) |
| Clock | I2C2 peripheral clock enabled (handled by I2cDriver) |
| EXTI | EXTI peripheral available (always-on on STM32L4) |

### 7.2 Phase 1: `magnetometer_init()` sequence

```
magnetometer_init()
  → i2c_driver->write(0x1E, {0x20, 0x70}, 2)  /* CTRL_REG1: OM=11, DO=100 (10 Hz) */
  → i2c_driver->write(0x1E, {0x21, 0x00}, 2)  /* CTRL_REG2: ±4 gauss */
  → i2c_driver->write(0x1E, {0x23, 0x0C}, 2)  /* CTRL_REG4: OMZ=11 */
  → i2c_driver->write(0x1E, {0x24, 0x40}, 2)  /* CTRL_REG5: BDU */
  → i2c_driver->write(0x1E, {0x22, 0x00}, 2)  /* CTRL_REG3: continuous — LAST */
  → gpio_configure_pin(PC8, input, no pull)
  → exti_configure(8, EXTI_PORT_C, EXTI_EDGE_RISING)   [CONFLICT accepted]
  → s_mag.ready = true
  → return MAGNETOMETER_ERR_OK
```

### 7.3 Phase 1: `imu_init()` sequence

```
imu_init()
  → i2c_driver->write(0x6A, {0x12, 0x44}, 2)  /* CTRL3_C: BDU, IF_INC */
  → i2c_driver->write(0x6A, {0x0D, 0x03}, 2)  /* INT1_CTRL: DRDY_G + DRDY_XL */
  → i2c_driver->write(0x6A, {0x10, 0x40}, 2)  /* CTRL1_XL: 104 Hz, ±2 g */
  → i2c_driver->write(0x6A, {0x11, 0x40}, 2)  /* CTRL2_G: 104 Hz, ±250 dps — LAST */
  → gpio_configure_pin(PD11, input, no pull)
  → exti_configure(11, EXTI_PORT_D, EXTI_EDGE_RISING)  [CONFLICT accepted]
  → s_imu.ready = true
  → return IMU_ERR_OK
```

### 7.4 Phase 2: callback attachment (post-scheduler)

```
[Post-scheduler — SensorTask startup, before first 100 ms tick]

magnetometer_attach_drdy_callback(prv_mag_drdy_cb,
                                   xTaskGetCurrentTaskHandle())
  → validate cb != NULL
  → if !s_mag.ready: return MAGNETOMETER_ERR_NOT_READY
  → s_mag.drdy_cb  = cb
  → s_mag.drdy_ctx = ctx
  → exti_enable(8, 6)               [priority 6 >= syscall ceiling 5]
  → gpio_read_pin(PC8): if high, read the 6 output bytes once (GPB-D11)
  → return MAGNETOMETER_ERR_OK

imu_attach_int1_callback(prv_imu_int1_cb,
                          xTaskGetCurrentTaskHandle())
  → validate cb != NULL
  → if !s_imu.ready: return IMU_ERR_NOT_READY
  → s_imu.int1_cb  = cb
  → s_imu.int1_ctx = ctx
  → exti_enable(11, 6)
  → gpio_read_pin(PD11): if high, read the 12 output bytes once (GPB-D11)
  → return IMU_ERR_OK
```

NVIC priority is **numerically ≥ 5** (`configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY`)
because the callbacks invoke FreeRTOS `*FromISR()` APIs; the drivers use 6, as the
other Gateway ISRs that notify tasks do.

---

## 8. Sequence integration

Both drivers are called from SensorTask in the SD-01 nominal
flow, after the Group A sensors (which use STATUS_REG polling
only).

### 8.1 Nominal read flow (per SensorTask 100 ms cycle)

```
SensorTask (100 ms tick fires)
  → read Group A sensors (STATUS_REG poll — no wait)
  → xTaskNotifyWait(MAG_DRDY_BIT | IMU_INT1_BIT, timeout=5 ms)
      [If bits already set from ISR: proceed immediately]
      [If timeout: log warning, skip Group B reads this cycle]
  → magnetometer_driver->read(&mag_data)
      → i2c_driver->write_read(0x1E, {0x27}, 1, &status, 1)
        — confirm ZYXDA=1
      → i2c_driver->write_read(0x1E, {0x28 | 0x80}, 1, buf, 6)
        — auto-increment burst
      → assemble int16_t x, y, z
  → imu_driver->read(&imu_data)
      → i2c_driver->write_read(0x6A, {0x1E}, 1, &status, 1)
        — confirm XLDA=1, GDA=1
      → i2c_driver->write_read(0x6A, {0x22}, 1, buf, 12)
        — auto-increment burst
      → assemble int16_t gyro x/y/z, accel x/y/z
```

### 8.2 SD trace

| SD | Component role | Key function |
|----|----------------|--------------|
| SD-01 | MagnetometerDriver and ImuDriver are called by SensorService during the periodic sensor read cycle. DRDY/INT1 ISR notification triggers the read; each driver checks STATUS_REG as defence-in-depth, burst-reads output registers, and returns raw data. | `magnetometer_read()`, `imu_read()` |

### 8.3 Impact on task-breakdown.md

The two DRDY/INT1 notification bits (MAG_DRDY_BIT, IMU_INT1_BIT)
must be added to:
- SensorTask IPC primitives table (§5.4)
- ISR inventory (§6.1): two new rows for EXTI8 and EXTI11

See GPB-O1.

---

## 9. Error and fault behaviour

All public functions return `magnetometer_err_t` or `imu_err_t`;
callers must not ignore non-OK returns. No retry is performed
inside the driver — the caller (SensorService) applies the retry
and observability policy.

### 9.1 MagnetometerDriver error codes

| Error value | Cause | Local behaviour | Retry | Observability |
|-------------|-------|-----------------|-------|---------------|
| `MAGNETOMETER_ERR_OK` | Success | Return raw data | N/A | — |
| `MAGNETOMETER_ERR_I2C` | I2C bus error (NACK, timeout, bus busy) from `i2c_driver->write()` or `i2c_driver->write_read()` | Return error; `out` unchanged | No — SensorService skips the sample; three consecutive failures raise `HEALTH_EVENT_SENSOR_FAIL` | SensorService logs at WARN via ILogger; `HEALTH_EVENT_SENSOR_FAIL` pushed to IHealthReport after threshold |
| `MAGNETOMETER_ERR_NOT_READY` | STATUS_REG ZYXDA bit not set | Return error; sensor state unchanged | No — SensorService skips the sample; three consecutive misses raise `HEALTH_EVENT_SENSOR_FAIL` | SensorService logs at WARN via ILogger; `HEALTH_EVENT_SENSOR_FAIL` pushed to IHealthReport after threshold |
| `MAGNETOMETER_ERR_INVALID_ARG` | NULL pointer passed to output parameter or NULL callback in attach | Return error; no I2C access | No — programming error | Caller logs at ERROR |

### 9.2 ImuDriver error codes

| Error value | Cause | Local behaviour | Retry | Observability |
|-------------|-------|-----------------|-------|---------------|
| `IMU_ERR_OK` | Success | Return raw data | N/A | — |
| `IMU_ERR_I2C` | I2C bus error from `i2c_driver->write()` or `i2c_driver->write_read()` | Return error; `out` unchanged | No — SensorService skips the sample; three consecutive failures raise `HEALTH_EVENT_SENSOR_FAIL` | SensorService logs at WARN via ILogger; `HEALTH_EVENT_SENSOR_FAIL` pushed to IHealthReport after threshold |
| `IMU_ERR_NOT_READY` | STATUS_REG XLDA or GDA bit not set | Return error; sensor state unchanged | No — SensorService skips; three consecutive misses raise `HEALTH_EVENT_SENSOR_FAIL` | SensorService logs at WARN via ILogger; `HEALTH_EVENT_SENSOR_FAIL` pushed to IHealthReport after threshold |
| `IMU_ERR_INVALID_ARG` | NULL pointer passed to output parameter or NULL callback in attach | Return error; no I2C access | No — programming error | Caller logs at ERROR |

### 9.3 Init error handling

If any I2C transaction fails during `magnetometer_init()` or
`imu_init()`, the function returns `_ERR_I2C` and the `ready`
flag remains `false`. `read()` and `attach_*_callback()` called
before a successful `init()` return `_ERR_NOT_READY` without touching
the bus or the EXTI (GPB-D12).

SensorService (caller) handles all non-OK returns per REQ-SA-080
(log error code) and REQ-SA-060 (continue with available
sensors). The drivers do not log — they return error codes and
let the application layer decide.

---

## 10. Hardware contract

### 10.1 LIS3MDL

| Parameter | Value | Source |
|-----------|-------|--------|
| Bus | I2C2 | UM2153 §7.12.3 |
| 7-bit address | 0x1E | UM2153 Table 3 |
| Write address (8-bit) | 0x3C | UM2153 Table 3 |
| Read address (8-bit) | 0x3D | UM2153 Table 3 |
| DRDY GPIO | **PC8** (EXTI8, `EXTI_PORT_C`) — GPB-O3 | ST board pin map; confirmed on hardware by TC-HW-SB-003 |
| DRDY polarity | Active high (rising edge triggers EXTI) | LIS3MDL datasheet |
| Full scale | ±4 gauss (default) | LIS3MDL datasheet CTRL_REG2 |
| Sensitivity | 6842 LSB/gauss | LIS3MDL datasheet Table 3 |
| Configured ODR | 10 Hz | CTRL_REG1: OM=11 (ultra-high perf), DO=100 → 0x70 |
| Supply voltage | 3.3 V | Board power rail |

**Registers written at init (Phase 1):**

| Register | Address | Value | Meaning |
|----------|---------|-------|---------|
| CTRL_REG1 | 0x20 | 0x70 | TEMP_EN=0, OM=11 (ultra-high perf XY), DO=100 (10 Hz), FAST_ODR=0 |
| CTRL_REG2 | 0x21 | 0x00 | FS=00 (±4 gauss), REBOOT=0, SOFT_RST=0 |
| CTRL_REG3 | 0x22 | 0x00 | Continuous conversion; written last because the power-on default is power-down (0x03) |
| CTRL_REG4 | 0x23 | 0x0C | OMZ=11 (ultra-high perf Z), BLE=0 (LSB first) |
| CTRL_REG5 | 0x24 | 0x40 | BDU=1 (block data update) |
| INT_CFG   | 0x30 | — | **Not written.** The power-on default leaves threshold interrupts off (IEN=0); the datasheet marks a reserved bit of this register as 'must be 1', so writing 0x00 was a risk with no benefit (GPB-D13). |

DRDY pin behaviour: the LIS3MDL asserts DRDY high when new data
is available in output registers and **holds it high until the
output registers are read** (GPB-D11). The EXTI is configured for
rising-edge trigger. The DRDY signal is the hardware signal; the
`INT_CFG` register controls a separate configurable interrupt
feature (threshold crossing), which is disabled.

### 10.2 LSM6DSL

| Parameter | Value | Source |
|-----------|-------|--------|
| Bus | I2C2 | UM2153 §7.12.4 |
| 7-bit address | 0x6A | UM2153 Table 3 |
| Write address (8-bit) | 0xD4 | UM2153 Table 3 |
| Read address (8-bit) | 0xD5 | UM2153 Table 3 |
| INT1 GPIO | **PD11** (EXTI11, `EXTI_PORT_D`) — GPB-O3 | ST board pin map; confirmed on hardware by TC-HW-SB-003 |
| INT1 polarity | Active high (rising edge triggers EXTI) | LSM6DSL datasheet |
| Accel full scale | ±2g (default) | LSM6DSL CTRL1_XL |
| Accel sensitivity | 16384 LSB/g | LSM6DSL datasheet Table 3 |
| Gyro full scale | ±250 dps (default) | LSM6DSL CTRL2_G |
| Gyro sensitivity | 8.75 mdps/LSB (≈ 114.3 LSB/dps) | LSM6DSL datasheet Table 3 |
| Configured accel ODR | 104 Hz | CTRL1_XL ODR_XL = 0100 |
| Configured gyro ODR | 104 Hz | CTRL2_G ODR_G = 0100 |
| Supply voltage | 3.3 V | Board power rail |

104 Hz ODR chosen so that at the 10 Hz effective SensorTask read
rate, data is always ready before `imu_read()` is called. The 10×
margin eliminates practical `ERR_NOT_READY` conditions (GPB-D3).

**Registers written at init (Phase 1):**

| Register | Address | Value | Meaning |
|----------|---------|-------|---------|
| CTRL1_XL | 0x10 | 0x40 | ODR_XL=0100 (104 Hz), FS_XL=00 (±2g), LPF1_BW_SEL=0 |
| CTRL2_G  | 0x11 | 0x40 | ODR_G=0100 (104 Hz), FS_G=00 (±250 dps) |
| CTRL3_C  | 0x12 | 0x44 | BDU=1, IF_INC=1 (auto-increment on I2C), H_LACTIVE=0 |
| INT1_CTRL| 0x0D | 0x03 | INT1_DRDY_G=1, INT1_DRDY_XL=1 (both routed to INT1) |

### 10.3 Registers / Pins / Clocks / NVIC

**Registers:** N/A — LIS3MDL and LSM6DSL are external I2C sensor
ICs. Their internal device registers are accessed via I2C
transactions delegated to I2cDriver. Neither driver directly
touches any MCU peripheral register.

**Pins:** I2C SDA/SCL pins are owned and configured by I2cDriver.
DRDY/INT1 GPIO pins are configured by GpioDriver and armed by
ExtiDriver; their specific pin assignments are documented in
§10.1 and §10.2 above. Port letters: PC8 and PD11 (GPB-O3).

**Clocks:** N/A — I2C peripheral clock is enabled by I2cDriver.
No additional clock enable is required by this companion.

**NVIC:** LIS3MDL DRDY (EXTI8) and LSM6DSL INT1 (EXTI11) are
armed by ExtiDriver. NVIC priority is assigned via
`exti_enable()` in Phase 2. Priority must be ≥
`configMAX_SYSCALL_INTERRUPT_PRIORITY` because the ISR callbacks
invoke FreeRTOS `*FromISR()` functions (per `task-breakdown.md`
§6.3).

---

## 11. Memory and sizing

### RAM budget

| Item | Size | Notes |
|------|------|-------|
| `s_mag` (magnetometer_state_t) | 12 B | cb pointer (4 B) + ctx pointer (4 B) + ready (1 B) + padding (3 B) |
| `s_imu` (imu_state_t) | 12 B | cb pointer (4 B) + ctx pointer (4 B) + ready (1 B) + padding (3 B) |
| Stack-local buffers in `magnetometer_read()` | 7 B | 6 B output burst + 1 B STATUS_REG |
| Stack-local buffers in `imu_read()` | 13 B | 12 B output burst + 1 B STATUS_REG |
| **Total static RAM** | **~24 B** | |

### ROM budget

| Item | Size | Notes |
|------|------|-------|
| `magnetometer_init()` + GPIO/EXTI config | ~180 B | Six register writes + GPIO + EXTI config |
| `magnetometer_attach_drdy_callback()` | ~40 B | Validate + store + EXTI enable |
| `magnetometer_read()` | ~120 B | STATUS_REG check + 6-byte burst + int16 assembly |
| `magnetometer_drdy_irq_handler()` | ~20 B | Null check + callback invoke |
| `imu_init()` + GPIO/EXTI config | ~140 B | Four register writes + GPIO + EXTI config |
| `imu_attach_int1_callback()` | ~40 B | Validate + store + EXTI enable |
| `imu_read()` | ~150 B | STATUS_REG check + 12-byte burst + int16 assembly |
| `imu_int1_irq_handler()` | ~20 B | Null check + callback invoke |
| Vtable instances (2) | ~24 B | Two three-member function-pointer structs |
| **Total estimated ROM** | **~740 B** | |

---

## 12. File layout

```
firmware/gateway/drivers/magnetometer_driver/
├── magnetometer_driver.h            /* IMagnetometer — public API +
                                        vtable */
├── magnetometer_driver.c            /* Implementation (singleton) */
└── magnetometer_driver_internal.h   /* ISR handler extern
                                        declaration */

firmware/gateway/drivers/imu_driver/
├── imu_driver.h                     /* IImu — public API + vtable */
├── imu_driver.c                     /* Implementation (singleton) */
└── imu_driver_internal.h            /* ISR handler extern
                                        declaration */

tests/gateway/drivers/
├── magnetometer_driver/test_magnetometer_driver.c   /* Unity, fakes */
└── imu_driver/test_imu_driver.c                     /* Unity, fakes */

tests/support/exti_driver_stub.h     /* EXTI types for fakes (new) */

firmware/gateway/integration-tests/sensor_drivers_group_b/
└── main_test_sensor_drivers_group_b.c   /* TC-HW-SB-001..009 */
```

Internal headers (`*_internal.h`) contain only the ISR handler
`extern` declaration. They are included by the EXTI vector glue and
by the test harness (for direct ISR handler testing). They are
not part of the public API.

---

## 13. Unit-test plan

**Framework:** Unity + CMock (project standard). Tests run on the
host machine (PC). I2C dependencies are mocked by CMock replacing
`i2c_driver.c` at link time. The mock provides
`i2c_driver->write()` and `i2c_driver->write_read()` via the
`II2c` vtable. GpioDriver and ExtiDriver calls are stubbed
(no-op or mock as needed).

**Mocked dependencies:** I2cDriver (`i2c_write`,
`i2c_write_read`), GpioDriver (`gpio_configure_input`),
ExtiDriver (`exti_configure`, `exti_enable`, `exti_clear_pending`).

**Test file:** `tests/gateway/drivers/magnetometer_driver/test_magnetometer_driver.c`

| TC ID | Scenario | Mechanism | Expected result |
|-------|----------|-----------|-----------------|
| TC-MG-001 | Nominal read — all axes correct | Mock I2C: STATUS_REG=0x08 (ZYXDA=1), known 6-byte OUT values | `magnetometer_read()` returns `OK`; x/y/z match expected signed int16_t assembly |
| TC-MG-002 | STATUS_REG ZYXDA=0 | Mock returns 0x00 on STATUS_REG read | Returns `MAGNETOMETER_ERR_NOT_READY` |
| TC-MG-003 | I2C failure on STATUS_REG read | Mock `i2c_driver->write_read()` returns `I2C_ERR_NACK` | Returns `MAGNETOMETER_ERR_I2C` |
| TC-MG-004 | I2C failure on data burst | STATUS_REG OK, 6-byte burst returns I2C error | Returns `MAGNETOMETER_ERR_I2C` |
| TC-MG-005 | NULL `out` pointer | Call `magnetometer_read(NULL)` | Returns `MAGNETOMETER_ERR_INVALID_ARG` |
| TC-MG-006 | Negative axis value — sign extension | Inject OUT bytes representing −1 LSB (0xFF, 0xFF) | Correct sign extension: x = −1 as int16_t |
| TC-MG-007 | Attach callback with NULL cb | Call `magnetometer_attach_drdy_callback(NULL, ctx)` | Returns `MAGNETOMETER_ERR_INVALID_ARG` |
| TC-MG-008 | ISR handler invokes callback | Call `magnetometer_drdy_irq_handler()` directly with registered mock callback | Mock callback invoked with correct ctx |
| TC-MG-009 | ISR handler with no callback registered | Call `magnetometer_drdy_irq_handler()` before attach | No crash; callback not invoked (NULL guard) |
| TC-MG-010 | `magnetometer_reset_for_test()` clears state | Call init + attach, then reset, then read | Read fails (ready = false); callback pointers cleared |

**Test file:** `tests/gateway/drivers/imu_driver/test_imu_driver.c`

| TC ID | Scenario | Mechanism | Expected result |
|-------|----------|-----------|-----------------|
| TC-IM-001 | Nominal read — all six axes correct | Mock I2C: STATUS_REG=0x03 (XLDA+GDA=1), known 12-byte OUT values | `imu_read()` returns `OK`; all six axes match expected signed int16_t assembly |
| TC-IM-002 | STATUS_REG GDA=0 | Mock STATUS_REG bit 1 = 0 | Returns `IMU_ERR_NOT_READY` |
| TC-IM-003 | STATUS_REG XLDA=0 | Mock STATUS_REG bit 0 = 0 | Returns `IMU_ERR_NOT_READY` |
| TC-IM-004 | I2C failure on 12-byte burst | STATUS_REG OK, burst returns I2C error | Returns `IMU_ERR_I2C` |
| TC-IM-005 | NULL `out` pointer | Call `imu_read(NULL)` | Returns `IMU_ERR_INVALID_ARG` |
| TC-IM-006 | Maximum positive axis value (0x7FFF) | Inject 0x7FFF in all six axis positions | Correct int16_t = 32767 in all fields |
| TC-IM-007 | Maximum negative axis value (0x8000) | Inject 0x8000 in all six axis positions | Correct int16_t = −32768 in all fields |
| TC-IM-008 | Attach callback with NULL cb | Call `imu_attach_int1_callback(NULL, ctx)` | Returns `IMU_ERR_INVALID_ARG` |
| TC-IM-009 | ISR handler invokes callback | Call `imu_int1_irq_handler()` directly with registered mock callback | Mock callback invoked with correct ctx |
| TC-IM-010 | ISR handler with no callback registered | Call `imu_int1_irq_handler()` before attach | No crash; callback not invoked (NULL guard) |
| TC-IM-011 | `imu_reset_for_test()` clears state | Call init + attach, then reset, then read | Read fails (ready = false); callback pointers cleared |

### 13.1 Implementation addendum (v1.1)

**Mechanism change.** The plan named CMock for I2C, GPIO and EXTI. Both suites use fakes
defined in the test TU instead: `i2c_driver` is a `const` pointer CMock cannot replace, and
`tests/support/gpio_driver_stub.h` plus the new `exti_driver_stub.h` keep the real driver
`.c` files out of the link. The magnetometer's I2C fake models the LIS3MDL, so a multi-byte
read without address bit 7 returns the same register repeatedly — the companion's v1.0
sequence would fail TC-MG-001. Run under `tests/project_gateway.yml` with the Group A and
I2cDriver tests.

**Added at implementation.** TC-MG-011..024 and TC-IM-012..024: init register sequence and
ordering; PC8 / PD11 pin and EXTI configuration (Phase 1 must not unmask); write failure at
every step; GPIO and EXTI failures; re-init with `EXTI_ERR_CONFLICT` and callback kept;
attach before init; EXTI enable at priority ≥ 5; stale-line release (high, low, and failure
paths); address bit 7 on the LIS3MDL burst; vtable dispatch.

**Mutation check.** Reintroducing `CTRL_REG1 = 0x7C` fails TC-MG-011; dropping address bit 7
fails TC-MG-001 / 006 / 022 / 024; inverting the stale-line test fails TC-MG-021 / 022 / 023.

**Results (2026-10-14):** MagnetometerDriver 24 / 24, ImuDriver 24 / 24, 0 ignored. Hardware
cases TC-HW-SB-001..009 are in `firmware/gateway/integration-tests/sensor_drivers_group_b/`
and have **not been run**; TC-HW-SB-004 measures the real interrupt rates.

---

## 14. Open items

| ID | Item | Owner | Resolution path | Status |
|----|------|-------|-----------------|--------|
| GPB-O1 | `task-breakdown.md` §5.4 IPC primitives table does not list DRDY → SensorTask notification paths. Two entries required: `LIS3MDL DRDY ISR → SensorTask` (notify bit MAG_DRDY_BIT) and `LSM6DSL INT1 ISR → SensorTask` (notify bit IMU_INT1_BIT). ISR inventory §6.1 also needs two new rows. | Luca | Entries were already present in `task-breakdown.md` §5.4 and §6.1 — twice. The duplicate rows are removed in this change. | Closed 2026-10-14 |
| GPB-O2 | 5 ms `xTaskNotifyWait()` timeout for Group B read is an assumed value. Too short risks false `ERR_NOT_READY`; too long delays SensorTask. | Luca | Validate at integration using logic analyser or GPIO toggle on DRDY pin. At LIS3MDL 10 Hz (period 100 ms), the DRDY bit is virtually always set when SensorTask wakes. 5 ms is safe. | Open |
| GPB-O3 | DRDY/INT1 GPIO pin assignments: LIS3MDL → EXTI8 (MCU port unknown), LSM6DSL → EXTI11 (MCU port unknown). Labels from UM2153 Fig. 27 / Fig. 23; exact port letters not confirmed. | Luca | Implemented as PC8 (EXTI8) and PD11 (EXTI11) from the ST board pin map, which is not in the repository. TC-HW-SB-003 confirms both on hardware; correct the two constants (`MAG_DRDY_*`, `IMU_INT1_*`) if it fails. | Resolved — pending TC-HW-SB-003 |
| GPB-O4 | I2C2 TIMINGR value (cross-cutting open item from I2cDriver companion, I2CD-O1 root). | Luca | I2cDriver is implemented (I2CD-O5 tracks the unmeasured value). | Open (see I2CD-O1 / I2CD-O5) |
| GPB-O5 | LIS3MDL CTRL_REG1 value 0x7C uses OM=11 (ultra-high performance), which increases power consumption (570 µA typ.). Acceptable for this project; would be revisited in a power-optimised build. | Luca | Note only — no action required for this project. | Open |
| GPB-O6 | The LIS3MDL 10 Hz DRDY and LSM6DSL 104 Hz INT1 are latched high until the output registers are read. The 5 ms `xTaskNotifyWait()` of GPB-O2 therefore also depends on SensorTask reading every notified sample; if it skips a cycle the line stays high and no new edge arrives. | Luca | SensorService must read both sensors after every wake-up, even when it discards the sample. Verify in the SensorTask integration. | Open |
| GPB-O7 | Hardware validation of both drivers (TC-HW-SB-001..009), including the real interrupt rates and the corrected register values. | Luca | Run `integration-tests/i2c` first, then `sensor_drivers_group_b`, board still and flat. | Open |
| GPB-O8 | The Gateway had no `stm32l4xx_it.c`; only bring-up harnesses defined EXTI vectors. | Luca | `firmware/gateway/Src/stm32l4xx_it.c` provides weak `EXTI1`, `EXTI9_5` and `EXTI15_10` handlers (WifiDriver, MagnetometerDriver, ImuDriver); 6 host tests; link order verified on the built ELF. | Closed 2026-10-14 |

---

## 15. Decisions log

| ID | Decision | Rationale |
|----|----------|-----------|
| GPB-D1 | DRDY/INT1 ISR used — two-phase init | At configurable ODR these sensors can produce data faster than the SensorTask period. The ISR-driven notification allows SensorTask to read immediately when data is ready rather than polling STATUS_REG at a fixed tick. Consistent with the touchscreen and ModbusUartDriver two-phase pattern. |
| GPB-D2 | LIS3MDL ODR = 10 Hz | At least 10× the 1 Hz default SensorTask effective read rate. DRDY fires 10 times per SensorTask read cycle; the notification bit remains set from the last DRDY event when SensorTask wakes. |
| GPB-D3 | LSM6DSL ODR = 104 Hz for both axes | Highest standard ODR below the 208 Hz high-performance tier. Ensures data is always available within 10 ms of a read request; eliminates `ERR_NOT_READY` in practice. |
| GPB-D4 | Raw LSB output (no in-driver physical-unit conversion) | No Modbus register consumes IMU or magnetometer data. Physical-unit conversion in the application layer (P2). Sensitivity constants documented in headers. |
| GPB-D5 | BDU = 1 on both sensors | Block data update prevents reading mixed old/new bytes during a conversion. Mandatory for correct int16_t assembly from two-register pairs. |
| GPB-D6 | STATUS_REG checked inside `_read()` despite DRDY ISR notification | Defence in depth: DRDY signals that a conversion completed, but STATUS_REG is the authoritative "registers are stable" signal. Also guards against stale DRDY notification bits left from a previous cycle. |
| GPB-D7 | ISR handlers exposed via `*_internal.h` headers | Resolves the contradiction of needing external linkage (called from `stm32l4xx_it.c`) while not being part of the public API. Internal headers are a clean middle ground — `#include` only by `stm32l4xx_it.c` and the test harness. |
| GPB-D8 | Vtable singleton pattern for both drivers | Consistent with I2cDriver (LLD-D10). SensorService depends on `imagnetometer_t` / `iimu_t` interfaces, enabling test mocking via vtable replacement. P2 conformant. |
| GPB-D9 | Real GpioDriver / ExtiDriver APIs are used, and a fifth error code `*_ERR_HW` is added | v1.0 named `gpio_configure_input()` and passed a `gpio_port_t` to `exti_configure()`; neither matches the drivers. A GPIO / EXTI failure is not an I2C failure, so it gets its own code instead of misleading the caller's fault handling. |
| GPB-D10 | The LIS3MDL burst read sets bit 7 of the register address (`0xA8`) | The LIS3MDL auto-increments only when bit 7 is set (ST BSP `lis3mdl.c`). v1.0 claimed it was on by default; the burst would have returned `OUT_X_L` six times. |
| GPB-D11 | `attach_*_callback()` arms the EXTI, then reads the pin and, if high, reads the output registers once | Both sensors hold DRDY / INT1 high until the data is read. A conversion that finished before the interrupt was unmasked produces no edge, so the first notification would never come and nothing would ever clear the line. Arming first means no edge can be lost. |
| GPB-D12 | `read()` and `attach_*()` before a successful init return `_ERR_NOT_READY` | An assertion would abort the host test run; the `ready` flag already exists. Same decision as GPA-D9. |
| GPB-D13 | LIS3MDL INT_CFG is not written; CTRL_REG3 (leave power-down) is written last; LSM6DSL ODR registers are written last | The default INT_CFG already disables threshold interrupts and one of its bits is reserved. Writing the registers that start conversions last means the first sample is taken with the final configuration. |
| GPB-D14 | CTRL_REG1 = 0x70, not 0x7C | `0x7C` encodes DO=111, 80 Hz, eight times the stated 10 Hz (ST BSP `lis3mdl.h`: `ODR_10_HZ = 0x10`, `OM_XY_ULTRAHIGH = 0x60`). |
| GPB-D15 | Directories are `magnetometer_driver/` and `imu_driver/`; tests run under `project_gateway.yml` | Matches the Group A layout and the `test-module.sh` convention (directory name = test name), and keeps the Gateway `i2c_driver.h` from being shadowed by the Field Device header in the default project. |

---

## 16. Principles applied

- **P1 (Strict directional layering).** Both drivers depend only
  on `II2c`, GpioDriver, and ExtiDriver (driver layer). No upward
  dependency on middleware or application. Calls flow downward via
  `i2c_driver->write()` and `i2c_driver->write_read()`.
- **P2 (Dependency Inversion).** Each sensor exposes its own
  vtable: `imagnetometer_t` and `iimu_t`. SensorService
  (application layer) depends on the interfaces, not on the
  concrete driver implementations. This enables test mocking via
  vtable replacement.
- **P3 (Interface Segregation).** Two separate interfaces
  (`IMagnetometer`, `IImu`) rather than one unified sensor
  interface. Each consumer (SensorService) calls only the
  interface it needs — each sensor is read independently.
- **P5 (Bounded resources, no dynamic allocation post-init).**
  Static state per driver; no dynamic allocation at any point.
  Callback pointers are stored in static structs and written once
  during Phase 2.
- **P6 (Responsibility traces to requirements).** Measurement
  functions trace to REQ-SA-031 (init), REQ-SA-071 (read). Error
  handling traces to REQ-SA-040, REQ-SA-060, REQ-SA-080.
- **P10 (Naming conventions).** Prefixes `magnetometer_` / `imu_`.
  Errors `MAGNETOMETER_ERR_*` / `IMU_ERR_*`. Static state
  `s_mag`, `s_imu`. Vtable pointers `magnetometer_driver`,
  `imu_driver`. Internal headers `*_internal.h`.

**P4 (Cross-cutting concern exception) does not apply.** These
drivers do not reference Logger directly; error codes are returned
to the caller (SensorService) which handles logging.

**P7 (Pull-based data access) does not apply.** These are leaf
drivers called synchronously — no data production/consumption
pattern. The DRDY ISR is a notification mechanism, not a data
push — data is still pulled via `_read()`.

**P8, P9 do not apply.** P8 (Infrastructure traces to pain) — no
helper infrastructure. P9 (Multi-view diagrams) — not a diagram
document.

**Coding standard compliance (BARR-C:2018).** Fixed-width types
throughout: sensor addresses `uint8_t`, register values `uint8_t`,
raw output `int16_t`. No floating-point in the driver layer. All
functions return typed `_err_t` — no silent failures. Static
allocation only. `const` correctness on vtable pointers. Braces
on all control structures. Module prefix on all public symbols.

---

## 17. References

- `docs/components.md` (GW driver layer, MagnetometerDriver and
  ImuDriver entries).
- `docs/SRS.md` (REQ-SA-031, REQ-SA-071, REQ-SA-040, REQ-SA-060,
  REQ-SA-080).
- `docs/sequence-diagrams.md` SD-01 (sensor read nominal flow).
- `docs/task-breakdown.md` §5.2 (SensorTask — priority 5,
  512 words, 100 ms period), §6.1 (ISR inventory — pending
  GPB-O1).
- `docs/lld/companions/i2c-driver.md` (I2cDriver companion —
  defines `II2c`, `i2c_err_t`, vtable pattern LLD-D10).
- `docs/architecture-principles.md` (P1, P2, P3, P5, P6, P10).
- LIS3MDL datasheet (ST Microelectronics).
- LSM6DSL datasheet (ST Microelectronics).
- UM2153 Discovery Kit for IoT Node User Manual.

---

## Phase H checklist

| # | Check | Status |
|---|-------|--------|
| H-01 | Every public function has a Doxygen block with `@brief`, `@param`, `@return`, `@note Threading` | Done (§4.2, §4.5) |
| H-02 | Every error code documented in an error table with cause, behaviour, retry policy, and observability | Done (§9) |
| H-03 | Init ordering documented with pre-conditions | Done (§7) |
| H-04 | Synchronisation strategy documented | Done (§6) |
| H-05 | `_reset_for_test()` hook present, guarded by `#ifdef TEST` | Done (§4.2, §4.5, §5.8, §5.9) |
| H-06 | File layout matches project directory structure | Done (§12) |
| H-07 | Unit-test plan uses CMock with TC-NNN identifiers | Done (§13) |
| H-08 | Open items have structured IDs and resolution paths | Done (§14) |
| H-09 | Principles section contains no false claims | Done (§16) — P4/P7/P8/P9 explicitly noted as not applicable; I2C (not SPI) correctly referenced throughout; coding standard separated from P-numbered principles |
| H-10 | SD trace table uses correct function names matching §4 API | Done (§8) |
| H-11 | State machine mapping to code documented | N/A — no state machine; sensor drivers are stateless read-only (ready flag is a guard, not a machine) |
| H-12 | Sizing estimates provided (RAM + ROM) | Done (§11) |
| H-13 | All SRS traces from header verified against section content | Done — traces checked against §3, §9 |
| H-14 | Decisions logged with rationale | Done (§15) |
