# LLD Companion — Gateway Sensor Drivers Group A
## HumidityTempDriver (HTS221) + BarometerDriver (LPS22HB)

**Board:** Gateway (B-L475E-IOT01A)  
**Branch:** `feature/lld-gw-sensor-drivers-group-a`  
**Status:** Draft  
**Methodology:** lld-methodology.md v1.1, steps 1–8  
**Version:** 0.1
**Date:** May 2026

**HLD anchor:** HumidityTempDriver + BarometerDriver in `components.md` (GW driver layer)

---

## 1. Sources

This document covers two Gateway driver components that share the same
bus (I2C2), the same consumer (SensorService via SensorTask), and the same
polling strategy (STATUS_REG polling — no DRDY ISR).

| Component | Device | I2C address (7-bit) | PROVIDES | Root req |
|-----------|--------|---------------------|----------|----------|
| HumidityTempDriver | HTS221 | 0x5F | IHumidityTemp | REQ-SA-031 |
| BarometerDriver | LPS22HB | 0x5D | IBarometer | REQ-SA-031 |

Both sensors are Gateway-only. There is no Field Device variant of these
drivers. No platform-split `.c` files are required.

---

### 1.1 Source references

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

---

## 2. Public API

### 3.1 HumidityTempDriver — IHumidityTemp

```c
/* humidity_temp_driver.h */

#ifndef HUMIDITY_TEMP_DRIVER_H
#define HUMIDITY_TEMP_DRIVER_H

#include <stdint.h>

typedef enum {
    HUMIDITY_TEMP_ERR_OK            = 0,
    HUMIDITY_TEMP_ERR_I2C           = 1,  /* I2C bus error          */
    HUMIDITY_TEMP_ERR_NOT_READY     = 2,  /* STATUS_REG data not ready */
    HUMIDITY_TEMP_ERR_INVALID_ARG   = 3   /* NULL pointer            */
} humidity_temp_err_t;

typedef struct {
    int32_t temperature_x100;   /* °C × 100; e.g. 2350 = 23.50 °C  */
    int32_t humidity_x100;      /* %RH × 100; e.g. 5500 = 55.00 %RH */
} humidity_temp_data_t;

humidity_temp_err_t humidity_temp_init(void);
humidity_temp_err_t humidity_temp_read(humidity_temp_data_t *out);

#endif /* HUMIDITY_TEMP_DRIVER_H */
```

`humidity_temp_init()` must be called once before any `humidity_temp_read()`.
No two-phase init is needed: the HTS221 has no ISR, so there is no
consumer task that must exist before init.

**Internal scale note:** the Modbus register map uses TEMPERATURE ×0.01 °C
(int16) and HUMIDITY ×0.01 %RH (uint16). This driver outputs ×100 to
preserve the same implicit decimal (×0.01 = /100 on read). SensorService
passes these values directly to SensorReading without conversion.

### 3.2 BarometerDriver — IBarometer

```c
/* barometer_driver.h */

#ifndef BAROMETER_DRIVER_H
#define BAROMETER_DRIVER_H

#include <stdint.h>

typedef enum {
    BAROMETER_ERR_OK            = 0,
    BAROMETER_ERR_I2C           = 1,
    BAROMETER_ERR_NOT_READY     = 2,
    BAROMETER_ERR_INVALID_ARG   = 3
} barometer_err_t;

typedef struct {
    int32_t pressure_x10;       /* hPa × 10; e.g. 10132 = 1013.2 hPa */
    int32_t temperature_x100;   /* °C × 100; e.g. 2350 = 23.50 °C     */
} barometer_data_t;

barometer_err_t barometer_init(void);
barometer_err_t barometer_read(barometer_data_t *out);

#endif /* BAROMETER_DRIVER_H */
```

**Internal scale note:** Modbus PRESSURE ×0.1 hPa (uint16) → driver outputs
×10 (same implicit decimal). TEMPERATURE from LPS22HB is ×0.01 °C
natively (16-bit signed output of the sensor is already in °C × 100 units).

### 3.3 Dependency-conformance check

| Dependency | In `components.md` | Actual usage | Action |
|------------|-------------------|--------------|--------|
| I2cDriver | Yes | Yes — all register accesses via `i2c_write_read()` | Correct |
| GpioDriver | Yes | No — DRDY/INT lines not used (polling-only design) | Flag: GpioDriver dependency removed at implementation level. See §9 open item GPA-O1. |

P3 (ISP) — IHumidityTemp and IBarometer are already segregated at HLD.
Each consumer (SensorService) calls only the interface it depends on.
No further split is needed at driver level.

---

## 3. Internal design

### 4.1 Module structure

Each driver is a singleton with a single private state struct.

```
humidity_temp_driver.h   — public API (IHumidityTemp)
humidity_temp_driver.c   — singleton state, init, compensation, read
barometer_driver.h       — public API (IBarometer)
barometer_driver.c       — singleton state, init, read
```

No handles. No dynamic allocation. State structs are `static` in `.c` files.

### 4.2 HTS221 — calibration and compensation (humidity_temp_driver.c)

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

Rationale for no float: P9 (bare-metal C) + BARR-C fixed-width types.
The intermediate products fit in int32_t:
- Max |h_raw| = 32767; max (h1 - h0) = 100 raw units × scale factor ≤ 100 × 100 = 10000. Product ≤ 10000 × 32767 ≈ 3.3 × 10⁸ — within int32_t range (2.1 × 10⁹).
- Temperature: similar analysis holds.

**Note on t0_degc_x8 assembly in `init()`:**

Register 0x35 holds the two MSBs for both T0 and T1:
- T0 MSBs at bits [1:0]
- T1 MSBs at bits [3:2]

```c
uint8_t msb_reg;  /* read from 0x35 */
s_ht.cal.t0_degc_x8 = (int16_t)t0_raw | (int16_t)((msb_reg & 0x03) << 8);
s_ht.cal.t1_degc_x8 = (int16_t)t1_raw | (int16_t)((msb_reg & 0x0C) << 6);
```

### 4.3 LPS22HB — pressure reconstruction (barometer_driver.c)

Pressure is a 24-bit signed two's-complement value across three registers:
PRESS_OUT_XL (0x28), PRESS_OUT_L (0x29), PRESS_OUT_H (0x2A).
Unit: hPa × 4096.

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

Overflow check: max raw = 1260 × 4096 = 5,160,960. 5,160,960 × 10 =
51,609,600 — well within int32_t. No overflow.

Temperature from LPS22HB (TEMP_OUT_L 0x2B, TEMP_OUT_H 0x2C):

```c
int16_t t_raw;  /* signed 16-bit, unit = °C × 100 */
out->temperature_x100 = (int32_t)t_raw;  /* already ×100, no conversion needed */
```

### 4.4 STATUS_REG polling strategy

Both sensors run in continuous mode at ODR = 1 Hz (matching the 1 Hz
default SensorTask period). Before reading output registers, the driver
checks the data-ready bit in STATUS_REG.

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

Rationale: at 1 Hz ODR and a 1 Hz SensorTask tick, data is virtually
always ready when polled. The 2 ms retry covers minor clock-edge jitter
without blocking SensorTask for a full ODR period.

---

### Principles applied

- **P1 (Strict directional layering).** Both drivers depend only on II2c (driver layer); no upward dependency on middleware or application.
- **P2 (Dependency Inversion).** Each sensor exposes its own vtable: `ihumidity_temp_t` and `ibarometer_t`; SensorService depends on the interfaces.
- **P3 (Interface Segregation).** Two separate interfaces rather than one unified sensor interface, because the consumer sets are distinct — each sensor is read independently by SensorService GW.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static state per driver; no dynamic allocation at any point.
- **P6 (Responsibility traces to requirements).** Measurement functions trace directly to REQ-SA-* sensor sampling requirements.
- **P8 (Total error propagation, no silent failures).** All read functions return a typed `_err_t`; I2C errors are propagated rather than swallowed.
- **P9 (BARR-C coding standard).** Temperature and humidity returned as fixed-point integers; no floating-point leaks from the driver layer.
- **P10 (Naming conventions).** Prefixes `humidity_temp_` / `barometer_`; errors `HUMIDITY_TEMP_ERR_*` / `BAROMETER_ERR_*`.


## 4. Hardware contract

### 5.1 HTS221

| Parameter | Value | Source |
|-----------|-------|--------|
| Bus | I2C2 | UM2153 §7.12.2 |
| 7-bit address | 0x5F | UM2153 Table 3 |
| Write address (8-bit) | 0xBE | UM2153 Table 3 |
| Read address (8-bit) | 0xBF | UM2153 Table 3 |
| DRDY GPIO | PD15 (EXTI15) | UM2153 Fig. 27 + Fig. 23 — verify against Appendix A (GPA-O2) |
| DRDY used at firmware level | No — STATUS_REG polling | Design decision GPA-D1 |
| Power-on ODR | Power-down (default) | HTS221 datasheet CTRL_REG1 |
| Configured ODR | 1 Hz (CTRL_REG1[1:0] = 01) | Matches SensorTask 1 Hz default |
| Auto-increment on I2C | Enabled by setting bit 7 of register address | Required for multi-byte reads |
| Supply voltage | 3.3 V | Board power rail |

**Registers written at init:**

| Register | Address | Value | Meaning |
|----------|---------|-------|---------|
| CTRL_REG1 | 0x20 | 0x83 | PD=1 (active), BDU=1, ODR=1 Hz |
| AV_CONF | 0xC0 | 0x1B | Default averaging (AVGH=32, AVGT=16) |

**Calibration registers read at init (auto-increment burst 0x30–0x3F with address | 0x80):**

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

### 5.2 LPS22HB

| Parameter | Value | Source |
|-----------|-------|--------|
| Bus | I2C2 | UM2153 §7.12.5 |
| 7-bit address | 0x5D | UM2153 Table 3 |
| Write address (8-bit) | 0xBA | UM2153 Table 3 |
| Read address (8-bit) | 0xBB | UM2153 Table 3 |
| INT_DRDY GPIO | PD10 (EXTI10) | UM2153 Fig. 27 + Fig. 23 — verify against Appendix A (GPA-O2) |
| INT_DRDY used at firmware level | No — STATUS_REG polling | Design decision GPA-D1 |
| Pressure range | 260–1260 hPa | UM2153 §7.12.5 |
| Power-on ODR | Power-down (default) | LPS22HB datasheet CTRL_REG1 |
| Configured ODR | 1 Hz (CTRL_REG1[4:2] = 001) | Matches SensorTask 1 Hz default |
| Block data update (BDU) | Enabled | Prevents reading mismatched high/low bytes |
| Supply voltage | 3.3 V | Board power rail |

**Registers written at init:**

| Register | Address | Value | Meaning |
|----------|---------|-------|---------|
| CTRL_REG1 | 0x10 | 0x12 | BDU=1, ODR=1 Hz |
| CTRL_REG2 | 0x11 | 0x00 | Normal mode (no one-shot, no reset) |

---

## 5. Sequence integration

Both drivers are called from SensorTask in the SD-01 nominal flow.

```
SensorTask (100 ms tick)
  → SensorService.read_all()
      → humidity_temp_read(&ht_data)   /* IHumidityTemp */
          → i2c_write_read(STATUS_REG)
          → [2 ms retry if not ready]
          → i2c_write_read(HUMIDITY_OUT_L | 0x80, 4 bytes)  /* auto-inc */
          → apply compensation → fill ht_data
      → barometer_read(&baro_data)     /* IBarometer */
          → i2c_write_read(STATUS_REG)
          → [2 ms retry if not ready]
          → i2c_write_read(PRESS_OUT_XL, 5 bytes)
          → reconstruct 24-bit pressure + 16-bit temp → fill baro_data
```

I2C bus serialisation: all four Group A + Group B sensor drivers share
I2C2 via I2cDriver. Caller serialises — I2cDriver has no internal mutex
(established convention). SensorTask calls all sensor reads sequentially
within a single task period; no concurrent I2C access occurs.

**Init sequence (pre-scheduler):**

```
humidity_temp_init()  — can be called in any order relative to barometer_init()
  → i2c_write(CTRL_REG1, 0x83)
  → i2c_write(AV_CONF, 0x1B)
  → i2c_write_read(0x30 | 0x80, calib_buf, 16)  /* burst-read calibration */
  → parse calib_buf → fill s_ht.cal
  → s_ht.ready = true

barometer_init()
  → i2c_write(CTRL_REG1, 0x12)
  → i2c_write(CTRL_REG2, 0x00)
  → s_baro.ready = true
```

`humidity_temp_init()` and `barometer_init()` are called from the board
init function before `vTaskStartScheduler()`. No two-phase init required.

---

## 6. Error and fault behaviour

| Condition | HumidityTempDriver response | BarometerDriver response |
|-----------|----------------------------|--------------------------|
| I2C bus error (NACK, timeout) | Return `HUMIDITY_TEMP_ERR_I2C` | Return `BAROMETER_ERR_I2C` |
| STATUS_REG data not ready after 2 ms retry | Return `HUMIDITY_TEMP_ERR_NOT_READY` | Return `BAROMETER_ERR_NOT_READY` |
| NULL `out` pointer | Return `HUMIDITY_TEMP_ERR_INVALID_ARG` | Return `BAROMETER_ERR_INVALID_ARG` |
| `read()` called before `init()` | Return `HUMIDITY_TEMP_ERR_I2C` (I2C will fail) — add ready-guard assertion in debug build | Same |

SensorService (caller) handles all non-OK returns per REQ-SA-080 (log
error code) and REQ-SA-060 (continue with available sensors). The driver
itself does not log; it returns the error code and lets the application
layer decide.

---

## 7. Unit-test plan

### HumidityTempDriver

| Test ID | Scenario | Mechanism | Expected result |
|---------|----------|-----------|-----------------|
| HT-T01 | Nominal read | Mock I2C returns valid STATUS_REG + known raw values; pre-loaded calibration constants | `humidity_temp_read()` returns `OK`; `temperature_x100` and `humidity_x100` match hand-calculated compensation |
| HT-T02 | I2C bus failure on STATUS_REG read | Mock I2C returns error | Returns `HUMIDITY_TEMP_ERR_I2C` |
| HT-T03 | STATUS_REG data-not-ready, clears on retry | Mock returns not-ready first, ready second | Returns `OK` after 2 ms retry |
| HT-T04 | STATUS_REG never ready | Mock always returns not-ready | Returns `HUMIDITY_TEMP_ERR_NOT_READY` |
| HT-T05 | NULL out pointer | Direct call | Returns `HUMIDITY_TEMP_ERR_INVALID_ARG` |
| HT-T06 | Calibration boundary values | Use H0_rH_x2 = H1_rH_x2 (degenerate, zero span) | No divide-by-zero; returns clamped or error |
| HT-T07 | Known compensation values | Use calibration constants from HTS221 datasheet application note example | Output matches documented example ±1 LSB |

### BarometerDriver

| Test ID | Scenario | Mechanism | Expected result |
|---------|----------|-----------|-----------------|
| BA-T01 | Nominal read | Mock I2C returns valid STATUS_REG + known XL/L/H bytes | `barometer_read()` returns `OK`; `pressure_x10` matches manual calculation |
| BA-T02 | Pressure sign extension (negative raw — unlikely in operation but possible) | Inject raw = 0xFFFFFF (−1 after sign-extend) | `pressure_x10` = −1 × 10 / 4096 = 0 (rounded) — verifies sign-extension |
| BA-T03 | I2C bus failure | Mock returns error | Returns `BAROMETER_ERR_I2C` |
| BA-T04 | STATUS_REG not ready | Mock always not-ready | Returns `BAROMETER_ERR_NOT_READY` |
| BA-T05 | NULL out pointer | Direct call | Returns `BAROMETER_ERR_INVALID_ARG` |
| BA-T06 | Max pressure boundary | raw = 1260 × 4096 | `pressure_x10` = 12600; no overflow |

All tests run on host (PC) using Unity. Mock I2C: `#define I2C_INSTANCE (&mock_i2c)` per established convention.

---

## 8. Open items

### Decisions

| ID | Decision | Rationale |
|----|----------|-----------|
| GPA-D1 | No DRDY/INT ISR for HTS221 or LPS22HB | SensorTask polls at 1 Hz; no real-time benefit to ISR-driven read at this rate. STATUS_REG check adds < 100 µs overhead. Simplifies design by eliminating callback registration and EXTI configuration for these two sensors. |
| GPA-D2 | Continuous mode ODR = 1 Hz for both sensors | Matches default 1 Hz SensorTask period. Power consumption negligible vs ODR alternatives. |
| GPA-D3 | BDU (block data update) enabled on LPS22HB | Prevents reading the high byte of a new conversion while still reading the low byte of the previous one. Mandatory for correct 24-bit reconstruction. |
| GPA-D4 | HTS221 calibration read as burst (auto-increment) | Reduces I2C transactions from 16 individual reads to 1 burst. Auto-increment enabled via address bit 7. |
| GPA-D5 | Integer-only compensation for HTS221 | No float. Verified intermediate products fit in int32_t. |

### Open items

| ID | Item | Owner | Resolution path |
|----|------|-------|-----------------|
| GPA-O1 | `components.md` lists GpioDriver as a dependency of both HumidityTempDriver and BarometerDriver. GpioDriver is not called by the polling-only design. | Luca | Raise as SRS/HLD fix: update `components.md` USES list for both components to remove GpioDriver. Commit as `docs: remove spurious GpioDriver dependency from HTS221/LPS22HB drivers`. |
| GPA-O2 | DRDY GPIO pin assignments (HTS221 → PD15 / EXTI15, LPS22HB → PD10 / EXTI10) read from schematic Fig. 27 and Fig. 23. Not cross-checked against Appendix A I/O assignment table. | Luca | Verify against UM2153 Appendix A before LLD coding. Although these pins are not used at firmware level, the assignment must be documented correctly for completeness. |
| GPA-O3 | I2C2 TIMINGR value (clock speed, rise/fall times for L475) is a cross-cutting open item from i2c-driver.md (DUART-O2 root). | Luca | Resolve in clock-config.md companion. |
| GPA-O4 | HTS221 AV_CONF register (averaging configuration) value 0x1B is the default from the datasheet. Optimal averaging for 1 Hz ODR should be confirmed against the datasheet noise/latency tradeoff table. | Luca | Verify at LLD coding stage. Value is safe as written. |

---

*This document is the LLD companion for HumidityTempDriver and BarometerDriver.
It is authored by Luca Agrippino and reviewed by the project mentor.*
