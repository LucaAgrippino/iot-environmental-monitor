# LLD Companion — TimeProvider

**Layer:** Middleware  
**Boards:** Field Device (FD) · Gateway (GW)  
**Provides:** `ITimeProvider`  
**Consumes:** `IRtc` (RtcDriver), `IHealthReport`, `ILogger`  
**SRS traces:** REQ-TS-040, REQ-NF-210, REQ-NF-211, REQ-NF-212  
**HLD ref:** `components.md` §Middleware — TimeProvider; `hld.md` §5.2, §5.5

---

## 1. Responsibility

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

## 3. Provided interface — `ITimeProvider`

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
 */
time_provider_err_t time_provider_init(IHealthReport *health);

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
 */
time_sync_state_t time_provider_get_sync_state(void);
```

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
#define TIME_PROVIDER_BKUP_REG   3U   /* RTC_BKP3R — index TBD, see TP-O2 */
#define TIME_PROVIDER_SYNC_MAGIC 0xA5A5A5A5UL
```

On `time_provider_init()`:
- If `RTC_BKP3R == TIME_PROVIDER_SYNC_MAGIC`, initialise as SYNCHRONISED.
- Otherwise, initialise as UNSYNCHRONISED.

On `time_provider_set_time()`: write `TIME_PROVIDER_SYNC_MAGIC` to the
backup register.

On `time_provider_mark_unsynchronised()`: write `0x00000000` to the
backup register.

**Constraint:** backup register index 3 must not conflict with other users
(reset-cause storage, watchdog flag). Resolution tracked as TP-O2.

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

## 7. Internal state and thread safety

```c
/* time_provider.c — static module state */
typedef struct {
    bool              initialised;
    time_sync_state_t sync_state;
    IHealthReport    *health;
    SemaphoreHandle_t mutex;   /* FreeRTOS mutex, priority inheritance enabled */
} TimeProviderState;

static TimeProviderState s_tp;
```

All public functions acquire `s_tp.mutex` before accessing `sync_state` or
calling `rtc_*`. The mutex is created in `time_provider_init()` — never
dynamically after init (P8: no post-init dynamic allocation).

`time_provider_get_sync_state()` reads `s_tp.sync_state` under the mutex and
returns a copy. No RTC access.

---

## 8. Init sequence and boot ordering

```
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

## 9. Host-side unit test stub

```c
/* For tests compiled on the host (no RTC hardware) */
#ifdef UNIT_TEST

#define rtc_read(out)        stub_rtc_read(out)
#define rtc_write(epoch)     stub_rtc_write(epoch)
#define rtc_bkup_read(reg)   stub_rtc_bkup_read(reg)
#define rtc_bkup_write(r, v) stub_rtc_bkup_write(r, v)

#endif /* UNIT_TEST */
```

Test cases to cover at minimum:
- `time_provider_get()` returns UNSYNCHRONISED + uptime epoch before any
  `set_time()` call.
- `time_provider_set_time()` transitions state to SYNCHRONISED and pushes
  `HEALTH_EVENT_TIME_SYNC_ACQUIRED` exactly once.
- Repeated `set_time()` calls do not push the event again.
- `time_provider_mark_unsynchronised()` transitions back and pushes
  `HEALTH_EVENT_TIME_SYNC_LOST` exactly once.
- Sanity-check rejection: `set_time()` with delta > `TIME_PROVIDER_SANITY_DELTA_S`
  returns `TIME_PROVIDER_ERR_RTC_FAIL` and does not change state.
- Init with magic backup register → starts SYNCHRONISED; without → starts
  UNSYNCHRONISED.

---

## 10. Per-board notes

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

## 11. Open items

| ID     | Item                                                                                  |
|--------|---------------------------------------------------------------------------------------|
| TP-O1  | `TIME_PROVIDER_SYNC_INTERVAL_S` — [TBD], driven by REQ-NF-210 / REQ-NF-211. Determined at integration testing via RTC drift measurement. |
| TP-O2  | Backup register index (`TIME_PROVIDER_BKUP_REG`). Must be allocated from a project-wide backup-register map to avoid collision with reset-cause and watchdog flags. Not yet produced. |
| TP-O3  | `TIME_PROVIDER_SANITY_DELTA_S` — NTP delta sanity threshold. Decided in TimeService LLD companion (GW). TimeProvider needs the value to compile `time_provider_config.h`. |
