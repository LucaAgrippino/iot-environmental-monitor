# LLD Companion — TimeProvider

**Layer:** Middleware  
**Boards:** Field Device (FD) · Gateway (GW)  
**Provides:** `ITimeProvider`  
**Consumes:** `IRtc` (RtcDriver), `IHealthReport`, `ILogger`  
**SRS traces:** REQ-TS-040, REQ-NF-210, REQ-NF-211, REQ-NF-212  
**HLD ref:** `components.md` §Middleware — TimeProvider; `hld.md` §5.2, §5.5  
**Version:** 1.0  
**Date:** May 2026  
**Status:** Released

**HLD anchor:** TimeProvider in `components.md` (FD + GW middleware layer)

---

## 1. Sources

TimeProvider wraps RtcDriver and exposes a single timestamping interface to
all application and middleware consumers. Its two jobs:

1. **Read:** return the current time as a Unix epoch value plus a
   synchronisation-state flag (REQ-TS-040).
2. **Write:** accept a time update from its per-board sync source, write it to
   the RTC, and push a sync-state transition event through `IHealthReport`.

The sync source differs by board:

| Board | Sync source | Caller of `time_provider_set_time()` |
|-------|-------------|--------------------------------------|
| GW    | NTP (via NtpClient) | `TimeService` (Application) |
| FD    | Modbus holding register write from GW | `ModbusRegisterMap` (Application) |

TimeProvider is a passive singleton — it has no thread of its own. Every
operation executes in the calling task's context, protected by an internal
mutex.

---

## 2. Data types

```c
/* time_provider.h */

typedef enum {
    TIME_PROVIDER_ERR_OK          = 0,
    TIME_PROVIDER_ERR_NOT_INIT    = 1,
    TIME_PROVIDER_ERR_RTC_FAIL    = 2,
    TIME_PROVIDER_ERR_NULL_ARG    = 3,
} time_provider_err_t;

typedef enum {
    TIME_SYNC_UNSYNCHRONISED = 0,
    TIME_SYNC_SYNCHRONISED   = 1,
} time_sync_state_t;

/**
 * @brief Timestamped value returned by time_provider_get().
 *
 * When sync_state == TIME_SYNC_SYNCHRONISED, epoch is a Unix epoch (seconds
 * since 1970-01-01 UTC). When TIME_SYNC_UNSYNCHRONISED, epoch is seconds
 * elapsed since last boot (uptime). Consumers MUST check sync_state before
 * interpreting epoch. (REQ-TS-040)
 */
typedef struct {
    uint32_t          epoch;
    time_sync_state_t sync_state;
} time_provider_ts_t;
```

The two-field struct is the single currency across both boards. Every reading
that carries a timestamp embeds `time_provider_ts_t` directly — no separate
sync flag is threaded through layers.

---

## 3. Public API — `ITimeProvider`

```c
/**
 * @brief  Initialise TimeProvider.
 *
 * Reads RTC. Checks the sync-persisted flag in RTC backup register
 * TIME_PROVIDER_BKUP_REG (see §5). Sets initial sync_state accordingly.
 * Must be called after rtc_init() and logger_init().
 *
 * @param  health  IHealthReport handle for sync-state event push.
 * @return TIME_PROVIDER_ERR_OK or TIME_PROVIDER_ERR_RTC_FAIL.
 * @note Threading: task-context only, non-blocking. Must be called before the scheduler starts.
 */
time_provider_err_t time_provider_init(const ihealth_report_t *health);

/**
 * @brief  Get the current timestamp.
 *
 * Thread-safe. May be called from any task context. Never from an ISR.
 *
 * @param[out] ts_out  Filled with epoch + sync_state.
 * @return TIME_PROVIDER_ERR_OK or TIME_PROVIDER_ERR_NULL_ARG.
 */
time_provider_err_t time_provider_get(time_provider_ts_t *ts_out);

/**
 * @brief  Set the current time and mark the provider as synchronised.
 *
 * Writes the new epoch to RtcDriver and sets the sync-persisted flag in the
 * RTC backup register. If sync_state was UNSYNCHRONISED before this call,
 * pushes a HEALTH_EVENT_TIME_SYNC_ACQUIRED event through IHealthReport.
 *
 * Sanity-check: if |new_epoch - rtc_current| > TIME_PROVIDER_SANITY_DELTA_S,
 * the update is rejected and TIME_PROVIDER_ERR_RTC_FAIL is returned. The
 * threshold is defined in time_provider_config.h (TBD — see TP-O3).
 * The sanity check is skipped when sync_state == TIME_SYNC_UNSYNCHRONISED
 * (first sync after cold boot must succeed regardless of RTC default date).
 *
 * Thread-safe.
 *
 * @param  new_epoch  Unix epoch seconds to set.
 * @return TIME_PROVIDER_ERR_OK or TIME_PROVIDER_ERR_RTC_FAIL.
 */
time_provider_err_t time_provider_set_time(uint32_t new_epoch);

/**
 * @brief  Mark the provider as unsynchronised.
 *
 * Called by TimeService (GW) when NTP fails after a prior successful sync.
 * Clears the backup register flag. Pushes HEALTH_EVENT_TIME_SYNC_LOST if
 * sync_state was SYNCHRONISED.
 *
 * Thread-safe.
 *
 * @return TIME_PROVIDER_ERR_OK.
 */
time_provider_err_t time_provider_mark_unsynchronised(void);

/**
 * @brief  Return current sync state without a full timestamp read.
 *
 * Lightweight poll — no RTC access, returns cached state only.
 * @return Current state value.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
time_sync_state_t time_provider_get_sync_state(void);

/* ------------------------------------------------------------------ */
/* Singleton vtable interface (ITimeProvider — LLD-D10)                */
/* ------------------------------------------------------------------ */

typedef struct {
    time_provider_err_t (*init)(const ihealth_report_t *health);
    time_provider_err_t (*get)(time_provider_ts_t *ts_out);
    time_provider_err_t (*set_time)(uint32_t new_epoch);
    time_provider_err_t (*mark_unsynchronised)(void);
    time_sync_state_t   (*get_sync_state)(void);
} itime_provider_t;

/** Singleton pointer to the TimeProvider vtable (FD + GW). */
extern const itime_provider_t * const time_provider;
```

### Internal design

```c
/* time_provider.c — static module state */
typedef struct {
    bool                  initialised;
    time_sync_state_t     sync_state;
    const ihealth_report_t *health;
    StaticSemaphore_t     mutex_buf;
    SemaphoreHandle_t     mutex;   /* FreeRTOS mutex, priority inheritance enabled */
} time_provider_state_t;

static time_provider_state_t s_tp;
```

All public functions acquire `s_tp.mutex` before accessing `sync_state` or
calling `rtc_*`. The mutex is created in `time_provider_init()` — never
dynamically after init (P8: no post-init dynamic allocation).

`time_provider_get_sync_state()` reads `s_tp.sync_state` under the mutex and
returns a copy. No RTC access.

#### Synchronisation

This component uses an internal mutex to serialise concurrent callers. The
mutex is created during `_init()` and held only for the duration of each
guarded operation (bounded, short hold time). All public functions are
task-safe but not ISR-safe. The health event push (`push_event`) is performed
**after** releasing the mutex to avoid priority inversion between TimeProvider
and HealthMonitor mutexes.

#### Function-level pre- and post-conditions

Each public function (except `init`) verifies `s_tp.initialised` and returns
`TIME_PROVIDER_ERR_NOT_INIT` on failure before acquiring the mutex.
`time_provider_get()` additionally checks `ts_out != NULL` and returns
`TIME_PROVIDER_ERR_NULL_ARG` before acquiring the mutex.

All error paths release the mutex before returning. See §7 (unit-test plan)
for the exhaustive error-path coverage requirements.

#### Principles applied

- **P1 (Strict directional layering).** Depends on IRtc (driver layer); IHealthReport and ILogger are cross-cutting exceptions (P4).
- **P2 (Dependency Inversion).** Exposes `itime_provider_t` vtable singleton (LLD-D10); all consumers depend on `ITimeProvider`. Backup-register access is through `irtc_t` vtable calls (LLD-D16), not direct register access.
- **P4 (Cross-cutting concern exception).** Logger and HealthMonitor (IHealthReport) referenced concretely per the cross-cutting exception; documented in §1 Sources.
- **P5 (Bounded resources, no dynamic allocation post-init).** Single static `time_provider_state_t` struct; FreeRTOS mutex created once in `time_provider_init()` and never freed.
- **P6 (Responsibility traces to requirements).** `time_provider_get()` and `time_provider_set_time()` trace to REQ-TS-040 / REQ-NF-210-212 timestamping requirements.
- **P7 (Pull-based downstream consumption).** All consumers call `time_provider_get()` on their own task schedule; TimeProvider does not push time values to any consumer.
- **P8 (Total error propagation, no silent failures).** All public functions return `time_provider_err_t`; RTC driver errors propagated; sanity-delta check returns a distinct error code.
- **P9 (BARR-C coding standard).** Unix epoch `uint32_t`; sync-flag magic `uint32_t`; mutex handle opaque pointer; no floating-point.
- **P10 (Naming conventions).** Prefix `time_provider_`; interface `ITimeProvider` -> `itime_provider_t`; errors `TIME_PROVIDER_ERR_*`.

### Error and fault behaviour

All public functions return `time_provider_err_t`; callers must not ignore
non-OK returns. No retry is performed by TimeProvider — TimeService decides
the retry and logging policy.

| Error value | Cause | Local behaviour | Caller-visible result | Retry | Observability |
|---|---|---|---|---|---|
| `TIME_PROVIDER_ERR_NOT_INIT` | Function called before `time_provider_init()` | Return error; no RTC access | Non-OK return | No retry — programming error; boot sequence must initialise TimeProvider after RtcDriver | Caller logs at ERROR via ILogger |
| `TIME_PROVIDER_ERR_RTC_FAIL` | `rtc_get_time()` or `rtc_set_time()` returned a non-OK code | Return error; time value not updated | Non-OK return | No retry by TimeProvider — TimeService may retry after a brief delay | Logged at WARN via ILogger |
| `TIME_PROVIDER_ERR_NULL_ARG` | Null pointer passed to an output parameter | Return error; no RTC access | Non-OK return | No retry — programming error | Caller logs at ERROR via ILogger |

---

## 4. Uptime fallback

When `sync_state == TIME_SYNC_UNSYNCHRONISED`, `time_provider_get()` returns:

```c
ts_out->epoch = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
```

This is seconds elapsed since the last reset. It is a monotonically increasing
counter with no calendar meaning — which is exactly what REQ-TS-040 requires.
Consumers that need to distinguish uptime from wall-clock time check
`sync_state` — the flag is the contract, not a documentation note.

The FD has an additional requirement (REQ-NF-212): if the gateway has not yet
written the time register, the FD uses uptime-based timestamps. This is handled
transparently by TimeProvider; no FD-specific branching is needed in any other
component.

---

## 5. Sync-state persistence across resets

**Problem:** after a watchdog or normal reset, the RTC keeps running (it is
battery-backed), but the in-RAM `sync_state` variable is lost. On reconnect it
would take until the next NTP cycle (GW) or the next ModbusSlave write (FD)
before TimeProvider is marked synchronised again, causing unnecessary
"unsynchronised" flags on telemetry.

**Solution:** use one RTC backup register as a persistence flag.

```c
/* LLD-D16: BKP0R (index 0) allocated for sync-flag persistence.
 * See lld.md §6 backup-register allocation table.               */
#define TIME_PROVIDER_BKUP_REG   0U
#define TIME_PROVIDER_SYNC_MAGIC 0xA5A55A5AUL
```

On `time_provider_init()`:

```c
uint32_t val = 0;
rtc_driver->read_backup(TIME_PROVIDER_BKUP_REG, &val);
s_tp.sync_state = (val == TIME_PROVIDER_SYNC_MAGIC)
                  ? TIME_SYNC_SYNCHRONISED
                  : TIME_SYNC_UNSYNCHRONISED;
```

On `time_provider_set_time()`: write the magic to backup register 0:

```c
rtc_driver->write_backup(TIME_PROVIDER_BKUP_REG, TIME_PROVIDER_SYNC_MAGIC);
```

On `time_provider_mark_unsynchronised()`: clear backup register 0:

```c
rtc_driver->write_backup(TIME_PROVIDER_BKUP_REG, 0x00000000UL);
```

**Backup register allocation:** index 0 (BKP0R on STM32F469 / BKP_DR0 on
STM32L475) is reserved for this flag. See `lld.md` §6 backup-register
allocation table; TP-O2 closed by LLD-D16.

**Persistence scope:** backup register 0 survives warm resets (watchdog,
soft reset, NVIC_SystemReset). It does **not** survive a full power-off
on the STM32F469-DISCO (FD) and B-L475E-IOT01A (GW), where VBAT is tied
to VDD with no coin cell — the backup domain resets on power removal.
Consequently:

- After a power cycle, TimeProvider will always restart as UNSYNCHRONISED,
  even if it was synchronised before power was removed. This is the correct
  behaviour: the RTC calendar has also reset (INITS = 0), so the timestamps
  are not valid wall-clock time anyway.
- After a warm reset (watchdog, soft restart, firmware update reboot), the
  sync flag is preserved. TimeProvider re-enters SYNCHRONISED state at init
  time without waiting for the next NTP cycle (GW) or the next Modbus time
  write (FD). This avoids a transient UNSYNCHRONISED window on the most
  common reset path.

This scope matches the SRS requirement: REQ-TS-040 / REQ-NF-212 do not
specify persistence across power removal; they specify that the sync state
is tracked and reported correctly at runtime. See also `rtc-driver.md` §4.5.

---

## 6. IHealthReport events

| Event constant                    | Trigger                                            |
|-----------------------------------|----------------------------------------------------|
| `HEALTH_EVENT_TIME_SYNC_ACQUIRED` | `time_provider_set_time()` called while state was UNSYNCHRONISED |
| `HEALTH_EVENT_TIME_SYNC_LOST`     | `time_provider_mark_unsynchronised()` called while state was SYNCHRONISED |

These are the only events TimeProvider pushes. Sync-interval statistics (counts
of successful vs failed NTP queries) are not owned here — they belong to
`NtpClient` (GW) via its own health reporting.

---

## 7. Unit-test plan

Inject the `rtc_driver` mock vtable and a stub `ihealth_report_t` in `setUp()`:

```c
/* For tests compiled on the host (no RTC hardware) */
#ifdef TEST
/* Replace rtc_driver singleton with a mock vtable (LLD-D10). */
static irtc_t s_mock_rtc;
void setUp(void) {
    s_mock_rtc.get_time        = stub_rtc_read;
    s_mock_rtc.set_time        = stub_rtc_write;
    s_mock_rtc.read_backup     = stub_rtc_bkup_read;
    s_mock_rtc.write_backup    = stub_rtc_bkup_write;
    s_mock_rtc.is_backup_valid = stub_rtc_is_backup_valid;
    *(const irtc_t **)&rtc_driver = &s_mock_rtc;
}
#endif /* TEST */
```

Minimum test cases (TC-TP-001 to TC-TP-010):

| ID | Function | Stimulus | Expected result |
|----|----------|----------|-----------------|
| TC-TP-001 | `time_provider_get()` | Called before `time_provider_init()` | Returns `TIME_PROVIDER_ERR_NOT_INIT` |
| TC-TP-002 | `time_provider_get()` | Called after `init()`, no `set_time()` called yet | Returns `TIME_PROVIDER_ERR_OK`; `ts.sync_state == TIME_SYNC_UNSYNCHRONISED`; `ts.epoch` equals uptime seconds |
| TC-TP-003 | `time_provider_get()` | `ts_out` argument is `NULL` | Returns `TIME_PROVIDER_ERR_NULL_ARG` |
| TC-TP-004 | `time_provider_set_time()` | First call with a valid epoch; state is `UNSYNCHRONISED` | Returns `TIME_PROVIDER_ERR_OK`; state transitions to `TIME_SYNC_SYNCHRONISED`; `HEALTH_EVENT_TIME_SYNC_ACQUIRED` pushed exactly once |
| TC-TP-005 | `time_provider_set_time()` | Called again while already `SYNCHRONISED`; delta within threshold | Returns `TIME_PROVIDER_ERR_OK`; event **not** pushed again |
| TC-TP-006 | `time_provider_mark_unsynchronised()` | Called while `SYNCHRONISED` | Returns `TIME_PROVIDER_ERR_OK`; state transitions to `TIME_SYNC_UNSYNCHRONISED`; `HEALTH_EVENT_TIME_SYNC_LOST` pushed exactly once |
| TC-TP-007 | `time_provider_mark_unsynchronised()` | Called while already `UNSYNCHRONISED` | Returns `TIME_PROVIDER_ERR_OK`; event **not** pushed |
| TC-TP-008 | `time_provider_set_time()` | `\|new_epoch − rtc_current\|` > `TIME_PROVIDER_SANITY_DELTA_S` while `SYNCHRONISED` | Returns `TIME_PROVIDER_ERR_RTC_FAIL`; sync state unchanged |
| TC-TP-009 | `time_provider_init()` | Backup register BKP0R contains `TIME_PROVIDER_SYNC_MAGIC` | Returns `TIME_PROVIDER_ERR_OK`; initial state is `TIME_SYNC_SYNCHRONISED` |
| TC-TP-010 | `time_provider_init()` | Backup register BKP0R does not contain magic value | Returns `TIME_PROVIDER_ERR_OK`; initial state is `TIME_SYNC_UNSYNCHRONISED` |

---

## 8. Init sequence and boot ordering

```text
rtc_init()           ← driver layer already up
logger_init()        ← Logger uses RtcDriver directly (bootstrap exception)
time_provider_init() ← now safe: RtcDriver ready, Logger ready
```

TimeProvider does **not** require a two-phase init. No ISR consumer needs
to exist before it. The only caller-ordering constraint is that `RtcDriver`
and `Logger` are initialised first.

On the GW, `NtpClient` is initialised later (after WiFi). TimeProvider starts
in UNSYNCHRONISED state regardless.

---

## 9. Per-board notes

### Field Device

`ModbusRegisterMap` holds the time register (address range 0x0200–0x02FF,
commands and control). When the gateway writes that register, ModbusRegisterMap
calls `time_provider_set_time(new_epoch)`. No other FD component calls
`time_provider_set_time()` or `time_provider_mark_unsynchronised()`.

### Gateway

`TimeService` is the sole caller of `time_provider_set_time()` and
`time_provider_mark_unsynchronised()`. NTP sanity checking (delta threshold)
is performed by TimeService before calling `time_provider_set_time()`, not
inside TimeProvider. TimeProvider trusts its caller on the GW; the validation
layer is TimeService.

The exception: the `TIME_PROVIDER_SANITY_DELTA_S` guard inside
`time_provider_set_time()` remains as a defence-in-depth check — it is not
the primary validation path.

---

## 10. Open items

| ID | Item | Resolution path | Status |
|--------|------|-----------------|--------|
| TP-O1  | `TIME_PROVIDER_SYNC_INTERVAL_S` — [TBD], driven by REQ-NF-210 / REQ-NF-211. Determined at integration testing via RTC drift measurement. | Determine via RTC drift measurement at integration; set in time_provider_config.h | Open |
| TP-O2  | Backup register index (`TIME_PROVIDER_BKUP_REG`). Must be allocated from a project-wide backup-register map to avoid collision with reset-cause and watchdog flags. | Closed by LLD-D16: index 0 (BKP0R) allocated; see `lld.md` §6 backup-register allocation table. Magic changed to `0xA5A55A5AUL` (asymmetric, distinguishes from other users). | Closed |
| TP-O3  | `TIME_PROVIDER_SANITY_DELTA_S` — NTP delta sanity threshold. Decided in TimeService LLD companion (GW). TimeProvider needs the value to compile `time_provider_config.h`. | Confirm threshold value at TimeService LLD companion (GW) | Open |
