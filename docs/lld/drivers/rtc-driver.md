# RtcDriver — LLD Companion

**Document:** `docs/lld/drivers/rtc-driver.md`
**Version:** 0.1 (draft — pending Luca review)
**Board scope:** Field Device (STM32F469) and Gateway (B-L475E-IOT01A)
**Layer:** Driver
**Status:** Draft
**Date:** May 2026

**HLD anchor:** RtcDriver in `components.md` (FD + GW driver layer)

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Keeps wall-clock time across reboots using the backup-domain RTC | `components.md` |
| PROVIDES (upward) | `IRtc` | `components.md` |
| USES (downward) | CMSIS | `components.md` |
| Root requirement | REQ-NF-213 | `SRS.md` §3.2 |
| Consumers | `Logger` (read, bootstrap exception), `TimeProvider` (read + write) | `components.md` preamble |
| Hardware | Backup-domain RTC; LSE 32.768 kHz crystal on both boards | UM1932 §4.3.2, UM2153 block diagram |

Both consumers are passive (task-breakdown.md §4.1, §5.1). All calls run in the calling task's context. No ISR, no DMA, no callback is required.

---

## 2. Public API

### 2.1 Dependency-conformance check (lld-methodology.md v1.1 §Step 2)

The public header (`rtc_driver.h`) must `#include` only what is in `USES (downward): CMSIS`. Permitted includes are `stm32f469xx.h` / `stm32l475xx.h` (via a board-agnostic `<device.h>` wrapper) and `stdint.h` / `stdbool.h` (C standard library — no dependency on any project component). No FreeRTOS headers. No `gpio_driver.h`. Confirmed clean.

### 2.2 P3 (Interface Segregation) consideration

`Logger` is read-only (`get_time` only); `TimeProvider` is read + write (`get_time`, `set_time`, `is_backup_valid`). P3 (architecture-principles.md) prescribes splitting when reader and writer consumers exist. However:

- The interface is only three functions — already minimal.
- The Logger dependency is a documented bootstrap exception, not a normal architectural pattern. Splitting the interface for a deliberate exception adds structural complexity without clarity benefit.
- In OOP-in-C, two separate function-pointer tables would be required.

**Decision:** do not split `IRtc`. Document that Logger uses only `get_time` and must not call `set_time`. This constraint is enforced by code review, not by the type system. The interface remains `IRtc` with all three operations, consistent with P10 naming (architecture-principles.md).

### 2.3 Data types

```c
/**
 * @brief Calendar date and time, all fields in binary (not BCD).
 *
 * The driver converts to/from the hardware BCD format internally.
 * Consumers never handle BCD.
 *
 * year:   2000..2099 (stored as full year, e.g. 2026)
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
    RTC_ERR_SYNC_TIMEOUT   = 2,  /**< RSF flag did not assert after exiting init mode. */
    RTC_ERR_NULL_ARG       = 3,  /**< Null pointer passed to an output parameter. */
    RTC_ERR_BACKUP_BOUNDS  = 4,  /**< Backup register index out of range for this board. */
} rtc_err_t;

/* Maximum backup register indices per board (LLD-D16). */
#define RTC_BACKUP_MAX_IDX_F469  19U   /* STM32F469: BKP0R..BKP19R (20 registers) */
#define RTC_BACKUP_MAX_IDX_L475  31U   /* STM32L475: BKPR[0..31]   (32 registers) */

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
 * Behaviour:
 *  - Checks whether the backup domain survived the last reset (RTC_ISR.INITS).
 *  - If the backup domain is valid, the calendar is left unchanged.
 *  - If the backup domain was reset (INITS = 0), the RTC is initialised
 *    with the default epoch (RTCD-O1 — see §8) and marked not synchronised.
 *  - Configures 24-hour format (RTC_CR.FMT = 0) if re-initialising.
 *  - Configures the prescaler for a 1 Hz calendar clock from a 32.768 kHz
 *    LSE source (PREDIV_A = 127, PREDIV_S = 255).
 *
 * @return RTC_ERR_OK on success; RTC_ERR_INIT_TIMEOUT if INITF does not
 *         assert within the timeout window.
 * @note Threading: task-context only, non-blocking. Must be called before the scheduler starts.
 */
rtc_err_t rtc_init(void);

/**
 * @brief Read the current wall-clock time from the RTC.
 *
 * Safe to call from any task context. Caller serialises concurrent calls
 * (driver is not internally synchronised — established convention,
 * lld-methodology.md v1.1).
 *
 * If called immediately after rtc_set_time(), the RSF flag must be clear;
 * the driver waits for RSF before reading to ensure the shadow registers
 * are consistent (see §4, hardware contract).
 *
 * @param[out] dt Pointer to caller-allocated rtc_datetime_t; populated on
 *               RTC_ERR_OK. Zeroed on error.
 * @return RTC_ERR_OK on success; RTC_ERR_SYNC_TIMEOUT if RSF does not
 *         assert within the timeout window.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
rtc_err_t rtc_get_time(rtc_datetime_t *dt);

/**
 * @brief Write a new wall-clock time to the RTC.
 *
 * Called by TimeProvider after a successful NTP synchronisation (REQ-TS-020).
 * Must NOT be called by Logger (bootstrap exception — Logger is read-only).
 *
 * Sequence: unlock WPR → enter init mode (wait INITF) → write TR/DR →
 * exit init mode → lock WPR → wait RSF.
 *
 * @param dt  Pointer to the new date/time (must not be NULL).
 * @return RTC_ERR_OK on success; RTC_ERR_INIT_TIMEOUT or
 *         RTC_ERR_SYNC_TIMEOUT on hardware timeout.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
rtc_err_t rtc_set_time(const rtc_datetime_t *dt);

/**
 * @brief Return whether the backup domain survived the last reset.
 *
 * Returns true if RTC_ISR.INITS was set at init time, meaning the calendar
 * has been previously initialised and the backup supply preserved it.
 * Returns false if the backup domain was reset, meaning the calendar was
 * reinitialised to the default epoch — timestamps are not meaningful until
 * a successful rtc_set_time() call.
 *
 * TimeProvider uses this flag to decide the initial sync state reported
 * through IHealthReport and to gate the "synchronised" flag exposed via
 * ITimeProvider (REQ-NF-212).
 *
 * @return true if backup domain was valid at init; false otherwise.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
bool rtc_is_backup_valid(void);

/**
 * @brief Read one RTC backup register.
 *
 * RTC backup registers survive warm resets (watchdog, soft reset, NRST pin)
 * as long as VBAT or VDD supplies the backup domain. They do not survive a
 * full power-off on boards where VBAT is tied to VDD (see §4.5).
 *
 * DBP (PWR_CR.DBP on F469, PWR_CR1.DBP on L475) must be set before the
 * backup domain can be accessed. rtc_init() sets DBP and leaves it set for
 * the application's lifetime; callers do not need to manage DBP.
 *
 * @param  idx      Backup register index. Valid range: 0..19 on F469,
 *                  0..31 on L475. Returns RTC_ERR_BACKUP_BOUNDS if exceeded.
 * @param[out] out  Set to the 32-bit register value on RTC_ERR_OK.
 * @return RTC_ERR_OK, RTC_ERR_NULL_ARG, or RTC_ERR_BACKUP_BOUNDS.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
rtc_err_t rtc_read_backup(uint8_t idx, uint32_t *out);

/**
 * @brief Write one RTC backup register.
 *
 * Same DBP and VBAT constraints as rtc_read_backup(). No WPR unlock needed —
 * backup registers are not protected by WPR (only TR, DR, and CR are).
 *
 * @param  idx    Backup register index. Valid range: 0..19 on F469, 0..31 on L475.
 * @param  value  32-bit value to write.
 * @return RTC_ERR_OK or RTC_ERR_BACKUP_BOUNDS.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
rtc_err_t rtc_write_backup(uint8_t idx, uint32_t value);

/* ------------------------------------------------------------------ */
/* Singleton vtable interface (IRtc — LLD-D10)                         */
/* ------------------------------------------------------------------ */

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

### 3.0 Private struct

```c
typedef struct {
    bool initialised;   /**< Set by rtc_init(); guards all entry points. */
    bool backup_valid;  /**< Captured at rtc_init(); immutable thereafter. */
} rtc_driver_t;

static rtc_driver_t s_rtc;
```


### 3.1 Module-level state (rtc_driver.c — not exposed in header)

There is exactly one RTC peripheral per board. The driver is a singleton; no handle is passed between caller and driver.

```c
static bool s_backup_valid = false; /* Captured once at rtc_init(); immutable thereafter. */
```

The peripheral is accessed directly via the CMSIS `RTC` macro (`RTC_TypeDef *` pointing to the fixed peripheral base address). No pointer indirection is needed at runtime.

### 3.2 BCD conversion helpers (internal linkage)

The STM32 RTC stores hours, minutes, seconds, and date in BCD. Consumers receive and supply binary values. Two `static` helper pairs are defined in `rtc_driver.c`:

```c
static uint8_t bcd_to_bin(uint8_t bcd);   /* e.g. 0x23 → 23 */
static uint8_t bin_to_bcd(uint8_t bin);   /* e.g. 23 → 0x23 */
```

Year is handled separately: the RTC stores the year offset from 2000 as a two-digit BCD value (00..99). `rtc_get_time` adds 2000; `rtc_set_time` subtracts 2000 before converting.

### 3.3 Write protection unlock/lock

The STM32 RTC WPR register protects TR, DR, and CR from accidental writes. The sequence is:

```c
/* Unlock */
handle->regs->WPR = 0xCA;
handle->regs->WPR = 0x53;

/* ... write operations ... */

/* Lock — any invalid key re-enables protection */
handle->regs->WPR = 0xFF;
```

This sequence wraps every call to `rtc_set_time` and the init-mode entry inside `rtc_init`.

### rtc_init

```
1. Set RTC_ISR.INIT = 1.
2. Poll RTC_ISR.INITF until set (hardware takes up to 2 RTCCLK cycles).
3. Apply the timeout defined in §4. If timeout expires → RTC_ERR_INIT_TIMEOUT.
4. Write TR and DR registers.
5. Clear RTC_ISR.INIT = 0.
6. Poll RTC_ISR.RSF until set (calendar shadow registers re-synchronised).
7. If RSF timeout expires → RTC_ERR_SYNC_TIMEOUT.
```

Step 6 is mandatory before `rtc_get_time` returns valid data after a write. `rtc_get_time` also polls RSF before reading if RSF is not already set (e.g., immediately after power-on before the first calendar update tick).

### 3.5 No ISR, no DMA, no callbacks

The driver has no interrupt handler and registers no callbacks. This is consistent with the passive consumer model confirmed in task-breakdown.md. The wakeup timer and alarm features of the STM32 RTC peripheral are not used in this project scope.

---

### 3.6 Principles applied

- **P1 (Strict directional layering).** Depends only on CMSIS RTC peripheral headers; no RTOS, no middleware.
- **P2 (Dependency Inversion).** Exposes `irtc_t` vtable (LLD-D10) with six functions including backup-register accessors (LLD-D16); TimeProvider and Logger depend on `IRtc`.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static `RtcState`; no heap; no RTOS objects.
- **P6 (Responsibility traces to requirements).** Time read/write traces to REQ-TS-040; backup-register API traces to LLD-D16 (sync-state persistence).
- **P8 (Total error propagation, no silent failures).** `rtc_err_t` on all operations; init-mode timeout returns `RTC_ERR_TIMEOUT`; backup index bounds checked with `RTC_ERR_BACKUP_BOUNDS`.
- **P9 (BARR-C coding standard).** BCD helpers use explicit bit-masking; `uint32_t` for Unix epoch; `uint8_t` for backup-register index.
- **P10 (Naming conventions).** Prefix `rtc_`; interface `IRtc` -> `irtc_t`; errors `RTC_ERR_*`; constants `RTC_BACKUP_MAX_IDX_F469` / `RTC_BACKUP_MAX_IDX_L475`.

### rtc_get_time

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### rtc_set_time

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### rtc_is_backup_valid

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### rtc_read_backup

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).

### rtc_write_backup

Pre-conditions: the component has been initialised (where an init function exists). Validates inputs and returns the appropriate error code on failure. Performs the operation described in §2; post-conditions as documented in the §2 Doxygen block. No synchronisation primitive is held across the call — the operation is bounded and deterministic (see §3 Synchronisation).


## 4. Hardware contract

### 4.1 Clock source — both boards

| Board | Crystal | LSE frequency | PREDIV_A | PREDIV_S | Calendar tick |
|---|---|---|---|---|---|
| STM32F469 (FD) | X3 (UM1932 §4.3.2) | 32.768 kHz | 127 | 255 | 1 Hz |
| STM32L475 (GW) | X2 (UM2153 block diagram) | 32.768 kHz | 127 | 255 | 1 Hz |

The prescaler formula is: f_calendar = f_LSE / (PREDIV_A + 1) / (PREDIV_S + 1) = 32768 / 128 / 256 = 1 Hz.

These values must only be written during init mode if the backup domain was reset. If the backup domain survived (INITS = 1), the prescaler is already set correctly and must not be overwritten.

### 4.2 Timeout budgets

| Wait condition | Maximum duration (from RM) | Driver timeout | Notes |
|---|---|---|---|
| INITF after INIT=1 | 2 RTCCLK cycles ≈ 61 µs | 2 ms (generous margin) | RTCCLK = LSE = 32.768 kHz |
| RSF after INIT=0 | 2 RTCCLK + 2 APB cycles | 2 ms | APB clock varies; 2 ms covers worst case at minimum APB |

Timeouts are polled (busy-wait), not RTOS-blocked. The total worst-case blocking time for `rtc_set_time` is ≤ 4 ms. This is acceptable; the only caller is `TimeProvider`, which runs in `TimeServiceTask` (GW, hourly) or is called from the NTP write path, neither of which has a hard latency requirement shorter than 4 ms.

For `rtc_get_time` called by `Logger`, RSF is normally already set (calendar updates every 1 Hz); the RSF wait is typically zero cycles in steady state.

### 4.3 Backup domain reset detection

`RTC_ISR.INITS` (bit 4) is set by hardware when the calendar has been initialised at least once since the last backup domain reset. Reading INITS at the start of `rtc_init` is the canonical way to detect whether a cold-start epoch load is required. This is consistent on both F469 (RM0386) and L475 (RM0351).

**Flag captured once at init:** `s_backup_valid = (RTC->ISR & RTC_ISR_INITS) != 0`. The flag is then immutable for the lifetime of the module.

### 4.4 Cross-target register compatibility

Both `stm32f469xx.h` and `stm32l475xx.h` define `RTC_TypeDef` with the same relevant registers (`TR`, `DR`, `CR`, `ISR`, `PRER`, `WPR`, `SSR`). The register field bit positions are identical for the fields used by this driver.

**Backup register count differs by board (LLD-D16):**

| Board | Backup registers | Valid `idx` range | CMSIS access |
|-------|-----------------|-------------------|--------------|
| STM32F469 (FD) | 20 (BKP0R..BKP19R) | 0..19 | `RTC->BKPR[idx]` |
| STM32L475 (GW) | 32 (BKP0R..BKP31R) | 0..31 | `RTC->BKPR[idx]` |

`rtc_read_backup` / `rtc_write_backup` check `idx` against the board-specific limit
(`RTC_BACKUP_MAX_IDX_F469` / `RTC_BACKUP_MAX_IDX_L475`) selected at compile time via
the board-specific CMSIS header.

**DBP unlock (backup domain protection):** `rtc_init()` sets `PWR_CR.DBP` (F469)
/ `PWR_CR1.DBP` (L475) and leaves it set. Backup registers are accessible without
per-call DBP management. This is the established convention on STM32 — the backup
domain is unlocked once at startup.

**WPR does not protect backup registers.** WPR only covers `TR`, `DR`, and `CR`.
Backup register writes are always permitted once DBP is set.

**Action required (Luca at implementation):** Verify the WPR key sequence and INITF/RSF timeout behaviour against RM0386 §27 (F469) and RM0351 §38 (L475) before writing code. Both RMs are required reading; the CMSIS headers alone are not sufficient for the init state machine.

### 4.5 Backup register persistence scope (LLD-D16)

RTC backup registers survive warm resets (watchdog, soft reset, NRST pin) on both
boards. They do **not** survive a full power-off when VBAT is tied to VDD.

On the **STM32F469-DISCO** (FD) and **B-L475E-IOT01A** (GW), VBAT is connected to
VDD through a filtering network — there is no separate coin cell. This means:

- Backup register contents **are lost on power removal**.
- The sync-persisted flag (`TIME_PROVIDER_BKUP_REG = 0`) will not survive a
  power-off → the system will correctly restart as UNSYNCHRONISED after a
  power cycle.
- The flag survives the resets that matter for the SRS requirement (watchdog
  reset, soft reset, NVIC_SystemReset) — REQ-TS-040 / REQ-NF-212.

If a hardware revision adds a dedicated VBAT supply, this constraint is lifted
automatically — no firmware change required.

### 4.7 DUART-O2 interaction

Not applicable. The RTC is clocked from LSE, not APB. The unresolved PCLK values (DUART-O2) have no effect on RTC timing.

---

### Pins

N/A — the RTC peripheral has no GPIO pins in this project. The LSE oscillator pins (PC14/PC15) are configured by the system clock setup code prior to `main()` and are not within the scope of this driver. The RTC alarm output pin (PC13) is not used.

### NVIC

| Board | ISR | Priority | Purpose |
|---|---|---|---|
| Both | `RTC_Alarm_IRQn` | 6 | RTC alarm interrupt (if the alarm feature is activated in Phase 4). |

The alarm ISR is currently not implemented (Phase 4 concern). Priority 6 is documented as the target allocation for all driver ISRs in lld.md §6.3.


## 5. Sequence integration

`RtcDriver` appears in one existing sequence diagram:

**SD-09 (time synchronisation):** message "write RTC" (REQ-TS-020) maps to `rtc_set_time`. The call path is `TimeService` → `TimeProvider` → `rtc_set_time(&dt)`. No new sequence diagram is needed for the driver itself; the existing SD-09 in `sequence-diagrams.md` is sufficient.

**Logger bootstrap path:** `Logger` calls `rtc_get_time` inside its timestamp-formatting routine. This call occurs pre-scheduler (during Init) and in any task context post-scheduler. It is not modelled in any sequence diagram — the bootstrap exception is documented in `components.md` and in §2.2 above.

**No SD changes required.** SD-09 "write RTC" message is the driver's only sequence-diagram surface. No escalation to `sequence-diagrams.md` is needed.

---

## 6. Error and fault behaviour

All public functions return `rtc_err_t`; callers must not ignore non-OK returns.
No retry is performed by the driver — callers apply retry and logging policy.

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `RTC_ERR_INIT_TIMEOUT` | INITF flag did not assert within timeout during `rtc_init()` | Return error; peripheral not configured | Non-OK return | No retry — `LifecycleController` treats RTC init failure as Faulted; boot cannot proceed without RTC | LifecycleController logs at ERROR via ILogger |
| `RTC_ERR_SYNC_TIMEOUT` | RSF (Register Sync Flag) did not assert after exiting init mode | Return error | Non-OK return | No retry — caller may re-attempt after a reset cycle | Caller logs at WARN via ILogger |
| `RTC_ERR_NULL_ARG` | Null pointer passed to an output parameter (`rtc_get_time()` / `rtc_get_backup_reg()`) | Return error; no register read performed | Non-OK return | No retry — programming error | Caller logs at ERROR via ILogger |
| `RTC_ERR_BACKUP_BOUNDS` | Backup register index out of range for the active board (F469 has 32, L4 has 32) | Return error; no register access performed | Non-OK return | No retry — programming error; caller must check `RTC_BACKUP_REG_COUNT` for the target board | Caller logs at ERROR via ILogger |


## 7. Unit-test plan

Host-platform tests (Unity framework, target-independent). The CMSIS `RTC` macro is redirected to a statically allocated `RTC_TypeDef` mock instance via a preprocessor `#define RTC (&mock_rtc)` in the test build. No linker tricks required.

| ID | Test case | Expected result |
|---|---|---|
| T-RTC-01 | `rtc_init` when INITS = 0: verify prescaler written, epoch set to default, `backup_valid = false` | PREDIV_A = 127, PREDIV_S = 255 written; TR/DR set to default epoch; `rtc_is_backup_valid` returns `false` |
| T-RTC-02 | `rtc_init` when INITS = 1: verify no prescaler write, no epoch overwrite, `backup_valid = true` | PRER register untouched; TR/DR unchanged; `rtc_is_backup_valid` returns `true` |
| T-RTC-03 | `rtc_get_time` happy path: mock TR = 0x162359, DR = 0x261231 (BCD) | Returns `{2026, 12, 31, 16, 23, 59}` |
| T-RTC-04 | `rtc_get_time` BCD boundary: TR = 0x000000, DR = 0x000101 | Returns `{2000, 1, 1, 0, 0, 0}` |
| T-RTC-05 | `rtc_set_time` happy path: call with `{2026, 5, 16, 10, 30, 00}` | WPR unlocked (0xCA, 0x53); INIT bit set; TR = 0x103000, DR = 0x260516 written; INIT cleared; WPR locked (0xFF) |
| T-RTC-06 | `rtc_set_time` INIT timeout: force INITF never set | Returns `RTC_ERR_INIT_TIMEOUT`; TR/DR not written |
| T-RTC-07 | `rtc_set_time` RSF timeout: INITF sets normally, RSF never sets | Returns `RTC_ERR_SYNC_TIMEOUT` |
| T-RTC-08 | `rtc_get_time` RSF not set on entry: mock RSF initially clear, then sets | Blocks until RSF set; returns correct datetime |
| T-RTC-09 | `rtc_get_time` RSF timeout: RSF never sets | Returns `RTC_ERR_SYNC_TIMEOUT`; `dt` zeroed |
| T-RTC-10 | `rtc_is_backup_valid` after init with INITS = 1 | Returns `true` |
| T-RTC-11 | `rtc_is_backup_valid` after init with INITS = 0 | Returns `false` |
| T-RTC-12 | `rtc_write_backup(0, 0xA5A55A5A)` then `rtc_read_backup(0, &v)` | Returns `RTC_ERR_OK`; `v == 0xA5A55A5A` |
| T-RTC-13 | `rtc_write_backup(0, 0)` then `rtc_read_backup(0, &v)` | Returns `RTC_ERR_OK`; `v == 0` |
| T-RTC-14 | `rtc_read_backup` with `out == NULL` | Returns `RTC_ERR_NULL_ARG` |
| T-RTC-15 | `rtc_write_backup` / `rtc_read_backup` with `idx > RTC_BACKUP_MAX_IDX_*` | Returns `RTC_ERR_BACKUP_BOUNDS` |
| T-RTC-16 | `rtc_write_backup(idx_max, value)` — maximum valid index on each board target | Returns `RTC_ERR_OK` (both F469 and L475 build targets) |

Test file: `tests/drivers/test_rtc_driver.c`.

---

## 8. Open items

| ID | Item | Owner | Resolution path |
|---|---|---|---|
| RTCD-O1 | Default epoch value when backup domain is reset. Candidate: `2000-01-01 00:00:00` (RTC hardware year-offset origin). Confirm before implementation. | Luca | Decide at implementation; define as a named constant in `rtc_driver.c` |
| RTCD-O2 | RSF wait timeout value under minimum APB clock. 2 ms assumed; verify against system clock configuration once `clock-config.md` companion is produced. DUART-O2 (unresolved PCLK values) is the dependency. | Luca | Resolve when `clock-config.md` lands |
| RTCD-O3 | Logger timestamps are unsynchronised and unmarked until NTP sync. Logger does not pass through TimeProvider and therefore cannot query the sync-state flag. This is the accepted cost of the bootstrap exception. Document explicitly in the Logger companion when produced. | Claude (Logger companion) | Noted here; actioned in Logger LLD |
| RTCD-O4 | Verify INITS flag behaviour on L475 after a full power-cycle (VDD and VBAT both removed). RM0351 §38.3 covers reset conditions; confirm INITS is reliably cleared when backup domain loses power. | Luca | Documented in §4.5: on Discovery boards VBAT=VDD so backup domain clears on power-off; verify empirically at integration. |

**Inherited open items with no surface area in this companion:**
- O1 (WiFi SPI driver naming): not applicable.
- O2 (worst-case stack measurements): not applicable.
- O3 (hardware watchdog): the RTC wakeup timer is not used. IWDG is the watchdog mechanism (REQ-NF-109). No interaction with this driver.

---

## 9. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| RTCD-D1 | API uses binary calendar values; BCD conversion is internal | Consumers (Logger, TimeProvider) have no reason to handle BCD; conversion is a driver responsibility. Prevents BCD arithmetic errors in upper layers. |
| RTCD-D2 | No ISP split of `IRtc` | Interface is three functions — already minimal. Logger's read-only constraint is enforced by convention, not type system. Adding two function-pointer tables for a documented exception case adds complexity without clarity. |
| RTCD-D3 | Singleton module (no handle parameter) | There is exactly one RTC peripheral per board. A handle adds no value and contradicts the pattern established in prior driver companions. Module-level static state is simpler and equally correct. |
| RTCD-D4 | No FreeRTOS primitives in driver | Consistent with all prior driver companions. Caller serialises as needed. |
| RTCD-D5 | Prescaler only written on backup domain reset | Writing the prescaler when the backup domain is valid corrupts the running calendar. The INITS check gates this safely. |
| RTCD-D6 | Busy-wait polling for INITF and RSF, not RTOS block | The total wait is ≤ 4 ms; both consumers can tolerate this. Adding an RTOS-aware timeout would import a FreeRTOS dependency into the driver, violating the established no-RTOS-in-drivers convention. |
| RTCD-D7 | `rtc_get_time` waits for RSF | Avoids returning stale shadow-register values immediately after a set. The alternative (document that callers must wait) would push hardware knowledge into the middleware layer, violating P1. |
| RTCD-D8 | `read_backup` / `write_backup` added to IRtc vtable (LLD-D16) | TimeProvider's sync-persisted flag must survive warm resets without direct register access from Middleware. Routing through the vtable preserves testability (mock substitution) and keeps the register-map knowledge inside RtcDriver, consistent with P1. |
