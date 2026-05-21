# LedDriver — LLD Companion

**Document:** `docs/lld/drivers/led-driver.md`
**Version:** 0.1 (draft — pending Luca review)
**Board scope:** Field Device (STM32F469) and Gateway (B-L475E-IOT01A)
**Layer:** Driver
**Status:** Draft
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

**REQ-LD-200 status:** flagged as not yet present in `SRS.md` (tracked as F-07 SRS fix per `components.md`). The requirement exists in the component specification; the SRS gap must be resolved before the LLD gate review.

**Consumer:** `HealthMonitor` (Application) on both boards. `HealthMonitor USES LedDriver, ILogger`. It drives LEDs to indicate device status (idle, acquiring, alarm, error).

**This is the first driver that `USES` another driver** rather than CMSIS directly. LedDriver consumes `IGpio` from GpioDriver; it never accesses GPIO registers. This is correct per the convention: *"Only LedDriver and device drivers with non-bus control lines USES GpioDriver."* (`lld-methodology.md` v1.1, established convention).

**Physical LEDs:**

| Board | LED | Colour | GPIO pin |
|---|---|---|---|
| STM32F469 (FD) | LD3 | Green | PG13 |
| STM32F469 (FD) | LD4 | Red | PD5 |
| STM32L475 (GW) | LED2 | Green | PB14 |

Pin assignments are from the UM1932 and UM2153 schematics. Verify against the board schematics at implementation.

---

## 2. Public API

### 2.1 Dependency-conformance check

`led_driver.h` includes `stdint.h` and `led_driver.h` itself produces no direct CMSIS or GPIO register includes. GPIO access goes through the `IGpio` interface provided by GpioDriver. Confirmed clean.

### 2.2 P3 consideration

Single consumer (`HealthMonitor`). No ISP split warranted.

### 2.3 LED identifiers

Both boards share the same `led_id_t` enum. The gateway has no red LED; calling `led_on(LED_RED)` on the gateway returns `LED_ERR_INVALID_ID`. `HealthMonitor` uses only IDs valid for its board — this is a compile-time concern for the application, not the driver.

```c
/**
 * @brief LED identifier.
 *
 * LED_GREEN is present on both boards.
 * LED_RED is present on the Field Device only; returns LED_ERR_INVALID_ID
 * on the Gateway.
 */
typedef enum {
    LED_GREEN = 0, /**< Green status LED. FD: LD3 (PG13). GW: LED2 (PB14). */
    LED_RED   = 1, /**< Red alarm LED.   FD: LD4 (PD5).  GW: not fitted.   */
    LED_COUNT      /**< Sentinel — do not pass to API functions. */
} led_id_t;

/**
 * @brief Error codes returned by all LedDriver operations.
 *
 * Naming follows the cross-cutting convention established in lld.md §3.2.
 */
typedef enum {
    LED_ERR_OK         = 0, /**< Operation succeeded. */
    LED_ERR_INVALID_ID = 1, /**< LED identifier not fitted on this board. */
} led_err_t;
```

### 2.4 Public API (`led_driver.h`)

```c
/**
 * @brief Initialise LedDriver.
 *
 * Configures the GPIO pins for all fitted LEDs (via GpioDriver) and
 * sets the initial state to off. Must be called once after gpio_init().
 *
 * @return LED_ERR_OK on success.
 * @note Threading: task-context only, non-blocking. Must be called before the scheduler starts.
 */
led_err_t led_init(void);

/**
 * @brief Turn an LED on.
 *
 * @param id  LED to turn on.
 * @return LED_ERR_OK on success; LED_ERR_INVALID_ID if the LED is not
 *         fitted on this board.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
led_err_t led_on(led_id_t id);

/**
 * @brief Turn an LED off.
 *
 * @param id  LED to turn off.
 * @return LED_ERR_OK on success; LED_ERR_INVALID_ID if the LED is not
 *         fitted on this board.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
led_err_t led_off(led_id_t id);

/**
 * @brief Toggle an LED state.
 *
 * @param id  LED to toggle.
 * @return LED_ERR_OK on success; LED_ERR_INVALID_ID if the LED is not
 *         fitted on this board.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
led_err_t led_toggle(led_id_t id);
```

---

## 3. Internal design

### 3.0 Private struct

```c
typedef struct {
    bool initialised; /**< Set by led_init(); confirms GpioDriver is ready. */
} led_driver_t;

static led_driver_t s_led;
```


### 3.1 Module-level state

```c
/* Compile-time table mapping led_id_t to the GpioDriver pin descriptor.
 * A NULL entry marks an LED not fitted on this board. */
static const led_pin_t s_led_pins[LED_COUNT] = {
    /* LED_GREEN */ { /* GpioDriver pin descriptor for green LED */ },
    /* LED_RED   */ { /* GpioDriver pin descriptor for red LED, or NULL sentinel on GW */ },
};
```

`led_pin_t` is an internal struct wrapping the port and pin values expected by `GpioDriver`'s `gpio_write()` and `gpio_toggle()` calls (per the IGpio API in `gpio-driver.md`). The table is `const` and populated at compile time — no dynamic state, no heap.

### 3.2 Validation

Every public function validates the `led_id_t` argument against the fitted-LED table before calling GpioDriver. If the entry is the null sentinel, `LED_ERR_INVALID_ID` is returned immediately. No silent failures (BARR-C §5).

### 3.3 No ISR, no DMA, no callbacks

LedDriver is purely synchronous. `HealthMonitor` calls it from within `LifecycleTask` or `SensorTask` context (both boards). Blocking time is negligible — a single GPIO write is a register store.

### 3.4 Active-low vs active-high

Both discovery boards drive their user LEDs active-high (LED on when GPIO pin is high). This is hardcoded in the `s_led_pins` table. If a future board uses active-low LEDs, the polarity is encapsulated in the table entry and the public API remains unchanged. This satisfies P1 (polarity knowledge stays in the driver).

---

### 3.5 Principles applied

- **P1 (Strict directional layering).** Depends only on GpioDriver (IGpio); no RTOS, no middleware.
- **P2 (Dependency Inversion).** Exposes `iled_t` vtable; consumers depend on `ILed`.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static pin-map table (compile-time constant); no heap; no RTOS objects.
- **P6 (Responsibility traces to requirements).** LED set/toggle traces to REQ-NF-202 status-indicator requirements.
- **P8 (Total error propagation, no silent failures).** `led_err_t` on configure and write; invalid-ID arguments return error.
- **P9 (BARR-C coding standard).** `uint8_t` for LED ID; active-level documented explicitly; no implicit widening.
- **P10 (Naming conventions).** Prefix `led_`; interface `ILed` -> `iled_t`; errors `LED_ERR_*`.


### Synchronisation

Caller serialises. The driver holds no FreeRTOS synchronisation primitives. All entry points are intended to be called from a single task context or from `main()` before the scheduler starts. Concurrent access from multiple tasks is not safe unless the caller provides a mutex.

### led_init

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### led_on

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### led_off

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### led_toggle

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).


## 4. Hardware contract

### 4.1 Pin configuration

LED GPIO pins must be configured as push-pull output, no pull-up, medium speed. This configuration is applied by `led_init()` via `gpio_configure()` (GpioDriver API). LedDriver does not call CMSIS directly for pin configuration.

| Board | LED | Pin | Active level |
|---|---|---|---|
| STM32F469 | LD3 (green) | PG13 | High |
| STM32F469 | LD4 (red) | PD5 | High |
| STM32L475 | LED2 (green) | PB14 | High |

**Action required (Luca at implementation):** confirm active level and pin assignments against the UM1932 and UM2153 schematics before writing code.

### 4.2 `gpio_init()` ordering requirement

`led_init()` calls `gpio_configure()` internally. `gpio_init()` must have been called before `led_init()`. This ordering is enforced by the boot sequence in `main()` and is documented here for the integration checklist.

---

## 5. Sequence integration

`LedDriver` has no HLD-level sequence diagram surface. LED state changes are side-effects of lifecycle and health transitions, not primary actors in any sequence. No changes to `sequence-diagrams.md` are required.

---

## 6. Error and fault behaviour

All public functions return `led_err_t`; callers must not ignore non-OK returns.
No retry is performed internally — the driver surfaces the error; the caller
decides the retry and logging policy.

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `LED_ERR_INVALID_ID` | `led_on()` / `led_off()` called with an `led_id_t` not fitted on the active board variant | Return error; no GPIO change | Non-OK return | No retry — programming error; caller must check the board variant before calling | Caller logs at WARN via ILogger |


## 7. Unit-test plan

Host-platform tests (Unity framework). GpioDriver calls are intercepted via a mock `IGpio` implementation substituted at link time (stub functions that record calls and return `GPIO_ERR_OK`).

| ID | Test case | Expected result |
|---|---|---|
| T-LED-01 | `led_init`: verify `gpio_configure` called for all fitted LEDs; initial state set to off | Mock records configure call per fitted LED; `gpio_write(LOW)` called for each |
| T-LED-02 | `led_on(LED_GREEN)`: verify `gpio_write(HIGH)` called on correct pin | Mock records write call with correct port/pin/state |
| T-LED-03 | `led_off(LED_GREEN)`: verify `gpio_write(LOW)` called | Correct pin set low |
| T-LED-04 | `led_toggle(LED_GREEN)`: verify `gpio_toggle` called on correct pin | Mock records toggle call |
| T-LED-05 | `led_on(LED_RED)` on Gateway build: verify `LED_ERR_INVALID_ID` returned | No GpioDriver call made |
| T-LED-06 | `led_on(LED_RED)` on Field Device build: verify succeeds | `gpio_write(HIGH)` called on PD5 |
| T-LED-07 | `led_on` with out-of-range id (e.g. `LED_COUNT`): verify `LED_ERR_INVALID_ID` | No GpioDriver call made |

Test files: `tests/drivers/test_led_driver_fd.c` and `tests/drivers/test_led_driver_gw.c` (separate builds per board).

---

## 8. Open items

| ID | Item | Owner | Resolution path |
|---|---|---|---|

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| LEDD-D1 | LedDriver consumes `IGpio`; never accesses GPIO registers directly | Consistent with the driver convention: *"Only LedDriver… USES GpioDriver."* Single responsibility — LedDriver knows LED semantics; GpioDriver knows GPIO registers |
| LEDD-D2 | Shared `led_id_t` enum on both boards; `LED_ERR_INVALID_ID` for missing LEDs | Keeps a single header; the caller (HealthMonitor) is responsible for using only the IDs valid for its board. The alternative (separate headers per board) would require HealthMonitor to `#ifdef` its LED calls — worse than an error return |
| LEDD-D3 | Active-level polarity encapsulated in the compile-time table | P1: polarity knowledge must not leak to the caller. If a board ever uses active-low LEDs, only the table changes |
| LEDD-D4 | Singleton module (no handle) | Two or fewer LEDs per board; consistent with all prior companion decisions |
