# LLD Companion — TimeService

**Board:** Gateway only.
**Layer:** Application.

Orchestrates NTP synchronisation at boot and periodically, applies a
delta sanity check before accepting a new timestamp, writes the
synchronised time to the RTC via `TimeProvider`, and propagates the
current time to the field device via `IModbusPoller`.

**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** TimeService in `components.md` (GW application layer)
---

## 1. Sources

| Field | Value |
|---|---|
| **Provides** | `ITimeService` |
| **Uses** | `TimeProvider`, `NtpClient`, `IModbusPoller`, `IConfigProvider`, `ILogger` |
| **Hosted in task** | `TimeServiceTask` priority 2, 512 words / 2 KB |
| **Activation** | Periodic NTP timer + retry-notify from `CloudPublisher` |

---

## 2. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| NTP sync at boot and periodically | REQ-TS-000, TS-010 | UC-13 |
| Write synchronised time to RTC | REQ-TS-020 | UC-13 |
| Push current time to FD via Modbus | REQ-TS-030 | UC-13 |
| Unsynchronised flag on NTP failure | REQ-TS-040 | UC-13 |
| Retry NTP on internet reconnect | REQ-TS-0E1 | UC-13 |

---

## 2. Public API — `ITimeService`

```c
/* application/include/i_time_service.h */

typedef enum {
    TIME_SYNC_NEVER     = 0,   /* no successful NTP sync since boot */
    TIME_SYNC_OK        = 1,   /* last sync succeeded               */
    TIME_SYNC_STALE     = 2,   /* last sync failed; RTC from prev boot or uptime */
} time_sync_state_t;

typedef struct ITimeService ITimeService;

struct ITimeService {
    void *ctx;

    time_sync_state_t (*get_sync_state)(void *ctx);

    /* Called by CloudPublisher on MQTT reconnect (REQ-TS-0E1).
     * Posts a retry-notify to TimeServiceTask; returns immediately. */
    void (*trigger_retry)(void *ctx);
};
```

`trigger_retry()` is the only cross-task call into `TimeService`. It uses
`xTaskNotify` (bit 1) and is non-blocking. `get_sync_state()` reads a
single `volatile` field — also non-blocking.

---

## 4. Activation model

`TimeServiceTask` blocks on a task notification bitmask:

| Bit | Source | Meaning |
|---|---|---|
| 0 | FreeRTOS software timer | Periodic NTP sync tick |
| 1 | `CloudPublisher.trigger_retry()` | Retry NTP on internet reconnect (REQ-TS-0E1) |

Timer period is configurable: read from `IConfigProvider.get_ntp_interval_s()`
at each tick, so interval changes take effect on the next cycle. Boot sync
fires immediately via a one-shot timer with delay 0 created at init.

---

## 5. Sync algorithm

```
do_ntp_sync():
    server_list = cfg_read->get_ntp_servers(cfg_read)  /* per-call, not cached */
    rc = ntp_client->sync(ntp_client, server_list, &new_utc)

    if rc != NTP_OK:
        sync_state = TIME_SYNC_STALE  /* or NEVER if first boot */
        log_warn("NTP sync failed: %d", rc)
        return TS_ERR_NTP_FAILED

    /* Delta sanity check (NTP-O2 decision: check in TimeService, not NtpClient) */
    current_utc = time_provider->get_utc(time_provider)
    delta = abs((int64_t)new_utc - (int64_t)current_utc)

    if sync_state != TIME_SYNC_NEVER and delta > NTP_MAX_DELTA_S:
        log_warn("NTP delta too large (%u s) — rejected", (uint32_t)delta)
        sync_state = TIME_SYNC_STALE
        return TS_ERR_DELTA_REJECTED

    /* Accept */
    time_provider->set_utc(time_provider, new_utc)  /* writes RtcDriver (REQ-TS-020) */
    sync_state = TIME_SYNC_OK
    log_info("NTP sync ok: UTC %u", new_utc)

    push_time_to_fd(new_utc)   /* REQ-TS-030 */
    return TS_OK
```

`NTP_MAX_DELTA_S = 3600` (1 hour). First sync (`TIME_SYNC_NEVER`) bypasses
the delta check — the RTC contains an arbitrary reset value and any
server-provided time is better.

---

## 6. FD time push (REQ-TS-030)

Current time is written to the field device via `IModbusPoller` (P1 —
`TimeService` must not call `ModbusMaster` directly):

```
push_time_to_fd(utc):
    uint16_t words[2];
    words[0] = (uint16_t)(utc >> 16);   /* high word */
    words[1] = (uint16_t)(utc & 0xFFFF); /* low word  */
    rc = modbus_poller->write_holding_registers(
             modbus_poller,
             FD_SLAVE_ADDR,       /* from IConfigProvider or DeviceProfileRegistry */
             TS_MODBUS_REG_ADDR,  /* timestamp holding register base address       */
             2, words)
    if rc != POLLER_OK:
        log_warn("FD time push failed: %d", rc)
        /* Non-fatal: FD will use uptime-based timestamps until next sync */
```

`TS_MODBUS_REG_ADDR` maps to the timestamp holding registers per
`docs/modbus-register-map.md` §6.4.

**Trigger:** FD push runs after every successful NTP sync. REQ-TS-030
also requires a push "on initial connection". The initial-connection case
is satisfied by the boot-time sync at delay 0, which fires as soon as
`ModbusPoller` has established the FD link. If the Modbus link is not yet
up at boot sync time, `push_time_to_fd` returns a non-fatal error and
retries on the next periodic sync. This is tracked as **TS-O1** — whether
an explicit "link-established" event-driven push is required in addition
to the periodic one.

---

## 7. Sync state and unsynchronised flag (REQ-TS-040)

`sync_state` is a `volatile time_sync_state_t` field in `time_service_t`,
written by `TimeServiceTask` and read by any task via `get_sync_state()`.
`volatile` guarantees visibility across tasks on single-core Cortex-M4;
no mutex is needed for a single-word enum read.

`SensorService` and `CloudPublisher` read `sync_state` via `ITimeService`
to decide whether to flag readings and payloads as "unsynchronised"
(REQ-NF-212, REQ-TS-040). They read it at the point of use (pull-based,
P7); `TimeService` does not push the state.

---

## 3. Internal design

```c
typedef struct {
    TimeProvider        *time_provider;
    NtpClient           *ntp_client;
    IModbusPoller       *modbus_poller;
    const IConfigProvider *cfg_read;
    ILogger             *log;

    volatile time_sync_state_t sync_state;  /* read by other tasks */
    TaskHandle_t               task_handle;
    TimerHandle_t              ntp_timer;
} time_service_t;
```

---

### Principles applied

- **P1 (Strict directional layering).** Depends on INtpClient (middleware), ITimeProvider (middleware), and IModbusPoller (application peer for FD time-write); all are at the same or lower layer.
- **P2 (Dependency Inversion).** Consumes INtpClient and ITimeProvider via injected vtable pointers; exposes `itime_service_t` vtable.
- **P4 (Cross-cutting concern exception).** Logger and HealthMonitor (IHealthReport) referenced concretely per the cross-cutting exception.
- **P5 (Bounded resources, no dynamic allocation post-init).** Synchronisation state in a static struct; NTP query executed with a statically-allocated 48-byte packet; no heap.
- **P6 (Responsibility traces to requirements).** Synchronisation loop, delta-sanity check, and FD time-write functions trace to REQ-TS-010 / REQ-NF-210-212 time-synchronisation requirements.
- **P8 (Total error propagation, no silent failures).** `time_service_err_t` on all operations; NTP query errors, delta-sanity rejections, and TimeProvider set errors are distinct error codes.
- **P9 (BARR-C coding standard).** NTP poll interval `uint32_t` seconds; delta threshold `int32_t`; no floating-point.
- **P10 (Naming conventions).** Prefix `time_service_`; interface `ITimeService` -> `itime_service_t`; errors `TIME_SERVICE_ERR_*`.


## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

### SD trace

| SD | Component role | Key function |
|---|---|---|
| SD-09 | `TimeService` orchestrates NTP synchronisation: calls `ntp_client_sync()`, writes the result to `RtcDriver` via `rtc_driver_set_time()`, posts a time-write command to `ModbusPoller`, and marks the sync flag | `time_service_sync()`, `time_service_get_time()` |

---

## 6. Error and fault behaviour

```c
typedef enum {
    TS_OK = 0,
    TS_ERR_NTP_FAILED,       /* NtpClient returned error            */
    TS_ERR_DELTA_REJECTED,   /* timestamp delta too large           */
    TS_ERR_RTC_WRITE,        /* TimeProvider.set_utc() failed       */
    TS_ERR_FD_PUSH,          /* IModbusPoller write failed          */
    TS_ERR_NULL_ARG,
    TS_ERR_NOT_INIT,
} ts_err_t;
```

All errors are logged. `TS_ERR_FD_PUSH` and `TS_ERR_DELTA_REJECTED` are
non-fatal — `TimeServiceTask` continues and retries on the next tick.
`TS_ERR_RTC_WRITE` is treated as a warning; the RTC retains its last
value and timestamps degrade to stale-but-consistent.

---

## 10. Concurrency

| State | Writer | Reader(s) | Protection |
|---|---|---|---|
| `sync_state` | `TimeServiceTask` | `CloudPublisher`, `SensorService`, `TimeServiceTask` | `volatile` — single word, Cortex-M4 single-cycle read |
| All other fields | `TimeServiceTask` only | — | None needed |

`trigger_retry()` called from `CloudPublisherTask` uses `xTaskNotify`
(non-blocking, atomic). No shared mutable state is accessed.

---

## 11. Initialisation

```c
ts_err_t time_service_init(time_service_t        *self,
                           TimeProvider           *time_provider,
                           NtpClient              *ntp_client,
                           IModbusPoller          *modbus_poller,
                           const IConfigProvider  *cfg_read,
                           ILogger                *log);
```

Steps:
1. Store handles; set `sync_state = TIME_SYNC_NEVER`.
2. Create FreeRTOS software timer (periodic NTP, auto-reload).
3. Create `TimeServiceTask`.
4. Start a one-shot timer (delay 0) to trigger boot-time NTP sync as
   soon as `TimeServiceTask` is unblocked by the start-gate.

---

## 12. Memory and sizing

| Item | Size |
|---|---|
| `time_service_t` context | ~48 B |
| Timer handle | ~8 B |
| **Total RAM** | **~56 B** |

Stack: 512 words / 2 KB. NTP sync involves a blocking UDP call inside
`NtpClient` — the stack must accommodate the NtpClient call frame
(estimated ~512 B). Remaining headroom: ~1.5 KB.

---

## 7. Unit-test plan

### 13.1 Unit tests — `tests/application/test_time_service.c`

Mocks: `TimeProvider`, `NtpClient`, `IModbusPoller`, `IConfigProvider`,
`ILogger`.

| Suite | Coverage |
|---|---|
| Init | Null-arg rejection; `sync_state = TIME_SYNC_NEVER` after init |
| Successful sync — first boot | NTP returns UTC; delta check bypassed (`NEVER` state); `time_provider->set_utc` called; `sync_state = TIME_SYNC_OK`; `modbus_poller->write_holding_registers` called with correct words |
| Successful sync — subsequent | Delta within `NTP_MAX_DELTA_S`; accepted; `SYNC_OK` |
| NTP failure | NtpClient returns error; `sync_state = SYNC_STALE`; FD push not attempted |
| Delta rejected | New time differs by `NTP_MAX_DELTA_S + 1`; rejected; `SYNC_STALE`; FD push not attempted |
| FD push failure | NTP ok; `modbus_poller` returns error; `SYNC_OK` retained; error logged |
| `trigger_retry` | Posts bit 1 notification to task; subsequent tick calls `do_ntp_sync` |
| `get_sync_state` | Returns current `sync_state` directly |
| Configurable interval | `get_ntp_interval_s` returns different value between ticks; timer period updated |

### 13.2 Integration tests — on target

| Test | Setup |
|---|---|
| Boot sync | Power on with WiFi connected; verify `sync_state = SYNC_OK` within 10 s; verify FD timestamp register matches NTP time |
| NTP retry on reconnect | Disconnect WiFi at boot; verify `SYNC_STALE`; reconnect; `trigger_retry` fires; verify `SYNC_OK` |
| FD timestamp propagation | After sync; read FD timestamp holding registers via Modbus; verify match to GW RTC within 1 s |
| Delta rejection | Inject a spoofed NTP response with timestamp 2 hours in the future; verify rejected; `SYNC_STALE` |

---

## 8. Open items

| ID | Item | Resolution path | Status |
|--------|------|-----------------|--------|
| **TS-O1** | REQ-TS-030 "on initial connection" — currently satisfied by the first periodic NTP sync at boot. If the Modbus link is not up at boot sync time, the push is deferred to the next sync. Evaluate at integration whether an explicit event-driven push on `link_established` is needed. |
| **NTP-O2** | DNS dependency: if `WifiDriver` does not expose DNS resolution, `NtpClient` must be provisioned with NTP server IP addresses instead of hostnames. Confirm DNS availability during WifiDriver LLD phase. |

---

## 15. References

- `docs/components.md` (GW TimeService entry).
- `docs/sequence-diagrams.md` SD-09 (time synchronisation flow).
- `docs/state-machines.md` Machine 2 (Cloud Connectivity — internet\_restored event triggers TS-0E1).
- `docs/modbus-register-map.md` §6.4 (timestamp holding registers).
- `docs/lld/ntp-client.md` (defines `NtpClient` interface).
- `docs/lld/time-provider.md` (defines `TimeProvider`).
- `docs/architecture-principles.md` P1 (P1 compliance via `IModbusPoller`), P7 (pull-based `sync_state` reads).

---

*Companion produced during the LLD Application Phase. Authored by Luca
Agrippino; reviewed against the V-Model gate criteria for LLD.*
