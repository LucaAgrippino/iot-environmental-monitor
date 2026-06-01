# I2cDriver — LLD Companion

**Document:** `docs/lld/drivers/i2c-driver.md`
**Version:** 1.0
**Board scope:** Field Device (STM32F469, I2C1) and Gateway (B-L475E-IOT01A, I2C2)
**Layer:** Driver
**Status:** Final (Phase H complete 2026-06-01)
**Date:** May 2026

**HLD anchor:** I2cDriver in `components.md` (FD + GW driver layer)

---

## 1. Sources

| Attribute | Field Device | Gateway |
|---|---|---|
| Responsibility | Serialises I2C bus transactions across multiple peripheral drivers | Serialises I2C bus transactions across multiple sensor drivers |
| PROVIDES (upward) | `II2c` | `II2c` |
| USES (downward) | CMSIS | CMSIS |
| Root requirement | REQ-LD-050 | REQ-SA-031 |
| Hardware peripheral | I2C1 | I2C2 |
| I2C IP version | v1 (F4 legacy) | v2 (L4 improved) |

**Consumers — Field Device:** `TouchscreenDriver` only. Single consumer, single task (`LcdUiTask`).

**Consumers — Gateway:** `MagnetometerDriver`, `ImuDriver`, `BarometerDriver`, `HumidityTempDriver`. All four are called exclusively from `SensorTask` (task-breakdown.md §5.1). Single-task access by construction — no concurrent bus access is possible.

Both boards satisfy the established "caller serialises" convention without any additional mechanism.

---

## 2. Public API

### 2.1 Dependency-conformance check

The public header (`i2c_driver.h`) includes only CMSIS device headers and `stdint.h`. No FreeRTOS headers, no `gpio_driver.h`. `GpioDriver` is consumed by the sensor drivers for their DRDY/INT lines, not by `I2cDriver`. Bus pins (SCL, SDA) are configured directly by `i2c_driver.c` via CMSIS, per the established bus-driver convention. Confirmed clean.

### 2.2 P3 (Interface Segregation) consideration

All consumers require both write and read transactions. There is no reader-only or writer-only consumer split. P3 does not call for a split here; a single `II2c` interface is correct.

### 2.3 Transaction model

Register-addressed sensor reads follow the pattern: **write phase** (device address + register address byte) followed immediately by a **read phase** (data bytes), with a repeated START between them and no STOP in between. This must be a single atomic driver call. Splitting it into a separate write then read would require the caller to manage the repeated START, leaking hardware knowledge upward and violating P1. `i2c_write_read` implements this as one atomic operation.

### 2.4 Data types

```c
/**
 * @brief Error codes returned by all I2cDriver operations.
 *
 * Naming follows the cross-cutting convention established in lld.md §3.2:
 * <module>_err_t, not _status_t.
 */
typedef enum {
    I2C_ERR_OK       = 0, /**< Operation succeeded. */
    I2C_ERR_NACK     = 1, /**< Device did not acknowledge address or data byte. */
    I2C_ERR_TIMEOUT  = 2, /**< A flag did not assert within the timeout window. */
    I2C_ERR_BUS_BUSY = 3, /**< BUSY flag set at transaction start. */
} i2c_err_t;
```

### 2.5 Public API (`i2c_driver.h`)

Address convention: the caller supplies a **7-bit device address**. The driver shifts it and sets the R/W bit internally. Callers never see the 8-bit wire format.

```c
/**
 * @brief Initialise the I2C peripheral and configure bus pins.
 *
 * Configures SCL and SDA as alternate-function open-drain outputs.
 * Sets the clock speed to 400 kHz (fast mode). Enables the peripheral.
 * Must be called once from main() before any consumer driver calls
 * i2c_write, i2c_read, or i2c_write_read.
 *
 * Peripheral: I2C1 on Field Device; I2C2 on Gateway.
 * Clock configuration: see §4 (hardware contract). Timing register
 * values depend on I2CCLK source (I2CD-O1, I2CD-O2 — see §8).
 *
 * @return I2C_ERR_OK on success.
 * @note Threading: task-context only, non-blocking. Must be called before the scheduler starts.
 */
i2c_err_t i2c_init(void);

/**
 * @brief Perform a write-only I2C transaction.
 *
 * Generates START → address (write) → data bytes → STOP.
 * Used for writing configuration registers on sensor or touchscreen devices.
 *
 * @param dev_addr  7-bit device address (not shifted).
 * @param data      Pointer to bytes to transmit (must not be NULL).
 * @param len       Number of bytes to transmit (must be ≥ 1).
 * @return I2C_ERR_OK, I2C_ERR_NACK, I2C_ERR_TIMEOUT, or I2C_ERR_BUS_BUSY.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
i2c_err_t i2c_write(uint8_t dev_addr, const uint8_t *data, uint16_t len);

/**
 * @brief Perform a read-only I2C transaction.
 *
 * Generates START → address (read) → data bytes → STOP.
 * Used when a device has already been addressed and advances its
 * internal pointer automatically (e.g. sequential reads).
 *
 * @param dev_addr  7-bit device address (not shifted).
 * @param buf       Pointer to receive buffer (must not be NULL).
 * @param len       Number of bytes to receive (must be ≥ 1).
 * @return I2C_ERR_OK, I2C_ERR_NACK, I2C_ERR_TIMEOUT, or I2C_ERR_BUS_BUSY.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
i2c_err_t i2c_read(uint8_t dev_addr, uint8_t *buf, uint16_t len);

/**
 * @brief Perform a combined write-then-read I2C transaction.
 *
 * Generates START → address (write) → tx_data bytes → repeated START →
 * address (read) → rx_len bytes → STOP.
 *
 * This is the primary transaction type for register-addressed sensor reads.
 * Typically tx_data is one or two bytes holding the register address;
 * rx_buf receives the register contents.
 *
 * The repeated START is issued atomically by the driver; the caller does
 * not manage it. (P1: hardware knowledge stays in the driver.)
 *
 * @param dev_addr  7-bit device address (not shifted).
 * @param tx_data   Pointer to bytes to transmit in write phase (must not be NULL).
 * @param tx_len    Number of bytes in write phase (must be ≥ 1).
 * @param rx_buf    Pointer to receive buffer for read phase (must not be NULL).
 * @param rx_len    Number of bytes to receive in read phase (must be ≥ 1).
 * @return I2C_ERR_OK, I2C_ERR_NACK, I2C_ERR_TIMEOUT, or I2C_ERR_BUS_BUSY.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
i2c_err_t i2c_write_read(uint8_t dev_addr,
                          const uint8_t *tx_data, uint16_t tx_len,
                          uint8_t       *rx_buf,  uint16_t rx_len);
```

### 2.6 Test-only hooks (`#ifdef TEST`)

```c
#ifdef TEST
/**
 * @brief Reset module-level state for unit tests.
 *
 * Clears s_i2c to its post-bss-init value so each Unity test case
 * starts from a clean slate. Test-only; not compiled into firmware
 * builds. Follows the project-wide convention established in
 * DebugUartDriver and carried forward through RtcDriver.
 */
void i2c_reset_for_test(void);
#endif
```

### 2.7 Singleton vtable interface (`II2c` — LLD-D10)

```c
/**
 * @brief Vtable interface for I2cDriver (LLD-D10 singleton pattern).
 *
 * All consumers (TouchscreenDriver FD; HumidityTempDriver, BarometerDriver,
 * ImuDriver, MagnetometerDriver GW) depend on this interface, not on the
 * concrete driver. The target-selected implementation file defines the
 * singleton instance; this header declares the extern pointer.
 */
typedef struct {
    i2c_err_t (*init)(void);
    i2c_err_t (*write)(uint8_t dev_addr, const uint8_t *data, uint16_t len);
    i2c_err_t (*read)(uint8_t dev_addr, uint8_t *buf, uint16_t len);
    i2c_err_t (*write_read)(uint8_t dev_addr,
                             const uint8_t *tx_data, uint16_t tx_len,
                             uint8_t       *rx_buf,  uint16_t rx_len);
} ii2c_t;

/** Singleton pointer — defined in the target-selected implementation file. */
extern const ii2c_t * const i2c_driver;
```

---

## 3. Internal design

### 3.0 Private struct

```c
typedef struct {
    bool initialised; /**< Set by i2c_init(); guards all entry points. */
} i2c_driver_t;

static i2c_driver_t s_i2c;
```


### 3.1 Module-level state

All module state is consolidated in `s_i2c` (§3.0); no additional file-scope variables are required. The peripheral maintains its own transaction state in hardware registers.

### 3.2 Two implementation files — one per IP version

The F469 I2C v1 and L475 I2C v2 peripherals have **entirely different register maps and programming models**. A single `.c` file with `#ifdef` gating would be unreadable. Two separate files share the same header:

| File | Target | Peripheral | IP version |
|---|---|---|---|
| `i2c_driver_f4.c` | STM32F469 (FD) | I2C1 | v1 — `DR`, `SR1`, `SR2`, `CCR`, `TRISE` |
| `i2c_driver_l4.c` | STM32L475 (GW) | I2C2 | v2 — `TIMINGR`, `ISR`, `ICR`, `TXDR`, `RXDR` |

The build system selects the correct file per target. Each file is independently readable and testable against its own mock register bank.

#### 3.2.1 Singleton vtable instance

Both `i2c_driver_f4.c` and `i2c_driver_l4.c` define the same symbol pattern:

```c
static const ii2c_t s_i2c_vtable = {
    .init       = i2c_init,
    .write      = i2c_write,
    .read       = i2c_read,
    .write_read = i2c_write_read,
};

const ii2c_t * const i2c_driver = &s_i2c_vtable;
```

The `i2c_driver` symbol is defined in the target-selected `.c` file. `i2c_driver.h` declares it `extern`; consumers reference only the header.

### 3.3 Polling, not interrupt or DMA

All transactions are polled (busy-wait on status flags). Rationale:

Sensor reads are short — four sensors × ~6 bytes each at 400 kHz ≈ 240 bytes total ≈ 6 ms. This is well within the 100 ms polling cycle (REQ-NF-100), leaving ample headroom. Adding interrupt-driven transfers would require a callback mechanism or a semaphore, importing a FreeRTOS dependency into the driver (forbidden by convention). DMA adds disproportionate complexity for transfers of this size. Polling is the correct choice here; it is revisited only if integration profiling reveals a timing problem.

### 3.4 Transaction sequences

#### I2C v1 — `i2c_write_read` on F469

```
1.  Check BUSY in SR2              → I2C_ERR_BUS_BUSY if set
2.  Set START in CR1
3.  Poll SB in SR1                 → I2C_ERR_TIMEOUT if expired
4.  Write (dev_addr << 1) to DR    (write phase)
5.  Poll ADDR in SR1               → I2C_ERR_NACK (AF flag) or I2C_ERR_TIMEOUT
6.  Read SR1 then SR2              (clears ADDR)
7.  For each tx byte:
      Write byte to DR
      Poll TXE in SR1              → I2C_ERR_TIMEOUT
8.  Poll BTF in SR1                → I2C_ERR_TIMEOUT
9.  Set START in CR1               (repeated START)
10. Poll SB in SR1                 → I2C_ERR_TIMEOUT
11. Write (dev_addr << 1 | 1) to DR (read phase)
12. If rx_len == 1: clear ACK before clearing ADDR
13. Poll ADDR in SR1               → I2C_ERR_NACK or I2C_ERR_TIMEOUT
14. Read SR1 then SR2              (clears ADDR)
15. For each rx byte except last:
      Poll RXNE                    → I2C_ERR_TIMEOUT
      Read DR
16. Before last byte: clear ACK, set STOP
17. Poll RXNE for last byte        → I2C_ERR_TIMEOUT
18. Read DR
19. Re-enable ACK for next transaction
```

The single-byte read case (step 12) requires disabling ACK before clearing ADDR — a well-known v1 errata workaround that must be implemented exactly as specified in RM0386 §27.3.3.

#### I2C v2 — `i2c_write_read` on L475

```
1.  Check BUSY in ISR              → I2C_ERR_BUS_BUSY if set
2.  Write CR2: SADD=dev_addr<<1, NBYTES=tx_len, RD_WRN=0, AUTOEND=0, START=1
3.  For each tx byte:
      Poll TXIS or NACKF in ISR   → I2C_ERR_NACK (NACKF) or I2C_ERR_TIMEOUT
      Write byte to TXDR
4.  Poll TC in ISR                 → I2C_ERR_TIMEOUT
5.  Write CR2: SADD=dev_addr<<1, NBYTES=rx_len, RD_WRN=1, AUTOEND=1, START=1
6.  For each rx byte:
      Poll RXNE in ISR             → I2C_ERR_TIMEOUT
      Read byte from RXDR
7.  Poll STOPF in ISR              → I2C_ERR_TIMEOUT
8.  Write ICR: STOPCF=1            (clear STOPF)
```

The v2 model is cleaner: AUTOEND=0 on the write phase holds the bus between phases; AUTOEND=1 on the read phase generates STOP automatically after the last byte.

### 3.5 Bus recovery

After `I2C_ERR_TIMEOUT` or persistent `I2C_ERR_BUS_BUSY`, the bus may be stuck (a peripheral is holding SDA low mid-transaction). The driver attempts recovery before returning the error:

1. Disable the I2C peripheral (PE=0 in CR1).
2. Reconfigure SCL as a GPIO output.
3. Toggle SCL nine times at ~10 kHz, checking SDA between each toggle.
4. If SDA releases, generate a STOP condition manually, then re-enable the peripheral.
5. If SDA does not release after nine clocks, return `I2C_ERR_TIMEOUT` to the caller.

This recovery sequence supports REQ-NF-205 (sensor reinitialisation after failure). Without it, a stuck bus requires a full MCU reset.

### 3.6 No ISR, no DMA, no callbacks

Consistent with all prior driver companions. Consumers call synchronously from their task context.

---

### 3.7 Principles applied

- **P1 (Strict directional layering).** Depends only on CMSIS device register map; no RTOS, no middleware.
- **P2 (Dependency Inversion).** Exposes `ii2c_t` vtable singleton; all consumers (HumidityTempDriver, BarometerDriver, TouchscreenDriver, RtcDriver GW) depend on the interface.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static `s_i2c` struct; no heap; no RTOS primitives (caller serialises — see §3.3 and Synchronisation).
- **P6 (Responsibility traces to requirements).** The I2C API traces to all consumers' sensor and peripheral requirements.
- **P8 (Total error propagation, no silent failures).** All calls return `i2c_err_t`; HAL timeout surfaces as `I2C_ERR_TIMEOUT`; bus errors propagated.
- **P9 (BARR-C coding standard).** `uint8_t*` data pointers; `uint16_t` for device address; no implicit widening.
- **P10 (Naming conventions).** Prefix `i2c_`; interface `II2c` -> `ii2c_t`; errors `I2C_ERR_*`.


### Synchronisation

Caller serialises. The driver holds no FreeRTOS synchronisation primitives. All entry points are intended to be called from a single task context or from `main()` before the scheduler starts. Concurrent access from multiple tasks is not safe unless the caller provides a mutex.

### i2c_init

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### i2c_write

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### i2c_read

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).


## 4. Hardware contract

### 4.1 Clock speed — both boards

Target: **400 kHz (fast mode)**. All sensors on the Gateway (LIS3MDL, LSM6DSL, LPS22HB, HTS221) support 400 kHz. The touchscreen controller on the Field Device (FT6206 per UM1932) also supports 400 kHz.

### 4.2 Timing register values — open items

**F469 (I2C v1):** the `CCR` and `TRISE` register values are derived from PCLK1 (the APB1 clock feeding I2C1). These cannot be finalised until the system clock configuration is resolved. Tracked as **I2CD-O2** (§8).

**L475 (I2C v2):** the `TIMINGR` register value depends on the I2CCLK source, which on the L475 can be PCLK1, SYSCLK, or HSI16 (selectable via RCC_CCIPR). STM32CubeIDE's I2C timing tool provides validated values for given input clock and target speed. Cannot be finalised until the clock configuration is resolved. Tracked as **I2CD-O1** (§8).

Both open items share the same root dependency as DUART-O2 — the unresolved `clock-config.md` companion.

### 4.3 Pin configuration — both boards

Bus pins must be configured as alternate-function, open-drain, with external pull-up resistors (present on both discovery boards). The driver configures its own pins directly via CMSIS (established bus-driver convention). It does not consume `IGpio`.

| Board | SCL | SDA | Peripheral |
|---|---|---|---|
| STM32F469 (FD) | To be confirmed against UM1932 schematic at implementation | — | I2C1 |
| STM32L475 (GW) | PB10 (I2C2_SCL) | PB11 (I2C2_SDA) per UM2153 schematic | I2C2 |

**Action required (Luca at implementation):** verify F469 I2C1 pin assignment against the UM1932 schematic and the touchscreen controller datasheet. The L475 I2C2 pins are confirmed from the hardware bottom-up sweep in `components.md`.

### 4.4 Cross-target register compatibility

No shared register-level code between the two implementations. The v1 and v2 peripherals are addressed independently in `i2c_driver_f4.c` and `i2c_driver_l4.c` respectively. The CMSIS headers (`stm32f469xx.h`, `stm32l475xx.h`) each define `I2C_TypeDef` with the correct fields for their IP version; there is no cross-contamination risk provided the correct header is included per target.

**Action required (Luca at implementation):** read RM0386 §27 (F469 I2C) and RM0351 §37 (L475 I2C) in full before writing either implementation. The v1 single-byte read errata (step 12 in §3.4) is a common source of defects and must be implemented exactly as specified.

---

### NVIC

N/A — the driver uses a polling model (busy-wait on status flags; see §3 Internal design). I2C event and error interrupts (`I2Cx_EV_IRQn`, `I2Cx_ER_IRQn`) are not enabled. This avoids importing a FreeRTOS dependency at the driver layer (lld.md §3.4 dependency rule).


## 5. Sequence integration

`I2cDriver` has no HLD-level sequence diagram surface. The SD-01 note explicitly excludes driver-to-driver traffic from the HLD. No changes to `sequence-diagrams.md` are required.

---

## 6. Error and fault behaviour

### 6.1 Error propagation

Every function returns `i2c_err_t`. No silent failures. Consumer response per error:

| Error | Meaning | Consumer responsibility |
|---|---|---|
| `I2C_ERR_NACK` | Device not present or refused data | Log via Logger; push sensor failure event via `IHealthReport`; apply REQ-NF-208 (substitute error indicator value) |
| `I2C_ERR_TIMEOUT` | Flag never asserted; bus may be stuck | Driver attempts recovery (§3.5); if recovery fails, return `I2C_ERR_TIMEOUT`; consumer treats as sensor failure per REQ-NF-205 |
| `I2C_ERR_BUS_BUSY` | BUSY at transaction start | Consumer may retry once; if still busy, treat as `I2C_ERR_TIMEOUT` |

### 6.2 REQ-NF-205 interaction

`SensorService` is responsible for reinitialising a failed sensor (REQ-NF-205). This means calling the sensor driver's `xxx_init()` again, which in turn calls `i2c_write` to reconfigure the sensor's registers. For this to work after a bus error, `i2c_driver` must have completed its recovery sequence and left the peripheral in a usable state before returning the error code. The bus recovery in §3.5 fulfils this contract.

---

## 7. Unit-test plan

Host-platform tests under the Ceedling/Unity harness. The two IP versions are too different for a shared parametrised test; each implementation file has its own dedicated test TU.

### 7.1 Mock strategy

**F469 (v1):** `#define I2C1 (&mock_i2c_f4)` in the test preamble redirects all register accesses. `mock_i2c_f4` is a `static I2C_TypeDef` instance whose fields are pre-loaded per test case. GPIO registers used during bus recovery are provided by `stm32_cmsis_mock.h` (GPIOB mock already established in GpioDriver tests). Each test calls `i2c_reset_for_test()` from `setUp()` to clear `s_i2c.initialised`.

**L475 (v2):** `#define I2C2 (&mock_i2c_l4)` redirects to a separate `static I2C_TypeDef` instance with v2-specific fields (`TIMINGR`, `ISR`, `ICR`, `TXDR`, `RXDR`). GPIO mock covers PB10/PB11 (bus recovery). Each test calls `i2c_reset_for_test()` from `setUp()`.

**Timeout simulation:** The driver polls flags in a bounded counter loop. Happy-path tests pre-set the relevant flag in the mock register before calling the API, so the first poll succeeds. Timeout-path tests leave the flag clear; the driver exhausts the counter and returns `I2C_ERR_TIMEOUT`.

**Not-initialised guard:** Calling any API function with `s_i2c.initialised == false` invokes `assert()` in the debug build. This is a programmer-error path and is not unit-tested (matches the convention established in RtcDriver RTCD-D9).

### 7.2 Test files and project.yml entries

- `tests/field-device/drivers/i2c/test_i2c_driver_f4.c`
  ```yaml
  :test_i2c_driver_f4:
    - STM32F469xx
    - TEST
  ```
- `tests/gateway/drivers/i2c/test_i2c_driver_l4.c`
  ```yaml
  :test_i2c_driver_l4:
    - STM32L475xx
    - TEST
  ```

Both paths are covered by the existing wildcard globs `field-device/**` and `gateway/**` in `project.yml`.

### 7.3 Test cases — STM32F469 (v1, `i2c_driver_f4.c`)

| ID | Test case | Expected result |
|---|---|---|
| TC-I2C-F4-001 | `i2c_init` happy path | CR1.PE = 1; GPIO SCL/SDA pins configured as AF, open-drain; `s_i2c.initialised = true`; returns `I2C_ERR_OK` |
| TC-I2C-F4-002 | `i2c_init` called twice (idempotent) | Second call returns `I2C_ERR_OK`; CR1 not re-written on second call |
| TC-I2C-F4-003 | `i2c_write` happy path — 2 bytes to 0x44 | CR1.START set; SR1.SB polled and found set; DR written with `0x44 << 1`; SR1.ADDR polled; SR1 + SR2 read (ADDR cleared); both bytes written to DR with TXE polling; BTF polled; CR1.STOP set; returns `I2C_ERR_OK` |
| TC-I2C-F4-004 | `i2c_write` NACK on address — SR1.AF set during ADDR poll | Returns `I2C_ERR_NACK`; CR1.STOP generated; AF cleared |
| TC-I2C-F4-005 | `i2c_write` SB timeout — SR1.SB never sets after START | Returns `I2C_ERR_TIMEOUT` |
| TC-I2C-F4-006 | `i2c_write` TXE timeout — SR1.TXE never sets during data phase | Returns `I2C_ERR_TIMEOUT` |
| TC-I2C-F4-007 | `i2c_write` BTF timeout — SR1.BTF never sets after last byte written | Returns `I2C_ERR_TIMEOUT` |
| TC-I2C-F4-008 | `i2c_write` bus busy — SR2.BUSY set at entry | Returns `I2C_ERR_BUS_BUSY`; CR1.START not set |
| TC-I2C-F4-009 | `i2c_write_read` happy path — 1 byte write, 2 bytes read | Write phase: START, addr+W, ADDR cleared, 1 byte via TXE, BTF; repeated START; addr+R, ADDR cleared; byte[0] read via RXNE; CR1.ACK cleared + CR1.STOP set before last poll; byte[1] read via RXNE; CR1.ACK re-enabled; returns `I2C_ERR_OK` with correct data |
| TC-I2C-F4-010 | `i2c_write_read` single-byte read — rx_len = 1 | CR1.ACK cleared **before** SR2 read (ADDR clear); CR1.STOP set; SR1.RXNE polled; DR read; CR1.ACK re-enabled; returns `I2C_ERR_OK` with correct byte |
| TC-I2C-F4-011 | `i2c_write_read` NACK during write phase | Returns `I2C_ERR_NACK`; repeated START not issued; read phase not entered |
| TC-I2C-F4-012 | `i2c_read` happy path — 6 bytes from 0x1E | DR written with `(0x1E << 1) \| 1`; ADDR polled and cleared; 5 bytes read via RXNE; before byte 6: CR1.ACK cleared + CR1.STOP set; byte 6 read; CR1.ACK re-enabled; returns `I2C_ERR_OK` |
| TC-I2C-F4-013 | Bus recovery — SR2.BUSY set, write call times out | CR1.PE cleared (peripheral disabled); SCL pin reconfigured as GPIO output; SCL toggled 9 times in mock GPIO; peripheral re-enabled (CR1.PE set); returns `I2C_ERR_TIMEOUT` |

### 7.4 Test cases — STM32L475 (v2, `i2c_driver_l4.c`)

| ID | Test case | Expected result |
|---|---|---|
| TC-I2C-L4-001 | `i2c_init` happy path | TIMINGR set to placeholder constant; CR1.PE = 1; GPIO PB10/PB11 configured as AF4, open-drain; `s_i2c.initialised = true`; returns `I2C_ERR_OK` |
| TC-I2C-L4-002 | `i2c_init` called twice (idempotent) | Second call returns `I2C_ERR_OK`; TIMINGR not re-written |
| TC-I2C-L4-003 | `i2c_write` happy path — 2 bytes to 0x44 | CR2 written with `SADD = 0x88, NBYTES = 2, RD_WRN = 0, AUTOEND = 1, START = 1`; ISR.TXIS polled twice; both bytes written to TXDR; ISR.STOPF polled; ICR.STOPCF set; returns `I2C_ERR_OK` |
| TC-I2C-L4-004 | `i2c_write` NACK — ISR.NACKF set during TX | Returns `I2C_ERR_NACK`; ICR.NACKCF written |
| TC-I2C-L4-005 | `i2c_write` TXIS timeout — ISR.TXIS and ISR.NACKF both clear | Returns `I2C_ERR_TIMEOUT` |
| TC-I2C-L4-006 | `i2c_write` bus busy — ISR.BUSY set at entry | Returns `I2C_ERR_BUS_BUSY`; CR2.START not set |
| TC-I2C-L4-007 | `i2c_write_read` happy path — 1 byte write, 2 bytes read | Write CR2: `SADD = dev<<1, NBYTES = 1, RD_WRN = 0, AUTOEND = 0, START = 1`; TXIS polled; byte written to TXDR; ISR.TC polled; read CR2: `SADD = dev<<1, NBYTES = 2, RD_WRN = 1, AUTOEND = 1, START = 1`; RXNE polled twice; 2 bytes read from RXDR; STOPF polled; ICR.STOPCF set; returns `I2C_ERR_OK` with correct data |
| TC-I2C-L4-008 | `i2c_write_read` NACK during write phase | Returns `I2C_ERR_NACK`; read-phase CR2 write not issued |
| TC-I2C-L4-009 | `i2c_write_read` TC timeout after write phase | Returns `I2C_ERR_TIMEOUT`; read-phase CR2 write not issued |
| TC-I2C-L4-010 | `i2c_read` happy path — 6 bytes from 0x1E | CR2 written with `SADD = 0x3C, NBYTES = 6, RD_WRN = 1, AUTOEND = 1, START = 1`; RXNE polled 6 times; 6 bytes read from RXDR; STOPF polled; ICR.STOPCF set; returns `I2C_ERR_OK` |
| TC-I2C-L4-011 | Bus recovery — ISR.BUSY persists, write call times out | CR1.PE cleared; PB10 (SCL) reconfigured as GPIO output; SCL toggled 9 times in mock GPIO; peripheral re-enabled; returns `I2C_ERR_TIMEOUT` |

### 7.5 Coverage notes

Every `i2c_err_t` value (`I2C_ERR_OK`, `I2C_ERR_NACK`, `I2C_ERR_TIMEOUT`, `I2C_ERR_BUS_BUSY`) is produced by ≥ 1 test in each test file. Every public API function has ≥ 1 happy-path and ≥ 1 error-path test. TC-I2C-F4-010 covers the single-byte read errata path explicitly — this is the highest-risk path in the F469 implementation per RM0386 §27.3.3. Bus recovery (TC-I2C-F4-013, TC-I2C-L4-011) verifies that PE is cleared and SCL is toggled 9 times via the mock GPIO; full electrical validation (SDA release detection) is deferred to hardware integration (I2CD-O4).

---

## 8. Open items

| ID | Item | Owner | Resolution path |
|---|---|---|---|
| I2CD-O1 | `TIMINGR` value for I2C2 on L475 at 400 kHz. Depends on I2CCLK source, resolved in `clock-config.md`. Use STM32CubeIDE I2C timing tool once clock tree is fixed. | Luca | Resolve when `clock-config.md` lands (DUART-O2 dependency) |
| I2CD-O2 | `CCR` and `TRISE` values for I2C1 on F469 at 400 kHz. Depends on PCLK1. Same root dependency as I2CD-O1. | Luca | Resolve when `clock-config.md` lands |
| I2CD-O3 | F469 I2C1 pin assignment (SCL, SDA, alternate function number). Verify against UM1932 schematic and FT6206 datasheet before implementation. | Luca | Check at implementation |
| I2CD-O4 | Bus recovery sequence validation. The 9-clock recovery is specified behaviour but the timing and GPIO reconfiguration must be validated on hardware; hard to test fully on host. | Luca | Validate at integration with an oscilloscope or logic analyser |

**Inherited open items with no surface area in this companion:**
- O1 (WiFi SPI driver naming): not applicable.
- O2 (worst-case stack measurements): not applicable.
- O3 (hardware watchdog): not applicable.

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| I2CD-D1 | Two separate `.c` files for v1 and v2 | v1 and v2 have entirely different register maps and programming models; a single file with `#ifdef` gating would be unreadable and hard to test independently |
| I2CD-D2 | Polling, not interrupt or DMA | Transactions are short (≤ 6 ms for all GW sensors); polling avoids FreeRTOS dependency in the driver; DMA adds disproportionate complexity for transfers of this size |
| I2CD-D3 | `i2c_write_read` as single atomic operation | Callers must not manage the repeated START; doing so would push hardware knowledge upward, violating P1 |
| I2CD-D4 | 7-bit address convention | Caller supplies the 7-bit address; driver shifts internally; cleaner API — callers never see the 8-bit wire format |
| I2CD-D5 | Singleton module (no handle) | One I2C peripheral per board; consistent with RtcDriver and GpioDriver; handle adds no value |
| I2CD-D6 | Bus recovery on timeout before returning error | Supports REQ-NF-205 (sensor reinitialisation); without recovery, a stuck SDA line requires a full MCU reset rather than a `SensorService`-level recovery |
