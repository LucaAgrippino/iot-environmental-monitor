# LLD Companion — TimeService

**Companion document to `hld.md`.** This artefact specifies the
**software design** of the `TimeService` Application component on
the Gateway — its interface, NTP synchronisation algorithm, FD
time propagation, sync-state management, and test plan.

TimeService is the **application-level orchestrator** for time
synchronisation. It consumes `NtpClient` (Middleware) as a reusable
NTP transport — NtpClient knows nothing about sync policy, delta
checks, or RTC writes. This companion documents only TimeService;
the NtpClient and TimeProvider companions are separate documents at
the Middleware layer.

**Layer:** Application
**Board:** Gateway (GW) only
**Provides:** `ITimeService`
**Consumes:** `TimeProvider`, `NtpClient` (Middleware, downward);
`ModbusPoller` via `modbus_poller_send_command()` (Application, peer);
`IConfigProvider` (Application, peer); `Logger` (cross-cutting, P4)
**SRS traces:** REQ-TS-000, TS-010, TS-020, TS-030, TS-040, TS-0E1;
REQ-NF-201, NF-212
**HLD ref:** `components.md` §Application — TimeService;
`sequence-diagrams.md` SD-09;
`state-machines.md` Machine 2 (Cloud Connectivity);
`task-breakdown.md` §TimeServiceTask
**Version:** 1.2 (interface header split out 2026-10-28, CP-D23)
**Date:** October 2026
**Status:** Pass H — Implementation ready · Gateway implemented (host-tested, bench-validated 2026-10-24)

**HLD anchor:** TimeService in `components.md` (GW application layer)

---

## 0. Reconciliation with the repository (v1.1)

v1.0 listed its consumed signatures as provisional. The Gateway modules it consumes now
exist, so the implementation follows them, not the v1.0 listings in §1.2, §3, §6, §7 and
§13. Where a v1.0 code block disagrees with this table, this table wins.

| v1.0 said | Repository / implementation | Decision |
|---|---|---|
| `time_sync_state_t` = NEVER / OK / STALE | `time_sync_state_t` already exists in `time_provider.h` (UNSYNCHRONISED / SYNCHRONISED) and is used by TimeProvider, SensorService and HealthMonitor | **TS-D11:** TimeService's enum is `time_service_sync_t` with `TIME_SERVICE_SYNC_NEVER / _OK / _STALE`. |
| `time_service_init(void)`; dependencies reached through singleton accessors | `ntp_client_query()` needs an `ntp_client_handle_t` created with WifiTask and HealthMonitor handles | **TS-D2 amended:** `time_service_init(ntp_client_handle_t ntp)`. NULL returns `TIME_SERVICE_ERR_NULL_ARG`, as the v1.0 text already described. |
| `ntp_client_sync(servers, n, &utc)` returning `NTP_CLIENT_ERR_DNS / TIMEOUT / RESPONSE` | `ntp_client_query(handle, servers, n, &epoch)` returning `NTP_CLIENT_ERR_ALL_FAILED` etc. | Any non-OK result is `TIME_SERVICE_ERR_NTP_FAILED`. |
| `time_provider_get_utc()` / `time_provider_set_utc()` | `time_provider_get(&ts)` (epoch + sync state) / `time_provider_set_time(epoch)` | Used as implemented. |
| Delta check whenever state is not NEVER | An RTC that is unreadable or reports UNSYNCHRONISED holds uptime, not wall-clock time | **TS-D12:** the delta check is skipped in those two cases and the NTP time is accepted. |
| `IConfigProvider` for interval and servers | No `IConfigProvider` exists on the Gateway (same gap as SS-O6) | **TS-D13 / TS-O4:** compile-time defaults: 3600 s and two IPv4 servers. |
| NTP servers by hostname | The ISM43362 has no DNS resolver (NtpClient harness) | Servers are IPv4 literals (162.159.200.1, 216.239.35.0). **TS-O3 closed for the Gateway.** |
| Modbus register `40001`, "register map §6.4" | No timestamp register existed in the map | **TS-D14:** `CMD_SET_TIME_HI/LO` added at 0x0210 / 0x0211 in `modbus-register-map.md` §6.5. One FC16 write of exactly two registers. **TS-O5:** the FD-side handler does not exist yet. |
| `modbus_command_t` field order per v1.0 | `response_queue` comes first (MBP-D6); `modbus_poller_send_command(const modbus_command_t *)` | Designated initialisers; no behavioural change. |
| `firmware/gateway/application/time_service/` | `firmware/gateway/application/time_service/` | Layer directory is `application`. |
| `queue.h`-style private includes in `time_service.c` | Ceedling links a `.c` for every header basename a test includes | Dependency headers are grouped in `time_service_internal.h` (same trick as SS-D11). |

Logging uses the repository's `LOG_WARN` / `LOG_INFO` macros; they compile to nothing in
host tests, so "error logged" in TC-TS-011..013 is verified by the returned state, not by
a log assertion.

---

## 1. Sources

| Field | Value |
|---|---|
| **Name** | `TimeService` |
| **Layer** | Application |
| **Board** | Gateway only |
| **Provides** | `ITimeService` |
| **Uses** | `TimeProvider`, `NtpClient`, `ModbusPoller` (`modbus_poller_send_command`), `IConfigProvider`, `Logger` |
| **Hosted in task** | `TimeServiceTask` (priority 2, stack 768 words / 3 KB, per `task-breakdown.md`) |
| **Activation** | Periodic NTP timer + retry-notify from `CloudPublisher` |

**Design decision TS-D1 (singleton with vtable interface):**
TimeService uses the singleton pattern for its internal state
(static `s_ts` struct, free-function init) — consistent with
ModbusPoller. The provided `ITimeService` is exposed as a vtable
struct with `void *ctx` so that consumers (`CloudPublisher`,
`SensorService`) depend on an abstraction, not on a concrete
module, and unit tests can inject a mock. The `ctx` field points
to the static instance; consumers call `ts->get_sync_state(ts->ctx)`.

### 1.1 Patterns applied

- **Pull-based data access** (P7) — consumers read `sync_state`
  via `ITimeService.get_sync_state()` at the point of use.
  `TimeService` does not push state to consumers.
- **Command routing** — FD time-push is routed through
  `ModbusPoller` as a command (`modbus_poller_send_command`).
  `TimeService` never calls `ModbusMaster` directly (P1).

### 1.2 Consumed interfaces

TimeService calls the following external functions. Full API
documentation is in each component's own companion.

**TimeProvider** (`time_provider.h`) — Middleware, concrete
dependency (downward):

```c
typedef enum {
    TIME_PROVIDER_ERR_OK = 0,
    TIME_PROVIDER_ERR_NOT_INIT,
    TIME_PROVIDER_ERR_RTC_FAIL,
} time_provider_err_t;

/**
 * @brief  Read the current UTC time from the RTC.
 * @param[out] utc_out  Unix timestamp (seconds since epoch).
 */
time_provider_err_t time_provider_get_utc(uint32_t *utc_out);

/**
 * @brief  Write a new UTC time to the RTC.
 * @param  utc  Unix timestamp to write.
 */
time_provider_err_t time_provider_set_utc(uint32_t utc);
```

**NtpClient** (`ntp_client.h`) — Middleware, concrete dependency
(downward):

```c
typedef enum {
    NTP_CLIENT_ERR_OK = 0,
    NTP_CLIENT_ERR_DNS,
    NTP_CLIENT_ERR_TIMEOUT,
    NTP_CLIENT_ERR_RESPONSE,
    NTP_CLIENT_ERR_NOT_INIT,
} ntp_client_err_t;

/**
 * @brief  Query an NTP server and return the UTC time.
 *
 * Blocks until a response is received or timeout expires. Routes
 * UDP through WifiTask's API (does not use its own socket).
 *
 * @param  servers       NULL-terminated array of NTP server hostnames or IPs.
 * @param  server_count  Number of entries in servers[].
 * @param[out] utc_out   Received Unix timestamp on success.
 */
ntp_client_err_t ntp_client_sync(const char * const *servers,
                                 uint8_t             server_count,
                                 uint32_t           *utc_out);
```

**ModbusPoller** (`modbus_poller.h`) — Application, peer. Called
via the free-function command queue (singleton pattern, consistent
with ModbusPoller companion §4):

```c
/**
 * @brief  Post a write command for ModbusPoller to execute.
 *
 * Non-blocking. Command drained in the next poll cycle.
 * TimeService uses FC16 (write multiple registers) to push
 * a 32-bit UTC timestamp as two 16-bit words.
 */
modbus_poller_err_t modbus_poller_send_command(modbus_command_t *cmd);
```

`modbus_command_t` layout per ModbusPoller companion §4.1 — relevant
fields for time-push:

```c
typedef struct {
    uint8_t       slave_addr;       /* FD_SLAVE_ADDR                */
    uint8_t       function_code;    /* 0x10 (FC16)                  */
    uint16_t      start_reg;        /* TS_MODBUS_REG_ADDR           */
    uint16_t      reg_count;        /* 2                            */
    uint16_t      data[125];        /* data[0]=high, data[1]=low    */
    QueueHandle_t response_queue;   /* caller-provided; receives result */
} modbus_command_t;
```

**IConfigProvider** (`i_config_provider.h`) — Application, peer
(interface dependency):

```c
/**
 * @brief  Read the NTP sync interval.
 * @return Interval in seconds (default 3600).
 */
uint32_t config_provider_get_ntp_interval_s(void);

/**
 * @brief  Get the NTP server list.
 * @param[out] count_out  Number of servers in the returned array.
 * @return NULL-terminated array of server hostname strings.
 */
const char * const *config_provider_get_ntp_servers(uint8_t *count_out);
```

**Logger** (`logger.h`) — cross-cutting, concrete (P4):

```c
void logger_log(logger_level_t level, const char *tag,
                const char *fmt, ...);
```

> **Note:** Consumed signatures are provisional where the providing
> module has not yet been implemented. Exact signatures will be
> confirmed during implementation. `IConfigProvider` is consumed by
> TimeService but not listed in `components.md` §TimeService USES —
> tracked as **TS-O2**.

---

## 2. Traceability

| Concern | SRS requirements | Use cases |
|---|---|---|
| NTP sync at boot and periodically | REQ-TS-000, REQ-TS-010 | UC-13 |
| Write synchronised time to RTC | REQ-TS-020 | UC-13 |
| Push current time to FD via Modbus | REQ-TS-030 | UC-13 |
| Unsynchronised flag on NTP failure | REQ-TS-040 | UC-13 |
| Retry NTP on internet reconnect | REQ-TS-0E1 | UC-13 |
| No dynamic allocation post-init | REQ-NF-201 | — |
| Timestamp quality flag in payloads | REQ-NF-212 | — |

---

## 3. Public API — ITimeService

### 3.1 Sync state enum

```c
/* itime_service.h (the vtable and this enum); time_service.h includes it. The split (CP-D23)
 * lets CloudPublisher include the interface without the whole module header. */

#ifndef TIME_SERVICE_H
#define TIME_SERVICE_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    TIME_SYNC_NEVER = 0,  /**< No successful NTP sync since boot. */
    TIME_SYNC_OK    = 1,  /**< Last sync succeeded.               */
    TIME_SYNC_STALE = 2,  /**< Last sync failed; RTC from previous
                               boot or free-running uptime.       */
} time_sync_state_t;
```

### 3.2 Error enum

```c
typedef enum {
    TIME_SERVICE_OK = 0,
    TIME_SERVICE_ERR_NTP_FAILED,      /**< NtpClient returned error.          */
    TIME_SERVICE_ERR_DELTA_REJECTED,  /**< Timestamp delta exceeds threshold. */
    TIME_SERVICE_ERR_RTC_WRITE,       /**< TimeProvider set_utc() failed.     */
    TIME_SERVICE_ERR_FD_PUSH,         /**< ModbusPoller send_command failed.  */
    TIME_SERVICE_ERR_NULL_ARG,        /**< NULL pointer passed to init.       */
    TIME_SERVICE_ERR_NOT_INIT,        /**< Service not initialised.           */
} time_service_err_t;
```

### 3.3 ITimeService vtable

```c
/**
 * @brief  Provided interface — consumed by CloudPublisher and SensorService.
 *
 * Consumers obtain a pointer via time_service_get_interface() and pass
 * ts->ctx back to each vtable call. The ctx field is opaque — consumers
 * never cast it.
 */
typedef struct {
    void *ctx;

    /**
     * @brief  Read the current synchronisation state.
     *
     * Non-blocking — reads a single volatile field. Called by
     * SensorService and CloudPublisher to decide whether to flag
     * readings and payloads as "unsynchronised" (REQ-TS-040,
     * REQ-NF-212).
     */
    time_sync_state_t (*get_sync_state)(void *ctx);

    /**
     * @brief  Request an immediate NTP retry.
     *
     * Non-blocking — posts a task notification (bit 1) to
     * TimeServiceTask. Called by CloudPublisher on MQTT reconnect
     * (REQ-TS-0E1). Returns immediately; the sync attempt happens
     * asynchronously in TimeServiceTask.
     */
    void (*trigger_retry)(void *ctx);
} itime_service_t;
```

### 3.4 Init and accessor

```c
/**
 * @brief  Initialise TimeService.
 *
 * Stores dependency references, creates the periodic NTP timer
 * and TimeServiceTask. Must be called after TimeProvider, NtpClient,
 * and ModbusPoller are initialised.
 *
 * @return TIME_SERVICE_OK on success;
 *         TIME_SERVICE_ERR_NULL_ARG if any argument is NULL.
 * @note   Threading: call before the scheduler starts.
 */
time_service_err_t time_service_init(void);

/**
 * @brief  Obtain the provided interface vtable.
 *
 * Valid only after time_service_init() has succeeded. Returns NULL
 * if not initialised.
 *
 * @return Pointer to the static itime_service_t vtable, or NULL.
 */
const itime_service_t *time_service_get_interface(void);

#ifdef TEST
/**
 * @brief  Reset internal state for unit-test isolation.
 *
 * Zeroes the singleton struct and vtable binding. Each test case
 * calls this in setUp() before re-initialising.
 */
void time_service_reset_for_test(void);
#endif

#endif /* TIME_SERVICE_H */
```

> **Design note (TS-D2):** `time_service_init()` takes no explicit
> dependency parameters. Dependencies are injected by calling their
> singleton accessors internally (e.g. `time_provider_get_utc()`,
> `ntp_client_sync()`). This matches the ModbusPoller singleton
> pattern — free functions call free functions. For unit testing,
> CMock replaces each dependency at link time. The `itime_service_t`
> vtable provides the abstraction consumers need (P2), while the
> implementation stays a clean singleton (TS-D1).

---

## 4. Internal design

### 4.1 Singleton state

```c
/* time_service.c */

#include "time_service.h"
#include "time_provider.h"
#include "ntp_client.h"
#include "modbus_poller.h"
#include "i_config_provider.h"
#include "logger.h"
#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"
#include <string.h>

#define TIME_SERVICE_TAG              "TS"
#define TIME_SERVICE_NTP_MAX_DELTA_S  3600U   /* 1 hour (TS-D5) */

#define TS_NOTIFY_BIT_PERIODIC  (1U << 0)
#define TS_NOTIFY_BIT_RETRY     (1U << 1)

static struct {
    bool                       initialised;
    volatile time_sync_state_t sync_state;
    TaskHandle_t               task_handle;
    TimerHandle_t              ntp_timer;
} s_ts;

/* FreeRTOS static allocation — task */
static StackType_t  s_task_stack[768];
static StaticTask_t s_task_tcb;

/* FreeRTOS static allocation — timer */
static StaticTimer_t s_timer_storage;

/* Vtable — populated by time_service_init() */
static itime_service_t s_vtable;
```

### 4.2 Static functions (forward declarations)

```c
static void           time_service_task(void *pvParameters);
static void           ntp_timer_cb(TimerHandle_t xTimer);
static time_service_err_t prv_do_ntp_sync(void);
static time_service_err_t prv_push_time_to_fd(uint32_t utc);
static time_sync_state_t  prv_get_sync_state(void *ctx);
static void               prv_trigger_retry(void *ctx);
```

---

## 5. Activation model

`TimeServiceTask` blocks on `xTaskNotifyWait()` with a bitmask:

| Bit | Source | Meaning |
|---|---|---|
| 0 (`TS_NOTIFY_BIT_PERIODIC`) | FreeRTOS software timer callback | Periodic NTP sync tick |
| 1 (`TS_NOTIFY_BIT_RETRY`) | `CloudPublisher` via `trigger_retry()` | Retry NTP on internet reconnect (REQ-TS-0E1) |

Timer period is read from `config_provider_get_ntp_interval_s()` each
time the timer fires, so interval changes take effect on the next
cycle (TS-D6).

**Design decision TS-D3 (boot sync in task entry):** The boot-time
NTP sync runs as the first action inside `time_service_task()`,
before starting the periodic timer. This is simpler than a separate
one-shot timer. If WiFi is not ready, the sync fails gracefully
(`TIME_SYNC_NEVER` retained) and retries on the next periodic tick
or when `CloudPublisher` calls `trigger_retry()` on MQTT connect.

### 5.1 Task entry point

```c
static void time_service_task(void *pvParameters)
{
    (void)pvParameters;

    /* Boot sync (TS-D3) — attempt immediately */
    (void)prv_do_ntp_sync();

    /* Start periodic timer with configurable interval */
    uint32_t interval_s = config_provider_get_ntp_interval_s();
    (void)xTimerChangePeriod(s_ts.ntp_timer,
                             pdMS_TO_TICKS(interval_s * 1000U),
                             portMAX_DELAY);
    (void)xTimerStart(s_ts.ntp_timer, portMAX_DELAY);

    for (;;)
    {
        uint32_t bits = 0U;
        (void)xTaskNotifyWait(0U, UINT32_MAX, &bits, portMAX_DELAY);

        if ((bits & (TS_NOTIFY_BIT_PERIODIC | TS_NOTIFY_BIT_RETRY))
            != 0U)
        {
            (void)prv_do_ntp_sync();

            /* Re-read interval on periodic tick (TS-D6) */
            if ((bits & TS_NOTIFY_BIT_PERIODIC) != 0U)
            {
                uint32_t new_s = config_provider_get_ntp_interval_s();
                (void)xTimerChangePeriod(
                    s_ts.ntp_timer,
                    pdMS_TO_TICKS(new_s * 1000U),
                    portMAX_DELAY);
            }
        }
    }
}
```

### 5.2 Timer callback

```c
static void ntp_timer_cb(TimerHandle_t xTimer)
{
    (void)xTimer;
    (void)xTaskNotify(s_ts.task_handle,
                      TS_NOTIFY_BIT_PERIODIC, eSetBits);
}
```

---

## 6. Sync algorithm

```c
static time_service_err_t prv_do_ntp_sync(void)
{
    /* 1. Get NTP server list from config (TS-D6) */
    uint8_t server_count = 0U;
    const char * const *servers =
        config_provider_get_ntp_servers(&server_count);

    /* 2. Query NTP (REQ-TS-000, REQ-TS-010) */
    uint32_t new_utc = 0U;
    ntp_client_err_t ntp_rc = ntp_client_sync(servers,
                                              server_count,
                                              &new_utc);
    if (ntp_rc != NTP_CLIENT_ERR_OK)
    {
        if (s_ts.sync_state != TIME_SYNC_NEVER)
        {
            s_ts.sync_state = TIME_SYNC_STALE;
        }
        /* TIME_SYNC_NEVER stays NEVER — no previous good sync */
        logger_log(LOG_WARN, TIME_SERVICE_TAG,
                   "NTP sync failed: %d", (int)ntp_rc);
        return TIME_SERVICE_ERR_NTP_FAILED;
    }

    /* 3. Delta sanity check (TS-D5) — bypass on first sync */
    if (s_ts.sync_state != TIME_SYNC_NEVER)
    {
        uint32_t current_utc = 0U;
        (void)time_provider_get_utc(&current_utc);

        uint32_t delta = (new_utc > current_utc)
                             ? (new_utc - current_utc)
                             : (current_utc - new_utc);

        if (delta > TIME_SERVICE_NTP_MAX_DELTA_S)
        {
            s_ts.sync_state = TIME_SYNC_STALE;
            logger_log(LOG_WARN, TIME_SERVICE_TAG,
                       "NTP delta %u s exceeds threshold — rejected",
                       (unsigned)delta);
            return TIME_SERVICE_ERR_DELTA_REJECTED;
        }
    }

    /* 4. Accept — write RTC (REQ-TS-020) */
    time_provider_err_t tp_rc = time_provider_set_utc(new_utc);
    if (tp_rc != TIME_PROVIDER_ERR_OK)
    {
        logger_log(LOG_WARN, TIME_SERVICE_TAG,
                   "RTC write failed: %d", (int)tp_rc);
        /* Non-fatal (TS-D7): NTP succeeded, so mark OK anyway.
         * RTC retains last value; timestamps stale-but-consistent. */
    }

    s_ts.sync_state = TIME_SYNC_OK;
    logger_log(LOG_INFO, TIME_SERVICE_TAG,
               "NTP sync ok: UTC %u", (unsigned)new_utc);

    /* 5. Push time to FD (REQ-TS-030) */
    time_service_err_t fd_rc = prv_push_time_to_fd(new_utc);
    (void)fd_rc;  /* logged inside prv_push_time_to_fd; non-fatal */

    return (tp_rc != TIME_PROVIDER_ERR_OK)
               ? TIME_SERVICE_ERR_RTC_WRITE
               : TIME_SERVICE_OK;
}
```

`TIME_SERVICE_NTP_MAX_DELTA_S = 3600` (1 hour). First sync
(`TIME_SYNC_NEVER`) bypasses the delta check — the RTC contains an
arbitrary reset value and any server-provided time is better (TS-D5).

---

## 7. FD time push (REQ-TS-030)

Current time is written to the field device via `ModbusPoller`'s
command queue (P1 — `TimeService` must not call `ModbusMaster`
directly):

```c
/* v1.1: slave 1, register CMD_SET_TIME_HI = 0x0210, see §0 and
 * docs/hld/modbus-register-map.md §6.5. */
#define TS_FD_SLAVE_ADDR       0x01U
#define TS_MODBUS_REG_ADDR     0x0210U

static StaticQueue_t         s_fd_resp_queue_storage;
static uint8_t               s_fd_resp_buf[sizeof(modbus_command_response_t)];
static QueueHandle_t         s_fd_resp_queue;  /* created in init */

static time_service_err_t prv_push_time_to_fd(uint32_t utc)
{
    modbus_command_t cmd;
    (void)memset(&cmd, 0, sizeof(cmd));

    cmd.slave_addr    = TS_FD_SLAVE_ADDR;
    cmd.function_code = 0x10U;  /* FC16 — write multiple registers */
    cmd.start_reg     = TS_MODBUS_REG_ADDR;
    cmd.reg_count     = 2U;
    cmd.data[0]       = (uint16_t)(utc >> 16U);     /* high word */
    cmd.data[1]       = (uint16_t)(utc & 0xFFFFU);  /* low word  */
    cmd.response_queue = s_fd_resp_queue;

    modbus_poller_err_t rc = modbus_poller_send_command(&cmd);
    if (rc != MODBUS_POLLER_ERR_OK)
    {
        logger_log(LOG_WARN, TIME_SERVICE_TAG,
                   "FD time push rejected: %d", (int)rc);
        return TIME_SERVICE_ERR_FD_PUSH;
    }

    /* Wait for ModbusPoller to execute the command */
    modbus_command_response_t resp;
    if (xQueueReceive(s_fd_resp_queue, &resp, pdMS_TO_TICKS(5000U))
        != pdTRUE)
    {
        logger_log(LOG_WARN, TIME_SERVICE_TAG,
                   "FD time push: response timeout");
        return TIME_SERVICE_ERR_FD_PUSH;
    }

    if (resp.status != MODBUS_MASTER_ERR_OK)
    {
        logger_log(LOG_WARN, TIME_SERVICE_TAG,
                   "FD time push failed: Modbus error %d",
                   (int)resp.status);
        return TIME_SERVICE_ERR_FD_PUSH;
    }

    return TIME_SERVICE_OK;
}
```

**Design decision TS-D4 (command queue, not direct call):** FD
time-push uses `modbus_poller_send_command()` and waits on a
response queue. This matches the ModbusPoller companion §4 command
protocol: TimeService posts the command (non-blocking), then
blocks on its own response queue with a 5-second timeout.
`ModbusPoller` drains the command in its next poll cycle and posts
the result. This ensures the Modbus bus is serialised through a
single gatekeeper (P1).

**Trigger:** FD push runs after every successful NTP sync.
REQ-TS-030 also requires a push "on initial connection". The
initial-connection case is satisfied by the boot-time sync (TS-D3),
which fires as soon as `TimeServiceTask` runs. If the Modbus link
is not yet up at boot sync time, `modbus_poller_send_command()`
returns a non-fatal error and the push retries on the next periodic
sync. Whether an explicit "link-established" event-driven push is
needed is tracked as **TS-O1**.

---

## 8. Sync state and unsynchronised flag (REQ-TS-040)

`sync_state` is a `volatile time_sync_state_t` field in the
singleton struct, written by `TimeServiceTask` and read by any task
via `get_sync_state()`. `volatile` guarantees visibility across
tasks on single-core Cortex-M4; no mutex is needed for a single-word
enum read (TS-D8).

```c
static time_sync_state_t prv_get_sync_state(void *ctx)
{
    (void)ctx;
    return s_ts.sync_state;
}
```

`SensorService` and `CloudPublisher` read `sync_state` via the
`itime_service_t` vtable to decide whether to flag readings and
payloads as "unsynchronised" (REQ-NF-212, REQ-TS-040). They read
it at the point of use (pull-based, P7); `TimeService` does not
push the state.

---

## 9. SD trace

| SD | Step | Component role | Key call |
|---|---|---|---|
| SD-09 | 2 | `TimeService` queries NTP servers | `ntp_client_sync(servers, count, &new_utc)` |
| SD-09 | 3–4 | `TimeService` applies delta sanity check | `time_provider_get_utc(&current)` → compare |
| SD-09 | 8 | `TimeService` writes synchronised time to RTC | `time_provider_set_utc(new_utc)` |
| SD-09 | 10 | `TimeService` pushes time to FD via ModbusPoller command queue | `modbus_poller_send_command(&cmd)` |
| SD-09 | 15 | `TimeService` marks sync state | `s_ts.sync_state = TIME_SYNC_OK` |
| SD-09 | (trigger) | `CloudPublisher` triggers retry on MQTT reconnect | `ts->trigger_retry(ts->ctx)` → posts `TS_NOTIFY_BIT_RETRY` |

---

## 10. Error and fault behaviour

| Error | Severity | Recovery |
|---|---|---|
| `TIME_SERVICE_ERR_NTP_FAILED` | Warning | `sync_state` → `STALE` (or stays `NEVER`). Retries on next periodic tick or `trigger_retry`. |
| `TIME_SERVICE_ERR_DELTA_REJECTED` | Warning | `sync_state` → `STALE`. Retries on next tick. Logs rejected delta for diagnosis. |
| `TIME_SERVICE_ERR_RTC_WRITE` | Warning | `sync_state` still set to `OK` (NTP itself succeeded). RTC retains last value. Timestamps degrade to stale-but-consistent (TS-D7). |
| `TIME_SERVICE_ERR_FD_PUSH` | Warning | Non-fatal. FD uses uptime-based timestamps until next successful push. Logged but does not affect sync_state. |
| `TIME_SERVICE_ERR_NULL_ARG` | Error | Init rejects; service stays uninitialised. |
| `TIME_SERVICE_ERR_NOT_INIT` | Error | Any public call before init returns this immediately. |

All errors logged at point of detection with severity and
context tag. No error is fatal — `TimeServiceTask` continues
and retries on the next tick. `prv_do_ntp_sync()` return value
reflects the most significant error (NTP > delta > RTC write);
FD push failure is logged inside `prv_push_time_to_fd()` and
does not affect the sync return value (TS-D9).

---

## 11. Concurrency

| State | Writer | Reader(s) | Protection |
|---|---|---|---|
| `sync_state` | `TimeServiceTask` | `CloudPublisherTask`, `SensorTask`, `TimeServiceTask` | `volatile` — single word, Cortex-M4 single-cycle read (TS-D8) |
| All other `s_ts` fields | `TimeServiceTask` only | — | None needed — single-writer ownership |
| `trigger_retry()` cross-task call | `CloudPublisherTask` | — | `xTaskNotify` — atomic, non-blocking |
| `s_fd_resp_queue` | `ModbusPollerTask` (send) | `TimeServiceTask` (receive) | FreeRTOS queue — thread-safe by design |

---

## 12. File layout

```
firmware/gateway/application/time_service/
├── time_service.h            /* itime_service_t vtable, time_sync_state_t,
                                 time_service_err_t, init, get_interface,
                                 reset_for_test */
└── time_service.c            /* implementation, singleton state, task entry,
                                 vtable, NTP sync algorithm, FD push */
```

TimeService is Gateway-only, so the task entry point lives inside
`time_service.c` (TS-D10). No separate task wrapper file — unlike
common modules that need per-board task files.

---

## 13. Initialisation

```c
time_service_err_t time_service_init(void)
{
    if (s_ts.initialised)
    {
        return TIME_SERVICE_OK;  /* idempotent */
    }

    s_ts.sync_state = TIME_SYNC_NEVER;

    /* Create response queue for FD time-push commands */
    s_fd_resp_queue = xQueueCreateStatic(
        1U, sizeof(modbus_command_response_t),
        s_fd_resp_buf, &s_fd_resp_queue_storage);

    /* Create periodic NTP timer (auto-reload, default 1 tick;
     * actual period set when task starts) */
    s_ts.ntp_timer = xTimerCreateStatic(
        "ntp", pdMS_TO_TICKS(1000U), pdTRUE,
        NULL, ntp_timer_cb, &s_timer_storage);

    /* Create TimeServiceTask */
    s_ts.task_handle = xTaskCreateStatic(
        time_service_task, "TS",
        768U,               /* stack words — per task-breakdown.md */
        NULL, 2U,           /* priority 2 — per task-breakdown.md  */
        s_task_stack, &s_task_tcb);

    /* Populate vtable */
    s_vtable.ctx            = &s_ts;
    s_vtable.get_sync_state = prv_get_sync_state;
    s_vtable.trigger_retry  = prv_trigger_retry;

    s_ts.initialised = true;
    return TIME_SERVICE_OK;
}

const itime_service_t *time_service_get_interface(void)
{
    if (!s_ts.initialised)
    {
        return NULL;
    }
    return &s_vtable;
}
```

### 13.1 Reset for test

```c
#ifdef TEST
void time_service_reset_for_test(void)
{
    (void)memset(&s_ts, 0, sizeof(s_ts));
    (void)memset(&s_vtable, 0, sizeof(s_vtable));
    s_fd_resp_queue = NULL;
}
#endif
```

---

## 14. Memory and sizing

| Item | Size |
|---|---|
| Singleton struct `s_ts` | ~24 B |
| `itime_service_t` vtable | ~12 B |
| FD response queue (storage + buffer) | ~sizeof(StaticQueue_t) + sizeof(modbus_command_response_t) ≈ 120 B |
| Timer storage (StaticTimer_t) | ~44 B |
| Task TCB (StaticTask_t) | ~92 B |
| Task stack (768 words) | 3072 B |
| **Total static RAM** | **~3364 B** (of which 3072 B is task stack) |

Stack: 768 words / 3 KB (per `task-breakdown.md` §TimeServiceTask).
NTP sync involves a blocking call into `NtpClient`, which routes UDP
through `WifiTask`'s API. The deepest call frame is:
`time_service_task` → `prv_do_ntp_sync` → `ntp_client_sync` (blocks
on queue to WifiTask). The FD push path adds
`prv_push_time_to_fd` → `modbus_poller_send_command` (non-blocking
post) → `xQueueReceive` (blocks on response). Runtime measurement
with `uxTaskGetStackHighWaterMark()` will refine the estimate.

---

## 15. Unit-test plan

### 15.1 Unit tests — `tests/gateway/app/test_time_service.c`

Mocks (CMock, link-time replacement): `time_provider`, `ntp_client`,
`modbus_poller`, `config_provider`, `logger`, FreeRTOS kernel stubs.

| ID | Test case | Verified behaviour |
|---|---|---|
| **TC-TS-001** | Init succeeds | `initialised = true`; `sync_state = TIME_SYNC_NEVER`; task and timer created |
| **TC-TS-002** | Init is idempotent | Second call returns `TIME_SERVICE_OK` without recreating task |
| **TC-TS-003** | `get_interface()` before init | Returns `NULL` |
| **TC-TS-004** | `get_interface()` after init | Returns non-NULL pointer; vtable functions non-NULL |
| **TC-TS-005** | First boot sync — NTP success | `ntp_client_sync` returns OK; delta check bypassed (`NEVER` state); `time_provider_set_utc` called with correct UTC; `sync_state = TIME_SYNC_OK`; FD push command posted |
| **TC-TS-006** | First boot sync — NTP failure | `ntp_client_sync` returns error; `sync_state` stays `TIME_SYNC_NEVER`; FD push not attempted |
| **TC-TS-007** | Subsequent sync — delta within threshold | NTP returns UTC within 3600 s of current; accepted; `sync_state = TIME_SYNC_OK` |
| **TC-TS-008** | Subsequent sync — delta exactly at threshold | NTP delta == `NTP_MAX_DELTA_S`; accepted (threshold is strict >) |
| **TC-TS-009** | Subsequent sync — delta exceeds threshold | NTP delta > `NTP_MAX_DELTA_S`; rejected; `sync_state = TIME_SYNC_STALE`; FD push not attempted |
| **TC-TS-010** | RTC write failure after successful NTP | `time_provider_set_utc` returns error; `sync_state` still set to `TIME_SYNC_OK`; returns `TIME_SERVICE_ERR_RTC_WRITE` |
| **TC-TS-011** | FD push — command rejected | `modbus_poller_send_command` returns `QUEUE_FULL`; error logged; `sync_state = TIME_SYNC_OK` retained; `prv_do_ntp_sync()` still returns `TIME_SERVICE_OK` |
| **TC-TS-012** | FD push — response timeout | Response queue receive times out (5 s); error logged; `sync_state = TIME_SYNC_OK` retained |
| **TC-TS-013** | FD push — Modbus error in response | Response received with non-OK status; error logged; non-fatal |
| **TC-TS-014** | FD push — correct register encoding | UTC = `0x12345678`; verify `cmd.data[0] = 0x1234`, `cmd.data[1] = 0x5678` |
| **TC-TS-015** | `trigger_retry()` posts notification | Call `prv_trigger_retry()`; verify `xTaskNotify` called with `TS_NOTIFY_BIT_RETRY` and `eSetBits` |
| **TC-TS-016** | `get_sync_state()` returns current state | After init → `NEVER`; after successful sync → `OK`; after failed sync from `OK` → `STALE` |
| **TC-TS-017** | Configurable interval | `config_provider_get_ntp_interval_s()` returns 7200; verify `xTimerChangePeriod` called with `pdMS_TO_TICKS(7200000)` |
| **TC-TS-018** | NTP failure from OK state → STALE | `sync_state` was `OK`; NTP fails; verify `sync_state = TIME_SYNC_STALE` (not `NEVER`) |
| **TC-TS-019** | Reset for test | After reset: `initialised = false`, `sync_state = TIME_SYNC_NEVER`, vtable zeroed |

Additions in v1.1 (all PASS): **TC-TS-020** init rejects a NULL NtpClient handle ·
**TC-TS-021** delta check skipped while TimeProvider is UNSYNCHRONISED (TS-D12) ·
**TC-TS-022** delta check skipped when the RTC cannot be read (TS-D12) ·
**TC-TS-023** FD command shape: FC16, start register 0x0210, two registers, response queue set ·
**TC-TS-024** boot sequence syncs, then arms and starts the timer ·
**TC-TS-025** periodic and retry notifications each trigger a sync, an empty word does not ·
**TC-TS-026** timer callback posts the periodic bit. Host result: 25 tests, 25 pass,
0 ignored (`tests/gateway/application/time_service/test_time_service.c`,
`tests/project_gateway.yml`).

### 15.2 Integration tests — on target

| ID | Test case | Setup | Pass criteria |
|---|---|---|---|
| **TC-HW-TS-001..005** | Bench harness `integration-tests/time_service`, 2026-10-24 | WiFi up, boot sync reaches OK without a caller, TimeProvider SYNCHRONISED and the epoch agrees with the host clock, one FC16 command to 0x0210 whose words reassemble to the RTC epoch, `trigger_retry()` causes a second sync. The Modbus bus is stubbed in the harness. | **PASS** (all five; first attempt failed at WiFi association, passed on reset) |
| **TC-TS-I01** | Boot sync | Power on with WiFi connected | `sync_state = TIME_SYNC_OK` within 10 s; FD timestamp register matches NTP time ±2 s |
| **TC-TS-I02** | NTP retry on reconnect | Disconnect WiFi at boot; verify `TIME_SYNC_NEVER`; reconnect WiFi; `trigger_retry` fires via CloudPublisher | `sync_state = TIME_SYNC_OK` within 15 s of reconnect |
| **TC-TS-I03** | FD timestamp propagation | After sync; read FD timestamp holding registers via debug Modbus tool | Matches GW RTC within 2 s |
| **TC-TS-I04** | Delta rejection | Inject a spoofed NTP response >1 hour in the future (test NTP server stub) | Rejected; `sync_state = TIME_SYNC_STALE`; RTC value unchanged |
| **TC-TS-I05** | Periodic re-sync | Let system run past one NTP interval (1 hour default, or shortened for test) | Second sync observed in log; `sync_state` refreshed |

---

## 16. Open items

| ID | Item | Resolution path | Status |
|---|---|---|---|
| **TS-O1** | REQ-TS-030 "on initial connection" — currently satisfied by boot-time NTP sync and periodic retries. If the Modbus link is not up at boot sync time, the push is deferred. Evaluate at integration whether an explicit event-driven push on `link_established` is needed. | Integration test TC-TS-I03 | Open |
| **TS-O2** | `IConfigProvider` is consumed for NTP interval and server list but not listed in `components.md` §TimeService USES. Update `components.md` to add `IConfigProvider` to TimeService's dependency list. | HLD update | Open |
| **TS-O3** | *(Closed for the Gateway, v1.1.)* DNS dependency: if `WifiDriver` does not expose DNS resolution, `NtpClient` must be provisioned with NTP server IP addresses instead of hostnames. Confirm DNS availability during WifiDriver LLD phase. | The ISM43362 has no resolver; servers are IPv4 literals. | Closed |
| **TS-O4** | No `IConfigProvider` on the Gateway: NTP interval (3600 s) and server list are compile-time defaults in `time_service.c`. Same gap as SS-O6; TS-O2 (HLD USES) goes with it. | Replace the two seams (`s_ntp_servers`, `s_ts.interval_s`) when the Gateway gets a config provider. | Open |
| **TS-O5** | The Field Device has no `CMD_SET_TIME` handler yet: the register pair is specified (`modbus-register-map.md` §6.5, TC-RM-033) but the FD firmware does not implement it, so the pushed write will be rejected until it does. Verify GW push and FD handler together when both boards run (TC-TS-I01, I03). | FD register map + `modbus_register_map` change; two-board test at the end of the project. | Open |
| **TS-O6** | No real Gateway `main()` instantiates TimeService, and nothing calls `trigger_retry()` yet: CloudPublisher does not hold the interface. `time_service.c` is compiled into the image but unreferenced. | Wire when the Gateway entry point exists; CloudPublisher takes `itime_service_t` for REQ-TS-0E1. | Open |

---

## 17. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| **TS-D1** | Singleton pattern for internal state (`s_ts`). | Consistent with ModbusPoller and other GW Application singletons. Single NTP sync service per gateway. CMock link-time replacement for testability. |
| **TS-D2** | `time_service_init()` takes no explicit dependency parameters. Dependencies consumed via their singleton free functions. | Matches ModbusPoller pattern. Simpler API. CMock replaces each dependency `.c` at link time for unit testing. |
| **TS-D3** | Boot sync runs as first action in task entry (no one-shot timer). | Simpler than a separate boot timer. If WiFi is not ready, sync fails gracefully and retries on next periodic tick or `trigger_retry`. |
| **TS-D4** | FD time-push uses `modbus_poller_send_command()` and waits on a response queue with 5 s timeout. | Consistent with ModbusPoller companion §4 command protocol. Ensures Modbus bus access serialised through single gatekeeper (P1). |
| **TS-D5** | `NTP_MAX_DELTA_S = 3600` (1 hour). Compile-time constant. First sync (`NEVER`) bypasses the check. | Sanity-check threshold protects against wildly wrong NTP responses. 1 hour is generous for normal RTC drift. First sync must accept any value since RTC holds arbitrary post-reset data. |
| **TS-D6** | NTP interval read from `IConfigProvider` at each periodic tick. | Runtime-configurable sync period. Changes take effect on the next cycle without restart. |
| **TS-D7** | RTC write failure is non-fatal (`sync_state` still set to `OK`). | NTP query succeeded — correct time is known. RTC hardware rarely fails. If it does, timestamps degrade to stale-but-consistent. Retrying on next sync is sufficient. |
| **TS-D8** | `sync_state` protected by `volatile`, not mutex. | Single-word enum on single-core Cortex-M4. `volatile` guarantees visibility. Mutex adds overhead with no safety benefit for atomic single-word reads. |
| **TS-D9** | FD push failure does not affect `prv_do_ntp_sync()` return value. | FD push is a follow-up action, not part of the core sync. Logged and retried on next sync. |
| **TS-D10** | Task entry point lives inside `time_service.c`, not a separate task file. | TimeService is Gateway-only. No per-board task files needed (cf. common modules with per-board wrappers). |
| **TS-D11** | The three-state flag is `time_service_sync_t` (`TIME_SERVICE_SYNC_NEVER / _OK / _STALE`). | `time_sync_state_t` is already TimeProvider's two-state enum and is in use elsewhere; reusing the name would redefine it. |
| **TS-D12** | The delta check is skipped when `time_provider_get()` fails or reports UNSYNCHRONISED. | In both cases the RTC holds uptime or an arbitrary value, so it is no reference. v1.0 would have compared against 0 and rejected a valid NTP time. |
| **TS-D13** | Interval and NTP servers are compile-time defaults until an `IConfigProvider` exists (TS-O4). | Matches SS-O6; keeps the USES list inside `components.md`. |
| **TS-D14** | FD time is pushed as one FC16 write of the two-word `CMD_SET_TIME` block at 0x0210. | The 32-bit value must arrive as a unit; the register map previously had no time register and forbade multi-register FC16 in the command block, so §6.5 now documents the exception. |

---

## 18. Principles

**P1 (Strict directional layering).** `TimeService` (Application)
depends downward on `TimeProvider` and `NtpClient` (Middleware) via
their singleton free-function APIs. FD time-write is routed through
`ModbusPoller` (`modbus_poller_send_command`) — `TimeService` never
calls `ModbusMaster` directly. All dependency arrows point at the
same layer or downward.

**P2 (Dependency Inversion).** Exposes `itime_service_t` vtable
so that consumers (`CloudPublisher`, `SensorService`) depend on an
abstraction, not on `time_service.c` directly. Downward dependencies
on `TimeProvider` and `NtpClient` are concrete free-function calls
— correct direction, no DIP violation.

**P3 (Interface Segregation).** The `itime_service_t` vtable
exposes only `get_sync_state` and `trigger_retry` — the two
operations consumers need. Init, sync algorithm, FD push, and all
internal state are hidden behind the compilation unit boundary.

**P4 (Cross-cutting concern exception).** `Logger` referenced
concretely via `logger_log()` per the cross-cutting convention.

**P5 (Bounded resources, no dynamic allocation post-init).** Static
singleton struct, static task stack and TCB, static timer storage,
static response queue storage. NTP query uses a stack-allocated
48-byte packet inside `NtpClient`. No heap allocation at any point
(REQ-NF-201).

**P6 (Responsibility traces to requirements).** `prv_do_ntp_sync()`
traces to REQ-TS-000, REQ-TS-010, REQ-TS-020. `prv_push_time_to_fd()`
traces to REQ-TS-030. `sync_state` traces to REQ-TS-040.
`prv_trigger_retry()` traces to REQ-TS-0E1.

**P7 (Pull-based data access).** Consumers read `sync_state` via
`get_sync_state()` at the point of use. `TimeService` does not
push the state to consumers.

**P10 (Naming conventions).** Prefix `time_service_`; interface
`itime_service_t`; error enum `time_service_err_t` with values
`TIME_SERVICE_OK`, `TIME_SERVICE_ERR_*`; private helpers prefixed
`prv_`.

All functions that can fail return `time_service_err_t`. Errors
are logged at the point of detection with severity, tag, and
context. No error is silently swallowed.

BARR-C:2018 applies throughout. Fixed-width integers (`uint32_t`,
`uint16_t`, `uint8_t`). `const` on read-only pointer parameters.
Braces on all control structures. No dynamic allocation post-init.
No floating-point in this module — all values are integer Unix
timestamps. Doxygen-style comments on all public API functions.

---

## 19. References

- `docs/components.md` — GW TimeService entry
- `docs/sequence-diagrams.md` — SD-09 (time synchronisation flow)
- `docs/state-machines.md` — Machine 2 (Cloud Connectivity —
  `internet_restored` event triggers REQ-TS-0E1)
- `docs/modbus-register-map.md` — §6.4 (timestamp holding registers)
- `docs/lld/modbus-poller-lld.md` — ModbusPoller command protocol §4
- `docs/lld/ntp-client-lld.md` — NtpClient interface
- `docs/lld/time-provider-lld.md` — TimeProvider interface
- `docs/architecture-principles.md` — P1, P2, P3, P4, P5, P6, P7, P10

---

## Phase H checklist

| ID | Gate criterion | Evidence |
|---|---|---|
| **H-01** | Every SRS requirement in §2 has a code-level trace | §9 SD trace maps each REQ-TS-* to implementation function |
| **H-02** | Public API types and signatures frozen | §3 — `itime_service_t`, `time_sync_state_t`, `time_service_err_t`, init, get_interface |
| **H-03** | Internal state struct defined | §4.1 singleton struct with all fields |
| **H-04** | Error enum covers every detectable failure | §3.2 — 7 values including OK |
| **H-05** | Concurrency analysis complete | §11 — `volatile` for sync_state; single-writer for all other fields; `xTaskNotify` for cross-task retry; FreeRTOS queue for FD response |
| **H-06** | Every test case has a TC-NNN identifier | §15 — TC-TS-001..019 (unit), TC-TS-I01..I05 (integration) |
| **H-07** | File layout matches project structure | §12 — `firmware/gateway/application/time_service/` |
| **H-08** | `_reset_for_test()` present with `#ifdef TEST` | §3.4 declaration, §13.1 implementation |
| **H-09** | Design decisions logged with rationale | §17 — TS-D1..D10 |
| **H-10** | Open items tracked with IDs | §16 — TS-O1..O3 |
| **H-11** | Principles section uses correct P-numbering from `architecture-principles.md` | §18 — P1, P2, P3, P4, P5, P6, P7, P10 |
| **H-12** | BARR-C compliance noted | §18 final paragraphs |
| **H-13** | Consumed interface signatures listed | §1.2 — TimeProvider, NtpClient, ModbusPoller, IConfigProvider, Logger |
| **H-14** | Stack and memory sizing documented | §14 — 768 words / 3 KB stack, ~3364 B total static RAM |

---

*Companion produced during the LLD Application Phase. Authored by
Luca Agrippino; reviewed against the V-Model gate criteria for LLD.*
