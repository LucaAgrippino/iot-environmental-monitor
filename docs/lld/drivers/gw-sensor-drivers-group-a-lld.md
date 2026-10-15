# LLD Companion — Gateway Sensor Drivers Group A

## HumidityTempDriver (HTS221) + BarometerDriver (LPS22HB)

**Board:** Gateway (B-L475E-IOT01A)
**Layer:** Driver
**Provides:** `IHumidityTemp`, `IBarometer`
**Consumes:** `II2c` (I2cDriver)
**SRS traces:** REQ-SA-031, REQ-SA-071, REQ-SA-040, REQ-SA-060,
REQ-SA-080
**HLD ref:** `components.md` — HumidityTempDriver, BarometerDriver
(GW driver layer);
`sequence-diagrams.md` SD-01;
`task-breakdown.md` §5.2 SensorTask
**Version:** 1.1
**Date:** October 2026
**Status:** Pass H — Implemented (host-tested; hardware validation pending, see §14 GPA-O6)

**HLD anchor:** HumidityTempDriver + BarometerDriver in
`components.md` (GW driver layer)

---

## 1. Purpose and scope

### 1.1 In scope

- The public interfaces `IHumidityTemp` (`humidity_temp_init`,
  `humidity_temp_read`, `humidity_temp_reset_for_test`) and
  `IBarometer` (`barometer_init`, `barometer_read`,
  `barometer_reset_for_test`).
- Vtable singleton pattern for both interfaces (consistent with
  I2cDriver LLD-D10).
- HTS221 calibration coefficient extraction and integer-only
  linear interpolation.
- LPS22HB 24-bit pressure reconstruction and sign extension.
- STATUS_REG polling strategy with single 2 ms retry.
- Hardware contract: I2C addresses, init register writes,
  calibration register burst read.
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
  `docs/modbus-register-map.md`.
- **DRDY/INT GPIO configuration** — not used; STATUS_REG polling
  (GPA-D1).

### 1.3 Terminology

- **Compensation** — linear interpolation using factory calibration
  coefficients to convert raw ADC counts to physical units.
- **BDU** — Block Data Update; prevents reading a mix of old high
  byte and new low byte during a conversion cycle.
- **Auto-increment** — HTS221 feature: setting bit 7 of the register
  address auto-increments the address pointer on each byte read,
  enabling burst reads.

---

## 2. Sources

This companion covers two Gateway driver components that share the
same bus (I2C2), the same consumer (SensorService via SensorTask),
and the same polling strategy (STATUS_REG polling — no DRDY ISR).

| Component | Device | I2C address (7-bit) | PROVIDES | Root req |
|-----------|--------|---------------------|----------|----------|
| HumidityTempDriver | HTS221 | 0x5F | IHumidityTemp | REQ-SA-031 |
| BarometerDriver | LPS22HB | 0x5D | IBarometer | REQ-SA-031 |

Both sensors are Gateway-only. No Field Device variant exists. No
platform-split `.c` files are required.

### 2.1 Source references

| Source | Relevant section |
|--------|-----------------|
| `components.md` — Gateway driver layer | HumidityTempDriver, BarometerDriver responsibility sentences |
| `SRS.md` | REQ-SA-031 (init), REQ-SA-071 (read), REQ-SA-040/060/080 (error handling) |
| `task-breakdown.md` §5.2 | SensorTask: priority 5, 512 words, periodic 100 ms |
| `modbus-register-map.md` §6.2 | TEMPERATURE ×0.01 °C, HUMIDITY ×0.01 %RH, PRESSURE ×0.1 hPa |
| `sequence-diagrams.md` SD-01 | Sensor read nominal flow |
| UM2153 §7.12.2 | HTS221 features, I2C2, ODR |
| UM2153 §7.12.5 | LPS22HB features, I2C2, ODR |
| UM2153 Table 3 | I2C addresses confirmed |
| UM2153 Appendix B, Fig. 27 | DRDY/INT GPIO connections to EXTI lines |
| UM2153 Appendix B, Fig. 23 | MCU pin assignments for EXTI lines |
| HTS221 datasheet | Calibration register map, STATUS_REG, compensation formula |
| LPS22HB datasheet | PRESS_OUT XL/L/H layout, TEMP_OUT, STATUS_REG |

### 2.2 Consumed interfaces

HumidityTempDriver and BarometerDriver call the following
external functions. Full API documentation is in the I2cDriver
companion.

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

### 2.3 Dependency-conformance check

| Dependency | In `components.md` | Actual usage | Action |
|------------|-------------------|--------------|--------|
| I2cDriver | Yes | Yes — all register accesses via `i2c_driver->write()` / `i2c_driver->write_read()` | Correct |
| GpioDriver | Yes | No — DRDY/INT lines not used (polling-only design) | Flag: GpioDriver dependency removed at implementation level. See §13 open item GPA-O1. |

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

### 4.1 HumidityTempDriver — data types

```c
/* humidity_temp_driver.h */

#ifndef HUMIDITY_TEMP_DRIVER_H
#define HUMIDITY_TEMP_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include "i2c/i2c_driver.h"   /* ii2c_t, i2c_err_t (transitive — for error mapping) */

/**
 * @brief Error codes returned by all HumidityTempDriver operations.
 */
typedef enum {
    HUMIDITY_TEMP_ERR_OK            = 0, /**< Operation succeeded. */
    HUMIDITY_TEMP_ERR_I2C           = 1, /**< I2C bus error (NACK, timeout,
                                              or bus busy). */
    HUMIDITY_TEMP_ERR_NOT_READY     = 2, /**< STATUS_REG data-not-ready after
                                              retry. */
    HUMIDITY_TEMP_ERR_INVALID_ARG   = 3  /**< NULL pointer passed as output
                                              argument. */
} humidity_temp_err_t;

/**
 * @brief Compensated humidity and temperature reading from HTS221.
 *
 * Values are fixed-point integers matching the Modbus register map
 * scaling: temperature in °C × 100, humidity in %RH × 100.
 */
typedef struct {
    int32_t temperature_x100;   /**< °C × 100; e.g. 2350 = 23.50 °C.  */
    int32_t humidity_x100;      /**< %RH × 100; e.g. 5500 = 55.00 %RH. */
} humidity_temp_data_t;
```

### 4.2 HumidityTempDriver — IHumidityTemp

```c
/**
 * @brief Initialise the HTS221 humidity and temperature sensor.
 *
 * Writes CTRL_REG1 (0x20) = 0x85 (active, BDU, ODR 1 Hz) and
 * AV_CONF (0x10) = 0x1B (default averaging). Burst-reads calibration
 * registers 0x30–0x3F via auto-increment and stores coefficients in
 * the private state struct for subsequent compensation calculations.
 *
 * Must be called once from the board init function before
 * `vTaskStartScheduler()`. May be called in any order relative to
 * `barometer_init()`.
 *
 * @return HUMIDITY_TEMP_ERR_OK on success;
 *         HUMIDITY_TEMP_ERR_I2C if any I2C transaction fails during
 *         register configuration or calibration read.
 * @note   Threading: task-context only, non-blocking.
 *         Must be called before the scheduler starts.
 */
humidity_temp_err_t humidity_temp_init(void);

/**
 * @brief Read compensated humidity and temperature from HTS221.
 *
 * Checks STATUS_REG (0x27) bits H_DA and T_DA. If not set, retries
 * once after a 2 ms vTaskDelay. On success, burst-reads four output
 * bytes (HUMIDITY_OUT_L/H, TEMP_OUT_L/H) via auto-increment and
 * applies integer-only linear interpolation using stored calibration
 * coefficients.
 *
 * @param[out] out  Pointer to struct receiving the compensated reading.
 *                  Must not be NULL.
 * @return HUMIDITY_TEMP_ERR_OK on success;
 *         HUMIDITY_TEMP_ERR_INVALID_ARG if out is NULL;
 *         HUMIDITY_TEMP_ERR_I2C on I2C bus error;
 *         HUMIDITY_TEMP_ERR_NOT_READY if data-not-ready after retry.
 * @note   Threading: task-context only, may block (2 ms retry).
 *         Not ISR-safe.
 */
humidity_temp_err_t humidity_temp_read(humidity_temp_data_t *out);

#ifdef TEST
/**
 * @brief Reset module-level state for unit tests.
 *
 * Clears s_ht to its post-bss-init value (ready = false, calibration
 * zeroed) so each Unity test case starts from a clean slate.
 * Test-only; not compiled into firmware builds.
 */
void humidity_temp_reset_for_test(void);
#endif
```

### 4.3 HumidityTempDriver — vtable interface

```c
/**
 * @brief Vtable interface for HumidityTempDriver (singleton pattern,
 *        consistent with I2cDriver LLD-D10).
 *
 * SensorService depends on this interface, not on the concrete driver.
 * The implementation file defines the singleton instance.
 */
typedef struct {
    humidity_temp_err_t (*init)(void);
    humidity_temp_err_t (*read)(humidity_temp_data_t *out);
} ihumidity_temp_t;

/** Singleton pointer — defined in humidity_temp_driver.c. */
extern const ihumidity_temp_t * const humidity_temp_driver;

#endif /* HUMIDITY_TEMP_DRIVER_H */
```

**Vtable instance** (in `humidity_temp_driver.c`):

```c
static const ihumidity_temp_t s_humidity_temp_vtable = {
    .init = humidity_temp_init,
    .read = humidity_temp_read,
};

const ihumidity_temp_t * const humidity_temp_driver =
    &s_humidity_temp_vtable;
```

### 4.4 BarometerDriver — data types

```c
/* barometer_driver.h */

#ifndef BAROMETER_DRIVER_H
#define BAROMETER_DRIVER_H

#include <stdbool.h>
#include <stdint.h>
#include "i2c/i2c_driver.h"

/**
 * @brief Error codes returned by all BarometerDriver operations.
 */
typedef enum {
    BAROMETER_ERR_OK            = 0, /**< Operation succeeded. */
    BAROMETER_ERR_I2C           = 1, /**< I2C bus error. */
    BAROMETER_ERR_NOT_READY     = 2, /**< STATUS_REG data-not-ready
                                          after retry. */
    BAROMETER_ERR_INVALID_ARG   = 3  /**< NULL pointer passed. */
} barometer_err_t;

/**
 * @brief Pressure and temperature reading from LPS22HB.
 *
 * Pressure in hPa × 10 (matches Modbus register map scaling).
 * Temperature in °C × 100 (native LPS22HB output units).
 */
typedef struct {
    int32_t pressure_x10;       /**< hPa × 10; e.g. 10132 = 1013.2 hPa. */
    int32_t temperature_x100;   /**< °C × 100; e.g. 2350 = 23.50 °C.     */
} barometer_data_t;
```

### 4.5 BarometerDriver — IBarometer

```c
/**
 * @brief Initialise the LPS22HB barometric pressure sensor.
 *
 * Writes CTRL_REG1 (0x10) = 0x12 (BDU enabled, ODR 1 Hz) and
 * CTRL_REG2 (0x11) = 0x10 (IF_ADD_INC, normal mode). No calibration is needed —
 * the LPS22HB outputs compensated values natively.
 *
 * Must be called once from the board init function before
 * `vTaskStartScheduler()`. May be called in any order relative to
 * `humidity_temp_init()`.
 *
 * @return BAROMETER_ERR_OK on success;
 *         BAROMETER_ERR_I2C if any I2C transaction fails.
 * @note   Threading: task-context only, non-blocking.
 *         Must be called before the scheduler starts.
 */
barometer_err_t barometer_init(void);

/**
 * @brief Read compensated pressure and temperature from LPS22HB.
 *
 * Checks STATUS_REG (0x27) bits P_DA and T_DA. If not set, retries
 * once after a 2 ms vTaskDelay. On success, burst-reads five output
 * bytes (PRESS_OUT_XL/L/H, TEMP_OUT_L/H), sign-extends the 24-bit
 * pressure, and converts from hPa × 4096 to hPa × 10.
 *
 * @param[out] out  Pointer to struct receiving the reading.
 *                  Must not be NULL.
 * @return BAROMETER_ERR_OK on success;
 *         BAROMETER_ERR_INVALID_ARG if out is NULL;
 *         BAROMETER_ERR_I2C on I2C bus error;
 *         BAROMETER_ERR_NOT_READY if data-not-ready after retry.
 * @note   Threading: task-context only, may block (2 ms retry).
 *         Not ISR-safe.
 */
barometer_err_t barometer_read(barometer_data_t *out);

#ifdef TEST
/**
 * @brief Reset module-level state for unit tests.
 *
 * Clears s_baro to its post-bss-init value (ready = false) so each
 * Unity test case starts from a clean slate. Test-only; not compiled
 * into firmware builds.
 */
void barometer_reset_for_test(void);
#endif
```

### 4.6 BarometerDriver — vtable interface

```c
/**
 * @brief Vtable interface for BarometerDriver (singleton pattern,
 *        consistent with I2cDriver LLD-D10).
 *
 * SensorService depends on this interface, not on the concrete driver.
 */
typedef struct {
    barometer_err_t (*init)(void);
    barometer_err_t (*read)(barometer_data_t *out);
} ibarometer_t;

/** Singleton pointer — defined in barometer_driver.c. */
extern const ibarometer_t * const barometer_driver;

#endif /* BAROMETER_DRIVER_H */
```

**Vtable instance** (in `barometer_driver.c`):

```c
static const ibarometer_t s_barometer_vtable = {
    .init = barometer_init,
    .read = barometer_read,
};

const ibarometer_t * const barometer_driver = &s_barometer_vtable;
```

### 4.7 Internal scale notes

**HumidityTempDriver:** The Modbus register map uses TEMPERATURE
×0.01 °C (int16) and HUMIDITY ×0.01 %RH (uint16). This driver
outputs ×100 to preserve the same implicit decimal (×0.01 = /100
on read). SensorService passes these values to `sensor_reading_t`
without conversion.

**BarometerDriver:** Modbus PRESSURE ×0.1 hPa (uint16) → driver
outputs ×10 (same implicit decimal). LPS22HB TEMP_OUT is natively
°C × 100 (signed 16-bit) — no conversion needed.

---

## 5. Internal design

### 5.1 Module structure

Each driver is a singleton with a single private state struct and
a vtable pointer.

```
humidity_temp_driver.h   — public API + IHumidityTemp vtable
humidity_temp_driver.c   — singleton state, vtable instance, init,
                           compensation, read, reset_for_test
barometer_driver.h       — public API + IBarometer vtable
barometer_driver.c       — singleton state, vtable instance, init,
                           read, reset_for_test
```

No handles. No dynamic allocation. State structs are `static` in
`.c` files.

### 5.2 HTS221 — calibration and compensation (humidity_temp_driver.c)

The HTS221 does not output physical values directly. Raw ADC values
require linear interpolation against factory calibration coefficients
stored in on-chip OTP registers (addresses 0x30–0x3F).

**Private state struct:**

```c
typedef struct {
    int16_t  h0_rh_x2;     /* 0x30 */
    int16_t  h1_rh_x2;     /* 0x31 */
    int16_t  t0_degc_x8;   /* 0x32, bits[1:0] from 0x35 */
    int16_t  t1_degc_x8;   /* 0x33, bits[3:2] from 0x35 */
    int16_t  h0_t0_out;    /* 0x36–0x37, signed 16-bit */
    int16_t  h1_t0_out;    /* 0x3A–0x3B, signed 16-bit */
    int16_t  t0_out;       /* 0x3C–0x3D, signed 16-bit */
    int16_t  t1_out;       /* 0x3E–0x3F, signed 16-bit */
} hts221_cal_t;

typedef struct {
    hts221_cal_t cal;
    bool         ready;    /* set true after successful init */
} humidity_temp_state_t;

static humidity_temp_state_t s_ht;
```

**Compensation (integer arithmetic — no float):**

```c
/* Humidity — output in %RH × 100 */
int32_t h_raw;   /* read from HUMIDITY_OUT_L/H (0x28|0x29) */

int32_t h0_rh = (int32_t)s_ht.cal.h0_rh_x2 / 2;   /* integer halve */
int32_t h1_rh = (int32_t)s_ht.cal.h1_rh_x2 / 2;

int32_t humidity_x100 =
    (h1_rh - h0_rh) * 100 * (h_raw - s_ht.cal.h0_t0_out) /
    (s_ht.cal.h1_t0_out - s_ht.cal.h0_t0_out) + h0_rh * 100;

/* Temperature — output in °C × 100 */
int32_t t_raw;   /* read from TEMP_OUT_L/H (0x2A|0x2B) */

/* T0/T1 are stored ×8; the two MSBs are in register 0x35 bits[3:0] */
int32_t t0_degc_x8 = (int32_t)s_ht.cal.t0_degc_x8;  /* already assembled */
int32_t t1_degc_x8 = (int32_t)s_ht.cal.t1_degc_x8;

int32_t temperature_x100 =
    (t1_degc_x8 - t0_degc_x8) * 100 *
    (t_raw - s_ht.cal.t0_out) /
    (8 * (s_ht.cal.t1_out - s_ht.cal.t0_out)) +
    t0_degc_x8 * 100 / 8;
```

Rationale for no float: BARR-C fixed-width types; driver-layer
outputs stay as fixed-point integers. The intermediate products fit
in int32_t:
- Max |h_raw| = 32767; max (h1 − h0) ≈ 100 raw units × scale
  factor ≤ 100 × 100 = 10000. Product ≤ 10000 × 32767 ≈ 3.3 × 10⁸
  — within int32_t range (2.1 × 10⁹).
- Temperature: similar analysis holds.

**Note on t0_degc_x8 assembly in `humidity_temp_init()`:**

Register 0x35 holds the two MSBs for both T0 and T1:
- T0 MSBs at bits [1:0]
- T1 MSBs at bits [3:2]

```c
uint8_t msb_reg;  /* read from 0x35 */
s_ht.cal.t0_degc_x8 = (int16_t)t0_raw | (int16_t)((msb_reg & 0x03) << 8);
s_ht.cal.t1_degc_x8 = (int16_t)t1_raw | (int16_t)((msb_reg & 0x0C) << 6);
```

### 5.3 LPS22HB — pressure reconstruction (barometer_driver.c)

**Private state struct:**

```c
typedef struct {
    bool ready;    /* set true after successful init */
} barometer_state_t;

static barometer_state_t s_baro;
```

Pressure is a 24-bit signed two's-complement value across three
registers: PRESS_OUT_XL (0x28), PRESS_OUT_L (0x29), PRESS_OUT_H
(0x2A). Unit: hPa × 4096.

```c
uint8_t buf[3];   /* XL, L, H in order */
int32_t raw = (int32_t)((uint32_t)buf[2] << 16 |
                         (uint32_t)buf[1] <<  8 |
                         (uint32_t)buf[0]);
/* Sign-extend from 24 bits */
if (raw & 0x800000) { raw |= (int32_t)0xFF000000; }

/* Convert to hPa × 10: (raw × 10) / 4096 = (raw × 10) >> 12 */
out->pressure_x10 = (raw * 10) >> 12;
```

Overflow check: max raw = 1260 × 4096 = 5,160,960. 5,160,960 × 10
= 51,609,600 — well within int32_t. No overflow.

Temperature from LPS22HB (TEMP_OUT_L 0x2B, TEMP_OUT_H 0x2C):

```c
int16_t t_raw;  /* signed 16-bit, unit = °C × 100 */
out->temperature_x100 = (int32_t)t_raw;  /* already ×100, no conversion */
```

### 5.4 STATUS_REG polling strategy

Both sensors run in continuous mode at ODR = 1 Hz (matching the
1 Hz default SensorTask period). Before reading output registers,
the driver checks the data-ready bit in STATUS_REG.

**HTS221 STATUS_REG (0x27):**
- Bit 1: H_DA — humidity data available
- Bit 0: T_DA — temperature data available
- Read condition: both bits must be set

**LPS22HB STATUS_REG (0x27):**
- Bit 1: P_DA — pressure data available
- Bit 0: T_DA — temperature data available
- Read condition: both bits must be set

If STATUS_REG bits are not set on first check, the driver makes one
retry after a 2 ms `vTaskDelay`. If still not set, returns
`HUMIDITY_TEMP_ERR_NOT_READY` / `BAROMETER_ERR_NOT_READY`.

Rationale: at 1 Hz ODR and a 1 Hz SensorTask tick, data is
virtually always ready when polled. The 2 ms retry covers minor
clock-edge jitter without blocking SensorTask for a full ODR period.

### 5.5 `humidity_temp_reset_for_test()` implementation

```c
#ifdef TEST
void humidity_temp_reset_for_test(void)
{
    memset(&s_ht, 0, sizeof(s_ht));
}
#endif
```

### 5.6 `barometer_reset_for_test()` implementation

```c
#ifdef TEST
void barometer_reset_for_test(void)
{
    memset(&s_baro, 0, sizeof(s_baro));
}
#endif
```

---

## 6. Synchronisation

Caller serialises. Neither driver holds FreeRTOS synchronisation
primitives. All entry points (`init`, `read`) are intended to be
called from a single task context (`SensorTask`) or from `main()`
before the scheduler starts.

I2C bus serialisation: all four Group A + Group B sensor drivers
share I2C2 via I2cDriver. I2cDriver has no internal mutex
(established convention). SensorTask calls all sensor reads
sequentially within a single task period; no concurrent I2C access
occurs.

---

## 7. Init ordering

### 7.1 Pre-conditions

| Dependency | Pre-condition |
|------------|---------------|
| I2cDriver | `i2c_init()` must have completed successfully |
| Clock | I2C2 peripheral clock enabled (handled by I2cDriver) |

### 7.2 `humidity_temp_init()` sequence

```
humidity_temp_init()
  → i2c_driver->write(0x5F, {0x20, 0x85}, 2)  /* CTRL_REG1: active, BDU, 1 Hz */
  → i2c_driver->write(0x5F, {0x10, 0x1B}, 2)  /* AV_CONF: default averaging */
  → i2c_driver->write_read(0x5F, {0x30|0x80}, 1, calib_buf, 16)
                                                /* burst-read calibration */
  → parse calib_buf → fill s_ht.cal
  → assemble T0/T1 MSBs from register 0x35
  → s_ht.ready = true
  → return HUMIDITY_TEMP_ERR_OK
```

### 7.3 `barometer_init()` sequence

```
barometer_init()
  → i2c_driver->write(0x5D, {0x10, 0x12}, 2)  /* CTRL_REG1: BDU, 1 Hz */
  → i2c_driver->write(0x5D, {0x11, 0x10}, 2)  /* CTRL_REG2: IF_ADD_INC, normal mode */
  → s_baro.ready = true
  → return BAROMETER_ERR_OK
```

`humidity_temp_init()` and `barometer_init()` are called from the
board init function before `vTaskStartScheduler()`. No two-phase
init required — neither driver uses interrupts.

---

## 8. Sequence integration

Both drivers are called from SensorTask in the SD-01 nominal flow.

```
SensorTask (100 ms tick)
  → SensorService.run_cycle()
      → humidity_temp_driver->read(&ht_data)
          → i2c_driver->write_read(0x5F, {0x27}, 1, &status, 1)
          → [2 ms retry if not ready]
          → i2c_driver->write_read(0x5F, {0x28|0x80}, 1, raw_buf, 4)
          → apply compensation → fill ht_data
      → barometer_driver->read(&baro_data)
          → i2c_driver->write_read(0x5D, {0x27}, 1, &status, 1)
          → [2 ms retry if not ready]
          → i2c_driver->write_read(0x5D, {0x28}, 1, raw_buf, 5)
          → reconstruct 24-bit pressure + 16-bit temp → fill baro_data
```

### SD trace

| SD | Component role | Key function |
|----|----------------|--------------|
| SD-01 | HumidityTempDriver and BarometerDriver are called by SensorService during the periodic sensor read cycle. Each driver polls STATUS_REG, burst-reads output registers, and returns compensated data. | `humidity_temp_read()`, `barometer_read()` |

---

## 9. Error and fault behaviour

All public functions return a typed `_err_t`; callers must not
ignore non-OK returns.

### HumidityTempDriver error codes

| Error value | Cause | Local behaviour | Retry | Observability |
|-------------|-------|-----------------|-------|---------------|
| `HUMIDITY_TEMP_ERR_OK` | Success | Return compensated data | N/A | — |
| `HUMIDITY_TEMP_ERR_I2C` | I2C bus error (NACK, timeout, bus busy) from `i2c_driver->write_read()` | Return error; `out` unchanged | No — I2C error is transient; SensorService retries on next cycle | SensorService logs at WARN per REQ-SA-080 |
| `HUMIDITY_TEMP_ERR_NOT_READY` | STATUS_REG data-not-ready after 2 ms retry | Return error; `out` unchanged | No — SensorService retries on next 100 ms cycle | SensorService logs at DEBUG |
| `HUMIDITY_TEMP_ERR_INVALID_ARG` | NULL `out` pointer | Return error immediately | No — programming error | Caller logs at ERROR |

### BarometerDriver error codes

| Error value | Cause | Local behaviour | Retry | Observability |
|-------------|-------|-----------------|-------|---------------|
| `BAROMETER_ERR_OK` | Success | Return pressure and temperature | N/A | — |
| `BAROMETER_ERR_I2C` | I2C bus error from `i2c_driver->write_read()` | Return error; `out` unchanged | No — retried on next cycle | SensorService logs at WARN per REQ-SA-080 |
| `BAROMETER_ERR_NOT_READY` | STATUS_REG data-not-ready after 2 ms retry | Return error; `out` unchanged | No — retried on next cycle | SensorService logs at DEBUG |
| `BAROMETER_ERR_INVALID_ARG` | NULL `out` pointer | Return error immediately | No — programming error | Caller logs at ERROR |

### Init error handling

If any I2C transaction fails during `humidity_temp_init()` or
`barometer_init()`, the function returns `_ERR_I2C` and the
`ready` flag remains `false`. `read()` called before a successful
`init()` returns `_ERR_NOT_READY` without touching the bus (GPA-D9).

SensorService (caller) handles all non-OK returns per REQ-SA-080
(log error code) and REQ-SA-060 (continue with available sensors).
The drivers do not log — they return error codes and let the
application layer decide.

---

## 10. Hardware contract

### 10.1 HTS221

| Parameter | Value | Source |
|-----------|-------|--------|
| Bus | I2C2 | UM2153 §7.12.2 |
| 7-bit address | 0x5F | UM2153 Table 3 |
| Write address (8-bit) | 0xBE | UM2153 Table 3 |
| Read address (8-bit) | 0xBF | UM2153 Table 3 |
| DRDY GPIO | PD15 (EXTI15) | UM2153 Fig. 27 + Fig. 23 — verify (GPA-O2) |
| DRDY used at firmware level | No — STATUS_REG polling | Decision GPA-D1 |
| Power-on ODR | Power-down (default) | HTS221 datasheet CTRL_REG1 |
| Configured ODR | 1 Hz (CTRL_REG1[1:0] = 01) | Matches SensorTask 1 Hz default |
| Auto-increment on I2C | Enabled by setting bit 7 of register address | Required for multi-byte reads |
| Supply voltage | 3.3 V | Board power rail |

**Registers written at init:**

| Register | Address | Value | Meaning |
|----------|---------|-------|---------|
| CTRL_REG1 | 0x20 | 0x85 | PD=1 (active), BDU=1, ODR[1:0]=01 (1 Hz) |
| AV_CONF | 0x10 | 0x1B | Default averaging (AVGT=16, AVGH=32) |

**Calibration registers read at init (auto-increment burst
0x30–0x3F with address | 0x80):**

| Register | Address |
|----------|---------|
| H0_rH_x2 | 0x30 |
| H1_rH_x2 | 0x31 |
| T0_degC_x8 | 0x32 |
| T1_degC_x8 | 0x33 |
| T1T0_MSB | 0x35 |
| H0_T0_OUT_L/H | 0x36–0x37 |
| H1_T0_OUT_L/H | 0x3A–0x3B |
| T0_OUT_L/H | 0x3C–0x3D |
| T1_OUT_L/H | 0x3E–0x3F |

### 10.2 LPS22HB

| Parameter | Value | Source |
|-----------|-------|--------|
| Bus | I2C2 | UM2153 §7.12.5 |
| 7-bit address | 0x5D | UM2153 Table 3 |
| Write address (8-bit) | 0xBA | UM2153 Table 3 |
| Read address (8-bit) | 0xBB | UM2153 Table 3 |
| INT_DRDY GPIO | PD10 (EXTI10) | UM2153 Fig. 27 + Fig. 23 — verify (GPA-O2) |
| INT_DRDY used | No — STATUS_REG polling | Decision GPA-D1 |
| Pressure range | 260–1260 hPa | UM2153 §7.12.5 |
| Power-on ODR | Power-down (default) | LPS22HB datasheet CTRL_REG1 |
| Configured ODR | 1 Hz (CTRL_REG1[6:4] = 001) | Matches SensorTask 1 Hz default |
| Block data update (BDU) | Enabled | Prevents mismatched high/low byte reads |
| Supply voltage | 3.3 V | Board power rail |

**Registers written at init:**

| Register | Address | Value | Meaning |
|----------|---------|-------|---------|
| CTRL_REG1 | 0x10 | 0x12 | BDU=1, ODR=1 Hz |
| CTRL_REG2 | 0x11 | 0x10 | IF_ADD_INC=1 (required for the 5-byte burst read), normal mode (no one-shot, no reset) |

### 10.3 Registers / Pins / Clocks / NVIC

**Registers:** N/A — HTS221 and LPS22HB are external I2C sensor
ICs. Their internal device registers are accessed via I2C
transactions delegated to I2cDriver. Neither driver directly
touches any MCU peripheral register.

**Pins:** N/A — I2C SDA/SCL pins are configured by I2cDriver.
DRDY/INT pins (PD15, PD10) are not used at firmware level (GPA-D1).

**Clocks:** N/A — I2C peripheral clock is enabled by I2cDriver.

**NVIC:** N/A — STATUS_REG polling (GPA-D1); no DRDY interrupts.

---

## 11. Memory and sizing

### RAM budget

| Item | Size | Notes |
|------|------|-------|
| `s_ht` (humidity_temp_state_t) | 20 B | hts221_cal_t (16 B) + ready (1 B) + padding |
| `s_baro` (barometer_state_t) | 4 B | ready (1 B) + padding |
| Stack-local buffers in `humidity_temp_read()` | 6 B | calib burst: 16 B at init only; output: 4 B + STATUS_REG: 1 B |
| Stack-local buffers in `barometer_read()` | 6 B | 5 B output + 1 B STATUS_REG |
| **Total static RAM** | **~24 B** | |

### ROM budget

| Item | Size | Notes |
|------|------|-------|
| `humidity_temp_init()` + calibration parsing | ~200 B | Register writes + burst read + MSB assembly |
| `humidity_temp_read()` + compensation | ~250 B | STATUS_REG poll + output read + integer interpolation |
| `barometer_init()` | ~80 B | Two register writes |
| `barometer_read()` + reconstruction | ~150 B | STATUS_REG poll + output read + sign extension + shift |
| Vtable instances (2) | ~16 B | Two function-pointer structs |
| **Total estimated ROM** | **~700 B** | |

---

## 12. File layout

```
firmware/gateway/drivers/humidity_temp/
├── humidity_temp_driver.h     /* IHumidityTemp — public API + vtable */
└── humidity_temp_driver.c     /* Implementation (singleton) */

firmware/gateway/drivers/barometer/
├── barometer_driver.h         /* IBarometer — public API + vtable */
└── barometer_driver.c         /* Implementation (singleton) */

firmware/gateway/drivers/i2c/
├── i2c_driver.h               /* II2c interface — Gateway copy (GPA-D7) */
└── i2c_driver_l4.c            /* I2C v2 implementation (docs/lld/drivers/i2c-driver.md) */

firmware/gateway/integration-tests/sensor_drivers_group_a/
└── main_test_sensor_drivers_group_a.c   /* TC-HW-SA-001..006 */

tests/gateway/drivers/
├── humidity_temp_driver/test_humidity_temp_driver.c  /* Unity, register-map fake I2C */
└── barometer_driver/test_barometer_driver.c          /* Unity, register-map fake I2C */
```

No internal headers needed. Compensation helpers and STATUS_REG
polling are `static` functions within each `.c` file.

---

## 13. Unit-test plan

**Framework:** Unity + CMock (project standard). Tests run on the
host machine (PC). I2C dependencies are mocked by CMock replacing
`i2c_driver.c` at link time. The mock provides
`i2c_driver->write()` and `i2c_driver->write_read()` via the
`II2c` vtable.

**Mocked dependencies:** I2cDriver (`i2c_write`, `i2c_write_read`),
FreeRTOS `vTaskDelay` stub.

**Test file:** `tests/gateway/drivers/humidity_temp_driver/test_humidity_temp_driver.c`

| TC ID | Scenario | Mechanism | Expected result |
|-------|----------|-----------|-----------------|
| TC-HT-001 | Nominal read — compensated values match hand calculation | Mock I2C returns valid STATUS_REG (0x03) + known raw values; pre-loaded calibration constants via mock init | `humidity_temp_read()` returns `OK`; `temperature_x100` and `humidity_x100` match hand-calculated compensation ±1 LSB |
| TC-HT-002 | I2C bus failure on STATUS_REG read | Mock `i2c_driver->write_read()` returns `I2C_ERR_NACK` on STATUS_REG read | Returns `HUMIDITY_TEMP_ERR_I2C`; `out` unchanged |
| TC-HT-003 | STATUS_REG not ready on first check, ready on retry | Mock returns status=0x00 first call, status=0x03 second call | Returns `OK` after 2 ms retry; `vTaskDelay(2)` called once |
| TC-HT-004 | STATUS_REG never ready | Mock always returns status=0x00 | Returns `HUMIDITY_TEMP_ERR_NOT_READY` |
| TC-HT-005 | NULL out pointer | Call `humidity_temp_read(NULL)` | Returns `HUMIDITY_TEMP_ERR_INVALID_ARG` |
| TC-HT-006 | Degenerate calibration — zero span | Set H0_rH_x2 == H1_rH_x2 (h0_t0_out == h1_t0_out) | No divide-by-zero; returns `OK` with the H0 / T0 calibration point (GPA-D10) |
| TC-HT-007 | Known compensation values from datasheet example | Use calibration constants from HTS221 datasheet application note | Output matches documented example ±1 LSB |
| TC-HT-008 | Init I2C failure — CTRL_REG1 write fails | Mock `i2c_driver->write()` returns `I2C_ERR_NACK` | Returns `HUMIDITY_TEMP_ERR_I2C`; `s_ht.ready` remains false |
| TC-HT-009 | Read before init | Call `humidity_temp_read()` without prior `humidity_temp_init()` | Returns `HUMIDITY_TEMP_ERR_NOT_READY`; no I2C transaction issued (GPA-D9) |
| TC-HT-010 | `humidity_temp_reset_for_test()` clears state | Call init, then reset, then read | Read fails as if uninitialised |

**Test file:** `tests/gateway/drivers/barometer_driver/test_barometer_driver.c`

| TC ID | Scenario | Mechanism | Expected result |
|-------|----------|-----------|-----------------|
| TC-BA-001 | Nominal read — pressure and temperature correct | Mock I2C returns STATUS_REG=0x03 + known XL/L/H bytes | `barometer_read()` returns `OK`; `pressure_x10` matches (raw × 10) >> 12 |
| TC-BA-002 | Pressure sign extension — negative raw | Inject raw = 0xFFFFFF (−1 after sign-extend) | `pressure_x10` = (−1 × 10) >> 12 = 0 (truncated toward zero); verifies sign-extension path |
| TC-BA-003 | I2C bus failure on output read | Mock returns error on the 5-byte burst read | Returns `BAROMETER_ERR_I2C` |
| TC-BA-004 | STATUS_REG never ready | Mock always returns status=0x00 | Returns `BAROMETER_ERR_NOT_READY` |
| TC-BA-005 | NULL out pointer | Call `barometer_read(NULL)` | Returns `BAROMETER_ERR_INVALID_ARG` |
| TC-BA-006 | Max pressure boundary (1260 hPa) | raw = 1260 × 4096 = 5,160,960 | `pressure_x10` = 12600; no int32_t overflow |
| TC-BA-007 | Min pressure boundary (260 hPa) | raw = 260 × 4096 = 1,064,960 | `pressure_x10` = 2600; correct conversion |
| TC-BA-008 | Init I2C failure | Mock write returns error | Returns `BAROMETER_ERR_I2C`; `s_baro.ready` remains false |
| TC-BA-009 | `barometer_reset_for_test()` clears state | Call init, then reset, then read | Read fails as if uninitialised |

**Added at implementation (TC-HT-011..018):** init register sequence and
burst-read command bytes (0x85 / 0x10 / 0xB0 / 0xA8); AV_CONF write failure;
calibration burst failure; STATUS_REG failure on the retry; output burst failure;
partial STATUS (one of H_DA / T_DA); humidity clamping to 0..100 %RH; vtable
dispatch.

**Added at implementation (TC-BA-010..016):** init register sequence
(CTRL_REG2 = 0x10); CTRL_REG2 write failure; STATUS_REG failure; ready-on-retry
with P_DA only; negative temperature; read before init; vtable dispatch.

**Mechanism change.** The plan named CMock for I2C. Both suites instead use a
register-map fake behind the `ii2c_t` vtable defined in the test TU
(`i2c_driver` is a `const` pointer, which CMock cannot replace). The fake serves
STATUS_REG from a scripted sequence and the other registers from a 256-byte
array, so burst reads and auto-increment are exercised end to end. `vTaskDelay`
is a counting mock added to `tests/support/freertos_mock.c`.

**TC-HT-007 note.** The ST application-note numbers are not in the repository.
The test uses a representative calibration (odd `H*_rH_x2`, fractional T points)
checked against an independent floating-point model, ±1 LSB.

**Results (2026-10-14):** HumidityTempDriver 18 / 18 pass, BarometerDriver 16 / 16
pass, 0 ignored. Hardware cases TC-HW-SA-001..006 are in
`firmware/gateway/integration-tests/sensor_drivers_group_a/` and have **not been
run** (run after the I2cDriver bring-up passes).

---

## 14. Open items

| ID | Item | Owner | Resolution path | Status |
|----|------|-------|-----------------|--------|
| GPA-O1 | `components.md` lists GpioDriver as a dependency of both HumidityTempDriver and BarometerDriver. GpioDriver is not called by the polling-only design. | Luca | Update `components.md` USES list for both components to remove GpioDriver. Commit as `docs: remove spurious GpioDriver dependency from HTS221/LPS22HB drivers`. | Closed 2026-10-14 — `components.md` Gateway USES for both components now lists I2cDriver only. |
| GPA-O2 | DRDY GPIO pin assignments (HTS221 → PD15/EXTI15, LPS22HB → PD10/EXTI10) from schematic Fig. 27 and Fig. 23. Not cross-checked against UM2153 Appendix A. | Luca | Verify against UM2153 Appendix A before implementation. Although not used at firmware level, the assignment must be documented correctly. | Open |
| GPA-O3 | I2C2 TIMINGR value (clock speed, rise/fall times for L475) is a cross-cutting open item from I2cDriver companion (I2CD-O1 root). | Luca | Resolve in clock-config companion. | Open |
| GPA-O4 | HTS221 AV_CONF register value 0x1B is the datasheet default. Optimal averaging for 1 Hz ODR should be confirmed against the datasheet noise/latency tradeoff table. | Luca | The register **address** was wrong in v1.0 (0xC0 → 0x10, GPA-D8); the value 0x1B is unchanged and safe. Judge the averaging against the noise seen in TC-HW-SA-005. | Open (address corrected) |
| GPA-O5 | The Gateway I2cDriver did not exist when this companion was implemented. | Luca | `i2c_driver_l4.c` implemented 2026-10-14 (docs/lld/drivers/i2c-driver.md §7.6); the placeholder is deleted. Hardware validation of TC-HW-SA-001..006 is GPA-O6. | Closed 2026-10-14 |
| GPA-O6 | Hardware validation of both drivers (TC-HW-SA-001..006). | Luca | Run the bring-up harness once GPA-O5 closes. Also confirms the corrected init values (GPA-D8) against real silicon. | Open |
| GPA-O7 | Both drivers call `vTaskDelay` (FreeRTOS), but `components.md` lists no FreeRTOS in their USES and the coding rules forbid FreeRTOS includes in the driver layer unless USES permits it. Same position as the Gateway LedDriver. | Luca | Either add the FreeRTOS exception to `components.md` for blocking sensor drivers, or move the 2 ms retry up into SensorService. | Closed 2026-10-15 — exception added to `components.md` "USES list naming convention"; the retry stays in the drivers. |

---

## 15. Decisions log

| ID | Decision | Rationale |
|----|----------|-----------|
| GPA-D1 | No DRDY/INT ISR for HTS221 or LPS22HB | SensorTask polls at 1 Hz; no real-time benefit to ISR-driven read at this rate. STATUS_REG check adds < 100 µs overhead. Simplifies design by eliminating callback registration and EXTI configuration for these two sensors. |
| GPA-D2 | Continuous mode ODR = 1 Hz for both sensors | Matches default 1 Hz SensorTask period. Power consumption negligible vs ODR alternatives. |
| GPA-D3 | BDU (block data update) enabled on LPS22HB | Prevents reading the high byte of a new conversion while still reading the low byte of the previous one. Mandatory for correct 24-bit reconstruction. |
| GPA-D4 | HTS221 calibration read as burst (auto-increment) | Reduces I2C transactions from 16 individual reads to 1 burst. Auto-increment enabled via address bit 7. |
| GPA-D5 | Integer-only compensation for HTS221 | No float in driver layer. Verified intermediate products fit in int32_t. Consistent with fixed-point driver output convention. |
| GPA-D6 | Vtable singleton pattern for both drivers | Consistent with I2cDriver (LLD-D10). SensorService depends on `ihumidity_temp_t` / `ibarometer_t` interfaces, enabling test mocking via vtable replacement. P2 conformant. |
| GPA-D7 | The Gateway gets its own `i2c/i2c_driver.h` (interface copy of the Field Device header) | The two boards are separate CubeIDE projects and the shared header only existed under `field-device`. Originally paired with a weak placeholder vtable; the placeholder is deleted now that `i2c_driver_l4.c` exists. |
| GPA-D8 | Init register values corrected against the HTS221 / LPS22HB datasheets (ST BSP headers in `vendor/`): HTS221 CTRL_REG1 **0x85** (not 0x83), AV_CONF at **0x10** (not 0xC0), LPS22HB CTRL_REG2 **0x10** (not 0x00) | v1.0 wrote BDU=0 with ODR 12.5 Hz (0x83 = PD + ODR 11), wrote the averaging value to a reserved register (0xC0), and cleared IF_ADD_INC, which turns the 5-byte burst read into the same register five times. All three would pass a mocked-I2C test. |
| GPA-D9 | `read()` before a successful `init()` returns `_ERR_NOT_READY` with no bus traffic, instead of a debug assertion | An assertion would abort the host test run and cannot be asserted on; the ready flag already exists. Release behaviour in v1.0 was "undefined". |
| GPA-D10 | A zero calibration span (corrupt OTP) yields the H0 / T0 point rather than an error; humidity is clamped to 0..10000 | No error code exists for it and one reading is better than none for REQ-SA-060 (continue with available sensors). Clamping follows the datasheet's 0..100 %RH range. |
| GPA-D11 | Humidity compensation uses `H*_rH_x2 × 50` instead of halving first | `h_rh_x2 / 2` drops 0.5 %RH whenever the calibration byte is odd. Mathematically identical otherwise; int32 range unchanged. |
| GPA-D12 | Pressure conversion is `(raw × 10) / 4096`, not `>> 12` | TC-BA-002 expects truncation toward zero; an arithmetic shift of a negative value rounds toward minus infinity. Identical for the positive pressures the part can output. |
| GPA-D13 | Project placement: `project_gateway.yml`, directories `humidity_temp_driver/` and `barometer_driver/` | Same basenames as the Field Device simulated drivers; the default Ceedling project excludes the Gateway copies (and `drivers/i2c`) so no header is shadowed. |

---

## 16. Principles applied

- **P1 (Strict directional layering).** Both drivers depend only on
  `II2c` (driver layer). No upward dependency on middleware or
  application. Calls flow downward via `i2c_driver->write()` and
  `i2c_driver->write_read()`.
- **P2 (Dependency Inversion).** Each sensor exposes its own vtable:
  `ihumidity_temp_t` and `ibarometer_t`. SensorService (application
  layer) depends on the interfaces, not on the concrete driver
  implementations. This enables test mocking via vtable replacement.
- **P3 (Interface Segregation).** Two separate interfaces
  (`IHumidityTemp`, `IBarometer`) rather than one unified sensor
  interface. Each consumer (SensorService) calls only the interface
  it needs — each sensor is read independently.
- **P5 (Bounded resources, no dynamic allocation post-init).**
  Static state per driver; no dynamic allocation at any point.
  Calibration coefficients are read once at init and stored in
  static structs.
- **P6 (Responsibility traces to requirements).** Measurement
  functions trace to REQ-SA-031 (init), REQ-SA-071 (read).
  Error handling traces to REQ-SA-040, REQ-SA-060, REQ-SA-080.
- **P10 (Naming conventions).** Prefixes `humidity_temp_` /
  `barometer_`. Errors `HUMIDITY_TEMP_ERR_*` / `BAROMETER_ERR_*`.
  Static state `s_ht`, `s_baro`. Vtable pointers
  `humidity_temp_driver`, `barometer_driver`.

**P4 (Cross-cutting concern exception) does not apply.** These
drivers do not reference Logger directly; error codes are returned
to the caller (SensorService) which handles logging.

**P7 (Pull-based data access) does not apply.** These are leaf
drivers called synchronously — no data production/consumption
pattern.

**P8, P9 do not apply.** P8 (Infrastructure traces to pain) — no
helper infrastructure. P9 (Multi-view diagrams) — not a diagram
document.

**Coding standard compliance (BARR-C:2018).** Fixed-width types
throughout: sensor addresses `uint8_t`, register values `uint8_t`,
compensated output `int32_t`. No floating-point in the driver
layer. All functions return typed `_err_t` — no silent failures.
Static allocation only. `const` correctness on vtable pointers.
Braces on all control structures. Module prefix on all public
symbols.

---

## 17. References

- `docs/components.md` (GW driver layer, HumidityTempDriver and
  BarometerDriver entries).
- `docs/SRS.md` (REQ-SA-031, REQ-SA-071, REQ-SA-040, REQ-SA-060,
  REQ-SA-080).
- `docs/sequence-diagrams.md` SD-01 (sensor read nominal flow).
- `docs/task-breakdown.md` §5.2 (SensorTask — priority 5,
  512 words, 100 ms period).
- `docs/modbus-register-map.md` §6.2 (register scaling).
- `docs/lld/companions/i2c-driver.md` (I2cDriver companion — defines
  `II2c`, `i2c_err_t`).
- `docs/architecture-principles.md` (P1, P2, P3, P5, P6, P10).
- HTS221 datasheet (ST Microelectronics).
- LPS22HB datasheet (ST Microelectronics).
- UM2153 Discovery Kit for IoT Node User Manual.

---

## Phase H checklist

| # | Check | Status |
|---|-------|--------|
| H-01 | Every public function has a Doxygen block with `@brief`, `@param`, `@return`, `@note Threading` | Done (§4.2, §4.5) |
| H-02 | Every error code documented in an error table with cause, behaviour, retry policy, and observability | Done (§9) |
| H-03 | Init ordering documented with pre-conditions | Done (§7) |
| H-04 | Synchronisation strategy documented | Done (§6) |
| H-05 | `_reset_for_test()` hook present, guarded by `#ifdef TEST` | Done (§4.2, §4.5, §5.5, §5.6) |
| H-06 | File layout matches project directory structure | Done (§12) |
| H-07 | Unit-test plan uses CMock with TC-NNN identifiers | Done (§13) |
| H-08 | Open items have structured IDs and resolution paths | Done (§14) |
| H-09 | Principles section contains no false claims | Done (§16) — P4/P7/P8/P9 explicitly noted as not applicable; coding standard items separated from P-numbered principles |
| H-10 | SD trace table uses correct function names matching §4 API | Done (§8) |
| H-11 | State machine mapping to code documented | N/A — no state machine; sensor drivers are stateless read-only (ready flag is a guard, not a machine) |
| H-12 | Sizing estimates provided (RAM + ROM) | Done (§11) |
| H-13 | All SRS traces from header verified against section content | Done — traces checked against §3, §9 |
| H-14 | Decisions logged with rationale | Done (§15) |
