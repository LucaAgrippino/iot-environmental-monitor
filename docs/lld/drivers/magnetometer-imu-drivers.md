# LLD Companion — Gateway Sensor Drivers Group B
## MagnetometerDriver (LIS3MDL) + ImuDriver (LSM6DSL)

**Board:** Gateway (B-L475E-IOT01A)  
**Branch:** `feature/lld-gw-sensor-drivers-group-b`  
**Status:** Draft  
**Methodology:** lld-methodology.md v1.1, steps 1–8  

---

## 1. Scope

This document covers two Gateway driver components that share I2C2 and use
the DRDY/INT GPIO interrupt line to notify SensorTask when data is ready.
This distinguishes them from Group A drivers, which use STATUS_REG polling
only.

| Component | Device | I2C address (7-bit) | PROVIDES | Root req |
|-----------|--------|---------------------|----------|----------|
| MagnetometerDriver | LIS3MDL | 0x1E | IMagnetometer | REQ-SA-031 |
| ImuDriver | LSM6DSL | 0x6A | IImu | REQ-SA-071 |

Both sensors are Gateway-only. No platform-split `.c` files required.

**Key design difference from Group A:** these drivers use a two-phase init
pattern. Phase 1 configures the sensor and the EXTI GPIO pin but does not
enable the interrupt. Phase 2 (post-scheduler) registers a callback and
enables the EXTI interrupt. This is required because the callback stores a
task handle, which only exists after the scheduler starts.

---

## 2. Source references (Step 1)

| Source | Relevant section |
|--------|-----------------|
| `components.md` — Gateway driver layer | MagnetometerDriver, ImuDriver responsibility sentences |
| `SRS.md` | REQ-SA-031 (magnetometer init/read), REQ-SA-071 (IMU init/read), REQ-SA-040/060/080 (error handling) |
| `task-breakdown.md` §5.2, §6.1 | SensorTask priority 5, 512 words; ISR inventory (EXTI lines not yet listed — GPB-O1) |
| `sequence-diagrams.md` SD-01 | Sensor read nominal flow |
| UM2153 §7.12.3 | LIS3MDL features, I2C2 |
| UM2153 §7.12.4 | LSM6DSL features, I2C2 |
| UM2153 Table 3 | I2C addresses confirmed |
| UM2153 Appendix B, Fig. 27 | DRDY/INT GPIO connections to EXTI lines |
| UM2153 Appendix B, Fig. 23 | MCU pin labels for EXTI lines |
| LIS3MDL datasheet | CTRL_REG registers, DRDY pin config, output registers, sensitivity |
| LSM6DSL datasheet | CTRL_REG registers, INT1_CTRL, output registers, sensitivity |

---

## 3. API — Step 2 (API + dependency-conformance check + P3 review)

### 3.1 MagnetometerDriver — IMagnetometer

```c
/* magnetometer_driver.h */

#ifndef MAGNETOMETER_DRIVER_H
#define MAGNETOMETER_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    MAGNETOMETER_ERR_OK          = 0,
    MAGNETOMETER_ERR_I2C         = 1,
    MAGNETOMETER_ERR_NOT_READY   = 2,  /* STATUS_REG data not ready within timeout */
    MAGNETOMETER_ERR_INVALID_ARG = 3
} magnetometer_err_t;

/*
 * Raw 16-bit per axis. Sensitivity: 6842 LSB/gauss at ±4 gauss full scale.
 * SensorService applies the sensitivity factor when building cloud telemetry.
 */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} magnetometer_data_t;

/* Callback type invoked from EXTI ISR context. Must be ISR-safe. */
typedef void (*magnetometer_drdy_cb_t)(void *ctx);

/* Phase 1 — call before vTaskStartScheduler() */
magnetometer_err_t magnetometer_init(void);

/* Phase 2 — call after vTaskStartScheduler(), from SensorTask init */
magnetometer_err_t magnetometer_attach_drdy_callback(magnetometer_drdy_cb_t cb,
                                                      void *ctx);

magnetometer_err_t magnetometer_read(magnetometer_data_t *out);

#endif /* MAGNETOMETER_DRIVER_H */
```

### 3.2 ImuDriver — IImu

```c
/* imu_driver.h */

#ifndef IMU_DRIVER_H
#define IMU_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    IMU_ERR_OK          = 0,
    IMU_ERR_I2C         = 1,
    IMU_ERR_NOT_READY   = 2,
    IMU_ERR_INVALID_ARG = 3
} imu_err_t;

/*
 * Raw 16-bit per axis.
 * Accelerometer sensitivity: 16384 LSB/g at ±2g full scale.
 * Gyroscope sensitivity:       131 LSB/dps at ±250 dps full scale.
 * SensorService applies the sensitivity factor when building cloud telemetry.
 */
typedef struct {
    int16_t accel_x;
    int16_t accel_y;
    int16_t accel_z;
    int16_t gyro_x;
    int16_t gyro_y;
    int16_t gyro_z;
} imu_data_t;

/* Callback type invoked from EXTI ISR context. Must be ISR-safe. */
typedef void (*imu_int1_cb_t)(void *ctx);

/* Phase 1 — call before vTaskStartScheduler() */
imu_err_t imu_init(void);

/* Phase 2 — call after vTaskStartScheduler(), from SensorTask init */
imu_err_t imu_attach_int1_callback(imu_int1_cb_t cb, void *ctx);

imu_err_t imu_read(imu_data_t *out);

#endif /* IMU_DRIVER_H */
```

**Output scale rationale:** neither LIS3MDL nor LSM6DSL data feeds a Modbus
register (those registers carry only temperature, humidity, and pressure).
IMU and magnetometer data is gateway-local, forwarded to cloud telemetry by
CloudPublisher. Raw LSB output with documented sensitivity is the correct
driver-layer contract; physical-unit conversion is an application concern
(P2 — layering).

### 3.3 Dependency-conformance check

| Dependency | In `components.md` | Actual usage |
|------------|-------------------|--------------|
| I2cDriver | Yes | Yes — all register accesses via `i2c_write_read()` |
| GpioDriver | Yes | Yes — DRDY/INT EXTI GPIO configuration in Phase 1 |

P3 (ISP): IMagnetometer and IImu are already segregated at HLD. No further
split needed.

---

## 4. Internal design (Step 3)

### 4.1 Module structure

```
magnetometer_driver.h   — public API (IMagnetometer)
magnetometer_driver.c   — singleton state, two-phase init, ISR handler, read
imu_driver.h            — public API (IImu)
imu_driver.c            — singleton state, two-phase init, ISR handler, read
```

Private state structs — static, no dynamic allocation:

```c
/* magnetometer_driver.c */
typedef struct {
    magnetometer_drdy_cb_t drdy_cb;
    void                  *drdy_ctx;
    bool                   ready;
} magnetometer_state_t;

static magnetometer_state_t s_mag;
```

```c
/* imu_driver.c */
typedef struct {
    imu_int1_cb_t int1_cb;
    void         *int1_ctx;
    bool          ready;
} imu_state_t;

static imu_state_t s_imu;
```

### 4.2 Two-phase init — detailed flow

**Phase 1 (`magnetometer_init()` / `imu_init()`) — pre-scheduler:**

1. Write sensor configuration registers (ODR, full scale, output mode).
2. Configure DRDY/INT GPIO pin as input via GpioDriver (falling-edge trigger,
   no pull-up — sensor drives the line actively).
3. Do NOT enable the EXTI interrupt line — no task handle exists yet.
4. Set `s_xxx.ready = false`.

**Phase 2 (`magnetometer_attach_drdy_callback()` / `imu_attach_int1_callback()`) — post-scheduler:**

1. Store callback pointer and context in module state.
2. Enable the EXTI interrupt line (unmask in EXTI_IMR, set priority in NVIC).
3. Return `ERR_INVALID_ARG` if `cb` is NULL.

**Why two phases (not one)?** The callback stores a task handle as `ctx`.
Task handles are only valid after `xTaskCreate()`, which must run before
`vTaskStartScheduler()`. However, EXTI interrupts must not fire until
the handler is registered — hence the split. This is the same rationale
as TouchscreenDriver and ModbusUartDriver.

### 4.3 ISR design

Each driver provides its own ISR handler function, registered in the
EXTI IRQ handler in `stm32l4xx_it.c`:

```c
/* In stm32l4xx_it.c */
void EXTI9_5_IRQHandler(void) {
    if (EXTI->PR1 & (1U << 8U)) {           /* LIS3MDL DRDY — EXTI8 */
        EXTI->PR1 = (1U << 8U);             /* clear pending bit     */
        magnetometer_drdy_irq_handler();
    }
}

void EXTI15_10_IRQHandler(void) {
    if (EXTI->PR1 & (1U << 11U)) {          /* LSM6DSL INT1 — EXTI11 */
        EXTI->PR1 = (1U << 11U);
        imu_int1_irq_handler();
    }
}
```

`magnetometer_drdy_irq_handler()` and `imu_int1_irq_handler()` are
internal (non-public) functions declared `static` in the `.c` file and
`extern` only where needed by `stm32l4xx_it.c`. They follow the ISR
contract from task-breakdown.md §6:

```c
/* magnetometer_driver.c — internal, not in public header */
void magnetometer_drdy_irq_handler(void) {
    /* 1. Interrupt already acknowledged in EXTI handler above */
    /* 2. No data to capture — data is in sensor output registers */
    /* 3. Notify via registered callback */
    if (s_mag.drdy_cb != NULL) {
        s_mag.drdy_cb(s_mag.drdy_ctx);
    }
    /* 4. Return — no state machine logic in ISR context */
}
```

The callback (registered by SensorTask) does:

```c
/* SensorTask init — example registration */
static void prv_mag_drdy_cb(void *ctx) {
    TaskHandle_t task = (TaskHandle_t)ctx;
    BaseType_t yield = pdFALSE;
    xTaskNotifyFromISR(task, SENSOR_TASK_MAG_DRDY_BIT, eSetBits, &yield);
    portYIELD_FROM_ISR(yield);
}
```

SensorTask uses `xTaskNotifyWaitBits()` on `SENSOR_TASK_MAG_DRDY_BIT` and
`SENSOR_TASK_IMU_INT1_BIT` inside its read cycle, with a timeout to guard
against missed interrupts (GPB-O2).

### 4.4 `_read()` function — STATUS_REG gate

`magnetometer_read()` and `imu_read()` check STATUS_REG before reading
output registers. This provides a double confirmation: the DRDY ISR has
already notified SensorTask, but STATUS_REG is the authoritative
"output registers are stable and complete" signal.

**LIS3MDL STATUS_REG (0x27):**
- Bit 3: ZYXDA — all axes data available. Read condition: bit 3 = 1.

**LSM6DSL STATUS_REG (0x1E):**
- Bit 1: GDA — gyroscope data available.
- Bit 0: XLDA — accelerometer data available.
- Read condition: both bits = 1.

If STATUS_REG is not set, the driver returns `ERR_NOT_READY`. At the
configured ODR (LIS3MDL 10 Hz, LSM6DSL 104 Hz) and SensorTask 10 Hz
read rate, this should never occur in practice.

### 4.5 LIS3MDL output register read

Six bytes via I2C auto-increment burst starting at OUT_X_L (0x28):

```
OUT_X_L (0x28), OUT_X_H (0x29),
OUT_Y_L (0x2A), OUT_Y_H (0x2B),
OUT_Z_L (0x2C), OUT_Z_H (0x2D)
```

Assembly: `out->x = (int16_t)((buf[1] << 8) | buf[0])` — LSB-first.

Auto-increment on LIS3MDL I2C: enabled by default (no special bit needed).

### 4.6 LSM6DSL output register read

Twelve bytes via I2C auto-increment burst starting at OUTX_L_G (0x22):

```
OUTX_L_G  (0x22), OUTX_H_G  (0x23),   /* gyro X  */
OUTY_L_G  (0x24), OUTY_H_G  (0x25),   /* gyro Y  */
OUTZ_L_G  (0x26), OUTZ_H_G  (0x27),   /* gyro Z  */
OUTX_L_XL (0x28), OUTX_H_XL (0x29),   /* accel X */
OUTY_L_XL (0x2A), OUTY_H_XL (0x2B),   /* accel Y */
OUTZ_L_XL (0x2C), OUTZ_H_XL (0x2D)    /* accel Z */
```

Assembly: same LSB-first signed int16_t pattern.

Auto-increment on LSM6DSL I2C: enabled by default.

---

## 5. Hardware contract (Step 4)

### 5.1 LIS3MDL

| Parameter | Value | Source |
|-----------|-------|--------|
| Bus | I2C2 | UM2153 §7.12.3 |
| 7-bit address | 0x1E | UM2153 Table 3 |
| Write address (8-bit) | 0x3C | UM2153 Table 3 |
| Read address (8-bit) | 0x3D | UM2153 Table 3 |
| DRDY GPIO | EXTI8 — MCU pin to verify (GPB-O3) | UM2153 Fig. 27 + Fig. 23 |
| DRDY polarity | Active high (rising edge triggers EXTI) | LIS3MDL datasheet |
| Full scale | ±4 gauss (default) | LIS3MDL datasheet CTRL_REG2 |
| Sensitivity | 6842 LSB/gauss | LIS3MDL datasheet Table 3 |
| Configured ODR | 10 Hz | CTRL_REG1: OM=11 (ultra-high perf), DO=100 |
| Supply voltage | 3.3 V | Board power rail |

**Registers written at init (Phase 1):**

| Register | Address | Value | Meaning |
|----------|---------|-------|---------|
| CTRL_REG1 | 0x20 | 0x7C | TEMP_EN=0, OM=11 (ultra-high perf), DO=100 (10 Hz) |
| CTRL_REG2 | 0x21 | 0x00 | FS=00 (±4 gauss), REBOOT=0, SOFT_RST=0 |
| CTRL_REG3 | 0x22 | 0x00 | Continuous conversion, no power-down |
| CTRL_REG4 | 0x23 | 0x0C | OMZ=11 (ultra-high perf Z), BLE=0 (LSB first) |
| CTRL_REG5 | 0x24 | 0x40 | BDU=1 (block data update) |
| INT_CFG   | 0x30 | 0x00 | Interrupt generation disabled; DRDY used for data-ready only |

DRDY pin behaviour: the LIS3MDL asserts DRDY high when new data is
available in output registers. The EXTI is configured for rising-edge
trigger.

### 5.2 LSM6DSL

| Parameter | Value | Source |
|-----------|-------|--------|
| Bus | I2C2 | UM2153 §7.12.4 |
| 7-bit address | 0x6A | UM2153 Table 3 |
| Write address (8-bit) | 0xD4 | UM2153 Table 3 |
| Read address (8-bit) | 0xD5 | UM2153 Table 3 |
| INT1 GPIO | EXTI11 — MCU pin to verify (GPB-O3) | UM2153 Fig. 27 + Fig. 23 |
| INT1 polarity | Active high (rising edge triggers EXTI) | LSM6DSL datasheet |
| Accel full scale | ±2g (default) | LSM6DSL CTRL1_XL |
| Accel sensitivity | 16384 LSB/g | LSM6DSL datasheet Table 3 |
| Gyro full scale | ±250 dps (default) | LSM6DSL CTRL2_G |
| Gyro sensitivity | 131 LSB/dps | LSM6DSL datasheet Table 3 |
| Configured accel ODR | 104 Hz | CTRL1_XL ODR_XL = 0100 |
| Configured gyro ODR | 104 Hz | CTRL2_G ODR_G = 0100 |
| Supply voltage | 3.3 V | Board power rail |

104 Hz ODR chosen so that at the 10 Hz effective SensorTask read rate,
data is always ready before `imu_read()` is called. The 10× margin
eliminates practical `ERR_NOT_READY` conditions.

**Registers written at init (Phase 1):**

| Register | Address | Value | Meaning |
|----------|---------|-------|---------|
| CTRL1_XL | 0x10 | 0x40 | ODR_XL=0100 (104 Hz), FS_XL=00 (±2g), LPF1_BW_SEL=0 |
| CTRL2_G  | 0x11 | 0x40 | ODR_G=0100 (104 Hz), FS_G=00 (±250 dps) |
| CTRL3_C  | 0x12 | 0x44 | BDU=1, IF_INC=1 (auto-increment on I2C), H_LACTIVE=0 |
| INT1_CTRL| 0x0D | 0x03 | INT1_DRDY_G=1, INT1_DRDY_XL=1 (both routed to INT1) |

---

## 6. Sequence integration (Step 5)

### Nominal read flow (per SensorTask 100 ms cycle)

The DRDY/INT1 notification arrives before or shortly after SensorTask
wakes from its periodic tick, given the ODR >> 10 Hz for both sensors.

```
SensorTask (100 ms tick fires)
  → read Group A sensors (STATUS_REG poll — no wait)
  → xTaskNotifyWait(MAG_DRDY_BIT | IMU_INT1_BIT, timeout=5 ms)
      [If bits already set from ISR: proceed immediately]
      [If timeout: log warning, skip Group B reads this cycle]
  → magnetometer_read(&mag_data)
      → i2c_write_read(STATUS_REG 0x27)  — confirm ZYXDA=1
      → i2c_write_read(OUT_X_L, 6 bytes) — auto-increment burst
      → assemble int16_t x, y, z
  → imu_read(&imu_data)
      → i2c_write_read(STATUS_REG 0x1E)  — confirm XLDA=1, GDA=1
      → i2c_write_read(OUTX_L_G 0x22, 12 bytes) — auto-increment burst
      → assemble int16_t gyro x/y/z, accel x/y/z
```

### Init sequence

```
[Pre-scheduler — board_init()]
  magnetometer_init()   /* sensor config + EXTI GPIO config, interrupt disabled */
  imu_init()            /* same */

[Post-scheduler — SensorTask startup, before first 100 ms tick]
  magnetometer_attach_drdy_callback(prv_mag_drdy_cb, xTaskGetCurrentTaskHandle())
  imu_attach_int1_callback(prv_imu_int1_cb, xTaskGetCurrentTaskHandle())
  /* Both: store callback, enable EXTI interrupt */
```

### Impact on task-breakdown.md

The two DRDY/INT1 notification bits (MAG_DRDY_BIT, IMU_INT1_BIT) must be
added to the SensorTask IPC primitives table (§5.4). See GPB-O1.

---

## 7. Error handling (Step 6)

| Condition | MagnetometerDriver | ImuDriver |
|-----------|-------------------|-----------|
| I2C bus error | `MAGNETOMETER_ERR_I2C` | `IMU_ERR_I2C` |
| STATUS_REG data not ready (5 ms timeout elapsed without DRDY bit) | `MAGNETOMETER_ERR_NOT_READY` | `IMU_ERR_NOT_READY` |
| NULL `out` pointer | `MAGNETOMETER_ERR_INVALID_ARG` | `IMU_ERR_INVALID_ARG` |
| `attach_drdy_callback()` called with NULL `cb` | `MAGNETOMETER_ERR_INVALID_ARG` | `IMU_ERR_INVALID_ARG` |
| `read()` called before Phase 2 (no callback registered, EXTI disabled) | Returns `ERR_NOT_READY` (STATUS_REG gate) | Same |

SensorTask handles all non-OK returns per REQ-SA-080 (log error code) and
REQ-SA-060 (continue with available sensors). Drivers do not log; they
return error codes.

---

## 8. Test plan (Step 7)

### MagnetometerDriver

| Test ID | Scenario | Mechanism | Expected result |
|---------|----------|-----------|-----------------|
| MAG-T01 | Nominal read | Mock I2C: STATUS_REG ZYXDA=1, known OUT bytes | `magnetometer_read()` returns `OK`; x/y/z match expected signed assembly |
| MAG-T02 | STATUS_REG ZYXDA=0 | Mock returns 0x00 on STATUS_REG read | Returns `MAGNETOMETER_ERR_NOT_READY` |
| MAG-T03 | I2C failure on STATUS_REG | Mock returns I2C error | Returns `MAGNETOMETER_ERR_I2C` |
| MAG-T04 | I2C failure on data burst | STATUS_REG OK, burst fails | Returns `MAGNETOMETER_ERR_I2C` |
| MAG-T05 | NULL `out` pointer | Direct call | Returns `MAGNETOMETER_ERR_INVALID_ARG` |
| MAG-T06 | Negative axis value | Inject OUT bytes representing −1 LSB | Correct sign-extension in int16_t assembly |
| MAG-T07 | Attach callback with NULL cb | Direct call | Returns `MAGNETOMETER_ERR_INVALID_ARG` |

### ImuDriver

| Test ID | Scenario | Mechanism | Expected result |
|---------|----------|-----------|-----------------|
| IMU-T01 | Nominal read | Mock I2C: STATUS_REG GDA=1+XLDA=1, known OUT bytes | `imu_read()` returns `OK`; all six axes match expected signed assembly |
| IMU-T02 | STATUS_REG GDA=0 | Mock STATUS_REG bit 1 = 0 | Returns `IMU_ERR_NOT_READY` |
| IMU-T03 | STATUS_REG XLDA=0 | Mock STATUS_REG bit 0 = 0 | Returns `IMU_ERR_NOT_READY` |
| IMU-T04 | I2C failure on data burst | STATUS_REG OK, 12-byte burst fails | Returns `IMU_ERR_I2C` |
| IMU-T05 | NULL `out` pointer | Direct call | Returns `IMU_ERR_INVALID_ARG` |
| IMU-T06 | Maximum positive axis value | Inject 0x7FFF in all six axes | Correct int16_t = 32767 in all fields |
| IMU-T07 | Maximum negative axis value | Inject 0x8000 in all six axes | Correct int16_t = −32768 in all fields |

ISR path testing: `magnetometer_drdy_irq_handler()` and
`imu_int1_irq_handler()` are tested by calling them directly in the test
harness with a mock callback; verify the callback is invoked and no state
machine logic runs inside the handler.

All tests run on host (PC) using Unity. Mock I2C via
`#define I2C_INSTANCE (&mock_i2c)` per established convention.

---

## 9. Open items and decisions log (Step 8)

### Decisions

| ID | Decision | Rationale |
|----|----------|-----------|
| GPB-D1 | DRDY/INT1 ISR used — two-phase init | At configurable ODR these sensors can produce data faster than the SensorTask period. The ISR-driven notification allows SensorTask to read immediately when data is ready rather than polling STATUS_REG at a fixed tick. Consistent with the touchscreen and ModbusUartDriver two-phase pattern. |
| GPB-D2 | LIS3MDL ODR = 10 Hz | At least 10× the 1 Hz default SensorTask effective read rate. DRDY fires 10 times per SensorTask read cycle; the notification bit remains set from the last DRDY event when SensorTask wakes. |
| GPB-D3 | LSM6DSL ODR = 104 Hz for both axes | Highest standard ODR below the 208 Hz high-performance tier. Ensures data is always available within 10 ms of a read request; eliminates `ERR_NOT_READY` in practice. |
| GPB-D4 | Raw LSB output (no in-driver physical-unit conversion) | No Modbus register consumes IMU or magnetometer data. Physical-unit conversion in the application layer (P2). Sensitivity constants documented in headers. |
| GPB-D5 | BDU = 1 on both sensors | Block data update prevents reading mixed old/new bytes during a conversion. Mandatory for correct int16_t assembly from two-register pairs. |
| GPB-D6 | STATUS_REG checked inside `_read()` despite DRDY ISR notification | Defence in depth: DRDY signals that a conversion completed, but STATUS_REG is the authoritative "registers are stable" signal. Also guards against stale DRDY notification bits left from a previous cycle. |

### Open items

| ID | Item | Owner | Resolution path |
|----|------|-------|-----------------|
| GPB-O1 | `task-breakdown.md` §5.4 IPC primitives table does not list DRDY → SensorTask notification paths. Two entries required: `LIS3MDL DRDY ISR → SensorTask` (notify bit MAG_DRDY_BIT) and `LSM6DSL INT1 ISR → SensorTask` (notify bit IMU_INT1_BIT). ISR inventory §6.1 also needs two new rows. | Luca | Commit `docs: add DRDY and INT1 ISR-to-SensorTask notification entries to task-breakdown.md` on the same branch. |
| GPB-O2 | 5 ms `xTaskNotifyWait()` timeout for Group B read is an assumed value. Too short risks false `ERR_NOT_READY`; too long delays SensorTask. | Luca | Validate at integration using logic analyser or GPIO toggle on DRDY pin. Minimum safe value = 1 / ODR_LIS3MDL = 100 ms; 5 ms is safe given LIS3MDL at 10 Hz fires every 100 ms. |
| GPB-O3 | DRDY GPIO pin assignments: LIS3MDL → EXTI8 (MCU port unknown), LSM6DSL → EXTI11 (MCU port unknown). Labels from UM2153 Fig. 27 / Fig. 23; exact port letters not confirmed. | Luca | Verify against UM2153 Appendix A I/O assignment table before LLD coding. Critical path: GpioDriver init call and EXTI_IMR bit depend on correct port/pin. |
| GPB-O4 | I2C2 TIMINGR value (cross-cutting open item from i2c-driver.md, DUART-O2 root). | Luca | Resolve in clock-config.md companion. |
| GPB-O5 | LIS3MDL CTRL_REG1 value 0x7C uses OM=11 (ultra-high performance), which increases power consumption (570 µA typ.). Acceptable for this project; would be revisited in a power-optimised build. | Luca | Note only — no action required. |

---

*This document is the LLD companion for MagnetometerDriver and ImuDriver.
It is authored by Luca Agrippino and reviewed by the project mentor.*
