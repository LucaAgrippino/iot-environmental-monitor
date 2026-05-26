# RtcDriver — LLD Companion

**Document:** `docs/lld/drivers/rtc-driver.md`
**Version:** 0.2 (Pass H complete — ready for implementation)
**Board scope:** Field Device (STM32F469) and Gateway (B-L475E-IOT01A)
**Layer:** Driver
**Status:** Pass H complete
**Date:** May 2026

**HLD anchor:** `RtcDriver` in `components.md` (FD + GW driver layer)

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Keeps wall-clock time across reboots using the backup-domain RTC | `components.md` |
| PROVIDES (upward) | `IRtc` | `components.md` |
| USES (downward) | CMSIS | `components.md` |
| Consumers | `Logger` (read-only, bootstrap exception), `TimeProvider` (read + write + backup-register access) | `components.md` preamble |
| Hardware | Backup-domain RTC; LSE 32.768 kHz crystal on both boards | UM1932 §4.3.2 (F469), UM2153 block diagram (L475) |

### 1.1 Requirements traceability

| Requirement / decision | Role in this driver |
|---|---|
| REQ-NF-213 | Wall-clock time persistence across resets (root requirement) |
| REQ-TS-020 | NTP write path — `rtc_set_time` is the terminal call |
| REQ-TS-040 | Sync-state persistence across warm resets (backup register API) |
| REQ-NF-212 | Time synchronisation flag survives warm resets (backup register API) |
| REQ-NF-500 | Logger consumer (timestamp formatting in log entries) |
| LLD-D10 | Driver exposes `irtc_t` vtable for testability |
| LLD-D16 | Backup-register accessors added to `IRtc` to keep register-map knowledge inside the driver |

Both consumers are passive (`task-breakdown.md` §4.1, §5.1). All calls run in the calling task's context. No ISR, no DMA, no callback is required.

---

## 2. Public API

### 2.1 Dependency-conformance check (`lld-methodology.md` v1.1 §Step 2)

The public header (`rtc_driver.h`) must `#include` only what is permitted by `USES (downward): CMSIS`. Permitted includes are `stm32f469xx.h` / `stm32l475xx.h` (resolved through the board-agnostic CMSIS device header) and `stdint.h` / `stdbool.h`. No FreeRTOS headers, no other driver headers. Confirmed clean.

### 2.2 P3 (Interface Segregation) — considered and declined

`Logger` is read-only (`get_time` only); `TimeProvider` is read + write + backup-register accessors. P3 (`architecture-principles.md`) prescribes splitting when reader and writer consumer classes exist. Considered but declined:

- The interface is six functions — already minimal for the responsibilities listed.
- The Logger dependency is a **documented bootstrap exception** (`components.md` preamble), not a normal architectural pattern. Splitting an interface to formalise an exception adds structural complexity without clarity benefit.
- In OOP-in-C, two separate function-pointer tables would double the static footprint without expressing any new constraint that code review cannot already enforce.

**Decision:** keep `IRtc` as a single interface. Document in the `Logger` companion that Logger must call only `get_time` and must never call `set_time`, `read_backup`, or `write_backup`. Enforcement is by code review.

### 2.3 Data types

```c
/**
 * @brief Calendar date and time, all fields in binary (not BCD).
 *
 * The driver converts to/from the hardware BCD format internally;
 * consumers never handle BCD.
 *
 * year:   2000..2099 (full year, e.g. 2026)
 * month:  1..12
 * day:    1..31
 * hour:   0..23 (24-hour format; 12-hour mode disabled in hardware)
 * minute: 0..59
 * second: 0..59
 */
typedef struct {
    uint16_t year;
    uint8_t  month;
    uint8_t  day;
    uint8_t  hour;
    uint8_t  minute;
    uint8_t  second;
} rtc_datetime_t;

/**
 * @brief Error codes returned by all RtcDriver operations.
 *
 * Naming follows the cross-cutting convention established in lld.md §3.2:
 * <module>_err_t, not _status_t.
 */
typedef enum {
    RTC_ERR_OK             = 0,  /**< Operation succeeded. */
    RTC_ERR_INIT_TIMEOUT   = 1,  /**< INITF flag did not assert within timeout. */
    RTC_ERR_SYNC_TIMEOUT   = 2,  /**< RSF flag did not assert within timeout. */
    RTC_ERR_NULL_ARG       = 3,  /**< Null pointer passed to a pointer parameter. */
    RTC_ERR_BACKUP_BOUNDS  = 4,  /**< Backup register index out of range for this board. */
    RTC_ERR_LSE_NOT_READY  = 5,  /**< LSE oscillator is not running at rtc_init() entry. */
} rtc_err_t;

/* Maximum backup register indices per board (LLD-D16).                    */
/* The driver selects one of these at compile time via the active CMSIS    */
/* device header. The corresponding RTC_BACKUP_MAX_IDX symbol is defined   */
/* inside the implementation translation unit.                             */
#define RTC_BACKUP_MAX_IDX_F469  19U   /**< F469: BKP0R..BKP19R (20 registers) */
#define RTC_BACKUP_MAX_IDX_L475  31U   /**< L475: BKP0R..BKP31R (32 registers) */
```

### 2.4 Public API (`rtc_driver.h`)

```c
/**
 * @brief Initialise the RTC peripheral.
 *
 * Must be called once from main() before the FreeRTOS scheduler starts,
 * so that Logger can read timestamps during the Init phase (bootstrap
 * exception — components.md §1 preamble).
 *
 * Pre-conditions:
 *  - LSE oscillator has been started and stabilised by system clock
 *    configuration code. The driver verifies LSERDY and returns
 *    RTC_ERR_LSE_NOT_READY if absent.
 *
 * Behaviour:
 *  - Enables the PWR peripheral clock and sets DBP to unlock the backup
 *    domain.
 *  - Selects LSE as RTC clock source and enables the RTC clock in
 *    RCC_BDCR.
 *  - Reads RTC_ISR.INITS to detect whether the backup domain survived
 *    the last reset, and captures the result in the internal
 *    backup_valid flag (immutable thereafter).
 *  - If the backup domain was reset (INITS = 0): enters init mode,
 *    configures 24-hour format (RTC_CR.FMT = 0), writes the prescaler
 *    (PREDIV_A = 127, PREDIV_S = 255) and the default epoch
 *    (2000-01-01 00:00:00), exits init mode, waits for RSF.
 *  - If the backup domain is valid (INITS = 1): leaves the prescaler
 *    and calendar registers untouched.
 *  - Idempotent: a second call returns RTC_ERR_OK with no side effects.
 *
 * @return RTC_ERR_OK on success.
 *         RTC_ERR_LSE_NOT_READY if LSE is not running.
 *         RTC_ERR_INIT_TIMEOUT if INITF does not assert within timeout.
 *         RTC_ERR_SYNC_TIMEOUT if RSF does not assert within timeout
 *         (only possible on cold-start path).
 *
 * @note Threading: task-context only. Must be called before the
 *       scheduler starts.
 */
rtc_err_t rtc_init(void);

/**
 * @brief Read the current wall-clock time from the RTC.
 *
 * Safe to call from any task context. Caller serialises concurrent
 * calls — the driver is not internally synchronised (LLD-D4).
 *
 * Waits for RTC_ISR.RSF to be set before reading, to avoid returning
 * stale shadow-register values immediately after rtc_set_time().
 *
 * @param[out] dt Pointer to caller-allocated rtc_datetime_t; populated
 *                on RTC_ERR_OK. Zeroed on error.
 * @return RTC_ERR_OK on success.
 *         RTC_ERR_NULL_ARG if dt is NULL.
 *         RTC_ERR_SYNC_TIMEOUT if RSF does not assert within timeout.
 *
 * @note Threading: task-context only, non-blocking (≤ 2 ms worst case).
 *       Not ISR-safe.
 * @note Pre-condition (debug builds): rtc_init() has returned OK.
 *       Asserted with assert(); undefined in release builds.
 */
rtc_err_t rtc_get_time(rtc_datetime_t *dt);

/**
 * @brief Write a new wall-clock time to the RTC.
 *
 * Called by TimeProvider after a successful NTP synchronisation
 * (REQ-TS-020). Must NOT be called by Logger (bootstrap exception —
 * Logger is read-only).
 *
 * Sequence: unlock WPR → enter init mode (wait INITF) → write TR/DR →
 * exit init mode → lock WPR → wait RSF.
 *
 * @param dt Pointer to the new date/time. Must not be NULL.
 * @return RTC_ERR_OK on success.
 *         RTC_ERR_NULL_ARG if dt is NULL.
 *         RTC_ERR_INIT_TIMEOUT if INITF does not assert within timeout.
 *         RTC_ERR_SYNC_TIMEOUT if RSF does not assert within timeout.
 *
 * @note Threading: task-context only, non-blocking (≤ 4 ms worst case).
 *       Not ISR-safe.
 * @note Pre-condition (debug builds): rtc_init() has returned OK.
 *       Asserted with assert(); undefined in release builds.
 */
rtc_err_t rtc_set_time(const rtc_datetime_t *dt);

/**
 * @brief Return whether the backup domain survived the last reset.
 *
 * Returns true if RTC_ISR.INITS was set at init time, meaning the
 * calendar had been previously initialised and the backup supply
 * preserved it. Returns false if the backup domain was reset — the
 * calendar was reinitialised to the default epoch and timestamps are
 * not meaningful until a successful rtc_set_time() call.
 *
 * The flag is captured once during rtc_init() and is immutable for
 * the lifetime of the module — calling rtc_set_time() does not flip
 * it.
 *
 * TimeProvider uses this flag to decide the initial sync state
 * reported through IHealthReport and to gate the "synchronised" flag
 * exposed via ITimeProvider (REQ-NF-212).
 *
 * @return true if backup domain was valid at init; false otherwise.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 * @note Pre-condition (debug builds): rtc_init() has returned OK.
 */
bool rtc_is_backup_valid(void);

/**
 * @brief Read one RTC backup register.
 *
 * Backup registers survive warm resets (watchdog, soft reset, NRST
 * pin) as long as VBAT or VDD supplies the backup domain. They do not
 * survive a full power-off on boards where VBAT is tied to VDD (see
 * §4.5).
 *
 * DBP is set by rtc_init() and remains set; callers do not need to
 * manage it.
 *
 * @param idx     Backup register index. Valid range: 0..19 on F469,
 *                0..31 on L475. Returns RTC_ERR_BACKUP_BOUNDS if
 *                exceeded.
 * @param[out] out Set to the 32-bit register value on RTC_ERR_OK.
 *                 Untouched on error.
 * @return RTC_ERR_OK, RTC_ERR_NULL_ARG, or RTC_ERR_BACKUP_BOUNDS.
 *
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 * @note Pre-condition (debug builds): rtc_init() has returned OK.
 */
rtc_err_t rtc_read_backup(uint8_t idx, uint32_t *out);

/**
 * @brief Write one RTC backup register.
 *
 * Same DBP and VBAT constraints as rtc_read_backup(). No WPR unlock
 * needed — backup registers are not protected by WPR (only TR, DR,
 * and CR are).
 *
 * @param idx   Backup register index. Valid range: 0..19 on F469,
 *              0..31 on L475.
 * @param value 32-bit value to write.
 * @return RTC_ERR_OK or RTC_ERR_BACKUP_BOUNDS.
 *
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 * @note Pre-condition (debug builds): rtc_init() has returned OK.
 */
rtc_err_t rtc_write_backup(uint8_t idx, uint32_t value);
```

### 2.5 Test-only hooks (`#ifdef TEST`)

```c
#ifdef TEST
/**
 * @brief Reset module-level state for unit tests.
 *
 * Clears the internal s_rtc state to its post-bss-init value, so
 * each Unity test case starts from a clean slate. Test-only; not
 * compiled into firmware builds.
 *
 * Project-wide convention established in DebugUartDriver
 * (see session_summary 23 May 2026, "Patterns now established").
 */
void rtc_reset_for_test(void);
#endif
```

### 2.6 Singleton vtable interface (`IRtc` — LLD-D10)

```c
typedef struct {
    rtc_err_t (*init)(void);
    rtc_err_t (*get_time)(rtc_datetime_t *dt);
    rtc_err_t (*set_time)(const rtc_datetime_t *dt);
    bool      (*is_backup_valid)(void);
    rtc_err_t (*read_backup)(uint8_t idx, uint32_t *out);
    rtc_err_t (*write_backup)(uint8_t idx, uint32_t value);
} irtc_t;

/** Singleton pointer to the RtcDriver vtable (FD + GW). */
extern const irtc_t * const rtc_driver;
```

---

## 3. Internal design

### 3.1 Private state (consolidated)

Exactly one RTC peripheral per board; the driver is a singleton with no handle. All file-scope state is consolidated into a single struct, matching the convention established in `DebugUartDriver` and `GpioDriver`.

```c
typedef struct {
    bool initialised;   /**< Set true at end of successful rtc_init().      */
    bool backup_valid;  /**< Captured at rtc_init() from RTC_ISR.INITS;     */
                        /*   immutable thereafter.                          */
} rtc_driver_t;

static rtc_driver_t s_rtc;  /* Zero-initialised by .bss startup. */
```

The peripheral itself is accessed directly via the CMSIS `RTC` macro (which expands to an `RTC_TypeDef *` at the fixed peripheral base address). No runtime pointer indirection is needed for register access; in test builds the macro is redefined to point at a mock `RTC_TypeDef` instance (see §7).

### 3.2 Singleton vtable instance

In `rtc_driver.c`:

```c
static const irtc_t s_rtc_vtable = {
    .init            = rtc_init,
    .get_time        = rtc_get_time,
    .set_time        = rtc_set_time,
    .is_backup_valid = rtc_is_backup_valid,
    .read_backup     = rtc_read_backup,
    .write_backup    = rtc_write_backup,
};

const irtc_t * const rtc_driver = &s_rtc_vtable;
```

### 3.3 BCD conversion helpers (internal linkage)

The STM32 RTC stores hours, minutes, seconds, and date fields in two-digit BCD. Consumers receive and supply binary values. Two `static` helpers:

```c
static uint8_t bcd_to_bin(uint8_t bcd);   /* e.g. 0x23 → 23 */
static uint8_t bin_to_bcd(uint8_t bin);   /* e.g. 23   → 0x23 */
```

Year is handled separately: the RTC stores the year as a two-digit offset from 2000 (`0..99`). `rtc_get_time` adds 2000; `rtc_set_time` subtracts 2000 before BCD conversion.

### 3.4 WPR unlock / lock sequence

The RTC `WPR` register protects `TR`, `DR`, and `CR` from accidental writes. The unlock sequence is two specific key writes; any other value re-locks.

```c
static inline void wpr_unlock(void) {
    RTC->WPR = 0xCAU;
    RTC->WPR = 0x53U;
}

static inline void wpr_lock(void) {
    RTC->WPR = 0xFFU;  /* Any invalid key re-enables protection. */
}
```

This sequence wraps every entry into init mode (inside `rtc_init` on cold start and inside `rtc_set_time`). `WPR` does **not** protect backup registers — those writes do not need WPR management.

### 3.5 DBP unlock and RCC clock enable

Performed once at the start of `rtc_init`. The PWR-peripheral register name differs between F469 (`PWR->CR`) and L475 (`PWR->CR1`); the implementation uses a board-conditional inline.

```c
static void backup_domain_unlock(void) {
    /* 1. Enable the PWR peripheral clock so its registers are writable. */
#if defined(STM32F469xx)
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;
    (void)RCC->APB1ENR;                 /* Read-back to ensure write took effect. */
    PWR->CR  |= PWR_CR_DBP;             /* Disable backup-domain write protection. */
#elif defined(STM32L475xx)
    RCC->APB1ENR1 |= RCC_APB1ENR1_PWREN;
    (void)RCC->APB1ENR1;
    PWR->CR1 |= PWR_CR1_DBP;
#else
#  error "Unsupported target — define STM32F469xx or STM32L475xx."
#endif
}

static rtc_err_t rtc_clock_select_and_enable(void) {
    /* Pre-condition: LSE has been started by system clock configuration. */
    if ((RCC->BDCR & RCC_BDCR_LSERDY) == 0U) {
        return RTC_ERR_LSE_NOT_READY;
    }

    /* Select LSE as RTC source and enable the RTC clock.                 */
    /* RTCSEL is write-once after a backup-domain reset — only write if   */
    /* not already configured, to avoid the spurious "ignored" path.      */
    uint32_t bdcr = RCC->BDCR;
    if ((bdcr & RCC_BDCR_RTCSEL) != RCC_BDCR_RTCSEL_0) {  /* LSE = 0b01 */
        bdcr &= ~RCC_BDCR_RTCSEL;
        bdcr |= RCC_BDCR_RTCSEL_0;
    }
    bdcr |= RCC_BDCR_RTCEN;
    RCC->BDCR = bdcr;
    return RTC_ERR_OK;
}
```

**Boundary with system clock config:** the driver does **not** start LSE itself. LSE setup (LSEON, waiting for LSERDY) is the responsibility of system clock configuration code, executed before `main()`. This split is deliberate: LSE is a shared resource that may be used by other peripherals (e.g., low-power timer), so its lifecycle belongs to the system layer, not to any single driver.

### 3.6 Per-function pseudocode

**`rtc_init`**

```
1. If s_rtc.initialised, return RTC_ERR_OK (idempotent).
2. backup_domain_unlock().
3. err = rtc_clock_select_and_enable(); if err, return err.
4. Capture s_rtc.backup_valid = (RTC->ISR & RTC_ISR_INITS) != 0.
5. If !s_rtc.backup_valid:
   a. wpr_unlock().
   b. Set RTC->ISR.INIT = 1.
   c. Poll RTC->ISR.INITF; if timeout (§4.2), wpr_lock() and return
      RTC_ERR_INIT_TIMEOUT.
   d. RTC->CR &= ~RTC_CR_FMT;                   /* 24-hour format */
   e. RTC->PRER = (127 << 16) | 255;            /* PREDIV_A, PREDIV_S */
   f. Write TR = bin_to_bcd-packed (0,0,0).
   g. Write DR = packed default-epoch (2000-01-01).
   h. Clear RTC->ISR.INIT.
   i. Poll RTC->ISR.RSF; if timeout, wpr_lock() and return
      RTC_ERR_SYNC_TIMEOUT.
   j. wpr_lock().
6. s_rtc.initialised = true.
7. Return RTC_ERR_OK.
```

**`rtc_get_time`**

```
1. assert(s_rtc.initialised).
2. If dt == NULL, return RTC_ERR_NULL_ARG.
3. Zero *dt for the error-return path.
4. Poll RTC->ISR.RSF until set; if timeout (§4.2), return
   RTC_ERR_SYNC_TIMEOUT.
5. Atomically snapshot TR and DR (read TR first, then DR — the read of
   TR locks the calendar shadow until DR is read; see RM0386 §27.3.6
   / RM0351 §38.4.4).
6. Decode BCD fields into *dt; add 2000 to year.
7. Return RTC_ERR_OK.
```

**`rtc_set_time`**

```
1. assert(s_rtc.initialised).
2. If dt == NULL, return RTC_ERR_NULL_ARG.
3. wpr_unlock().
4. Set RTC->ISR.INIT = 1.
5. Poll RTC->ISR.INITF; if timeout, wpr_lock() and return
   RTC_ERR_INIT_TIMEOUT.
6. Encode dt as BCD and pack into TR / DR (subtract 2000 from year).
7. Write RTC->TR and RTC->DR.
8. Clear RTC->ISR.INIT.
9. Poll RTC->ISR.RSF; if timeout, wpr_lock() and return
   RTC_ERR_SYNC_TIMEOUT.
10. wpr_lock().
11. Return RTC_ERR_OK.
```

**`rtc_is_backup_valid`**

```
1. assert(s_rtc.initialised).
2. Return s_rtc.backup_valid.
```

**`rtc_read_backup`**

```
1. assert(s_rtc.initialised).
2. If out == NULL, return RTC_ERR_NULL_ARG.
3. If idx > RTC_BACKUP_MAX_IDX, return RTC_ERR_BACKUP_BOUNDS.
4. *out = RTC->BKPR[idx].
5. Return RTC_ERR_OK.
```

**`rtc_write_backup`**

```
1. assert(s_rtc.initialised).
2. If idx > RTC_BACKUP_MAX_IDX, return RTC_ERR_BACKUP_BOUNDS.
3. RTC->BKPR[idx] = value.
4. Return RTC_ERR_OK.
```

### 3.7 No ISR, no DMA, no callbacks

The driver has no interrupt handler and registers no callbacks. The RTC alarm and wakeup-timer features of the peripheral are not used in this project's scope; the corresponding interrupt vectors are left at the CMSIS default. Consistent with the passive-consumer model documented in `task-breakdown.md` and with the convention used by `GpioDriver`.

### 3.8 Test-only hooks

```c
#ifdef TEST
void rtc_reset_for_test(void) {
    s_rtc.initialised = false;
    s_rtc.backup_valid = false;
}
#endif
```

Pattern established in `DebugUartDriver`. Every module with file-scope static state exposes this hook; tests call it from `setUp()`.

### 3.9 Principles applied

- **P1 (Strict directional layering).** Depends only on CMSIS device headers and standard library. No RTOS, no middleware, no other driver.
- **P2 (Dependency Inversion).** Exposes `irtc_t` vtable (LLD-D10). `Logger` and `TimeProvider` depend on `IRtc`, not on the concrete driver.
- **P3 (Interface Segregation).** Considered and declined — see §2.2. Read-only constraint on `Logger` enforced by code review.
- **P5 (Bounded resources, no dynamic allocation post-init).** Single `static rtc_driver_t s_rtc`; no heap; no RTOS objects.
- **P6 (Responsibility traces to requirements).** §1.1 lists root requirements (REQ-NF-213, REQ-TS-020, REQ-TS-040, REQ-NF-212, REQ-NF-500) and LLD decisions (LLD-D10, LLD-D16).
- **P8 (Total error propagation, no silent failures).** All public functions return `rtc_err_t`; every documented error path has a concrete enum value and a test case (§7).
- **P9 (BARR-C coding standard).** Fixed-width integer types; explicit unsigned literals (`0xCAU`); braces on all control flow; `const`-qualified pointer parameters where data is not modified.
- **P10 (Naming conventions).** Module prefix `rtc_`; interface type `irtc_t`; error values `RTC_ERR_*`; board-limit constants `RTC_BACKUP_MAX_IDX_F469` / `RTC_BACKUP_MAX_IDX_L475`.

---

## 4. Hardware contract

### 4.1 Clock source — both boards

| Board | Crystal | LSE frequency | PREDIV_A | PREDIV_S | Calendar tick |
|---|---|---|---|---|---|
| STM32F469 (FD) | X3 (UM1932 §4.3.2) | 32.768 kHz | 127 | 255 | 1 Hz |
| STM32L475 (GW) | X2 (UM2153 block diagram) | 32.768 kHz | 127 | 255 | 1 Hz |

Prescaler formula: `f_calendar = f_LSE / (PREDIV_A + 1) / (PREDIV_S + 1) = 32768 / 128 / 256 = 1 Hz`.

These values are written by the driver **only** when the backup domain has been reset (INITS = 0). When the backup domain survived (INITS = 1), the prescaler is already set correctly and must not be overwritten.

### 4.2 Timeout budgets

| Wait condition | Maximum duration (from RM) | Driver timeout | Notes |
|---|---|---|---|
| INITF after INIT = 1 | 2 RTCCLK cycles ≈ 61 µs | 2 ms | RTCCLK = LSE = 32.768 kHz |
| RSF after INIT = 0 or after read | 2 RTCCLK + 2 APB cycles | 2 ms | Generous margin at the lowest expected APB frequency |
| LSERDY check | N/A — read-only flag | 0 (immediate) | Pre-condition, no polling |

Timeouts are polled (busy-wait) using a software loop counter calibrated against the worst-case CPU clock. The total worst-case blocking time of `rtc_set_time` is **≤ 4 ms** (INITF wait + RSF wait). The only post-boot caller is `TimeProvider`, which runs in the gateway's `TimeServiceTask` (hourly NTP cadence) — 4 ms is well below any latency requirement.

For `rtc_get_time` called by `Logger`, RSF is normally already set in steady state (the calendar updates every 1 Hz); the RSF poll exits on the first iteration.

The use of busy-wait rather than an RTOS-blocking primitive is deliberate (LLD-D6): the wait is bounded and short, and importing FreeRTOS would violate the no-RTOS-in-drivers convention (LLD-D3).

### 4.3 Backup domain reset detection

`RTC_ISR.INITS` (bit 4) is set by hardware when the calendar has been initialised at least once since the last backup-domain reset. Reading INITS at the start of `rtc_init` is the canonical way to decide whether a cold-start epoch load is required. Behaviour is identical on F469 (RM0386 §27.6.4) and L475 (RM0351 §38.7.4).

The flag is captured once into `s_rtc.backup_valid` and is immutable thereafter — calling `rtc_set_time` does not flip it.

### 4.4 Cross-target register compatibility

Both `stm32f469xx.h` and `stm32l475xx.h` define `RTC_TypeDef` with the same relevant registers: `TR`, `DR`, `CR`, `ISR`, `PRER`, `WPR`, `SSR`. The bit positions for the fields used by this driver are identical across families.

**Backup register count differs by board (LLD-D16):**

| Board | Backup registers | Valid `idx` range | CMSIS access |
|---|---|---|---|
| STM32F469 (FD) | 20 (BKP0R..BKP19R) | 0..19 | `RTC->BKPR[idx]` |
| STM32L475 (GW) | 32 (BKP0R..BKP31R) | 0..31 | `RTC->BKPR[idx]` |

The implementation defines a single `RTC_BACKUP_MAX_IDX` symbol inside `rtc_driver.c`, selected at compile time via the active CMSIS device macro. `rtc_read_backup` / `rtc_write_backup` validate `idx` against this symbol.

### 4.5 Backup register persistence scope

RTC backup registers survive **warm resets** (watchdog, soft reset, NRST pin) on both boards. They do **not** survive a full power-off when VBAT is tied to VDD.

On the **STM32F469-DISCO** (FD) and **B-L475E-IOT01A** (GW), VBAT is connected to VDD through a filtering network — there is no separate coin cell. Consequences:

- Backup register contents **are lost on power removal**.
- The sync-persisted flag (the TimeProvider backup register) will not survive a power cycle → the system correctly restarts as **UNSYNCHRONISED** after a power-off.
- The flag **does** survive the resets that matter for REQ-TS-040 / REQ-NF-212 (watchdog, software reset, NVIC_SystemReset).

A hardware revision that adds a dedicated coin-cell VBAT would lift this constraint with no firmware change.

### 4.6 LSE pre-condition and clock-boundary split

The driver does **not** start LSE. System clock configuration code, executed before `main()`, is responsible for:

1. Enabling LSE in `RCC_BDCR` (`LSEON = 1`).
2. Polling `RCC_BDCR.LSERDY` until set.

The driver verifies `LSERDY` at the start of `rtc_init` and returns `RTC_ERR_LSE_NOT_READY` if absent. This surfaces a system-init ordering bug clearly rather than letting the driver hang on a non-running clock.

LSE is a shared resource (other peripherals may consume it). Its lifecycle belongs to system clock config, not to this driver.

### 4.7 No interaction with PCLK pinning

The RTC is clocked from LSE, not APB. Unresolved PCLK assumptions elsewhere in the project (see relevant clock-config TBDs in `project_status.md`) do not affect RTC timing.

### 4.8 Pins

N/A — the RTC peripheral has no GPIO pins in this driver's scope. The LSE oscillator pins (PC14 / PC15) are configured by system clock setup code prior to `main()`. The RTC alarm output pin (PC13) and the wakeup timer are not used.

### 4.9 NVIC

Not used. The driver registers no ISR. RTC alarm and wakeup-timer features are out of scope for this project.

---

## 5. Sequence integration

`RtcDriver` appears in one existing sequence diagram:

**SD-09 (time synchronisation):** the message labelled "write RTC" (REQ-TS-020) maps to `rtc_set_time`. The call path is `TimeService → TimeProvider → rtc_set_time(&dt)`. No new sequence diagram is required for the driver.

**Logger bootstrap path:** `Logger` calls `rtc_get_time` inside its timestamp-formatting routine. The call occurs pre-scheduler during Init and in any task context post-scheduler. This is documented in `components.md` and §2.2 above; it is not modelled in any sequence diagram (bootstrap exceptions are a textual concept, not a sequence message).

**No SD changes required.** SD-09's "write RTC" message is the driver's only sequence-diagram surface.

---

## 6. Error and fault behaviour

All public functions return `rtc_err_t`; callers must not ignore non-OK returns. The driver performs no retry — callers apply retry and logging policy.

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `RTC_ERR_INIT_TIMEOUT` | INITF flag did not assert within the §4.2 timeout during `rtc_init` (cold-start path) or `rtc_set_time` | Return error; WPR re-locked; peripheral state safe | Non-OK return | No retry on init — `LifecycleController` treats RTC init failure as Faulted | LifecycleController logs at ERROR via `ILogger` |
| `RTC_ERR_SYNC_TIMEOUT` | RSF flag did not assert after exiting init mode or before a read | Return error; WPR re-locked if applicable | Non-OK return | No retry — caller may re-attempt after a reset cycle | Caller logs at WARN via `ILogger` |
| `RTC_ERR_NULL_ARG` | Null pointer passed to `rtc_get_time`, `rtc_set_time`, or `rtc_read_backup` | Return error; no register access | Non-OK return | No retry — programming error | Caller logs at ERROR via `ILogger` |
| `RTC_ERR_BACKUP_BOUNDS` | Backup register index exceeds the board limit (`RTC_BACKUP_MAX_IDX`) | Return error; no register access | Non-OK return | No retry — programming error | Caller logs at ERROR via `ILogger` |
| `RTC_ERR_LSE_NOT_READY` | `RCC_BDCR.LSERDY` was 0 at `rtc_init` entry | Return error; no RTC clock enabled | Non-OK return | No retry from the driver — system clock config bug | LifecycleController logs at ERROR via `ILogger` and Faults the board |

---

## 7. Unit-test plan

Host-platform tests under the Ceedling/Unity harness (target-independent). The CMSIS `RTC` macro is redirected to a statically allocated `RTC_TypeDef` mock instance via `#define RTC (&mock_rtc)` in the test build, matching the pattern established in `DebugUartDriver`'s USART mocking. CMock is used for any helper-function stubs introduced during implementation.

Each test calls `rtc_reset_for_test()` from `setUp()` to clear file-scope state. Test files are per-board, near-identical, with board-specific bits (`RTC_BACKUP_MAX_IDX`) selected by the CMSIS device macro:

- `tests/field-device/drivers/rtc/test_rtc_driver.c`
- `tests/gateway/drivers/rtc/test_rtc_driver.c`

Both paths to be added to `tests/project.yml`.

### 7.1 Test cases

| ID | Test case | Expected result |
|---|---|---|
| TC-RTC-001 | `rtc_init` with LSERDY = 0 | Returns `RTC_ERR_LSE_NOT_READY`; RTC clock not enabled; `s_rtc.initialised` remains false |
| TC-RTC-002 | `rtc_init` cold start (LSERDY = 1, INITS = 0) | PWR clock enabled; DBP set; LSE selected and RTC clock enabled in BDCR; `PRER = (127<<16) \| 255`; TR/DR set to default-epoch packed values; WPR locked at exit; `s_rtc.backup_valid == false`; `s_rtc.initialised == true`; returns `RTC_ERR_OK` |
| TC-RTC-003 | `rtc_init` warm start (LSERDY = 1, INITS = 1) | PWR clock enabled; DBP set; LSE selected; PRER untouched; TR/DR unchanged; `s_rtc.backup_valid == true`; `s_rtc.initialised == true`; returns `RTC_ERR_OK` |
| TC-RTC-004 | `rtc_init` INITF timeout on cold start | Returns `RTC_ERR_INIT_TIMEOUT`; WPR locked; `s_rtc.initialised` remains false |
| TC-RTC-005 | `rtc_init` RSF timeout on cold start | Returns `RTC_ERR_SYNC_TIMEOUT`; WPR locked; `s_rtc.initialised` remains false |
| TC-RTC-006 | `rtc_init` called twice | Second call returns `RTC_ERR_OK` with no register writes (idempotent) |
| TC-RTC-007 | WPR write sequence ordering | Mock WPR records writes; verify the sequence is `0xCA, 0x53, …, 0xFF` (unlock-write-lock) |
| TC-RTC-008 | `rtc_get_time` NULL argument | Returns `RTC_ERR_NULL_ARG`; no register access |
| TC-RTC-009 | `rtc_get_time` happy path (TR = 0x162359, DR = 0x261231 BCD) | Returns `RTC_ERR_OK`; `dt == {2026, 12, 31, 16, 23, 59}` |
| TC-RTC-010 | `rtc_get_time` BCD boundary low (TR = 0x000000, DR = 0x000101) | Returns `RTC_ERR_OK`; `dt == {2000, 1, 1, 0, 0, 0}` |
| TC-RTC-011 | `rtc_get_time` BCD boundary high (TR = 0x235959, DR = 0x991231) | Returns `RTC_ERR_OK`; `dt == {2099, 12, 31, 23, 59, 59}` |
| TC-RTC-012 | `rtc_get_time` RSF clear on entry, then sets after one poll | Returns `RTC_ERR_OK`; correct datetime |
| TC-RTC-013 | `rtc_get_time` RSF never sets | Returns `RTC_ERR_SYNC_TIMEOUT`; `*dt` zeroed |
| TC-RTC-014 | `rtc_set_time` NULL argument | Returns `RTC_ERR_NULL_ARG`; no register access; WPR not unlocked |
| TC-RTC-015 | `rtc_set_time` happy path (`{2026, 5, 16, 10, 30, 0}`) | WPR unlocked (0xCA, 0x53); INIT set; INITF observed; `TR = 0x103000`, `DR = 0x260516` written; INIT cleared; RSF observed; WPR locked (0xFF); returns `RTC_ERR_OK` |
| TC-RTC-016 | `rtc_set_time` INITF timeout | Returns `RTC_ERR_INIT_TIMEOUT`; TR/DR not written; WPR re-locked |
| TC-RTC-017 | `rtc_set_time` RSF timeout | Returns `RTC_ERR_SYNC_TIMEOUT`; WPR re-locked |
| TC-RTC-018 | `rtc_set_time` does not modify `backup_valid` | After cold-init (`backup_valid == false`), call set_time; `rtc_is_backup_valid()` still returns false |
| TC-RTC-019 | `rtc_is_backup_valid` after cold-start init | Returns `false` |
| TC-RTC-020 | `rtc_is_backup_valid` after warm-start init | Returns `true` |
| TC-RTC-021 | `rtc_write_backup(0, 0xA5A55A5A)` + `rtc_read_backup(0, &v)` | Both return `RTC_ERR_OK`; `v == 0xA5A55A5A` |
| TC-RTC-022 | `rtc_write_backup(0, 0)` + `rtc_read_backup(0, &v)` | Both return `RTC_ERR_OK`; `v == 0` |
| TC-RTC-023 | `rtc_read_backup` with `out == NULL` | Returns `RTC_ERR_NULL_ARG`; no register access |
| TC-RTC-024 | `rtc_read_backup` at max valid index (F469: 19; L475: 31) | Returns `RTC_ERR_OK`; correct value returned |
| TC-RTC-025 | `rtc_read_backup` at first invalid index (F469: 20; L475: 32) | Returns `RTC_ERR_BACKUP_BOUNDS`; no register access |
| TC-RTC-026 | `rtc_read_backup` with `idx = 0xFF` (obvious overflow) | Returns `RTC_ERR_BACKUP_BOUNDS` |
| TC-RTC-027 | `rtc_write_backup` at max valid index | Returns `RTC_ERR_OK`; register written |
| TC-RTC-028 | `rtc_write_backup` at first invalid index | Returns `RTC_ERR_BACKUP_BOUNDS`; no register access |
| TC-RTC-029 | DBP bit set after `rtc_init` | Read PWR_CR (F469) or PWR_CR1 (L475); DBP bit is set |
| TC-RTC-030 | PWR clock enabled after `rtc_init` | Read RCC_APB1ENR (F469) or RCC_APB1ENR1 (L475); PWREN bit is set |
| TC-RTC-031 | RTC clock enabled and LSE selected after `rtc_init` | Read RCC_BDCR; `RTCEN == 1` and `RTCSEL == 0b01` |
| TC-RTC-032 | BCD round-trip (`bin_to_bcd(bcd_to_bin(x)) == x` for x in 0x00..0x99 valid BCD) | Helper-level test on the internal conversion functions; exposed for test via `#ifdef TEST` if needed |

**Coverage notes:** every documented error value in §6 is produced by ≥ 1 test. Every public API function has happy-path + at least one error-path test. Pre-condition `assert()` calls are **not** unit-tested (they are debug-build aids; tests run as release-equivalent unless an assertion-trapping harness is added later).

---

## 8. Open items

| ID | Item | Owner | Resolution path |
|---|---|---|---|
| RTCD-O1 | Calibration of the busy-wait timeout loop counter to actual CPU cycles. The §4.2 budgets are time-based (2 ms); the implementation needs a concrete loop iteration count once the CPU clock is pinned (see clock-config TBDs in `project_status.md`). | Luca | Resolve at implementation; replace placeholder constant once `clock-config.md` lands. |
| RTCD-O2 | Logger timestamps are unmarked-as-unsynchronised until NTP completes. Logger does not go through TimeProvider and therefore cannot query the sync-state flag — this is the accepted cost of the bootstrap exception. | Luca | Document explicitly in the future Logger companion's open-items section. |
| RTCD-O3 | Empirical verification on L475 that INITS clears reliably after a full power-cycle (VDD + VBAT both removed). RM0351 §38 describes the reset domains; this is a board-level check at integration. | Luca | Verify during Phase 5 integration; §4.5 already documents the expected behaviour on Discovery boards. |
| RTCD-O4 | BCD helper unit-test exposure — `bcd_to_bin` / `bin_to_bcd` are `static`. TC-RTC-032 needs either a `#ifdef TEST` declaration in the header or a dedicated `rtc_driver_internal.h` test seam. Decide at first implementation pass. | Luca | Pick the lighter option (forward-declaration inside the test TU) at implementation time. |

**Inherited open items with no surface area here:**

- Worst-case stack measurements — RtcDriver consumes negligible stack (≤ 64 bytes, no recursion, no large locals); inherited project-wide TBD remains at integration time.
- Hardware watchdog scope — IWDG is the project watchdog (REQ-NF-109); RTC wakeup timer is not used.

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| RTCD-D1 | API uses binary calendar values; BCD conversion is internal | Consumers have no reason to handle BCD; centralising the conversion prevents BCD arithmetic errors in upper layers |
| RTCD-D2 | No ISP split of `IRtc` | Interface is already minimal; Logger's read-only constraint is enforced by code review. See §2.2 |
| RTCD-D3 | Singleton module (no handle parameter) | Exactly one RTC peripheral per board; a handle adds no value and contradicts the convention used by prior drivers (`DebugUartDriver`, `GpioDriver`) |
| RTCD-D4 | No FreeRTOS primitives inside the driver | Project-wide LLD-D3 convention; caller serialises if needed (LLD-D4) |
| RTCD-D5 | Prescaler written only when backup domain was reset | Writing PRER while the calendar is running would corrupt timekeeping; the INITS check gates this safely |
| RTCD-D6 | Busy-wait polling for INITF and RSF, not RTOS block | Total wait ≤ 4 ms; importing FreeRTOS would violate the no-RTOS-in-drivers convention (LLD-D3) |
| RTCD-D7 | `rtc_get_time` waits for RSF before reading | Avoids returning stale shadow-register values after a write; pushing the wait responsibility to callers would leak hardware knowledge into Middleware (P1) |
| RTCD-D8 | `read_backup` / `write_backup` exposed on `IRtc` (LLD-D16) | TimeProvider's sync-persisted flag must survive warm resets without direct register access from Middleware. Routing through the vtable preserves testability and keeps register-map knowledge inside the driver (P1) |
| RTCD-D9 | Init-guard policy: `assert()` in debug, undefined in release; no `RTC_ERR_NOT_INITIALISED` enum | Logger's bootstrap call always follows `rtc_init`; a misordered boot is a programmer error caught in test, not a runtime fault to recover from. Matches `DebugUartDriver`'s ISR-path discipline |
| RTCD-D10 | Idempotent `rtc_init` (no-op + OK on second call) | Matches `DebugUartDriver`; simpler caller contract than an error code for double-init |
| RTCD-D11 | Default epoch = 2000-01-01 00:00:00 | The STM32 RTC stores the year as a two-digit offset from 2000; this is the hardware origin and the safest default value for an uninitialised calendar |
| RTCD-D12 | Driver verifies `LSERDY` and returns `RTC_ERR_LSE_NOT_READY`; LSE oscillator setup remains the system-clock-config responsibility | Surfaces a system-init ordering bug clearly without claiming ownership of a shared resource |
