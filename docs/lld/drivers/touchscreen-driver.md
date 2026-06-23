# TouchscreenDriver — LLD Companion

**Document:** `docs/lld/companions/touchscreen-driver.md`
**Version:** 1.0
**Board scope:** Field Device (STM32F469) only
**Layer:** Driver
**Status:** Pass H — Implementation ready
**Date:** June 2026

**HLD anchor:** TouchscreenDriver in `components.md` (FD driver layer)

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Reads touch coordinate events from the FT6x06 capacitive touch controller | `components.md` |
| PROVIDES (upward) | TouchscreenDriver public API (direct C functions) consumed by `GraphicsLibrary` | `components.md` |
| USES (downward) | `I2cDriver`, CMSIS (for EXTI / GPIO of the IRQ pin) | `components.md` |
| Root requirements | REQ-LD-000 (touch navigation), REQ-LD-100 (configuration screen interaction) | `SRS.md` |
| Hardware | FT6206 (or FT6336, pin- and register-compatible) on I2C1; IRQ on PJ5 | UM1932 §4.14 |

**Traceability note:** `components.md` cites REQ-LD-050 for this
component. REQ-LD-050 is a display-refresh requirement, not a
touchscreen requirement. The correct traceability is REQ-LD-000
(navigation via touch) and REQ-LD-100 (configuration screen
interaction). This is a pre-existing gap in `components.md` to be
fixed in the SRS-update backlog. The driver design itself is
unaffected.

**Consumer:** `GraphicsLibrary` (middleware, LVGL wrapper). Called from
`LcdUiTask`.

**ISR surface** (`task-breakdown.md` §6.1):
- `EXTI9_5_IRQHandler` — touch IRQ pin asserted (PJ5, falling edge) →
  notifies `LcdUiTask`

**Reset:** PH7 is shared with the LCD panel reset and is driven by
`LcdDriver`. `touchscreen_init()` MUST be called after `lcd_init()`,
which releases PH7 and allows the FT6x06 to come out of reset.

---

## 2. Public API

### 2.1 Dependency-conformance check

`touchscreen_driver.h` includes only `stdint.h` and `stdbool.h`. I2C
calls are made from `touchscreen_driver.c` via `i2c_driver.h`. No
FreeRTOS in the header. Confirmed clean.

### 2.2 Two-phase init

The EXTI IRQ callback requires `LcdUiTask` to exist.
`touchscreen_init()` configures the FT6x06 and the EXTI pin but does
not enable the EXTI interrupt. `touchscreen_attach_irq()` registers
the callback and enables the interrupt. Called from `LcdUiTask`
startup prologue.

### 2.3 Data types

```c
typedef enum {
    TS_ERR_OK      = 0,
    TS_ERR_NO_DATA = 1, /**< No touch point active. */
    TS_ERR_I2C     = 2, /**< Underlying I2C transaction failed. */
    TS_ERR_NULL    = 3, /**< Null pointer argument. */
} ts_err_t;

typedef enum {
    TS_EVENT_PRESS   = 0, /**< New touch point detected. */
    TS_EVENT_RELEASE = 1, /**< Touch point released. */
    TS_EVENT_CONTACT = 2, /**< Ongoing contact (no change from prior read). */
} ts_event_t;

/**
 * @brief A single touch point reading.
 *
 * Coordinates are in display pixels (0..799 for x, 0..479 for y).
 */
typedef struct {
    uint16_t   x;
    uint16_t   y;
    ts_event_t event;
} ts_touch_t;

/**
 * @brief IRQ callback type. Called from EXTI9_5 ISR context.
 *        The callback typically issues xTaskNotifyFromISR().
 */
typedef void (*ts_irq_cb_t)(void *context);
```

### 2.4 Public API (`touchscreen_driver.h`)

```c
/**
 * @brief Initialise the FT6x06 touchscreen controller.
 *
 * Pre-condition: lcd_init() must have completed (PH7 released,
 * FT6x06 out of reset).
 *
 * Sequence:
 *   1. Configure PJ5 as input, no pull (or pull-up if external is absent).
 *   2. Configure SYSCFG to route EXTI5 to PJ5; configure EXTI5 falling edge.
 *      Do NOT enable EXTI5 in the EXTI IMR yet.
 *   3. Write FT6x06 INT_MODE register (0xA4) = 0x00 (trigger mode).
 *   4. Write FT6x06 TH_GROUP threshold register (0x80) to default.
 *   5. Read DEV_MODE (0x00) to confirm device responds.
 *
 * @return TS_ERR_OK on success; TS_ERR_I2C if any FT6x06 register
 *         access fails.
 * @note   Threading: task-context only, non-blocking. Must be called
 *         before the scheduler starts.
 */
ts_err_t touchscreen_init(void);

/**
 * @brief Register the IRQ callback and enable the EXTI5 interrupt.
 *
 * Called from LcdUiTask startup prologue.
 *
 * @param callback  Function called from EXTI9_5 ISR. Must not be NULL.
 * @param context   Opaque pointer passed unchanged to the callback.
 * @return TS_ERR_OK or TS_ERR_NULL.
 * @note   Callback executes in ISR context.
 */
ts_err_t touchscreen_attach_irq(ts_irq_cb_t callback, void *context);

/**
 * @brief Read the current touch point from the FT6x06.
 *
 * Called from LcdUiTask after the IRQ callback fires, to retrieve
 * the touch coordinates. Issues an I2C read of TD_STATUS (0x02) and
 * the touch point registers (P1_XH..P1_YL, addresses 0x03..0x06).
 *
 * @param[out] touch  Populated on TS_ERR_OK. Must not be NULL.
 * @return TS_ERR_OK / TS_ERR_NO_DATA / TS_ERR_I2C / TS_ERR_NULL.
 * @note   Threading: task-context only, non-blocking. Not ISR-safe.
 */
ts_err_t touchscreen_read(ts_touch_t *touch);
```

---

## 3. Internal design

### 3.1 Private struct

```c
typedef struct {
    ts_irq_cb_t  irq_cb;       /**< EXTI5 interrupt callback. */
    void        *irq_ctx;      /**< Caller context for irq_cb. */
    bool         initialised;  /**< Set by touchscreen_init(). */
} touchscreen_driver_t;

static touchscreen_driver_t s_ts;
```

### 3.2 FT6x06 register access

The FT6x06 family (FT6206, FT6236, FT6336) uses an identical I2C
register map for single-touch operation. Reads use the I2cDriver
write-then-read pattern (single-byte register-address write, then
multi-byte read of consecutive registers).

| Register | Address | Description |
|---|---|---|
| `DEV_MODE` | 0x00 | Device mode (working / factory) |
| `GEST_ID` | 0x01 | Gesture ID (single point: 0x00) |
| `TD_STATUS` | 0x02 | Number of active touch points (0..2) |
| `P1_XH` | 0x03 | Touch 1 X MSB[3:0] + event flag[7:6] |
| `P1_XL` | 0x04 | Touch 1 X LSB[7:0] |
| `P1_YH` | 0x05 | Touch 1 Y MSB[3:0] |
| `P1_YL` | 0x06 | Touch 1 Y LSB[7:0] |
| `TH_GROUP` | 0x80 | Gesture detection threshold |
| `CTRL` | 0x86 | Active / monitor mode |
| `PERIOD_ACTIVE` | 0x88 | Active polling period |
| `INT_MODE` | 0xA4 | Interrupt mode (0 = trigger, 1 = polling) |

The driver writes `INT_MODE = 0x00` (trigger mode) at init so the IRQ
pin asserts only on touch events.

Event flag in `P1_XH[7:6]`:
- `00b` → `TS_EVENT_PRESS` (new contact)
- `01b` → `TS_EVENT_RELEASE`
- `10b` → `TS_EVENT_CONTACT` (ongoing)

### 3.3 I2C address

**FT6x06 slave address: 0x2A (7-bit).** Equivalent forms:
- 7-bit raw: `0x2A`
- 8-bit write (HAL convention, 7-bit shifted): `0x54`
- 8-bit read: `0x55`

Use the form expected by the `I2cDriver` API in this project. If
`i2c_write()` takes a 7-bit address and shifts internally, the constant
is `0x2A`. If it takes the 8-bit shifted address, the constant is
`0x54`.

```c
#define FT6X06_I2C_ADDR_7BIT  (0x2AU)
```

### 3.4 EXTI IRQ pin

FT6x06 IRQ pin is connected to **PJ5** on the F469 Discovery
(confirmed at implementation review). Active-low, falling-edge
trigger. Maps to EXTI5 → `EXTI9_5_IRQHandler`.

Configuration in `touchscreen_init()`:
- GPIO PJ5 as input, no internal pull (external pull-up on the panel
  module asserts the line high when no touch)
- `SYSCFG->EXTICR[1]` configured so EXTI5 maps to port J
- `EXTI->FTSR1` bit 5 set (falling edge), `EXTI->RTSR1` bit 5 clear
- `EXTI->IMR1` bit 5 left clear — enabled later in
  `touchscreen_attach_irq()`

NVIC `EXTI9_5_IRQn` enabled at priority **6** (≥
`configMAX_SYSCALL_INTERRUPT_PRIORITY`, FromISR-safe).

### 3.5 No FreeRTOS in driver

`EXTI9_5_IRQHandler` (when fired by line 5) calls
`s_ts.irq_cb(s_ts.irq_ctx)`. The callback is wired by GraphicsLibrary
to `xTaskNotifyFromISR`. The driver holds only a function pointer.

If EXTI9_5 is shared with another peripheral on a different EXTI line
in this band, the handler must dispatch by reading `EXTI->PR1` and
calling only the matching callback. Mark this as an implementation
note for the shared-handler integration step.

### 3.6 Principles applied

- **P1 (Strict directional layering).** Depends on `I2cDriver` (peer
  driver) and CMSIS (EXTI/GPIO/SYSCFG). No middleware.
- **P2 (Dependency Inversion).** GraphicsLibrary depends on
  `touchscreen_driver.h`; the callback function pointer is the only
  outbound dependency, supplied at `touchscreen_attach_irq()` by the
  consumer.
- **P5 (Bounded resources, no dynamic allocation post-init).** One
  static `touchscreen_driver_t`; no heap; pull-based read (no internal
  ring buffer).
- **P6 (Responsibility traces to requirements).** Touch-read function
  traces to REQ-LD-000 (touch navigation) and REQ-LD-100
  (configuration screen interaction).
- **P8 (Total error propagation, no silent failures).** `ts_err_t` on
  every public function; I2C errors propagated; no silent drops.
- **P9 (BARR-C coding standard).** Coordinates `uint16_t`; event flag
  `uint8_t`; no floating-point.
- **P10 (Naming conventions).** Prefix `touchscreen_` (long form) /
  `ts_` (short form on types and errors). Errors `TS_ERR_*`.

### 3.7 Synchronisation

Caller serialises. The driver holds no FreeRTOS synchronisation
primitives. All entry points run in a single task context (or `main()`
before the scheduler starts).

### 3.8 Per-function notes

**`touchscreen_init`** — pre-condition `lcd_init()` succeeded.
Configures PJ5/EXTI5 (interrupt not yet enabled), writes FT6x06
INT_MODE and threshold via I2C, reads DEV_MODE as a probe. Sets
`s_ts.initialised = true` on success.

**`touchscreen_attach_irq`** — validates `callback != NULL`. Stores
callback and context in `s_ts`. Enables EXTI5 in `EXTI->IMR1`; enables
`EXTI9_5_IRQn` in NVIC at priority 6.

**`touchscreen_read`** — validates `touch != NULL`. Reads
`TD_STATUS`; if 0 active points returns `TS_ERR_NO_DATA`. Otherwise
reads P1_XH..P1_YL (4 bytes), unpacks coordinates and event flag,
populates `*touch`.

---

## 4. Hardware contract

### 4.1 Touch controller

**FT6x06 family** (FT6206 / FT6236 / FT6336 — pin- and
register-compatible for single-touch operation). UM1932 does not
specify the exact part for every board revision; ST's BSP
(`ft6x06.c`) supports the family generically. The driver works with
any of these.

I2C address: **0x2A (7-bit)**, equivalent to 0x54 (8-bit write).

### 4.2 IRQ pin

PJ5, falling edge, EXTI5 → `EXTI9_5_IRQHandler`. Active-low. Internal
pull-up not enabled (external pull-up present on the panel module).

### 4.3 I2C bus

I2C1 on the F469 — PB8 (SCL) and PB9 (SDA), per UM1932 §4.14. Shared
with other I2C peripherals on the board's I2C1 extension connector
CN11 (UM1932 §4.16). Bus speed: standard mode 100 kHz (matches
I2cDriver default — confirm against `I2cDriver` companion).

### 4.4 PH7 reset ordering

`touchscreen_init()` MUST be called after `lcd_init()`. The FT6x06
shares PH7 with the LCD panel; calling `touchscreen_init()` before
`lcd_init()` releases PH7 would result in all I2C transactions NACKing
because the FT6x06 is held in reset.

### 4.5 Registers

N/A at the MCU peripheral level — FT6x06 registers are accessed via
I2cDriver. The driver also touches:

| Peripheral | Registers | Purpose |
|---|---|---|
| `GPIOJ`  | `MODER`, `PUPDR` | PJ5 input mode |
| `SYSCFG` | `EXTICR[1]` | Route EXTI5 to port J |
| `EXTI`   | `FTSR1`, `RTSR1`, `IMR1`, `PR1` | Falling-edge trigger, mask, pending |
| `NVIC`   | EXTI9_5_IRQn enable + priority | Vector to ISR |

### 4.6 Pins

| Pin | Function | Mode | AF |
|---|---|---|---|
| PB8 | I2C1_SCL | AF | 4 (configured by I2cDriver) |
| PB9 | I2C1_SDA | AF | 4 (configured by I2cDriver) |
| PJ5 | Touch IRQ | Input | — (CMSIS GPIO, no GpioDriver dependency for non-bus pins) |
| PH7 | Touch+LCD reset | Output | (owned by LcdDriver) |

### 4.7 Clocks

I2C1 clock enabled by I2cDriver. GPIOJ and SYSCFG clocks enabled
within `touchscreen_init()`:

```c
RCC->AHB1ENR  |= RCC_AHB1ENR_GPIOJEN;
RCC->APB2ENR  |= RCC_APB2ENR_SYSCFGEN;
```

### 4.8 NVIC

`EXTI9_5_IRQn` priority **6** (≥ `configMAX_SYSCALL_INTERRUPT_PRIORITY`,
FromISR-safe).

---

## 5. Sequence integration

`TouchscreenDriver` is not an explicit lifeline in any HLD sequence
diagram. Its ISR is documented in `task-breakdown.md` §6.1. No SD
changes required.

---

## 6. Error and fault behaviour

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `TS_ERR_NO_DATA` | `touchscreen_read()` called when `TD_STATUS` reports 0 active points | Return error; touch point registers not read | Non-OK return | No retry — LcdUi waits for the next EXTI event; this is a normal race | Not logged (expected transient) |
| `TS_ERR_I2C` | `i2c_write()` or `i2c_read()` returned non-OK | Return error; touch state unchanged | Non-OK return | No retry — LcdUi skips the touch sample; persistent failures reported via `IHealthReport` | LcdUi logs at WARN; persistent failures push `HEALTH_EVENT_SENSOR_FAIL` |
| `TS_ERR_NULL` | Null pointer argument | Return error; no I2C interaction | Non-OK return | No retry — programming error | Caller logs at ERROR |

---

## 7. Unit-test plan

Host-platform tests. I2C transactions are mocked via stub functions
provided by `tests/support/i2c_driver_stub.h`. The EXTI IRQ path is
tested by calling the handler directly.

Test file: `tests/field-device/drivers/touchscreen_driver/test_touchscreen_driver.c`.

| Test ID | Description | Verifies |
|---|---|---|
| TC-TS-001 | `touchscreen_init()` happy path with mocked I2C ACK → `TS_ERR_OK`; `s_ts.initialised == true`; INT_MODE register written with `0x00` | Init happy path |
| TC-TS-002 | `touchscreen_init()` with first I2C call returning error → `TS_ERR_I2C`; `s_ts.initialised == false` | I2C failure during init |
| TC-TS-003 | `touchscreen_init()` with DEV_MODE probe returning unexpected value — only fails if probe returns I2C error; data value is logged but does not fail init | Probe value tolerance |
| TC-TS-004 | `touchscreen_attach_irq(NULL, NULL)` → `TS_ERR_NULL`; `EXTI->IMR1` unchanged | Argument validation |
| TC-TS-005 | `touchscreen_attach_irq(cb, ctx)` → callback stored; `EXTI->IMR1` bit 5 set | Attach happy path |
| TC-TS-006 | Simulate EXTI5 firing → registered callback invoked once with stored context | ISR dispatch |
| TC-TS-007 | `touchscreen_read(NULL)` → `TS_ERR_NULL` | Argument validation |
| TC-TS-008 | `touchscreen_read()` with mocked TD_STATUS = 0 → `TS_ERR_NO_DATA`; no follow-up I2C reads issued | No-data path |
| TC-TS-009 | `touchscreen_read()` with TD_STATUS = 1 and coords (320, 240), event = press → `TS_ERR_OK`; touch.x = 320, touch.y = 240, touch.event = TS_EVENT_PRESS | Read happy path + coordinate decode |
| TC-TS-010 | `touchscreen_read()` with event flag P1_XH[7:6] = 0b01 → touch.event = TS_EVENT_RELEASE | Event flag decoding |
| TC-TS-011 | `touchscreen_read()` with event flag P1_XH[7:6] = 0b10 → touch.event = TS_EVENT_CONTACT | Event flag decoding |
| TC-TS-012 | `touchscreen_read()` with TD_STATUS read returning I2C error → `TS_ERR_I2C` | I2C error path |
| TC-TS-013 | `touchscreen_read()` with coordinate read returning I2C error → `TS_ERR_I2C` | I2C error path mid-transaction |
| TC-TS-014 | All public functions called before init → consistent error (`TS_ERR_I2C` for read since FT6x06 is in reset, `TS_ERR_NULL` for null args, `TS_ERR_OK` only after init) | Init guard semantics |

### 7.1 Integration tests — on target

| Test ID | Description |
|---|---|
| TC-TS-015 | After `lcd_init()` then `touchscreen_init()`, touch the screen at known points and verify the reported (x, y) matches within ±5 px |
| TC-TS-016 | Verify EXTI5 fires on touch press and again on release; confirm 100 touch events produce 200 ISR invocations (press + release) |
| TC-TS-017 | Long-press: hold finger steady for 1 s; verify TS_EVENT_PRESS once followed by TS_EVENT_CONTACT readings, then TS_EVENT_RELEASE on lift |

---

## 8. Open items

| ID | Item | Resolution | Status |
|---|---|---|---|
| TSD-O1 | FT6x06 IRQ pin and I2C address. | IRQ on PJ5 (EXTI5 → EXTI9_5_IRQHandler). I2C address 0x2A (7-bit) / 0x54 (8-bit write). | Resolved |
| TSD-O2 | FT6x06 variant. | FT6x06 family (FT6206 / FT6236 / FT6336) — register-compatible for single-touch operation. Driver works with any of them. | Resolved |
| TSD-O3 | Multi-touch support. | Out of scope — REQ-LD-000 and REQ-LD-100 require only tap-based interaction. Driver reads only point 1 (P1_*). | Resolved (single touch by design) |
| TSD-O4 | I2C driver address convention (7-bit vs 8-bit). | Pre-implementation check: inspect `i2c_driver.h`; use `0x2A` if the API takes 7-bit and shifts internally, `0x54` otherwise. | Resolved at implementation review |

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| TSD-D1 | Interrupt-driven (EXTI), not polled | FT6x06 trigger mode fires only on touch events; polling wastes CPU and needs a timer. EXTI is the architecturally correct approach. |
| TSD-D2 | `touchscreen_read()` called from task, not from ISR | ISR does the minimum (notify task); reading 5+ register bytes over I2C from an ISR would violate the ISR contract. |
| TSD-D3 | Single touch point only (read P1_*; ignore P2_*) | REQ-LD-000 / REQ-LD-100 require only tap navigation. Reduces register read size and parsing complexity. |
| TSD-D4 | Singleton — no handle | One FT6x06 per board; consistent with all driver companions. |
| TSD-D5 | `touchscreen_init()` post-condition on `lcd_init()` | PH7 ownership belongs to LcdDriver; FT6x06 NACKs while in reset. Ordering is a documented contract, not type-enforced. |
| TSD-D6 | Configure PJ5/EXTI directly via CMSIS, not through GpioDriver/ExtiDriver | PJ5 is a non-bus device-specific control line owned by this driver. Consistent with LCDD-D3 (PH7) and the project convention that bus-pins go through GpioDriver, device-specific lines do not. |
