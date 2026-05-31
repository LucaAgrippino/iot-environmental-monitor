# LedDriver — LLD Companion

**Document:** `docs/lld/drivers/led-driver.md`
**Version:** 0.2
**Board scope:** Field Device (STM32F469) and Gateway (B-L475E-IOT01A)
**Layer:** Driver
**Status:** Approved for implementation
**Date:** May 2026

**HLD anchor:** LedDriver in `components.md` (FD + GW driver layer)

---

## 1. Sources

| Attribute | Field Device | Gateway |
|---|---|---|
| Responsibility | Controls the on/off state of the board LEDs | Controls the on/off state of the board LEDs |
| PROVIDES (upward) | `ILed` | `ILed` |
| USES (downward) | `GpioDriver` | `GpioDriver` |
| Root requirement | REQ-LD-250 | REQ-LD-250 |

**Consumer:** `HealthMonitor` (Application) on both boards. It drives LEDs to
indicate device health state (idle, acquiring, alarm, error).

**This is the first driver that `USES` another driver** rather than CMSIS
directly. LedDriver consumes `IGpio` from GpioDriver; it never accesses GPIO
registers directly. This is correct per the established project convention:
bus drivers self-configure via CMSIS; non-bus control-line drivers (including
LED drivers) consume `IGpio`.

**Physical LEDs — Field Device (STM32F469):**

| LED | Colour | GPIO pin | Active level | Notes |
|---|---|---|---|---|
| LD3 | Green | PG6 | Low | Common anode → GND via resistor; GPIO low = on |
| LD4 | Red | PD5 | Low | Same circuit as LD3 |

**Physical LEDs — Gateway (STM32L475):**

| LED | Colour | GPIO pin | Active level | Notes |
|---|---|---|---|---|
| LD2 | Green | PB14 | High | Cathode → GND; MCU drives anode via resistor; GPIO high = on |

**Gateway LED constraints (from UM2153 schematic):**

- **LD1 (PA5):** Shared with SPI1_SCK/ARD.D13 via solder bridge SB1. Not
  usable without hardware modification — omitted from the Gateway pin table.
- **LD3/LD4 (PC9):** Wi-Fi and BLE activity LEDs driven from the same pin;
  not independently controllable by the MCU application — omitted.
- The Gateway therefore has **one fitted user LED** (LD2, PB14).

**The two boards use opposite active levels.** This is a key driver of the
LED-D5 dependency-injection design decision (§9).

---

## 2. Public API

### 2.1 Dependency-conformance check

`led_driver.h` includes `stdint.h` and `stdbool.h`. No CMSIS or GPIO register
includes. GPIO access goes through the `IGpio` interface. Confirmed clean.

### 2.2 P3 consideration

Single consumer (`HealthMonitor`). No ISP split warranted.

### 2.3 Types

```c
/**
 * @brief LED logical identifier.
 *
 * LED_GREEN is present on both boards.
 * LED_RED is present on the Field Device only. Calling any API function
 * with LED_RED on the Gateway (where the pin table marks it not fitted)
 * returns LED_ERR_INVALID_ID.
 */
typedef enum {
    LED_GREEN = 0U, /**< Green status LED. */
    LED_RED   = 1U, /**< Red alarm LED.   */
    LED_COUNT       /**< Sentinel — never pass to API functions. */
} led_id_t;

/**
 * @brief Error codes returned by LedDriver operations.
 */
typedef enum {
    LED_ERR_OK          = 0, /**< Operation succeeded. */
    LED_ERR_INVALID_ID  = 1, /**< LED not fitted on this board. */
    LED_ERR_NOT_INIT    = 2, /**< led_init() not yet called. */
    LED_ERR_NULL_ARG    = 3, /**< NULL argument passed. */
} led_err_t;

/**
 * @brief Hardware descriptor for one LED pin.
 *
 * The caller (board-level config) provides an array of these to led_init().
 * The driver reads polarity from the active_high field — it never hard-codes
 * board identity or active-level assumptions internally.
 */
typedef struct {
    uint8_t port;         /**< GPIO port identifier (gpio_port_t from gpio_driver.h). */
    uint8_t pin;          /**< GPIO pin number (0–15). */
    bool    active_high;  /**< true → GPIO high = LED on; false → GPIO low = LED on. */
    bool    fitted;       /**< false → API calls for this ID return LED_ERR_INVALID_ID. */
} led_pin_t;
```

### 2.4 Public API (`led_driver.h`)

```c
/**
 * @brief Initialise LedDriver from a caller-supplied pin table.
 *
 * Configures the GPIO pins for all fitted LEDs via GpioDriver and sets the
 * initial state to off. Must be called once, after gpio_init().
 *
 * The pin table is owned by the caller (typically board_config.c or the
 * integration-test main). The driver stores only a pointer — the caller
 * must ensure the array's lifetime exceeds the driver's lifetime (static
 * storage in practice).
 *
 * @param pins   Array of LED pin descriptors. One entry per led_id_t,
 *               indexed by the led_id_t value.
 * @param count  Number of entries in @p pins. Must equal LED_COUNT.
 * @return LED_ERR_OK on success; LED_ERR_NULL_ARG if @p pins is NULL;
 *         LED_ERR_INVALID_ID if @p count != LED_COUNT.
 * @note Threading: task-context only, non-blocking. Call before the
 *       scheduler starts.
 */
led_err_t led_init(const led_pin_t *pins, uint8_t count);

/**
 * @brief Turn an LED on.
 *
 * @param id  LED to turn on.
 * @return LED_ERR_OK; LED_ERR_NOT_INIT; LED_ERR_INVALID_ID.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
led_err_t led_on(led_id_t id);

/**
 * @brief Turn an LED off.
 *
 * @param id  LED to turn off.
 * @return LED_ERR_OK; LED_ERR_NOT_INIT; LED_ERR_INVALID_ID.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
led_err_t led_off(led_id_t id);

/**
 * @brief Toggle an LED.
 *
 * @param id  LED to toggle.
 * @return LED_ERR_OK; LED_ERR_NOT_INIT; LED_ERR_INVALID_ID.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
led_err_t led_toggle(led_id_t id);

/**
 * @brief Query the current logical state of an LED.
 *
 * @param id     LED to query.
 * @param state  Output — LED_STATE_ON or LED_STATE_OFF.
 * @return LED_ERR_OK; LED_ERR_NOT_INIT; LED_ERR_INVALID_ID;
 *         LED_ERR_NULL_ARG if @p state is NULL.
 * @note Threading: task-context only, non-blocking.
 */
led_err_t led_get_state(led_id_t id, led_state_t *state);
```

Additionally expose the LED state enum:

```c
typedef enum {
    LED_STATE_OFF = 0U,
    LED_STATE_ON  = 1U
} led_state_t;
```

### 2.5 Board pin-table convention

The pin tables are defined at board-configuration scope, NOT inside the
driver. Each board's integration-test main (and eventually `board_config.c`)
defines its own table as a `static const` array:

```c
/* --- Field Device (STM32F469) board pin table --- */
static const led_pin_t k_fd_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_G, .pin =  6U,
                   .active_high = false, .fitted = true},
    [LED_RED]   = {.port = GPIO_PORT_D, .pin =  5U,
                   .active_high = false, .fitted = true},
};

/* --- Gateway (STM32L475) board pin table --- */
static const led_pin_t k_gw_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_B, .pin = 14U,
                   .active_high = true, .fitted = true},
    [LED_RED]   = {.port = GPIO_PORT_A, .pin =  5U,
                   .active_high = true, .fitted = false},  /* LD1/SB1 — not usable */
};
```

---

## 3. Internal design

### 3.1 Module structure

```
led_driver.h   — public types, API, led_pin_t definition
led_driver.c   — singleton state, init, set/toggle/query logic
```

### 3.2 Singleton state

```c
typedef struct {
    const led_pin_t *pins;        /**< Caller-supplied pin table (pointer only). */
    uint8_t          count;       /**< Number of entries; validated == LED_COUNT. */
    led_state_t      state[LED_COUNT]; /**< Current logical state per LED. */
    bool             initialised;
} led_driver_t;

static led_driver_t s_led;
```

The driver stores only a pointer to the caller's pin table — no copy. This
is valid because the pin tables are `static const` at board-config scope.

### 3.3 led_init flow

```
1. Validate pins != NULL; count == LED_COUNT → LED_ERR_NULL_ARG / INVALID_ID.
2. Store pointer + count in s_led.
3. For each entry i in 0..LED_COUNT-1:
     a. If !pins[i].fitted → skip.
     b. gpio_configure(port, pin, OUTPUT_PUSHPULL, SPEED_LOW, PULL_NONE).
     c. Write the GPIO level for "off":
          active_high → gpio_write(port, pin, GPIO_LEVEL_LOW)
          !active_high → gpio_write(port, pin, GPIO_LEVEL_HIGH)
     d. s_led.state[i] = LED_STATE_OFF.
4. s_led.initialised = true.
5. Return LED_ERR_OK.
```

### 3.4 led_on / led_off / led_toggle flow

All three follow the same guard pattern:

```
1. If !s_led.initialised → LED_ERR_NOT_INIT.
2. If id >= LED_COUNT → LED_ERR_INVALID_ID.
3. If !s_led.pins[id].fitted → LED_ERR_INVALID_ID.
4. Compute target GPIO level from active_high + desired state.
   led_on:     target = active_high ? HIGH : LOW
   led_off:    target = active_high ? LOW  : HIGH
   led_toggle: invert s_led.state[id] → derive target
5. gpio_write(port, pin, target).
6. Update s_led.state[id].
7. Return LED_ERR_OK.
```

The `active_high` field is read from the injected pin table at runtime — the
driver contains zero board-conditional `#if` directives.

### 3.5 Principles applied

- **P1 (Strict directional layering).** Depends only on GpioDriver (IGpio);
  no RTOS, no middleware dependencies.
- **P2 (Dependency Inversion).** Exposes `iled_t` vtable; consumers depend on
  `ILed`, not the concrete driver.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static singleton
  state; pointer to caller's static pin table; no heap.
- **P6 (Responsibility traces to requirements).** LED control traces to
  REQ-LD-250.
- **P8 (Total error propagation, no silent failures).** Every public function
  returns `led_err_t`; no fallthrough on invalid ID.
- **P9 (BARR-C coding standard).** Fixed-width types; explicit active-level
  handling; no implicit widening.
- **P10 (Naming conventions).** Prefix `led_`; interface `iled_t`; errors
  `LED_ERR_*`.

---

## 4. Hardware contract

### 4.1 Pin configuration (applied by led_init via GpioDriver)

| Setting | Value | Reason |
|---|---|---|
| Mode | Output push-pull | Must actively drive both levels |
| Speed | Low | No timing requirement; reduces EMI |
| Pull | None | External resistor provides the return path |
| Initial state | Off (via active-level logic) | Safe default at boot |

### 4.2 Init ordering requirement

`gpio_init()` must complete before `led_init()` is called. Enforced by the
boot sequence in `main()` / `board_init()`.

### 4.3 L475 notes

- **LED_RED is not fitted on the Gateway.** The driver returns
  `LED_ERR_INVALID_ID` for any call with `LED_RED` on the Gateway pin table.
  `HealthMonitor` must handle this gracefully (log at WARN and continue).
- **LD1 (PA5) is explicitly excluded** — SB1 solder bridge connects PA5 to
  SPI1_SCK/ARD.D13. Using it as a user LED would conflict with any SPI
  peripheral usage and is not permitted without hardware modification.

---

## 5. Sequence integration

LedDriver has no HLD-level sequence diagram surface. LED state changes are
side-effects of lifecycle and health transitions, not primary actors in any
HLD sequence. No changes to `sequence-diagrams.md` are required.

---

## 6. Error and fault behaviour

| Error | Cause | Local behaviour | Caller responsibility |
|---|---|---|---|
| `LED_ERR_NULL_ARG` | `led_init()` called with `pins == NULL` or `led_get_state()` with `state == NULL` | Return error; no state change | Assert in debug; log + halt |
| `LED_ERR_INVALID_ID` | `led_id_t` not fitted; `LED_COUNT` passed; `count != LED_COUNT` in init | Return error; no GPIO change | Log at WARN; continue |
| `LED_ERR_NOT_INIT` | Any API called before `led_init()` | Return error; no GPIO access | Assert in debug |

No internal retry. No silent failures.

---

## 7. Unit-test plan

Host-platform tests (Unity). GpioDriver calls intercepted via
`gpio_driver_stub.h` (stub functions recording calls and returning
`GPIO_ERR_OK`). Each test file passes its own static pin table to
`led_init()` — no board-conditional `#if` in the test files.

**Field Device tests** (`tests/field-device/drivers/led/test_led_driver_fd.c`):

```c
static const led_pin_t k_test_pins_fd[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_G, .pin =  6U, .active_high = false, .fitted = true},
    [LED_RED]   = {.port = GPIO_PORT_D, .pin =  5U, .active_high = false, .fitted = true},
};
```

**Gateway tests** (`tests/field-device/drivers/led/test_led_driver_gw.c`):

```c
static const led_pin_t k_test_pins_gw[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_B, .pin = 14U, .active_high = true, .fitted = true},
    [LED_RED]   = {.port = GPIO_PORT_A, .pin =  5U, .active_high = true, .fitted = false},
};
```

| ID | Test case | Expected result |
|---|---|---|
| TC-LED-FD-01 | `led_init(k_test_pins_fd, LED_COUNT)` — verify configure called for both LEDs; initial state off (GPIO_LEVEL_HIGH for active-low) | Two `gpio_configure` calls; two `gpio_write(HIGH)` calls |
| TC-LED-FD-02 | `led_on(LED_GREEN)` — active-low; verify `gpio_write(LOW)` on PG6 | GPIO driven LOW |
| TC-LED-FD-03 | `led_off(LED_GREEN)` — verify `gpio_write(HIGH)` on PG6 | GPIO driven HIGH |
| TC-LED-FD-04 | `led_toggle(LED_GREEN)` from off state — verify `gpio_write(LOW)` on PG6 | GPIO driven LOW; state updated to ON |
| TC-LED-FD-05 | `led_on(LED_RED)` — verify `gpio_write(LOW)` on PD5 | GPIO driven LOW |
| TC-LED-FD-06 | `led_get_state(LED_GREEN)` after `led_on` — verify returns `LED_STATE_ON` | State matches driver internal state |
| TC-LED-GW-01 | `led_init(k_test_pins_gw, LED_COUNT)` — one fitted LED; initial state off (GPIO_LEVEL_LOW for active-high) | One `gpio_configure` call; one `gpio_write(LOW)` call |
| TC-LED-GW-02 | `led_on(LED_GREEN)` — active-high; verify `gpio_write(HIGH)` on PB14 | GPIO driven HIGH |
| TC-LED-GW-03 | `led_off(LED_GREEN)` — verify `gpio_write(LOW)` on PB14 | GPIO driven LOW |
| TC-LED-GW-04 | `led_on(LED_RED)` on Gateway — verify `LED_ERR_INVALID_ID`; no GPIO call | Returns error; stub records zero write calls |
| TC-LED-GW-05 | `led_toggle(LED_GREEN)` from on state — verify `gpio_write(LOW)` on PB14 | GPIO driven LOW; state updated to OFF |
| TC-LED-CMN-01 | `led_init(NULL, LED_COUNT)` — verify `LED_ERR_NULL_ARG` | Returns error; not initialised |
| TC-LED-CMN-02 | `led_init(valid_pins, LED_COUNT - 1U)` — wrong count; verify `LED_ERR_INVALID_ID` | Returns error |
| TC-LED-CMN-03 | `led_on(LED_COUNT)` — out-of-range id; verify `LED_ERR_INVALID_ID` | No GPIO call |
| TC-LED-CMN-04 | `led_on(LED_GREEN)` before `led_init()` — verify `LED_ERR_NOT_INIT` | No GPIO call |

---

## 8. Open items

| ID | Item | Owner | Resolution path |
|---|---|---|---|
| LED-O1 | `task-breakdown.md` has no LedDriver entry (no task — purely synchronous). Confirm no update needed. | Luca | Verify at HealthMonitor PR time |
| LED-O2 | L475 has no red LED. HealthMonitor must handle `LED_ERR_INVALID_ID` gracefully on Gateway. Verify HealthMonitor companion covers this case. | Luca | Check HealthMonitor companion §6 |

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| LED-D1 | LedDriver consumes `IGpio`; never accesses GPIO registers directly | Single responsibility — LedDriver knows LED semantics; GpioDriver knows GPIO registers. Consistent with established project convention |
| LED-D2 | Shared `led_id_t` enum on both boards; `LED_ERR_INVALID_ID` for missing LEDs | Single header; HealthMonitor handles the error gracefully. Alternative (separate enums per board) forces `#ifdef` into HealthMonitor — worse than an error return |
| LED-D3 | Active-level polarity encapsulated in the injected pin table | P1: polarity knowledge must not leak to the caller. Changing a board's active level requires only editing the pin table, not the driver |
| LED-D4 | Singleton module (no handle parameter) | Two or fewer LEDs per board; consistent with all prior companion decisions (GpioDriver, RtcDriver, Logger) |
| LED-D5 | `led_init()` takes a caller-supplied `const led_pin_t *` pin table instead of a compile-time internal table | Removes all `#if defined(STM32F469xx)` conditionals from the driver. The driver contains zero board-identity logic — it is a pure implementation of LED semantics over any GPIO-backed pin map. Board wiring belongs at board-configuration scope (board_config.c or the integration-test main), not in the driver. Adding a third board requires only a new pin table, not a driver edit. Test files pass their own pin tables, eliminating board-conditional `#if` from test code too |
