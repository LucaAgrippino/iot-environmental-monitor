# GpioDriver — LLD Companion

**Version:** 1.0
**Date:** May 2026
**Status:** Draft

**HLD anchor:** `GpioDriver` in `components.md` (Field Device §4 driver layer; Gateway §4 driver layer). Layer: Driver. Targets: STM32F469 Discovery (Field Device) and STM32L475 IoT Discovery (Gateway).

---

## 1. Sources

### 1.1 Component specification

The driver appears identically in `components.md` for both boards:

> **NAME:** GpioDriver
> **LAYER:** Driver
> **RESPONSIBILITY:** Configures GPIO pins and provides read/write access to single-pin digital I/O (REQ-NF-202).
> **PROVIDES (upward):** `IGpio`
> **USES (downward):** CMSIS

### 1.2 Consumers

Identified by walking the `USES (downward)` lines of every driver in `components.md`:

| Board | Consumer | Reference |
|---|---|---|
| Field Device | LedDriver | `components.md` line 220 |
| Gateway | LedDriver | `components.md` line 478 |
| Gateway | WifiDriver | `components.md` line 436 |
| Gateway | MagnetometerDriver | `components.md` line 442 |
| Gateway | ImuDriver | `components.md` line 448 |
| Gateway | BarometerDriver | `components.md` line 454 |
| Gateway | HumidityTempDriver | `components.md` line 460 |

No component above the Driver layer depends on `IGpio`. P1 satisfied by construction.

### 1.3 Traceability

The only requirement cited from the responsibility line:

| ID | Text | SRS trace | Use case trace |
|---|---|---|---|
| REQ-NF-202 | "The system shall restart and resume normal operation within 5 seconds after a watchdog reset." | SRS §3.2 line 335 | — (Vision §7) |

GPIO is foundational infrastructure: it enables every consumer's pin-level operation. REQ-NF-202 traces through GPIO because timely reset recovery depends on rapid re-initialisation of every peripheral driver, each of which configures its pins through `IGpio`. No further direct trace is required by `components.md`; transitive traces flow through each consumer.

### 1.4 HLD context

`hld.md` §9 places `GpioDriver` in the "CMSIS register-level" category: *"GPIO is the simplest peripheral. HAL would be overhead."* No STM32 HAL above the driver layer; CMSIS device headers (`stm32f469xx.h` for Field Device, `stm32l475xx.h` for Gateway) provide the peripheral struct definitions and bit-field macros.

### 1.5 Runtime context

- **Sequence diagrams (`sequence-diagrams.md`):** no occurrences of `GpioDriver`, `IGpio`, or any GPIO call. GPIO is implicit in driver initialisation flows and is correctly abstracted below the SD level of detail.
- **State machines (`state-machines.md`):** no occurrences. GPIO has no behavioural state of its own; it is stateless register access from the consumer's point of view.
- **Task breakdown (`task-breakdown.md`):** no occurrences. `GpioDriver` is not hosted by any FreeRTOS task. `gpio_init()` is called from `main` before the scheduler starts; per-pin configuration runs in each consumer driver's init context (also pre-scheduler). Runtime pin reads and writes execute in the context of whichever task or ISR calls them.

### 1.6 Hardware references

- **STM32F469 (Field Device):** RM0386 Reference Manual — GPIO chapter — for register definitions. Board pin assignments in UM1932 ("Discovery kit with STM32F469NI MCU").
- **STM32L475 (Gateway):** RM0351 Reference Manual — GPIO chapter — for register definitions. Board pin assignments in UM2153 ("Discovery kit for IoT node"), Appendix A Table 11.

This companion does not enumerate consumer pin assignments — each consumer driver does that in its own companion, citing the same board user manuals.

---

## 2. Public API

### 2.1 API style — direct, single-handle-free

The `IGpio` interface has **one realisation per board** and no multi-implementation case. Per the methodology §3 Step 2 decision rule, this is a direct C API rather than a vtable.

There is no opaque instance handle. GPIO is a hardware singleton — the device has exactly one set of GPIO peripherals, and every consumer accesses the same peripherals. A `gpio_driver_t` handle would add ceremony without separation: handle storage, init/deinit lifecycle, no second instance ever created.

Instead, the API is a flat module:

- `gpio_init()` — once-at-boot initialisation of the GPIO subsystem itself (clock enables for the ports the build will use). Idempotent.
- `gpio_configure_pin()`, `gpio_read_pin()`, `gpio_write_pin()`, `gpio_toggle_pin()` — per-pin operations identified by `(port, pin)`.

Both boards share the same `gpio_driver.h`. The implementation file `gpio_driver.c` is board-specific (selected by build target), but the public surface is identical.

### 2.2 Header content

```c
/**
 * @file gpio_driver.h
 * @brief CMSIS-level GPIO driver — pin configuration and digital I/O.
 *
 * Provides IGpio (per components.md): configure, read, write, and toggle
 * single-pin digital I/O. Used by every driver that owns physical pins.
 *
 * @note See docs/lld/gpio-driver.md for the full design specification.
 */

#ifndef GPIO_DRIVER_H
#define GPIO_DRIVER_H

#include <stdint.h>
#include <stdbool.h>

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief GPIO driver result codes.
 */
typedef enum
{
    GPIO_OK                  =  0, /**< Success. */
    GPIO_ERR_NOT_INITIALISED =  1, /**< gpio_init() has not been called. */
    GPIO_ERR_INVALID_PORT    =  2, /**< Port value out of range or not enabled. */
    GPIO_ERR_INVALID_PIN     =  3, /**< Pin number outside 0..15. */
    GPIO_ERR_INVALID_MODE    =  4, /**< Mode value out of range. */
    GPIO_ERR_INVALID_CONFIG  =  5, /**< Config struct combination not permitted. */
    GPIO_ERR_NULL_POINTER    =  6  /**< Required output pointer is NULL. */
} gpio_err_t;

/* ------------------------------------------------------------------ */
/* Enumerations                                                        */
/* ------------------------------------------------------------------ */

/**
 * @brief GPIO port identifiers.
 *
 * The set is the superset across both boards. A build-time check rejects
 * use of a port not present on the current target.
 */
typedef enum
{
    GPIO_PORT_A = 0,
    GPIO_PORT_B,
    GPIO_PORT_C,
    GPIO_PORT_D,
    GPIO_PORT_E,
    GPIO_PORT_F,
    GPIO_PORT_G,
    GPIO_PORT_H,
    GPIO_PORT_I,  /**< STM32F469 only. */
    GPIO_PORT_J,  /**< STM32F469 only. */
    GPIO_PORT_K,  /**< STM32F469 only. */
    GPIO_PORT_COUNT
} gpio_port_t;

/**
 * @brief Pin operating mode.
 */
typedef enum
{
    GPIO_MODE_INPUT     = 0,
    GPIO_MODE_OUTPUT    = 1,
    GPIO_MODE_ALTERNATE = 2,
    GPIO_MODE_ANALOGUE  = 3
} gpio_mode_t;

/**
 * @brief Output driver type.
 */
typedef enum
{
    GPIO_OTYPE_PUSH_PULL  = 0,
    GPIO_OTYPE_OPEN_DRAIN = 1
} gpio_otype_t;

/**
 * @brief Output slew rate.
 */
typedef enum
{
    GPIO_SPEED_LOW       = 0,
    GPIO_SPEED_MEDIUM    = 1,
    GPIO_SPEED_HIGH      = 2,
    GPIO_SPEED_VERY_HIGH = 3
} gpio_speed_t;

/**
 * @brief Internal pull configuration.
 */
typedef enum
{
    GPIO_PULL_NONE = 0,
    GPIO_PULL_UP   = 1,
    GPIO_PULL_DOWN = 2
} gpio_pull_t;

/**
 * @brief Logical pin level.
 */
typedef enum
{
    GPIO_LEVEL_LOW  = 0,
    GPIO_LEVEL_HIGH = 1
} gpio_level_t;

/* ------------------------------------------------------------------ */
/* Configuration struct                                                */
/* ------------------------------------------------------------------ */

/**
 * @brief Per-pin configuration descriptor.
 *
 * Passed by value-via-pointer to gpio_configure_pin(). All fields are
 * inspected; callers must initialise every field, including @c alternate
 * when @c mode is not GPIO_MODE_ALTERNATE (set to 0 in that case).
 */
typedef struct
{
    gpio_port_t  port;       /**< Port. */
    uint8_t      pin;        /**< Pin number, 0..15. */
    gpio_mode_t  mode;       /**< Operating mode. */
    gpio_otype_t otype;      /**< Output type (ignored if mode is INPUT or ANALOGUE). */
    gpio_speed_t speed;      /**< Slew rate (ignored if mode is INPUT or ANALOGUE). */
    gpio_pull_t  pull;       /**< Pull configuration. */
    uint8_t      alternate;  /**< AF0..AF15, valid only when mode is ALTERNATE. */
} gpio_pin_config_t;

/* ------------------------------------------------------------------ */
/* API                                                                 */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise the GPIO subsystem.
 *
 * Enables peripheral clocks for every GPIO port the current build target
 * exposes. Must be called once, from main(), before the FreeRTOS scheduler
 * is started, and before any other GPIO function. Subsequent calls are
 * no-ops and return GPIO_OK.
 *
 * @return GPIO_OK on success.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
gpio_err_t gpio_init(void);

/**
 * @brief Configure a single pin.
 *
 * Applies the full config in one call. The order of register writes is
 * chosen so that the pin is not transiently driven incorrectly during
 * reconfiguration (mode is set last among configuration writes).
 *
 * @param[in] config Pointer to a fully populated config descriptor.
 *
 * @return GPIO_OK on success; GPIO_ERR_NOT_INITIALISED, GPIO_ERR_INVALID_PORT,
 *         GPIO_ERR_INVALID_PIN, GPIO_ERR_INVALID_MODE, GPIO_ERR_INVALID_CONFIG,
 *         or GPIO_ERR_NULL_POINTER on failure.
 *
 * @note Threading: task-context only, non-blocking. NOT ISR-safe — performs
 *       read-modify-write on shared per-port registers. NOT safe to call
 *       concurrently on pins of the same port; serialise externally if
 *       configuration must occur after the scheduler has started.
 */
gpio_err_t gpio_configure_pin(const gpio_pin_config_t *config);

/**
 * @brief Read a pin's input level.
 *
 * @param[in]  port      Port containing the pin.
 * @param[in]  pin       Pin number, 0..15.
 * @param[out] out_level Resulting level (HIGH or LOW).
 *
 * @return GPIO_OK on success; GPIO_ERR_NOT_INITIALISED, GPIO_ERR_INVALID_PORT,
 *         GPIO_ERR_INVALID_PIN, or GPIO_ERR_NULL_POINTER on failure.
 *
 * @note Threading: ISR-safe. The implementation is a single 32-bit register
 *       read.
 */
gpio_err_t gpio_read_pin(gpio_port_t port, uint8_t pin, gpio_level_t *out_level);

/**
 * @brief Drive an output pin to the requested level.
 *
 * @param[in] port  Port containing the pin.
 * @param[in] pin   Pin number, 0..15.
 * @param[in] level Target level.
 *
 * @return GPIO_OK on success; GPIO_ERR_NOT_INITIALISED, GPIO_ERR_INVALID_PORT,
 *         or GPIO_ERR_INVALID_PIN on failure.
 *
 * @note Threading: ISR-safe. The implementation is a single 32-bit write to
 *       the bit-set/reset register (BSRR), which is atomic with respect to
 *       other writers on the same port.
 */
gpio_err_t gpio_write_pin(gpio_port_t port, uint8_t pin, gpio_level_t level);

/**
 * @brief Invert an output pin's current level.
 *
 * @param[in] port Port containing the pin.
 * @param[in] pin  Pin number, 0..15.
 *
 * @return GPIO_OK on success; GPIO_ERR_NOT_INITIALISED, GPIO_ERR_INVALID_PORT,
 *         or GPIO_ERR_INVALID_PIN on failure.
 *
 * @note Threading: task-context only, non-blocking. NOT ISR-safe — performs
 *       read-modify-write on ODR. If toggling from an ISR is required,
 *       use gpio_write_pin() with an externally tracked desired level.
 */
gpio_err_t gpio_toggle_pin(gpio_port_t port, uint8_t pin);

#endif /* GPIO_DRIVER_H */
```

### 2.3 API design rationale

A few choices deserve note:

- **Struct-passed configuration.** `gpio_configure_pin()` takes a config struct rather than a long argument list. Seven distinct fields would be error-prone as positional parameters; the struct also makes call sites self-documenting and allows future field additions without breaking existing callers.
- **All-or-nothing configuration.** There is no per-attribute setter (`gpio_set_mode`, `gpio_set_pull`, …). Each consumer configures its pins once at init with a complete descriptor and never reconfigures them. Per-attribute setters would invite the anti-pattern of partial reconfiguration in unexpected contexts.
- **No interrupt configuration in this API.** EXTI (external interrupt) setup is a separate concern, owned by an `ExtiDriver` companion (planned). GPIO configures pins; EXTI wires them to the NVIC. Splitting keeps each driver narrow.
- **No deinit.** GPIO has no resource to release. The peripheral clocks remain enabled for the lifetime of the system. Adding a `gpio_deinit` would suggest a use case that does not exist.

---

## 3. Internal design

### 3.0 Private struct

```c
typedef struct {
    bool          initialised;                  /**< Set by gpio_init(); guards all entry points. */
    GPIO_TypeDef *port_map[GPIO_PORT_COUNT];    /**< CMSIS peripheral pointer per gpio_port_t. */
    uint32_t      clock_bits[GPIO_PORT_COUNT];  /**< RCC AHB enable bit per gpio_port_t. */
} gpio_driver_t;

static gpio_driver_t s_gpio;
```


### 3.1 Module state

The module holds three pieces of static state:

| Symbol | Type | Purpose |
|---|---|---|
| `s_initialised` | `static bool` | Set true by `gpio_init()`; checked by every other entry point. |
| `s_port_to_peripheral` | `static GPIO_TypeDef * const []` | Maps `gpio_port_t` to the CMSIS peripheral pointer (`GPIOA`, `GPIOB`, …) defined in the device header. |
| `s_port_to_clock_bit` | `static const uint32_t []` | Maps `gpio_port_t` to the RCC enable-register bit for that port. |

No instance data, no FreeRTOS objects, no buffers. The driver is a thin wrapper over register access.

### 3.2 Per-function internal flow

### gpio_init

**`gpio_init()`**

1. If `s_initialised` is true, return `GPIO_OK` (idempotent).
2. For each port in the build target's port set, set the corresponding bit in the RCC enable register (`AHB1ENR` on F469, `AHB2ENR` on L475).
3. Issue the standard RM-mandated dummy read of the enable register after the write, to allow the clock to stabilise before any GPIO register access.
4. Set `s_initialised` to true.
5. Return `GPIO_OK`.

### gpio_configure_pin

**`gpio_configure_pin(config)`**

1. Reject if `config` is NULL → `GPIO_ERR_NULL_POINTER`.
2. Reject if `s_initialised` is false → `GPIO_ERR_NOT_INITIALISED`.
3. Validate `config->port` against the build target → `GPIO_ERR_INVALID_PORT` on failure.
4. Validate `config->pin <= 15` → `GPIO_ERR_INVALID_PIN` on failure.
5. Validate `config->mode` is a known enum value → `GPIO_ERR_INVALID_MODE` on failure.
6. Validate `config->alternate <= 15` when `mode == GPIO_MODE_ALTERNATE` → `GPIO_ERR_INVALID_CONFIG` on failure.
7. Look up the peripheral pointer.
8. Apply registers in this order to avoid transient mis-driving:
   - `OTYPER`  — output type (clear-then-set the 1 bit at offset `pin`).
   - `OSPEEDR` — output speed (clear-then-set the 2 bits at offset `2*pin`).
   - `PUPDR`   — pull configuration (clear-then-set the 2 bits at offset `2*pin`).
   - `AFR[pin/8]` — alternate function (clear-then-set the 4 bits at offset `4*(pin%8)`).
   - `MODER`   — mode, written last (clear-then-set the 2 bits at offset `2*pin`).

   Writing `MODER` last ensures that when the pin changes function, all of its supporting attributes are already in place. Each write is a read-modify-write of the corresponding port register.
9. Return `GPIO_OK`.

### gpio_read_pin

**`gpio_read_pin(port, pin, out_level)`**

1. Reject if `out_level` is NULL → `GPIO_ERR_NULL_POINTER`.
2. Reject if `s_initialised` is false → `GPIO_ERR_NOT_INITIALISED`.
3. Validate `port` and `pin`.
4. Read the port's `IDR`, mask the pin bit, write `GPIO_LEVEL_HIGH` or `GPIO_LEVEL_LOW` through `out_level`.
5. Return `GPIO_OK`.

### gpio_write_pin

**`gpio_write_pin(port, pin, level)`**

1. Reject if `s_initialised` is false.
2. Validate `port` and `pin`.
3. Write to `BSRR`: bit `pin` to set (level high), bit `pin + 16` to reset (level low). A single 32-bit write — atomic.
4. Return `GPIO_OK`.

### gpio_toggle_pin

**`gpio_toggle_pin(port, pin)`**

1. Reject if `s_initialised` is false.
2. Validate `port` and `pin`.
3. Read the port's `ODR`, XOR the pin bit, write back. (Read-modify-write on `ODR` — see threading note in §2.2.)
4. Return `GPIO_OK`.

### 3.3 Synchronisation

The driver has no internal mutex. The thread-safety contract is:

- `gpio_init()` runs from `main()` before the scheduler is started — no concurrency possible.
- `gpio_configure_pin()` is invoked from each consumer driver's init function. By the system design, all driver init runs sequentially in `main()` before the scheduler is started. Concurrent configuration of pins on the same port is therefore impossible by construction.
- `gpio_read_pin()` and `gpio_write_pin()` access `IDR` and `BSRR` respectively. Both are inherently atomic (single 32-bit access to dedicated read or set-reset registers); no synchronisation is required.
- `gpio_toggle_pin()` performs a read-modify-write on `ODR`. Concurrent calls on pins of the same port from different tasks would race. The driver does not protect against this — consumers must own their pins (each pin has one owner) and ensure they do not call `gpio_toggle_pin()` on a port from multiple contexts. In practice, toggle is used for blink-style indicators owned by one task each.

This contract is documented in the threading annotations in §2.2 and is the deliberate cost of avoiding mutex overhead in a driver that runs predominantly during init.

### 3.4 Principles applied

- **P1 (Strict directional layering).** The driver uses only CMSIS device headers. No upward dependency on any middleware or application component. Verified by the empty inclusion list of `gpio_driver.c` above CMSIS.
- **P5 (Bounded resources, no dynamic allocation post-init).** All driver state is `static const` or `static`. No heap, no FreeRTOS objects.
- **P6 (Responsibility traces to requirements).** The driver's existence is justified by REQ-NF-202 directly and by every consumer that depends on it transitively. No speculative API surface — interrupts, lock-protect, and debounce are deliberately omitted because no current consumer needs them.
- **P10 (Naming conventions).** Module prefix `gpio_` on every public symbol; types `_t`-suffixed; enums `GPIO_`-prefixed; the interface name in `components.md` is `IGpio`, which maps 1:1 to this header.

**Principles considered and found not to apply:**

- **P3 (Interface Segregation)** would split if reader-consumers and writer-consumers had different needs. They do not: every GPIO consumer configures, reads, and writes its own pins. A `IGpioReader` / `IGpioWriter` split would have no separate consumer set and would be ceremony.
- **P7 (Pull-based access)** addresses producers of shared data with multiple consumers. GPIO is per-pin ownership — each consumer drives its own pins, and there is no shared data set being polled.

---

## 4. Hardware contract

### 4.1 Peripherals

| Aspect | STM32F469 (Field Device) | STM32L475 (Gateway) |
|---|---|---|
| Available ports | GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH, GPIOI, GPIOJ, GPIOK | GPIOA, GPIOB, GPIOC, GPIOD, GPIOE, GPIOF, GPIOG, GPIOH |
| Clock enable register | `RCC->AHB1ENR` | `RCC->AHB2ENR` |
| Clock enable bits | `GPIOAEN` (bit 0) through `GPIOKEN` (bit 10) | `GPIOAEN` (bit 0) through `GPIOHEN` (bit 7) |
| Reference manual | RM0386, GPIO chapter | RM0351, GPIO chapter |
| Device header | `stm32f469xx.h` (CMSIS) | `stm32l475xx.h` (CMSIS) |

The CMSIS device header defines `GPIOA`, `GPIOB`, … as pointers to `GPIO_TypeDef` instances mapped at the correct peripheral base addresses. The driver does not hard-code addresses; it uses the CMSIS pointers.

### 4.2 Registers used

The register set is the standard STM32 GPIO peripheral and is identical between RM0386 and RM0351. The driver touches the following members of `GPIO_TypeDef`:

| Register | Width | R/W | Purpose | Touched in |
|---|---|---|---|---|
| `MODER` | 32-bit | R/W | Mode (input / output / alternate / analogue), 2 bits per pin. | `gpio_configure_pin` |
| `OTYPER` | 32-bit | R/W | Output type (push-pull / open-drain), 1 bit per pin. | `gpio_configure_pin` |
| `OSPEEDR` | 32-bit | R/W | Output slew rate, 2 bits per pin. | `gpio_configure_pin` |
| `PUPDR` | 32-bit | R/W | Pull configuration, 2 bits per pin. | `gpio_configure_pin` |
| `IDR` | 32-bit | R | Input data, 1 bit per pin. | `gpio_read_pin` |
| `ODR` | 32-bit | R/W | Output data, 1 bit per pin. | `gpio_toggle_pin` |
| `BSRR` | 32-bit | W | Bit set/reset, 16 set bits and 16 reset bits. | `gpio_write_pin` |
| `AFR[0]` / `AFR[1]` | 32-bit each | R/W | Alternate function low (pins 0..7) and high (pins 8..15), 4 bits per pin. | `gpio_configure_pin` |

`LCKR` (configuration lock register) is not used — pin configuration locks are not required by any consumer and would prevent legitimate reconfiguration during testing.

### 4.3 Clock tree dependency

GPIO peripheral clocks are gated by RCC. The driver enables every port present on the build target during `gpio_init()`. The clock source for these ports is the AHB clock domain (AHB1 on F469, AHB2 on L475), which is configured by the system clock setup code prior to `main()` — outside the scope of this driver.

### 4.4 NVIC

The GPIO driver itself uses no interrupts. EXTI (external interrupt) configuration is delegated to a separate `ExtiDriver` companion (planned). When a consumer needs an interrupt-driven input (e.g., the WiFi module's DRDY line on the Gateway, UM2153 §A pin PE1), it calls both `gpio_configure_pin()` and the EXTI driver. The two drivers do not depend on each other.

### 4.5 Pin assignments

Pin selection is the consumer's responsibility. The GPIO driver provides the mechanism; each consumer companion (`led-driver.md`, `wifi-driver.md`, sensor companions) cites the specific pins it uses against:

- **STM32F469 Discovery (Field Device):** UM1932 §4 (peripheral connections per on-board component) and §7 (board layout).
- **STM32L475 IoT Discovery (Gateway):** UM2153 Appendix A Table 11 (I/O assignment).

This companion intentionally does not enumerate per-consumer pin lists — that would duplicate consumer companions and break when those companions evolve.

---

## 5. Sequence integration

`grep -i "gpio\|igpio" sequence-diagrams.md` returns no matches. GPIO does not appear as a lifeline in any of the 18 HLD sequence diagrams.

This is the expected and correct outcome. GPIO operations are nested inside each consumer driver's init and runtime sequences:

- During board initialisation, each consumer driver calls `gpio_configure_pin()` zero or more times. The sequence diagrams capture this as a single high-level `<consumer>_init()` lifeline event, with the GPIO calls treated as implementation detail.
- During runtime, GPIO read/write/toggle calls occur inside higher-level operations (e.g., `LedDriver::led_set_on()` calls `gpio_write_pin()`). The SDs capture the higher-level operation; the GPIO call is implicit.

No `<consumer>_init()` SD exists in `sequence-diagrams.md` for the driver layer either. Driver initialisation is treated as a precondition for all SDs and is not itself sequenced. This is consistent with the HLD's stance that SDs document interesting runtime flows, not boot setup.

**Verification:** the public API in §2 is sufficient for any future SD that wishes to make GPIO operations explicit — configure-once-at-init plus runtime read/write/toggle covers the full set of operations any consumer performs.

---

## 6. Error and fault behaviour

### 6.1 Error enum

Defined in §2.2. Recap with producing conditions:

| Code | Produced by | Trigger |
|---|---|---|
| `GPIO_OK` | All functions | Operation completed successfully. |
| `GPIO_ERR_NOT_INITIALISED` | All functions except `gpio_init` | Called before `gpio_init()`. |
| `GPIO_ERR_INVALID_PORT` | `configure`, `read`, `write`, `toggle` | Port enum value outside the build target's port set. |
| `GPIO_ERR_INVALID_PIN` | `configure`, `read`, `write`, `toggle` | Pin number > 15. |
| `GPIO_ERR_INVALID_MODE` | `configure` | Mode enum value outside `INPUT`/`OUTPUT`/`ALTERNATE`/`ANALOGUE`. |
| `GPIO_ERR_INVALID_CONFIG` | `configure` | `alternate` field out of range (>15) when `mode == ALTERNATE`. |
| `GPIO_ERR_NULL_POINTER` | `configure`, `read` | NULL pointer where a valid pointer is required. |

### 6.2 Retry policy

None. GPIO operations are register accesses with deterministic outcomes; retry would not change the result. Validation failures are programmer errors and are surfaced immediately.

### 6.3 Downstream failure

`USES (downward): CMSIS`. CMSIS register access cannot fail at runtime — there is no return code, no I/O timeout, no resource contention beneath the register layer. The driver therefore has no downstream failure to handle.

### 6.4 Observability

GPIO does not expose an `IGpioStats` interface. The driver has no counters worth surfacing: every operation either succeeds or fails synchronously on input validation, and validation failures are the consumer's bug, not a runtime metric.

No log calls are issued in the happy path. On a validation failure, the driver returns the error code and lets the caller decide whether to log. This is the standard convention for thin register drivers: silent on success, return-code on failure, no implicit side effects.

The driver does not contribute to `IHealthReport` either. No condition the GPIO driver detects rises to the level of a system health event.

---

## 7. Unit-test plan

### 7.1 Test framework and location

- **Framework:** Unity (ThrowTheSwitch.org).
- **File:** `tests/field-device/drivers/test_gpio_driver.c` and `tests/gateway/drivers/test_gpio_driver.c`.
- **Build target:** host (PC). The test does not run on the target board.

### 7.2 Mock strategy

The driver `#include`s a build-target-specific device header (`stm32f469xx.h` or `stm32l475xx.h`) that defines `GPIOA`, `GPIOB`, … as `GPIO_TypeDef *` pointers to peripheral addresses. For host testing, a dedicated stub header — `tests/mocks/stm32_cmsis_mock.h` — provides:

- A `GPIO_TypeDef` struct identical in layout to the real one.
- An array of `GPIO_TypeDef` instances backing each port, declared `volatile`.
- Macros `GPIOA`, `GPIOB`, … that resolve to pointers into this array.
- An `RCC_TypeDef` struct and `RCC` pointer with the same convention.

The driver source is compiled unchanged against the mock header (selected via test-target `CFLAGS`). All register writes target the mock arrays; tests assert on the resulting in-memory values.

CMock not used here because the seam is register access, not function calls; CMock will land with the first driver whose dependencies are function calls (likely Logger consuming UartDriver).

### 7.3 Test cases

For each public function: one happy-path case minimum, one error-path case minimum, and one boundary-condition case where applicable. Listed by `test_<module>_<function>_<scenario>` naming.

**`gpio_init`**

- `test_gpio_init_succeeds_first_call`
- `test_gpio_init_idempotent_second_call_returns_ok`
- `test_gpio_init_sets_rcc_ahb1enr_gpio_a_through_k_bits` (verifies the right RCC bits set)
- `test_gpio_init_sets_rcc_ahb1enr_gpio_a_through_h_bits` (verifies the right RCC bits set)

**`gpio_configure_pin`**

- `test_gpio_configure_pin_output_push_pull_succeeds`
- `test_gpio_configure_pin_input_pull_up_succeeds`
- `test_gpio_configure_pin_alternate_function_writes_afr_correctly`
- `test_gpio_configure_pin_analogue_clears_mode_bits_correctly`
- `test_gpio_configure_pin_writes_moder_last` (order verification — see §3.2)
- `test_gpio_configure_pin_returns_not_initialised_before_init`
- `test_gpio_configure_pin_rejects_null_config`
- `test_gpio_configure_pin_rejects_invalid_port`
- `test_gpio_configure_pin_rejects_pin_above_15`
- `test_gpio_configure_pin_rejects_invalid_mode`
- `test_gpio_configure_pin_rejects_alternate_above_15`

**`gpio_read_pin`**

- `test_gpio_read_pin_high_when_idr_bit_set`
- `test_gpio_read_pin_low_when_idr_bit_clear`
- `test_gpio_read_pin_rejects_null_out_level`
- `test_gpio_read_pin_rejects_invalid_port`
- `test_gpio_read_pin_rejects_pin_above_15`
- `test_gpio_read_pin_returns_not_initialised_before_init`

**`gpio_write_pin`**

- `test_gpio_write_pin_high_sets_lower_bsrr_bit`
- `test_gpio_write_pin_low_sets_upper_bsrr_bit`
- `test_gpio_write_pin_rejects_invalid_port`
- `test_gpio_write_pin_rejects_pin_above_15`
- `test_gpio_write_pin_returns_not_initialised_before_init`

**`gpio_toggle_pin`**

- `test_gpio_toggle_pin_inverts_odr_bit`
- `test_gpio_toggle_pin_preserves_other_odr_bits`
- `test_gpio_toggle_pin_rejects_invalid_port`
- `test_gpio_toggle_pin_rejects_pin_above_15`
- `test_gpio_toggle_pin_returns_not_initialised_before_init`

### 7.4 Coverage target

- 100% of public API functions exercised.
- ≥ 90% statement coverage in `gpio_driver.c`.
- Every error code in `gpio_err_t` produced by at least one test case.

Coverage measured by `gcov` during host builds. Coverage report committed alongside the test results during integration.

### 7.5 Cannot be host-tested

- Actual hardware behaviour (electrical levels, settling times, pull strength) — verified only on board during integration.
- Concurrency hazards on `gpio_configure_pin` and `gpio_toggle_pin` — the host build is single-threaded; the contract in §3.3 is the only safeguard.

These are documented limitations, not gaps. Hardware verification is the integration-phase task.

---

## 8. Open items

| ID | Item | Resolution path | Status |
|---|---|---|---|
| GPIO-O1 | Final list of GPIO ports actually used by the build, per board. `gpio_init()` enables every available port today; trimming to "only ports any consumer uses" would save a few microamps of clock domain power but complicates the init. | Decision deferred until all consumer driver companions land; final port set re-evaluated then. Default for v1.0: enable every available port. | Open |
| GPIO-O2 | EXTI integration. Several Gateway consumers (WifiDriver DRDY, BUTTON_EXTI13 if a console-button feature is added, etc.) need interrupt-driven pin events. EXTI configuration is intentionally outside this companion's scope. | Address in `exti-driver.md` (Tier 1, to be added to `lld.md` §4 companion catalogue). | Open |
| GPIO-O3 | Wake-up pin behaviour during low-power modes. Not specified by any SRS requirement at LLD time; low-power is not in current scope. | Defer until / unless a low-power requirement is added to the SRS. | Open |

None of the inherited TBDs from `lld.md` §5 (O1 WiFi SPI naming, O2 stack measurements, O3 watchdog scope) is resolved by this companion. They remain open in `lld.md`.

---

*This is the GpioDriver companion. It is the first LLD companion to be drafted and serves as the validation case for the methodology in `lld-methodology.md`. Subsequent companions follow the same eight-section structure and the same gate criteria.*
