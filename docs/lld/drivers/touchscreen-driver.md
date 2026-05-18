# TouchscreenDriver — LLD Companion

**Document:** `docs/lld/drivers/touchscreen-driver.md`
**Version:** 0.1 (draft — pending Luca review)
**Board scope:** Field Device (STM32F469) only
**Layer:** Driver
**Status:** Draft

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Reads touch coordinate events from the touchscreen controller | `components.md` |
| PROVIDES (upward) | `ITouchscreen` | `components.md` |
| USES (downward) | `I2cDriver` | `components.md` |
| Root requirement | REQ-LD-050 | `components.md` |
| Hardware | FT6206 (or equivalent) capacitive touch controller on I2C1 | UM1932 §4.14 |

**Note on REQ-LD-050:** `components.md` cites REQ-LD-050 for `TouchscreenDriver`. From `SRS.md`, REQ-LD-050 reads "The system shall refresh the displayed sensor readings at the polling rate or upon receiving new data" — this is a display refresh requirement, not a touchscreen requirement. The correct SRS traceability is REQ-LD-000 (navigation between screens via touch) and REQ-LD-100 (configuration screen interaction). This is a pre-existing traceability gap in `components.md`; noted here for the SRS fix backlog. The driver design is unaffected.

**Consumer:** `GraphicsLibrary` (middleware, LVGL wrapper). Called from `LcdUiTask`.

**ISR surface (task-breakdown.md §6.1):**
- `EXTI_touch_IRQHandler` — touchscreen IRQ pin asserted → `LcdUiTask` → `«notify»`

**Reset:** PH7 is shared with the LCD panel reset and is driven by `LcdDriver`. `touchscreen_init()` must be called after `lcd_init()` (which releases PH7 and allows the FT6206 to come out of reset).

---

## 2. Public API

### 2.1 Dependency-conformance check

`touchscreen_driver.h` includes only `stdint.h`, `stdbool.h`. It does NOT include `i2c_driver.h` in the header — I2C calls are made from `touchscreen_driver.c` via the internal `i2c_write_read()` interface. No FreeRTOS in the header. Confirmed clean.

### 2.2 Two-phase init

The EXTI IRQ callback requires `LcdUiTask` to exist. `touchscreen_init()` configures the FT6206 and the EXTI pin but does not enable the EXTI interrupt. `touchscreen_attach_irq()` registers the callback and enables the interrupt. Called from `LcdUiTask` startup prologue, consistent with the established two-phase pattern.

### 2.3 Data types

```c
typedef enum {
    TS_ERR_OK      = 0,
    TS_ERR_NO_DATA = 1, /**< No touch point active (called when no touch pending). */
    TS_ERR_I2C     = 2, /**< Underlying I2C transaction failed. */
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
 * Matches the 800×480 display resolution.
 */
typedef struct {
    uint16_t   x;
    uint16_t   y;
    ts_event_t event;
} ts_touch_t;

/**
 * @brief IRQ callback type. Called from EXTI ISR context.
 * Consumer calls xTaskNotifyFromISR internally.
 */
typedef void (*ts_irq_cb_t)(void *context);
```

### 2.4 Public API (`touchscreen_driver.h`)

```c
/**
 * @brief Initialise the FT6206 touchscreen controller.
 *
 * Pre-condition: lcd_init() must have been called first (PH7 released,
 * FT6206 out of reset).
 *
 * Configures the FT6206 via I2C: sets interrupt trigger mode (INT_TRIGGER),
 * gesture detection thresholds, and wakes the device from standby.
 * Configures the MCU EXTI line for the FT6206 IRQ pin (falling edge).
 * Does NOT enable the EXTI interrupt — call touchscreen_attach_irq() for that.
 *
 * @return TS_ERR_OK on success; TS_ERR_I2C if FT6206 configuration fails.
 */
ts_err_t touchscreen_init(void);

/**
 * @brief Register the IRQ callback and enable the EXTI interrupt.
 *
 * Called from LcdUiTask startup prologue (after scheduler has started).
 *
 * @param callback  Function called from ISR when the FT6206 IRQ fires.
 *                  Must not be NULL.
 * @param context   Opaque pointer passed unchanged to the callback.
 */
void touchscreen_attach_irq(ts_irq_cb_t callback, void *context);

/**
 * @brief Read the current touch point from the FT6206.
 *
 * Called from LcdUiTask after the IRQ callback has fired, to retrieve
 * the touch coordinates. Issues an I2C read of the FT6206 status and
 * touch point registers (P1_XH, P1_XL, P1_YH, P1_YL, P1_EVENT).
 *
 * Returns TS_ERR_NO_DATA if no active touch point is reported.
 *
 * @param[out] touch  Populated on TS_ERR_OK. Must not be NULL.
 * @return TS_ERR_OK, TS_ERR_NO_DATA, or TS_ERR_I2C.
 */
ts_err_t touchscreen_read(ts_touch_t *touch);
```

---

## 3. Internal design

### 3.1 Module-level state

```c
static ts_irq_cb_t s_irq_cb  = NULL;
static void       *s_irq_ctx = NULL;
static bool        s_initialised = false;
```

### 3.2 FT6206 register access

The FT6206 is a standard capacitive touch controller with an I2C register map. Reads use `i2c_write_read()` (single-byte register address write, then multi-byte read). The relevant registers:

| Register | Address | Description |
|---|---|---|
| `DEV_MODE` | 0x00 | Device mode (working, factory) |
| `GEST_ID` | 0x01 | Gesture ID (swipe, tap, etc.) |
| `TD_STATUS` | 0x02 | Number of active touch points (0..2) |
| `P1_XH` | 0x03 | Touch 1 X MSB + event flag |
| `P1_XL` | 0x04 | Touch 1 X LSB |
| `P1_YH` | 0x05 | Touch 1 Y MSB |
| `P1_YL` | 0x06 | Touch 1 Y LSB |
| `TH_GROUP` | 0x80 | Gesture detection threshold |
| `CTRL` | 0x86 | Control register (active/monitor mode) |
| `PERIOD_ACTIVE` | 0x88 | Active polling period |
| `INT_MODE` | 0xA4 | Interrupt mode (0 = polling, 1 = trigger) |

The driver configures `INT_MODE = 1` (trigger mode) at init so the IRQ pin asserts only on touch events, not periodically.

### 3.3 I2C address

FT6206 I2C address: **0x38** (7-bit). This is a standard fixed address. Confirm against the UM1932 schematic (TSD-O1).

### 3.4 EXTI IRQ

The FT6206 IRQ pin is an active-low, level-triggered or falling-edge-triggered signal. The MCU EXTI line is configured for falling edge in `touchscreen_init()`. The pin assignment must be confirmed (TSD-O1).

### 3.5 No FreeRTOS in driver

`EXTI_touch_IRQHandler` calls `s_irq_cb(s_irq_ctx)`. The callback (wired by GraphicsLibrary to `xTaskNotifyFromISR`) handles the RTOS notification. The driver holds only a function pointer.

---

## 4. Hardware contract

### 4.1 EXTI IRQ pin and I2C address (open item — TSD-O1)

The FT6206 IRQ output pin and its mapping to an MCU EXTI line must be confirmed from the UM1932 schematic. The touchscreen module connector CN10 (UM1932 §4.14) carries the IRQ signal — identify which MCU GPIO it connects to.

FT6206 7-bit I2C address is 0x38 (standard). Verify from UM1932 schematic (address pins are typically tied to GND → address 0x38).

### 4.2 PH7 reset ordering

`touchscreen_init()` MUST be called after `lcd_init()`. The FT6206 shares the PH7 reset line with the LCD panel IC. `lcd_init()` deasserts PH7 after the LCD panel is configured. Calling `touchscreen_init()` before `lcd_init()` would attempt to communicate with a device still in reset — all I2C transactions would NACK.

---

## 5. Sequence integration

`TouchscreenDriver` is not an explicit sequence diagram lifeline at HLD level. Its ISR is documented in task-breakdown.md §6.1. No SD changes required.

---

## 6. Error and fault behaviour

On `TS_ERR_I2C` from `touchscreen_init()`, the BringingUpLCD Init sub-state fails → Faulted. The LCD is essential (REQ-LD-000); touchscreen failure prevents configuration and alarm screen interaction and is non-recoverable at boot.

On `TS_ERR_I2C` from `touchscreen_read()` (runtime), GraphicsLibrary discards the touch event and logs via Logger. The display continues operating.

On `TS_ERR_NO_DATA`, GraphicsLibrary discards the call silently (spurious IRQ or race condition). No error logged — this is normal transient behaviour.

---

## 7. Unit-test plan

Host-platform tests. I2C transactions are mocked by substituting `i2c_write_read()` with a test stub. The EXTI IRQ is tested by calling `EXTI_touch_IRQHandler()` directly.

| ID | Test case | Expected result |
|---|---|---|
| T-TS-01 | `touchscreen_init` happy path: mock I2C returning ACK for all writes | INT_MODE register written 0x01; EXTI configured falling edge; returns TS_ERR_OK |
| T-TS-02 | `touchscreen_init` I2C NACK | Returns TS_ERR_I2C |
| T-TS-03 | `touchscreen_attach_irq`: callback stored; EXTI interrupt enabled | s_irq_cb set; EXTI_IMR bit set |
| T-TS-04 | `EXTI_touch_IRQHandler` fires: callback called with correct context | Mock callback invoked once |
| T-TS-05 | `touchscreen_read` with mock TD_STATUS = 1, coordinates (320, 240) | Returns TS_ERR_OK; touch.x = 320, touch.y = 240 |
| T-TS-06 | `touchscreen_read` with mock TD_STATUS = 0 | Returns TS_ERR_NO_DATA |
| T-TS-07 | `touchscreen_read` I2C failure | Returns TS_ERR_I2C |
| T-TS-08 | Event flag decoding: P1_XH[7:6] = 01b | ts_event = TS_EVENT_RELEASE |

Test file: `tests/drivers/test_touchscreen_driver.c`.

---

## 8. Open items

| ID | Item | Owner | Resolution path |
|---|---|---|---|
| TSD-O1 | FT6206 IRQ pin MCU mapping. Identify which GPIO/EXTI line the FT6206 IRQ signal connects to via CN10 (UM1932 §4.14 schematic). Confirm I2C address (expected 0x38). | Luca | Check UM1932 schematic at implementation |
| TSD-O2 | FT6206 variant. The board may use FT6206, FT6336, or OTM8009A's integrated touch controller. Identify the exact part from UM1932 BOM; download register map. The registers above are FT6206 — verify compatibility. | Luca | Check UM1932 BOM |

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| TSD-D1 | Interrupt-driven (EXTI), not polled | FT6206 INT_TRIGGER mode fires only on touch events; polling would waste CPU and require a timer. EXTI is the architecturally correct approach |
| TSD-D2 | `touchscreen_read()` called from task, not from ISR | ISR does only the minimum (notify task); reading 6 register bytes over I2C from an ISR would violate the ISR contract (task-breakdown.md §6) |
| TSD-D3 | Single touch point only | REQ-LD-000 through REQ-LD-150 require only tap-based navigation and configuration. Multi-touch is not required. Reduces register read and parsing complexity |
| TSD-D4 | Singleton — no handle | One FT6206 per board; consistent with all prior companions |
| TSD-D5 | `touchscreen_init()` pre-condition: lcd_init() must precede it | PH7 is owned by LcdDriver; calling touchscreen_init() before lcd_init() would attempt I2C to a device in reset. This is a documented ordering contract, not enforced by the type system |
