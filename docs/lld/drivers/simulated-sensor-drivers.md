# BarometerDriver and HumidityTempDriver — LLD Companion (Field Device, Simulated)

**Document:** `docs/lld/drivers/simulated-sensor-drivers.md`
**Version:** 0.1 (draft — pending Luca review)
**Board scope:** Field Device (STM32F469) only
**Layer:** Driver
**Status:** Draft

---

## 1. Sources

Both drivers are covered in one companion because they are structurally identical: simulated, no hardware dependency, same consumer, same task context, same error model.

| Attribute | BarometerDriver | HumidityTempDriver |
|---|---|---|
| Responsibility | Provides ambient pressure readings (simulated) | Provides humidity and temperature readings (simulated) |
| PROVIDES (upward) | `IBarometer` | `IHumidityTemp` |
| USES (downward) | *(none)* | *(none)* |
| Root requirement | REQ-SA-030 | REQ-SA-030 |

**Note on USES (downward):** `components.md` explicitly records these as having no downward dependency — the simulation is self-contained inside the driver. This is the correct model per Vision §5.1.1: *"Replacing the simulation with physical sensors requires changes only in the driver layer."* The `I2cDriver` and `GpioDriver` used by the Gateway equivalents are entirely absent here.

**Consumer (both drivers):** `SensorService` (Application, FD), called from `SensorTask` (priority 3, task-breakdown.md §4.2). Single consumer, single task — no concurrency concern.

**Key architectural purpose (Vision §5.1.1):** these drivers prove that `SensorService` is hardware-agnostic. They implement the same interfaces (`IBarometer`, `IHumidityTemp`) as the real Gateway sensor drivers. `SensorService` never knows it is talking to a simulation; the abstraction boundary is the proof.

**Physical value ranges (from `modbus-register-map.md` §6.2):**

| Sensor | Scale | Unit | Physical range |
|---|---|---|---|
| Temperature | ×0.01 | °C | −40.00 to +85.00 (int32: −4000 to 8500) |
| Humidity | ×0.01 | %RH | 0.00 to 100.00 (uint32: 0 to 10000) |
| Pressure | ×0.1 | hPa | 300.0 to 1100.0 (int32: 3000 to 11000) |

The simulation must produce values within these ranges. Values outside the range would be clamped by `SensorService` (REQ-SA-130), but it is cleaner to stay within bounds at the source.

**Sequence diagram (SD-01):** both drivers appear as explicit lifelines. Messages 2–3 (`HumidityTempDriver`) and messages 4–5 (`BarometerDriver`) are synchronous calls from `SensorService` that may return either a reading or an error code. The alt fragment (messages 3', 3'', 3''', 5', 5'', 5''') exercises the failure path — this is exercisable via fault injection without any hardware modification.

---

## 2. Public API

### 2.1 Dependency-conformance check

Neither header includes CMSIS, FreeRTOS, or any other driver header. The only standard include is `stdint.h` and `stdbool.h`. Confirmed clean — no downward dependencies, consistent with `components.md`.

### 2.2 P3 consideration

Single consumer for each driver. No split warranted.

### 2.3 Data types — BarometerDriver (`barometer_driver.h`)

```c
/**
 * @brief Error codes for BarometerDriver.
 * Naming follows the cross-cutting convention in lld.md §3.2.
 */
typedef enum {
    BARO_ERR_OK    = 0, /**< Reading produced successfully. */
    BARO_ERR_FAULT = 1, /**< Fault injection active; simulate sensor failure. */
} baro_err_t;

/**
 * @brief A single pressure sample.
 *
 * pressure_x10: atmospheric pressure in units of 0.1 hPa.
 * Example: 10132 → 1013.2 hPa (standard sea-level pressure).
 * Valid range: 3000..11000 (300.0..1100.0 hPa).
 * Matches the PRESSURE register encoding in modbus-register-map.md §6.2.
 */
typedef struct {
    int32_t pressure_x10;
} baro_reading_t;
```

### 2.4 Public API — BarometerDriver

```c
/**
 * @brief Initialise the barometer simulation module.
 *
 * Seeds the simulation with the default pressure (REQ-SA-030).
 * Called from SensorTask startup prologue (BringingUpSensors Init sub-step).
 *
 * @return BARO_ERR_OK always (simulation init cannot fail unless fault
 *         injection is pre-armed — see barometer_inject_fault()).
 */
baro_err_t barometer_init(void);

/**
 * @brief Read one pressure sample from the simulation.
 *
 * Advances the simulation by one step (random walk, §3.2).
 * Returns BARO_ERR_FAULT if fault injection is active; in that case
 * reading is unchanged and SensorService marks the sample invalid
 * (REQ-SA-0E1).
 *
 * @param[out] reading  Populated on BARO_ERR_OK. Must not be NULL.
 * @return BARO_ERR_OK or BARO_ERR_FAULT.
 */
baro_err_t barometer_read(baro_reading_t *reading);

/**
 * @brief Arm or disarm fault injection.
 *
 * When inject = true, all subsequent barometer_read() calls return
 * BARO_ERR_FAULT. When inject = false, normal simulation resumes.
 *
 * This function exists to exercise the SensorService error-handling
 * path (SD-01 alt fragment) without physical hardware. It is called
 * from the CLI self-test command (REQ-LI-130) and from unit tests.
 *
 * Not part of IBarometer — callers above the driver layer do not use it.
 * Only the test harness and CLI service call it directly.
 *
 * @param inject  true to inject faults; false to resume normal operation.
 */
void barometer_inject_fault(bool inject);
```

### 2.5 Data types — HumidityTempDriver (`humidity_temp_driver.h`)

```c
/**
 * @brief Error codes for HumidityTempDriver.
 */
typedef enum {
    HT_ERR_OK    = 0, /**< Reading produced successfully. */
    HT_ERR_FAULT = 1, /**< Fault injection active. */
} ht_err_t;

/**
 * @brief A single combined temperature and humidity sample.
 *
 * temperature_x100: temperature in units of 0.01 °C.
 *   Example: 2350 → 23.50°C. Valid range: −4000..8500.
 *   Matches TEMPERATURE register encoding (modbus-register-map.md §6.2).
 *
 * humidity_x100: relative humidity in units of 0.01 %RH.
 *   Example: 5500 → 55.00%RH. Valid range: 0..10000.
 *   Matches HUMIDITY register encoding.
 */
typedef struct {
    int32_t  temperature_x100;
    uint32_t humidity_x100;
} ht_reading_t;
```

### 2.6 Public API — HumidityTempDriver

```c
/**
 * @brief Initialise the temperature and humidity simulation module.
 *
 * Seeds both simulations with default values (REQ-SA-030).
 *
 * @return HT_ERR_OK always.
 */
ht_err_t humidity_temp_init(void);

/**
 * @brief Read one combined temperature and humidity sample.
 *
 * Advances both simulations by one step.
 * Returns HT_ERR_FAULT if fault injection is active.
 *
 * @param[out] reading  Populated on HT_ERR_OK. Must not be NULL.
 * @return HT_ERR_OK or HT_ERR_FAULT.
 */
ht_err_t humidity_temp_read(ht_reading_t *reading);

/**
 * @brief Arm or disarm fault injection (same semantics as barometer_inject_fault).
 */
void humidity_temp_inject_fault(bool inject);
```

---

## 3. Internal design

### 3.1 Module-level state

**barometer_driver.c:**
```c
static int32_t s_pressure_x10  = 10132;   /* default: 1013.2 hPa */
static bool    s_fault_injected = false;
```

**humidity_temp_driver.c:**
```c
static int32_t  s_temperature_x100 = 2200;  /* default: 22.00°C */
static uint32_t s_humidity_x100    = 5000;  /* default: 50.00%RH */
static bool     s_fault_injected   = false;
```

No dynamic allocation. All state is module-level static (REQ-NF-408).

### 3.2 Simulation algorithm — bounded random walk

Each `_read()` call advances the simulation by one step. The algorithm is a simple bounded random walk:

```c
/* Example for pressure (same pattern for temperature and humidity) */
static int32_t random_delta(void)
{
    /* Returns a value in [-2, +2] using stdlib rand().
     * This is a coarse approximation, sufficient for simulation. */
    return (int32_t)(rand() % 5) - 2;
}

baro_err_t barometer_read(baro_reading_t *reading)
{
    if (s_fault_injected) {
        return BARO_ERR_FAULT;
    }
    s_pressure_x10 += random_delta();
    /* Clamp to physical range */
    if (s_pressure_x10 < 3000)  { s_pressure_x10 = 3000;  }
    if (s_pressure_x10 > 11000) { s_pressure_x10 = 11000; }
    reading->pressure_x10 = s_pressure_x10;
    return BARO_ERR_OK;
}
```

The algorithm is intentionally simple. The portfolio value is in the architecture — not in the simulation fidelity. REQ-SA-030 says "initialise... with default parameters" and Vision §5.1.1 says "produces realistic, time-varying values". A bounded random walk satisfies both without importing floating-point arithmetic or a signal generation library.

`rand()` from `<stdlib.h>` is used without a custom seed. On reset, `rand()` produces the same sequence — this is acceptable for a simulation; it means the device always produces the same reading pattern per boot, which is actually useful for deterministic testing.

### 3.3 No ISR, no DMA, no callbacks

Purely synchronous. `SensorTask` calls both drivers on the same thread every 1 Hz polling cycle. Total compute time per cycle: negligible (a handful of arithmetic operations).

### 3.4 `inject_fault` visibility

`barometer_inject_fault()` and `humidity_temp_inject_fault()` are declared in the respective headers but are NOT part of `IBarometer` / `IHumidityTemp`. They are direct calls made only by the CLI service (REQ-LI-130, board self-test) and by the test harness. `SensorService` never calls them. This preserves P1: `SensorService` only sees the reading interface.

---

## 4. Hardware contract

None. These drivers have no hardware peripheral. There is no pin to configure, no register to write, no clock to enable. This is the point — the hardware contract for a simulated sensor is the simulation model itself (§3.2), not a physical device.

---

## 5. Sequence integration

Both drivers appear as lifelines in **SD-01** (Sensor acquisition cycle, FD). No changes to `sequence-diagrams.md` are required — the existing messages (2–5 and their alt variants) correctly describe the driver behaviour, including the fault path.

The fault injection path (3', 5') is the most valuable part of this driver from a verification perspective: it makes the SD-01 alt fragment exercisable on a host machine without any hardware.

---

## 6. Error and fault behaviour

On `BARO_ERR_FAULT` or `HT_ERR_FAULT`, `SensorService`:

1. Logs the error via `Logger` (REQ-SA-080).
2. Marks the sample invalid (REQ-SA-0E1).
3. Continues the cycle with the remaining sensor (REQ-SA-060).
4. Posts the invalid reading downstream with the error flag set (REQ-SA-160).

The drivers have no further obligation on error — they return the error code and leave state unchanged.

---

## 7. Unit-test plan

Both drivers are pure software — all tests run on the host without any mock register banks.

**BarometerDriver:**

| ID | Test case | Expected result |
|---|---|---|
| T-BARO-01 | `barometer_init`: verify s_pressure_x10 = 10132, fault flag = false | Module state matches defaults |
| T-BARO-02 | `barometer_read` called 100 times: verify output always in [3000, 11000] | No out-of-range values produced |
| T-BARO-03 | `barometer_read` after `barometer_inject_fault(true)`: returns BARO_ERR_FAULT | reading unchanged; error returned |
| T-BARO-04 | `barometer_inject_fault(false)` after fault: next read returns BARO_ERR_OK | Simulation resumes |
| T-BARO-05 | `barometer_read` with NULL reading pointer | Returns BARO_ERR_OK or defined behaviour — document choice |
| T-BARO-06 | Simulate boundary: force s_pressure_x10 to 10999, call read: verify not > 11000 | Clamping works at upper bound |
| T-BARO-07 | Simulate boundary: force s_pressure_x10 to 3001, call read: verify not < 3000 | Clamping works at lower bound |

**HumidityTempDriver:**

| ID | Test case | Expected result |
|---|---|---|
| T-HT-01 | `humidity_temp_init`: verify defaults | temperature_x100 = 2200, humidity_x100 = 5000, fault = false |
| T-HT-02 | `humidity_temp_read` 100 times: temperature in [−4000, 8500], humidity in [0, 10000] | Both always in range |
| T-HT-03 | `humidity_temp_read` after inject_fault(true) | Returns HT_ERR_FAULT; reading unchanged |
| T-HT-04 | inject_fault(false): simulation resumes | Next read returns HT_ERR_OK |
| T-HT-05 | Both simulations advance independently per read | temperature and humidity move on the same call; neither lags the other |

Test files: `tests/drivers/test_barometer_driver.c` and `tests/drivers/test_humidity_temp_driver.c`.

---

## 8. Open items

| ID | Item | Owner | Resolution path |
|---|---|---|---|
| SIMD-O1 | `rand()` seeding. Currently unseeded → same sequence every boot. If deterministic simulation is undesirable (e.g., for integration testing variety), seed with `srand(uptime_ms)` at init. Decide before implementation. | Luca | Decide at implementation; document in code |
| SIMD-O2 | NULL pointer handling in `_read()` functions. The API documents the pointer as "must not be NULL" — decide whether to add a defensive NULL check (returns error) or rely on caller discipline (assert in DEBUG builds). | Luca | Consistent with GpioDriver decision — recommend DEBUG assert |

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| SIMD-D1 | Combined companion for both drivers | Identical structure; one document eliminates duplication and is easier to maintain |
| SIMD-D2 | Bounded random walk simulation | Simple, deterministic, requires no floating-point or signal library. Portfolio value is in architecture, not simulation fidelity |
| SIMD-D3 | `inject_fault()` not part of `IBarometer`/`IHumidityTemp` | Preserves interface purity; SensorService is unaware of fault injection; the CLI and test harness call it directly through the concrete module |
| SIMD-D4 | Fixed-point integers matching Modbus register scale | Consistent with the data model downstream; avoids float-to-int conversion and rounding errors in SensorService; the Modbus register values are directly derivable from the driver output |
| SIMD-D5 | USES *(none)* — no downward dependency | Simulation is self-contained; this is the architecture's proof-of-concept: the same interface works whether backed by a simulation or a physical I2C device |
| SIMD-D6 | Singleton modules (no handles) | One simulated sensor per type per board; consistent with all prior companions |
