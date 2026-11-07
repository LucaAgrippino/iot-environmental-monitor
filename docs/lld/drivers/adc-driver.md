# AdcDriver — LLD Companion

**Document:** `docs/lld/drivers/adc-driver.md`
**Version:** 1.0
**Board scope:** Gateway (B-L475E-IOT01A) and Field Device (STM32F469I-DISCO)
**Layer:** Driver
**Status:** Ready for implementation (this document closes the design gate HM-D13 of the HealthMonitor)
**Date:** November 2026

**HLD anchor:** AdcDriver, driver layer, both boards. The module is new; `components.md` does not list it yet (open item ADC-O1).

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Reads the MCU's internal temperature sensor and returns it in tenths of a degree Celsius | REQ-CC-010, REQ-LD-070 (MCU temperature); `health-monitor-lld.md` HM-D13 |
| PROVIDES (upward) | `adc_init`, `adc_read_mcu_temp_deci_c` (direct API, no vtable: one MCU, one ADC, one consumer) | this document |
| USES (downward) | CMSIS only (`ADC1`, ADC common registers, `RCC`; Gateway also `RCC->CCIPR`) | `hld.md` section 9 |
| Consumers | The housekeeping loop of each board's `main.c`, which passes the value to `health_monitor_update_resources` | `health-monitor-lld.md` |

**Why not a vtable.** Nothing above the driver needs to substitute it, and the only caller is the loop that feeds HealthMonitor. HealthMonitor itself takes a plain number, so it stays free of the ADC.

**Why `adc` and not `temperature_sensor`.** The peripheral is the ADC; a later consumer (battery voltage on the Field Device's VBAT channel, for example) adds a function to the same driver without a new module. Same name and directory on both boards (`drivers/adc/adc.c`, `adc.h`), like CpuDriver.

## 2. Public API (`adc.h`)

```c
typedef enum
{
    ADC_OK = 0,
    ADC_ERR_NOT_INIT = 1, /**< adc_init() has not succeeded. */
    ADC_ERR_NULL = 2,     /**< A required pointer is NULL. */
    ADC_ERR_TIMEOUT = 3   /**< The ADC did not become ready or finish a conversion in time. */
} adc_err_t;

adc_err_t adc_init(void);
adc_err_t adc_read_mcu_temp_deci_c(int16_t *deci_c);
```

| Function | Behaviour | Threading |
|---|---|---|
| `adc_init` | Enables the clock, powers and (Gateway) calibrates the ADC, enables the temperature sensor. Idempotent. | Once, before the scheduler or from the housekeeping task |
| `adc_read_mcu_temp_deci_c` | One conversion of the sensor, converted to tenths of a degree (`512` is 51.2 degC). Blocks for the sample time plus the conversion (about 50 us) in a bounded poll. | One task only; not ISR-safe; no lock |

Fixed-point result, no floating point (principle P9). Range of the type: -3276.8 to +3276.7 degC.

## 3. Internal design

### 3.1 Conversion

Both boards read the sensor with the longest sample time, because the sensor's source impedance is high and the datasheets ask for at least 10 us (F469) and 5 us (L475).

**Gateway (STM32L475).** The sensor is factory-calibrated at VDDA = 3.0 V, and the board runs at 3.3 V, so the supply is measured first with VREFINT:

```text
vdda_mv  = 3000 * VREFINT_CAL / raw_vrefint
ts_data  = raw_ts * vdda_mv / 3000                       (the reading as if VDDA were 3.0 V)
T (0.1 degC) = 1000 * (ts_data - TS_CAL1) / (TS_CAL2 - TS_CAL1) + 300
```

`TS_CAL1` (30 degC), `TS_CAL2` (130 degC) and `VREFINT_CAL` are 16-bit values in system memory at
`0x1FFF75A8`, `0x1FFF75CA` and `0x1FFF75AA` (DS11929 section "Temperature sensor calibration"; RM0351
section 16.4.32). All arithmetic is integer, with 32-bit intermediates sized so `raw * 3300` cannot
overflow.

**Field Device (STM32F469).** The F469 has no factory calibration. The datasheet's typical values are
used with the board's 3.3 V supply:

```text
mv  = raw * 3300 / 4095
T (0.1 degC) = (mv - 760) * 4 + 250                      (V25 = 760 mV, Avg_Slope = 2.5 mV/degC)
```

Accuracy: the F469 datasheet allows an offset of several degrees between parts, so the Field Device value
is indicative (trend and gross over-temperature), not a measurement. The Gateway value is within the
calibrated accuracy of the part (about 2 degC). This difference is recorded in the open item ADC-O2.

The two conversions are separate pure functions, `adc_mcu_temp_from_raw_*`, so the host tests check
them against known points without any register.

### 3.2 Sequence

| Step | Gateway | Field Device |
|---|---|---|
| Clock | `RCC->AHB2ENR.ADCEN`; ADC clock = SYSCLK (`RCC->CCIPR.ADCSEL = 3`), common prescaler /4 (20 MHz) | `RCC->APB2ENR.ADC1EN`; common prescaler /4 of PCLK2 (22.5 MHz) |
| Power | leave deep power-down, enable the regulator, wait 20 us, calibrate (`ADCAL`), enable (`ADEN`, wait `ADRDY`) | `CR2.ADON` |
| Sensor | `CCR.VREFEN` and `CCR.TSEN` | `CCR.TSVREFE` (and `VBATE` clear: channel 18 is the sensor, not VBAT) |
| Convert | `SQR1`: one conversion, channel 0 (VREFINT) then channel 17 (temperature); `SMPR` 640.5 cycles; `CR.ADSTART`, wait `ISR.EOC`, read `DR` | `SQR3`: channel 18; `SMPR1` 480 cycles; `CR2.SWSTART`, wait `SR.EOC`, read `DR` |

Every wait is a bounded loop (`ADC_POLL_LIMIT` iterations) that returns `ADC_ERR_TIMEOUT`.

### 3.3 Principles

One responsibility; no hidden state beyond one `initialised` flag; every wait bounded; no floating
point; no RTOS call.

## 4. Hardware contract

| Item | Gateway | Field Device |
|---|---|---|
| Peripheral | ADC1 (`0x50040000`), common `0x50040300` | ADC1 (`0x40012000`), common `0x40012300` |
| Channel | temperature 17, VREFINT 0 | temperature 18 (shared with VBAT, selected by `CCR.VBATE`) |
| Reference | RM0351 section 16 | RM0386 section 13 |
| Calibration data | system memory `0x1FFF75A8/AA/CA` | none |
| Pins | none (internal channels) | none |
| NVIC | none (polled) | none |

## 5. Sequence integration

| Message | Producer | Effect |
|---|---|---|
| `adc_init()` | `main()` before the scheduler | ADC ready |
| `adc_read_mcu_temp_deci_c()` | housekeeping loop, once a second (Field Device) or the 5 s report task (Gateway) | Value for `health_monitor_update_resources` |

## 6. Error and fault behaviour

| Failure | Result | Consumer policy |
|---|---|---|
| `adc_init` never called | `ADC_ERR_NOT_INIT` | the caller skips the update; HealthMonitor keeps `mcu_temp_valid = false` |
| ADC does not become ready or finish | `ADC_ERR_TIMEOUT` | same; the next period retries |
| NULL output pointer | `ADC_ERR_NULL` | none |
| Gateway: `raw_vrefint` of 0 | `ADC_ERR_TIMEOUT` (a dead reference is a failed conversion) | same |

## 7. Unit-test plan

| ID | Case |
|---|---|
| T-ADC-01 | Gateway conversion: the two calibration points give 30.0 and 130.0 degC; a reading in between interpolates; VDDA of 3.3 V is corrected |
| T-ADC-02 | Field Device conversion: 760 mV gives 25.0 degC; +25 mV gives 35.0 degC; below 0 degC is negative |
| T-ADC-03 | `adc_init` enables the clock and the sensor bit, is idempotent, and times out when the ADC never becomes ready |
| T-ADC-04 | `adc_read_mcu_temp_deci_c` before init returns `ADC_ERR_NOT_INIT`; with a NULL pointer returns `ADC_ERR_NULL` |
| T-ADC-05 | A conversion that never completes returns `ADC_ERR_TIMEOUT` |
| T-ADC-06 | A completed conversion returns `ADC_OK` and the converted value |

Mock strategy: the ADC, common-ADC and RCC registers of the CMSIS mock; `EOC` and `ADRDY` are set by the
test. Hardware: `integration-tests/adc/main_test_adc.c` on each board prints ten readings and checks that
they are between 0 and 85 degC and stable within 2 degC; the Gateway also prints the measured VDDA
(expected 3.0 to 3.6 V). TC-HW-ADC-001 and 002.

## 8. Open items

| ID | Item | Status |
|---|---|---|
| ADC-O1 | `components.md` does not list AdcDriver | Open: add on the next HLD revision |
| ADC-O2 | The Field Device sensor is uncalibrated (several degrees of part-to-part offset) | Accepted; calibrate against a reference at 25 degC if a precise value is ever needed |
| ADC-O3 | A second consumer (VBAT, VREFINT on the Field Device) would need a function each | Closed as not needed now |

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| ADC-D1 | One driver for both boards, same names | like CpuDriver and WatchdogDriver |
| ADC-D2 | Polled single conversions, no DMA or interrupt | one reading a second |
| ADC-D3 | Gateway measures VDDA with VREFINT every read | the 3.0 V calibration would otherwise read about 10 degC off at 3.3 V |
| ADC-D4 | Result in tenths of a degree in an `int16_t` | matches the display format and avoids floating point |
