# LedDriver — LLD Companion

**Document:** `docs/lld/drivers/led-driver-lld.md`
**Version:** 0.3 (Gateway implemented and hardware-validated)
**Board scope:** Field Device (STM32F469) and Gateway (B-L475E-IOT01A)
**Layer:** Driver
**Status:** Implementation-ready — GW implemented and hardware-validated (2026-10-11); FD still on the v0.1 API
**Date:** October 2026

**HLD anchor:** LedDriver in `components.md` (FD + GW driver layer)

---

## 1. Sources

| Attribute | Field Device | Gateway |
|---|---|---|
| Responsibility | Controls the on/off and blink state of the board LEDs | Controls the on/off and blink state of the board LEDs |
| PROVIDES (upward) | ILed | ILed |
| USES (downward) | GpioDriver, FreeRTOS software timers | GpioDriver, FreeRTOS software timers |
| Root requirement | REQ-LD-200 | REQ-LD-200 |

**Consumer:** `HealthMonitor` (Application) on both boards. It drives LEDs
to indicate device health state (Init, Operational, Alarm, Fault) via blink
patterns.

**Driver-to-driver dependency.** LedDriver consumes `IGpio` from GpioDriver;
it never accesses GPIO registers directly. This is correct per the
established project convention: bus drivers self-configure via CMSIS;
non-bus control-line drivers (including LED drivers) consume `IGpio`.

**Physical LEDs — Field Device (STM32F469):**

| LED | Colour | GPIO pin | Active level | Notes |
|---|---|---|---|---|
| LD3 | Green | PG6 | Low | Common anode; GPIO low = on |
| LD4 | Red | PD5 | Low | Same circuit as LD3 |

**Physical LEDs — Gateway (STM32L475):**

| LED | Colour | GPIO pin | Active level | Notes |
|---|---|---|---|---|
| LD2 | Green | PB14 | High | Cathode → GND; GPIO high = on |

**Gateway LED constraints (from UM2153 schematic):**

- **LD1 (PA5):** Shared with SPI1_SCK/ARD.D13 via solder bridge SB1. Not
  usable without hardware modification — omitted.
- **LD3/LD4 (PC9):** Wi-Fi and BLE activity LEDs driven from the same pin;
  not independently controllable — omitted.
- The Gateway therefore has **one fitted user LED** (LD2, PB14).

**The two boards use opposite active levels.** The pin-table injection
design (LED-D5) handles this without conditional compilation.

---

## 2. Traceability

| Concern | SRS requirement | API function |
|---|---|---|
| LED state control | REQ-LD-200 | `led_set()` |
| Init LED pattern | REQ-LD-200 | `led_set(id, LED_PATTERN_BLINK_MEDIUM)` |
| Health status indication | REQ-LD-200 (gap F-07) | `led_set()` called by HealthMonitor |

**Note:** REQ-LD-200 currently reads *"display a splash screen on the LCD
during the boot phase"* — the LED-indication requirement is tracked as gap
F-07 in `components.md`. A future SRS update should add a dedicated LED
requirement. The driver is built to this gap requirement.

---

## 3. Public API

### 3.1 Dependency-conformance check

`led_driver.h` includes `stdint.h`, `stdbool.h`, and `FreeRTOS.h` /
`timers.h` (for `TimerHandle_t` used internally). No CMSIS or GPIO register
includes. GPIO access goes through `IGpio`. Confirmed clean.

### 3.2 P3 consideration

Single consumer (`HealthMonitor`). No ISP split warranted.

### 3.3 Types

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
    LED_RED   = 1U, /**< Red alarm LED.    */
    LED_COUNT       /**< Sentinel — never pass to API functions. */
} led_id_t;

/**
 * @brief LED output pattern.
 *
 * Blink periods are full on-off cycles. The timer fires at half the
 * period (e.g. BLINK_FAST fires every 100 ms for a 200 ms full cycle).
 */
typedef enum {
    LED_PATTERN_OFF          = 0U, /**< Solid off.                    */
    LED_PATTERN_ON           = 1U, /**< Solid on.                     */
    LED_PATTERN_BLINK_FAST   = 2U, /**< 200 ms full cycle (5 Hz).     */
    LED_PATTERN_BLINK_MEDIUM = 3U, /**< 500 ms full cycle (2 Hz).     */
    LED_PATTERN_BLINK_SLOW   = 4U, /**< 1000 ms full cycle (1 Hz).    */
    LED_PATTERN_COUNT              /**< Sentinel — never pass to API. */
} led_pattern_t;

/**
 * @brief Error codes returned by LedDriver operations.
 */
typedef enum {
    LED_ERR_OK            = 0, /**< Operation succeeded.                */
    LED_ERR_INVALID_ID    = 1, /**< LED not fitted on this board.       */
    LED_ERR_NOT_INIT      = 2, /**< led_init() not yet called.          */
    LED_ERR_NULL_ARG      = 3, /**< NULL argument passed.               */
    LED_ERR_INVALID_PARAM = 4, /**< Pattern value out of range.         */
} led_err_t;

/**
 * @brief Hardware descriptor for one LED pin.
 *
 * The caller (board-level config) provides an array of these to led_init().
 * The driver reads polarity from the active_high field — it never
 * hard-codes board identity or active-level assumptions internally.
 */
typedef struct {
    uint8_t port;         /**< GPIO port identifier (gpio_port_t). */
    uint8_t pin;          /**< GPIO pin number (0–15).             */
    bool    active_high;  /**< true → GPIO high = LED on.          */
    bool    fitted;       /**< false → API calls return LED_ERR_INVALID_ID. */
} led_pin_t;
```

### 3.4 Public API (`led_driver.h`)

```c
/**
 * @brief Initialise LedDriver from a caller-supplied pin table.
 *
 * Configures GPIO pins for all fitted LEDs via GpioDriver, sets each
 * LED to off, and creates one static FreeRTOS software timer per fitted
 * LED (for blink support). Must be called once after gpio_init().
 *
 * The pin table is owned by the caller (typically board_config.c or the
 * integration-test main). The driver stores only a pointer — the caller
 * must ensure the array's lifetime exceeds the driver's.
 *
 * @param pins   Array of LED pin descriptors, indexed by led_id_t.
 * @param count  Number of entries. Must equal LED_COUNT.
 * @return LED_ERR_OK on success; LED_ERR_NULL_ARG if pins is NULL;
 *         LED_ERR_INVALID_ID if count != LED_COUNT.
 * @note Threading: task-context only, non-blocking. Call before the
 *       scheduler starts.
 */
led_err_t led_init(const led_pin_t *pins, uint8_t count);

/**
 * @brief Set the output pattern for an LED.
 *
 * Solid patterns (ON, OFF) take effect immediately. Blink patterns
 * start the LED in the on-state and begin the blink timer. Switching
 * from one blink pattern to another restarts the timer with the new
 * period. Switching from blink to solid stops the timer.
 *
 * May be called before the scheduler starts — timer commands are queued
 * and processed when the scheduler begins (LED-D9). During the
 * pre-scheduler window the LED is set to the on-level but does not
 * blink until the timer daemon task starts.
 *
 * @param id       LED to control.
 * @param pattern  Desired output pattern.
 * @return LED_ERR_OK; LED_ERR_NOT_INIT; LED_ERR_INVALID_ID;
 *         LED_ERR_INVALID_PARAM if pattern >= LED_PATTERN_COUNT.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
led_err_t led_set(led_id_t id, led_pattern_t pattern);

/**
 * @brief Query the current pattern of an LED.
 *
 * @param id       LED to query.
 * @param pattern  Output — receives the active pattern.
 * @return LED_ERR_OK; LED_ERR_NOT_INIT; LED_ERR_INVALID_ID;
 *         LED_ERR_NULL_ARG if pattern is NULL.
 * @note Threading: task-context only, non-blocking.
 */
led_err_t led_get_pattern(led_id_t id, led_pattern_t *pattern);

/**
 * @brief Reset all driver state for unit-test isolation.
 *
 * Stops all blink timers, zeroes the singleton, sets initialised to
 * false. Call in setUp() of every test file.
 *
 * @note Production code must never call this function.
 */
void led_reset_for_test(void);
```

### 3.5 Board pin-table convention

Pin tables are defined at board-configuration scope, NOT inside the driver.
Each board's `board_config.c` (and each integration-test main) defines its
own table as a `static const` array:

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
                   .active_high = true,  .fitted = true},
    [LED_RED]   = {.port = GPIO_PORT_A, .pin =  5U,
                   .active_high = true,  .fitted = false},  /* LD1/SB1 — not usable */
};
```

---

## 4. Internal design

### 4.1 Module structure

```
led_driver.h   — public types, API, led_pin_t definition
led_driver.c   — singleton state, init, pattern logic, blink timer callback
```

### 4.2 Singleton state

```c
/* led_driver.c */

#define LED_TIMER_NAME_LEN  8U  /* "LedN\0" */

typedef struct {
    const led_pin_t *pins;                   /**< Caller-supplied pin table. */
    uint8_t          count;                  /**< Validated == LED_COUNT.    */
    led_pattern_t    pattern[LED_COUNT];      /**< Current pattern per LED.  */
    bool             gpio_on[LED_COUNT];      /**< Current GPIO logical on.  */
    TimerHandle_t    timer[LED_COUNT];        /**< Blink timer per LED.      */
    StaticTimer_t    timer_buf[LED_COUNT];    /**< Static timer storage.     */
    bool             initialised;
} led_driver_t;

static led_driver_t s_led;
```

The driver stores only a pointer to the caller's pin table — no copy.
Valid because pin tables are `static const` at board-config scope.

### 4.3 Blink period lookup

```c
static TickType_t blink_half_period(led_pattern_t pattern)
{
    switch (pattern) {
        case LED_PATTERN_BLINK_FAST:   return pdMS_TO_TICKS(100U);
        case LED_PATTERN_BLINK_MEDIUM: return pdMS_TO_TICKS(250U);
        case LED_PATTERN_BLINK_SLOW:   return pdMS_TO_TICKS(500U);
        default:                       return pdMS_TO_TICKS(500U);
    }
}
```

Half-period values: the timer fires once per half-cycle, toggling the LED
on each firing.

### 4.4 GPIO helpers

```c
static void led_gpio_on(led_id_t id)
{
    gpio_level_t lvl = s_led.pins[id].active_high
                     ? GPIO_LEVEL_HIGH : GPIO_LEVEL_LOW;
    gpio_write(s_led.pins[id].port, s_led.pins[id].pin, lvl);
    s_led.gpio_on[id] = true;
}

static void led_gpio_off(led_id_t id)
{
    gpio_level_t lvl = s_led.pins[id].active_high
                     ? GPIO_LEVEL_LOW : GPIO_LEVEL_HIGH;
    gpio_write(s_led.pins[id].port, s_led.pins[id].pin, lvl);
    s_led.gpio_on[id] = false;
}
```

### 4.5 Blink timer callback

```c
static void blink_callback(TimerHandle_t timer)
{
    led_id_t id = (led_id_t)(uintptr_t)pvTimerGetTimerID(timer);

    /* Guard: pattern may have been changed to solid since this
     * timer was started. One residual firing after xTimerStop()
     * is possible — the guard prevents a visible glitch. */
    if (s_led.pattern[id] < LED_PATTERN_BLINK_FAST) {
        return;
    }

    /* Toggle GPIO */
    if (s_led.gpio_on[id]) {
        led_gpio_off(id);
    } else {
        led_gpio_on(id);
    }
}
```

The callback runs in the FreeRTOS timer daemon task context. It performs a
single `gpio_write()` — a non-blocking register write — so it is safe and
fast. No mutex is needed because `gpio_write()` targets a distinct pin per
LED, and the BSRR register supports atomic bit-level access.

**Residual-fire guard.** `xTimerStop()` is asynchronous — the timer may
fire once more after the stop command is sent. The pattern check at the
top of the callback prevents this residual firing from producing a visible
glitch (LED-D6).

### 4.6 led_init flow

```
1. Validate pins != NULL  → LED_ERR_NULL_ARG.
2. Validate count == LED_COUNT → LED_ERR_INVALID_ID.
3. Store pointer + count in s_led.
4. For each i in 0..LED_COUNT-1:
     a. If !pins[i].fitted → skip (no GPIO configure, no timer).
     b. gpio_configure(port, pin, OUTPUT_PUSHPULL, SPEED_LOW, PULL_NONE).
     c. led_gpio_off(i)  — safe default at boot.
     d. s_led.pattern[i] = LED_PATTERN_OFF.
     e. Create static timer:
        s_led.timer[i] = xTimerCreateStatic(
            "LedN",                       /* name (N = id) */
            pdMS_TO_TICKS(500U),          /* default period — overwritten by led_set */
            pdTRUE,                       /* auto-reload */
            (void *)(uintptr_t)i,         /* timer ID = led_id_t */
            blink_callback,
            &s_led.timer_buf[i]);
5. s_led.initialised = true.
6. Return LED_ERR_OK.
```

### 4.7 led_set flow

```
1. If !s_led.initialised     → LED_ERR_NOT_INIT.
2. If id >= LED_COUNT        → LED_ERR_INVALID_ID.
3. If !s_led.pins[id].fitted → LED_ERR_INVALID_ID.
4. If pattern >= LED_PATTERN_COUNT → LED_ERR_INVALID_PARAM.
5. s_led.pattern[id] = pattern.
6. Switch on pattern:
     LED_PATTERN_OFF:
       xTimerStop(s_led.timer[id], 0).
       led_gpio_off(id).

     LED_PATTERN_ON:
       xTimerStop(s_led.timer[id], 0).
       led_gpio_on(id).

     LED_PATTERN_BLINK_*:
       led_gpio_on(id).           /* start in the on-state */
       xTimerChangePeriod(s_led.timer[id],
                          blink_half_period(pattern), 0).
       /* xTimerChangePeriod starts the timer if stopped,
        * restarts it if already running — one call covers
        * both start and period-change transitions. */
7. Return LED_ERR_OK.
```

### 4.8 led_get_pattern flow

```
1. If !s_led.initialised     → LED_ERR_NOT_INIT.
2. If id >= LED_COUNT        → LED_ERR_INVALID_ID.
3. If !s_led.pins[id].fitted → LED_ERR_INVALID_ID.
4. If pattern == NULL        → LED_ERR_NULL_ARG.
5. *pattern = s_led.pattern[id].
6. Return LED_ERR_OK.
```

### 4.9 led_reset_for_test flow

```
1. For each i in 0..LED_COUNT-1:
     If s_led.timer[i] != NULL:
       xTimerStop(s_led.timer[i], 0).
2. memset(&s_led, 0, sizeof(s_led)).
```

### 4.10 Principles applied

- **P1 (Strict directional layering).** Depends only on GpioDriver (IGpio)
  and FreeRTOS timers. No middleware, no upward dependencies.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static
  singleton; `StaticTimer_t` storage; pointer to caller's static pin table;
  no heap.
- **P6 (Responsibility traces to requirements).** LED control traces to
  REQ-LD-200 (gap F-07).
- **P8 (Total error propagation, no silent failures).** Every public
  function returns `led_err_t`; no fallthrough on invalid ID or pattern.
- **P9 (BARR-C coding standard).** Fixed-width types; explicit active-level
  handling; no implicit widening.
- **P10 (Naming conventions).** Prefix `led_`; errors `LED_ERR_*`; patterns
  `LED_PATTERN_*`.

**P2 note (DIP).** LedDriver is a singleton driver — no ADT handle, no
vtable (LED-D8). HealthMonitor depends on the concrete module via
`#include "led_driver.h"` and calls `led_set()` directly. This is
acceptable because LedDriver has no alternative implementations — the DIP
seam is the header boundary. The HealthMonitor-to-LedDriver link is
Application → Driver (downward, P1-compliant).

---

## 5. Hardware contract

### 5.1 Pin configuration (applied by led_init via GpioDriver)

| Setting | Value | Reason |
|---|---|---|
| Mode | Output push-pull | Must actively drive both levels |
| Speed | Low | No timing requirement; reduces EMI |
| Pull | None | External resistor provides the return path |
| Initial state | Off (via active-level logic) | Safe default at boot |

### 5.2 Init ordering requirement

`gpio_init()` must complete before `led_init()` is called. `led_init()`
must be called before the scheduler starts (timer creation is safe
pre-scheduler; timer commands are queued and processed at scheduler start).

### 5.3 L475 notes

- **LED_RED is not fitted on the Gateway.** The driver returns
  `LED_ERR_INVALID_ID` for any call with `LED_RED` on the Gateway pin
  table. `HealthMonitor` must handle this gracefully (log WARN, continue).
- **LD1 (PA5) is explicitly excluded** — SB1 solder bridge connects PA5
  to SPI1_SCK/ARD.D13. Using it as a user LED would conflict with SPI.

---

## 6. Sequence integration

LedDriver has no HLD-level sequence diagram surface. LED state changes
are side-effects of lifecycle and health transitions driven by
HealthMonitor's `update_led_state()`, not primary actors in any HLD
sequence. No changes to `sequence-diagrams.md` are required.

---

## 7. Error and fault behaviour

| Error | Cause | Local behaviour | Caller responsibility |
|---|---|---|---|
| `LED_ERR_NULL_ARG` | `led_init(pins == NULL)` or `led_get_pattern(pattern == NULL)` | Return error; no state change | Assert in debug; log + halt |
| `LED_ERR_INVALID_ID` | `led_id_t` not fitted; `LED_COUNT` passed; `count != LED_COUNT` in init | Return error; no GPIO change | Log WARN; continue |
| `LED_ERR_NOT_INIT` | Any API called before `led_init()` | Return error; no GPIO access | Assert in debug |
| `LED_ERR_INVALID_PARAM` | `pattern >= LED_PATTERN_COUNT` | Return error; no state change | Assert in debug |

No internal retry. No silent failures. No health events — LedDriver is
too low in the stack to report through HealthMonitor (it would be circular).

---

## 8. Unit-test plan

Host-platform tests (Unity + CMock). `gpio_driver.h` mocked via CMock
(auto-generated). FreeRTOS timer functions (`xTimerCreateStatic`,
`xTimerChangePeriod`, `xTimerStop`, `pvTimerGetTimerID`) mocked via CMock.

Each test file passes its own static pin table to `led_init()` — no
board-conditional `#if` in the test files. `led_reset_for_test()` called
in `setUp()`.

**Field Device pin table (test):**

```c
static const led_pin_t k_test_pins_fd[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_G, .pin =  6U,
                   .active_high = false, .fitted = true},
    [LED_RED]   = {.port = GPIO_PORT_D, .pin =  5U,
                   .active_high = false, .fitted = true},
};
```

**Gateway pin table (test):**

```c
static const led_pin_t k_test_pins_gw[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_B, .pin = 14U,
                   .active_high = true,  .fitted = true},
    [LED_RED]   = {.port = GPIO_PORT_A, .pin =  5U,
                   .active_high = true,  .fitted = false},
};
```

### Group 1 — Init lifecycle

| ID | Precondition | Action | Expected |
|---|---|---|---|
| LED-T01 | FD pin table | `led_init(k_test_pins_fd, LED_COUNT)` | `LED_ERR_OK`; `gpio_configure` called twice; `gpio_write(GPIO_LEVEL_HIGH)` called twice (active-low off); `xTimerCreateStatic` called twice; pattern == OFF for both LEDs |
| LED-T02 | GW pin table | `led_init(k_test_pins_gw, LED_COUNT)` | `LED_ERR_OK`; `gpio_configure` called once (green only); `gpio_write(GPIO_LEVEL_LOW)` called once (active-high off); `xTimerCreateStatic` called once; red skipped |
| LED-T03 | — | `led_init(NULL, LED_COUNT)` | `LED_ERR_NULL_ARG`; no GPIO calls |
| LED-T04 | — | `led_init(valid_pins, LED_COUNT - 1U)` | `LED_ERR_INVALID_ID`; no GPIO calls |

### Group 2 — Solid patterns (active-low, FD)

| ID | Precondition | Action | Expected |
|---|---|---|---|
| LED-T05 | FD init done | `led_set(LED_GREEN, LED_PATTERN_ON)` | `xTimerStop` called; `gpio_write(PORT_G, 6, GPIO_LEVEL_LOW)`; pattern == ON |
| LED-T06 | FD init done | `led_set(LED_GREEN, LED_PATTERN_OFF)` | `xTimerStop` called; `gpio_write(PORT_G, 6, GPIO_LEVEL_HIGH)`; pattern == OFF |
| LED-T07 | FD init done | `led_set(LED_RED, LED_PATTERN_ON)` | `xTimerStop` called; `gpio_write(PORT_D, 5, GPIO_LEVEL_LOW)` |

### Group 3 — Solid patterns (active-high, GW)

| ID | Precondition | Action | Expected |
|---|---|---|---|
| LED-T08 | GW init done | `led_set(LED_GREEN, LED_PATTERN_ON)` | `xTimerStop` called; `gpio_write(PORT_B, 14, GPIO_LEVEL_HIGH)` |
| LED-T09 | GW init done | `led_set(LED_GREEN, LED_PATTERN_OFF)` | `xTimerStop` called; `gpio_write(PORT_B, 14, GPIO_LEVEL_LOW)` |

### Group 4 — Blink patterns

| ID | Precondition | Action | Expected |
|---|---|---|---|
| LED-T10 | FD init done | `led_set(LED_GREEN, LED_PATTERN_BLINK_FAST)` | `gpio_write(LOW)` (on, active-low); `xTimerChangePeriod(timer, pdMS_TO_TICKS(100), 0)` called; pattern == BLINK_FAST |
| LED-T11 | FD init done | `led_set(LED_GREEN, LED_PATTERN_BLINK_MEDIUM)` | `xTimerChangePeriod(timer, pdMS_TO_TICKS(250), 0)` called |
| LED-T12 | FD init done | `led_set(LED_GREEN, LED_PATTERN_BLINK_SLOW)` | `xTimerChangePeriod(timer, pdMS_TO_TICKS(500), 0)` called |

### Group 5 — Timer callback

| ID | Precondition | Action | Expected |
|---|---|---|---|
| LED-T13 | FD init; set BLINK_FAST; GPIO currently on | Invoke `blink_callback(timer_green)` | `gpio_write(HIGH)` (off, active-low); `gpio_on[GREEN]` == false |
| LED-T14 | As above; GPIO now off | Invoke `blink_callback(timer_green)` again | `gpio_write(LOW)` (on); `gpio_on[GREEN]` == true |
| LED-T15 | Pattern changed to LED_PATTERN_ON | Invoke `blink_callback(timer_green)` (residual fire) | Guard triggers; no `gpio_write` call (residual-fire protection) |

### Group 6 — Pattern transitions

| ID | Precondition | Action | Expected |
|---|---|---|---|
| LED-T16 | FD init; set BLINK_FAST | `led_set(LED_GREEN, LED_PATTERN_ON)` | `xTimerStop` called; `gpio_write(LOW)` (on); pattern == ON |
| LED-T17 | FD init; set ON | `led_set(LED_GREEN, LED_PATTERN_BLINK_SLOW)` | `gpio_write(LOW)` (on); `xTimerChangePeriod(500ms)` |
| LED-T18 | FD init; set BLINK_FAST | `led_set(LED_GREEN, LED_PATTERN_BLINK_SLOW)` | `gpio_write(LOW)` (on); `xTimerChangePeriod(500ms)` (timer restarted with new period) |

### Group 7 — Get pattern

| ID | Precondition | Action | Expected |
|---|---|---|---|
| LED-T19 | FD init; set ON | `led_get_pattern(LED_GREEN, &pat)` | `LED_ERR_OK`; `pat == LED_PATTERN_ON` |
| LED-T20 | FD init; set BLINK_FAST | `led_get_pattern(LED_GREEN, &pat)` | `LED_ERR_OK`; `pat == LED_PATTERN_BLINK_FAST` |
| LED-T21 | FD init done | `led_get_pattern(LED_GREEN, NULL)` | `LED_ERR_NULL_ARG` |

### Group 8 — Error paths

| ID | Precondition | Action | Expected |
|---|---|---|---|
| LED-T22 | GW init done | `led_set(LED_RED, LED_PATTERN_ON)` | `LED_ERR_INVALID_ID`; no GPIO call |
| LED-T23 | FD init done | `led_set(LED_COUNT, LED_PATTERN_ON)` | `LED_ERR_INVALID_ID` |
| LED-T24 | Not initialised | `led_set(LED_GREEN, LED_PATTERN_ON)` | `LED_ERR_NOT_INIT` |
| LED-T25 | Not initialised | `led_get_pattern(LED_GREEN, &pat)` | `LED_ERR_NOT_INIT` |
| LED-T26 | FD init done | `led_set(LED_GREEN, LED_PATTERN_COUNT)` | `LED_ERR_INVALID_PARAM` |

### Group 9 — Reset for test

| ID | Precondition | Action | Expected |
|---|---|---|---|
| LED-T27 | FD init done; timer running | `led_reset_for_test()` then `led_set(LED_GREEN, LED_PATTERN_ON)` | `xTimerStop` called during reset; `led_set` returns `LED_ERR_NOT_INIT`; re-init possible after reset |

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| LED-D1 | LedDriver consumes `IGpio`; never accesses GPIO registers directly | Single responsibility. Consistent with project convention |
| LED-D2 | Shared `led_id_t` enum on both boards; `LED_ERR_INVALID_ID` for missing LEDs | Single header. Alternative (separate enums per board) forces `#ifdef` into HealthMonitor |
| LED-D3 | Active-level polarity encapsulated in the injected pin table | P1: polarity knowledge does not leak to the caller. Changing a board's active level requires editing only the pin table |
| LED-D4 | Singleton module (no handle parameter) | Two or fewer LEDs per board; consistent with GpioDriver, RtcDriver, Logger |
| LED-D5 | `led_init()` takes a caller-supplied `const led_pin_t *` pin table | Removes all `#if defined(STM32F469xx)` from the driver. Adding a board requires only a new pin table, not a driver edit. Tests pass their own pin tables |
| LED-D6 | Blink patterns via FreeRTOS static software timers | HealthMonitor needs four distinct LED indications through one GPIO pin — blinking is the only option. FreeRTOS software timers are lightweight (44 bytes each), use static allocation (P5), and run in the timer daemon task. No hardware timer peripheral consumed |
| LED-D7 | `led_set()` is the single control API (replaces v0.1 on/off/toggle) | One function handles all patterns. Avoids ambiguity about whether `led_on()` overrides an active blink. HealthMonitor only needs `led_set()` |
| LED-D8 | No vtable, no handle. DIP seam is the header boundary | LedDriver has no alternative implementations. Singleton with direct function calls is the simplest correct approach. Adding a vtable for a two-LED driver is over-engineering (v0.1 incorrectly claimed a vtable existed — corrected) |
| LED-D9 | Pre-scheduler blink: timer commands queued, processed at scheduler start | FreeRTOS queues `xTimerChangePeriod`/`xTimerStart` commands sent before the scheduler starts. The LED is set to the on-level immediately; blinking begins when the timer daemon task runs. No degradation visible to the user — the init phase is brief |
| LED-D10 | Residual-fire guard in blink callback | `xTimerStop()` is asynchronous — one more callback may fire after the stop command is sent. The guard checks the current pattern; if it is no longer a blink pattern, the callback returns without toggling. This prevents a sub-millisecond glitch during pattern transitions |
| LED-D11 | `led_reset_for_test()` — test-only singleton reset | Allows `setUp()` to clear all state between tests. Consistent with HealthMonitor, ConfigStore, SAF convention |

---

## 10. Open items

| ID | Item | Status |
|---|---|---|
| LED-O1 | `task-breakdown.md` has no LedDriver entry. Confirm no update needed (purely synchronous, no task). | Open |
| LED-O2 | HealthMonitor must handle `LED_ERR_INVALID_ID` for `LED_RED` on Gateway. Verify HealthMonitor companion §7 covers this. | Open |
| LED-O3 | HealthMonitor v0.2 companion requires update to match singleton API — see §11 below. | Open — must resolve before either module is implemented |
| LED-O4 | SRS gap F-07: no dedicated LED-indication requirement exists. REQ-LD-200 covers LCD splash, not LED patterns. Add REQ-NF-2xx when SRS is next updated. | Open |
| LED-O5 | The Field Device driver (`firmware/field-device/drivers/led/`) still implements the v0.1 API (`led_on/off/toggle`, no patterns, no timers). Bring it to this companion's `led_set()` API before the FD HealthMonitor uses patterns. | Open |
| LED-O6 | Timer and GPIO failures are not reported by `led_set()`: `xTimerChangePeriod`/`xTimerStop` can fail when the timer command queue is full, and `gpio_write_pin` can fail, but the error enum has no code for either. They are ignored, so a failed blink start leaves the LED solid on. Add an error code if this matters. | Open |
| LED-O7 | Visual confirmation that LD2 (PB14) is the LED beside the ST-LINK connector and not shared with another signal. The bring-up test leaves LD2 blinking at 1 Hz for this. | Open — confirm by eye |

---

## 11. Required HealthMonitor v0.2 companion update

LedDriver v0.2 is a singleton (LED-D4, LED-D8). The HealthMonitor v0.2
companion was written assuming an ADT-handle-based LedDriver. The
following changes are required in the HealthMonitor companion before either
module is implemented:

**Config struct (`health_monitor_config_t`):**

```diff
 typedef struct {
-    led_handle_t            led;
     lifecycle_reset_cause_t boot_reset_cause;
 } health_monitor_config_t;
```

**Internal struct (`struct health_monitor_inst`):**

```diff
 struct health_monitor_inst {
-    /* Injected dependencies */
-    led_handle_t        led;
-
     /* Health state */
     health_snapshot_t   snapshot;
```

**Header include:**

```diff
-#include "led_driver.h"             /* led_handle_t          */
+#include "led_driver.h"             /* led_set(), led_pattern_t */
```

**`update_led_state()` function (4 call sites):**

```diff
-    led_set(self->led, LED_STATUS, LED_BLINK_FAST);
+    led_set(LED_GREEN, LED_PATTERN_BLINK_FAST);

-    led_set(self->led, LED_STATUS, LED_BLINK_SLOW);
+    led_set(LED_GREEN, LED_PATTERN_BLINK_SLOW);

-    led_set(self->led, LED_STATUS, LED_ON);
+    led_set(LED_GREEN, LED_PATTERN_ON);

-    led_set(self->led, LED_STATUS, LED_BLINK_MEDIUM);
+    led_set(LED_GREEN, LED_PATTERN_BLINK_MEDIUM);
```

**Test mock description:**

```diff
-| LedDriver | Mocked via CMock from `led_driver.h`. Stubs verify
-  `led_set()` call parameters. |
+| LedDriver | Mocked via CMock from `led_driver.h`. Stubs verify
+  `led_set(led_id_t, led_pattern_t)` call parameters (2-arg form,
+  singleton — no handle). |
```

**Affected test cases:** HM-T01, HM-T15, HM-T21, HM-T27..T31, HM-T33 —
update `led_set` expectations from 3-arg to 2-arg form.

---

## 12. File layout

The boards are separate CubeIDE projects and the repository keeps one driver
copy per board (the same split as RtcDriver and GpioDriver), so the layout below
supersedes the earlier `firmware/shared/` sketch.

```
firmware/gateway/drivers/led/
├── led_driver.h            (public API, pin table type)
└── led_driver.c            (singleton, patterns, blink timer callback)

firmware/field-device/drivers/led/      (FD copy — still v0.1, see LED-O5)

tests/gateway/drivers/led/
└── test_led_driver_gateway.c           (Unity, GpioDriver stubbed)

firmware/gateway/integration-tests/led/
└── main_test_led_driver.c              (hardware bring-up)
```

The pin tables live in the bring-up main for now; the GW has no
`board_config.c` yet.

---

## 12.1 Gateway implementation notes

Where the code settles something this companion left open, or differs from its
pseudocode. The public API in §3 is unchanged.

| # | Topic | Gateway behaviour |
|---|---|---|
| 1 | GPIO API names | The real GpioDriver exposes `gpio_configure_pin(const gpio_pin_config_t *)` and `gpio_write_pin(port, pin, level)`, not the `gpio_configure(...)` / `gpio_write(...)` of the pseudocode. |
| 2 | Callback lookup | `blink_callback` finds its LED by comparing the timer handle with the driver's own timer table instead of `pvTimerGetTimerID` and a `uintptr_t` cast. Same behaviour, no pointer-to-integer conversion, and the shared FreeRTOS test mock needs no change. The timer is created with a NULL ID. |
| 3 | Re-init | A second `led_init()` while initialised returns `LED_ERR_OK` with no side effects; re-creating a static timer on a live buffer would corrupt FreeRTOS's timer list. |
| 4 | Init failures | A GPIO configure failure (for example `gpio_init()` not yet called) or a NULL timer handle makes `led_init()` return `LED_ERR_NOT_INIT` and leaves the driver uninitialised, so a later call can succeed. |
| 5 | Pin table type | `led_pin_t.port` stays `uint8_t` so `led_driver.h` has no GpioDriver dependency; the driver casts it to `gpio_port_t`. |
| 6 | Timer names | Each timer name ("Led0", "Led1") lives in the driver's state, because FreeRTOS keeps the pointer. |
| 7 | Test file name | `test_led_driver_gateway.c` with defines block `:test_led_driver_gateway:`, because `test_led_driver_gw.c` is already the FD module's test of the Gateway pin table. |
| 8 | Test project | Gateway and FD share the basename `led_driver`, so the Gateway tests run under `tests/project_gateway.yml` and the Gateway directories are excluded from `tests/project.yml`, as for GpioDriver. |

### 12.2 Gateway test results

`tests/gateway/drivers/led/test_led_driver_gateway.c`: 33 tests, 33 pass, 0
ignored (2026-10-11). LED-T01..T27 follow §8; T28 onward cover an unknown timer
handle, `led_get_pattern` on unfitted ids, idempotent init, GPIO and timer-create
failures, and solid patterns never starting a timer. The tests use the Gateway
pin table and a two-LED active-low table (the FD polarity). The shared FreeRTOS
mock returns one dummy handle for every timer, so the callback is exercised on
the first fitted LED only.

### 12.3 Hardware bring-up

Harness: `firmware/gateway/integration-tests/led/main_test_led_driver.c`. The LED
cannot be watched by a script, so the harness reads `GPIOB->ODR` and times the
transitions with the DWT cycle counter. That exercises the real `gpio_write_pin`
path, the polarity and the software timer on silicon.

| ID | What to observe | Verifies |
|---|---|---|
| TC-HW-LED-001 | `gpio_init` + `led_init`: PB14 driven low | Init and active-level handling |
| TC-HW-LED-002 | `led_set(BLINK_MEDIUM)` before the scheduler: LED on at once, then toggling at 250 ms once it starts | LED-D9 |
| TC-HW-LED-003 / 004 | `LED_PATTERN_ON` / `OFF`: PB14 high / low and steady for 700 ms | Solid patterns |
| TC-HW-LED-005 | `BLINK_FAST`: transitions every 100 ms (±15 %) | Fast half-period |
| TC-HW-LED-006 | `BLINK_MEDIUM`: every 250 ms | Medium half-period |
| TC-HW-LED-007 | `BLINK_SLOW`: every 500 ms | Slow half-period |
| TC-HW-LED-008 | Blink → ON and blink → OFF stop the toggling | Timer stop and residual-fire guard (LED-D10) |
| TC-HW-LED-009 | FAST → SLOW restarts with a 500 ms period | Period change on a running timer |
| TC-HW-LED-010 | `LED_RED` not fitted, bad pattern, NULL output, read-back | Error paths |

**Status: run on hardware 2026-10-11 — all 10 cases PASS** (B-L475E-IOT01A).
Measured mean half-periods: 99.8 ms (fast), 249.8 ms (medium), 499.9 ms (slow);
the pre-scheduler blink ran at 245 ms with its first interval shorter, as it
starts counting from task start. The harness leaves LD2 blinking at 1 Hz;
confirmation by eye is tracked as LED-O7.

---

## 13. Memory sizing

| Resource | Estimate | Notes |
|---|---|---|
| Static RAM | ~120 B | `led_driver_t` singleton: pins pointer (4 B), count + padding (4 B), pattern array (8 B), gpio_on array (2 B + pad), timer handles (8 B), `StaticTimer_t` × 2 (88 B), initialised + pad (4 B) |
| ROM | ~0.6 KB | `led_init`, `led_set`, `led_get_pattern`, `blink_callback`, `blink_half_period`, GPIO helpers, string literals |
| Stack per call | ~32 B | `led_set()` is non-blocking; callback runs in timer daemon task context (single `gpio_write` call) |

---

## Phase H — Readiness review

| # | Check | Result |
|---|---|---|
| H1 | PROVIDES / USES match `components.md` | PASS — PROVIDES ILed; USES GpioDriver. FreeRTOS timers added (infrastructure dependency, not a component link) |
| H2 | Root requirement exists in SRS | PASS (with gap) — REQ-LD-200 exists. LED-indication is tracked as gap F-07 (LED-O4) |
| H3 | Every public function has a Doxygen block | PASS — `led_init`, `led_set`, `led_get_pattern`, `led_reset_for_test` |
| H4 | Error enum covers all failure modes | PASS — 5 error codes: OK, INVALID_ID, NOT_INIT, NULL_ARG, INVALID_PARAM |
| H5 | Internal design specifies every public function flow | PASS — §4.6 (init), §4.7 (set), §4.8 (get_pattern), §4.9 (reset_for_test), §4.5 (callback) |
| H6 | No `#if BOARD_xxx` in driver source | PASS — board differences handled by pin-table injection (LED-D5) |
| H7 | Hardware contract specifies pin config and init ordering | PASS — §5.1 (pin config), §5.2 (ordering) |
| H8 | Every error code has a test case | PASS — LED_ERR_OK (T01–T02, T05–T12, T19–T20), INVALID_ID (T22–T23), NOT_INIT (T24–T25), NULL_ARG (T03, T21), INVALID_PARAM (T26) |
| H9 | Test IDs use project-standard format | PASS — LED-Tnn |
| H10 | Test plan specifies mock strategy | PASS — CMock for gpio_driver.h and FreeRTOS timer API |
| H11 | File layout specified | PASS — §12 (per-board copies) |
| H12 | Memory sizing specified | PASS — §13 |
| H13 | Decisions log complete | PASS — LED-D1..D11 |
| H14 | No dynamic allocation post-init (P5) | PASS — static singleton, `StaticTimer_t` buffers |
| H15 | `_reset_for_test()` defined | PASS — §3.4, §4.9 |
| H16 | Consumer companion cross-reference checked | PASS (with action) — HealthMonitor v0.2 update required (§11). Must be applied before implementation |
| H17 | Open items documented | PASS — LED-O1..O7 |
| H18 | Gateway implemented, host-tested and hardware-validated | PASS — §12.1–12.3: 33 host tests, 10 hardware cases |

**Verdict: PASS — Gateway implemented and hardware-validated.** The
HealthMonitor companion update in §11 must still be applied before HealthMonitor
is implemented.
