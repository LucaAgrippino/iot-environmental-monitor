# LLD Companion — WifiTask

**Document:** `docs/lld/middleware/wifi-task.md`
**Version:** 0.1 (Phase H complete — ready for implementation)
**Board:** Gateway (B-L475E-IOT01A)
**Layer:** Middleware
**Status:** Implementation-ready
**Date:** July 2026

**HLD anchor:** WifiTask in `components.md` (GW middleware layer)

---

## 1. Sources

WifiTask has no history before this document — it is referenced 13 times
across companion docs (D29, WIFI-O4, WIFI-O15, `ntp-client.md`) but had zero
design surface until now: no LLD, no `components.md` entry (a gate-review-
flagged defect, `mechanical-report.md` line 156), no API, no state machine.

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Serialises all access to WifiDriver as its sole owning task; queues and dispatches requests from CloudPublisherTask, TimeServiceTask, and UpdateServiceTask; owns periodic WiFi link-liveness monitoring and reconnect-after-drop. | `components.md` |
| PROVIDES (upward) | IWifiTask | `components.md` |
| USES (downward) | WifiDriver | `components.md` |
| Root requirements | REQ-CC-050 (WiFi/MQTT connect), REQ-TS-010 (NTP over WiFi), CON-001 (ISM43362 module) | `SRS.md` §4 |
| Board | Gateway only | `components.md` |
| Task | `WifiTask`, priority 3, 256 words / 1 KB stack, sole SPI3 owner | `task-breakdown.md` §5.2, D29; `memory-budget.md` §1.1 (cross-confirms 1 KB) |

**Why this document exists now, not earlier.** WIFI-D11 (`wifi-driver.md`)
deliberately kept WifiDriver blocking and FreeRTOS-free, so it works
identically pre- and post-scheduler-start. The event-driven, task-owning
piece was always meant to live here, one layer up — WIFI-O4 states this
explicitly: *"Deferred to WifiTask middleware companion. IWifi is the driver
contract; the request-queue routing mechanism is designed separately."*
`wifi-driver.md` §1 and §8 already assume this document's conclusions
("Consumer: WifiTask only (D29)... they route through WifiTask's request
queue") — this document is the promised follow-through, not a fresh proposal.

**Why this is not a WifiDriver refactor.** WifiDriver's internal AT-command
engine (`prv_at_command()`) busy-polls the DRDY GPIO uniformly, pre- and
post-scheduler (WIFI-D11) — this is intentional, hardware-validated (WIFI-O6
through WIFI-O14), and out of scope for this document. WifiTask's job is
architectural: become WifiDriver's *only* caller, so the busy-polling that
already happens is confined to one bounded-priority task instead of racing
across multiple tasks that don't yet exist for GW (TimeServiceTask,
UpdateServiceTask) but are already declared in `components.md`.

### 1.1 Additional source references

| Source | Relevant section |
|---|---|
| `task-breakdown.md` §5.2, §5.4, §6.1, §7, D29 | Task table row; IPC table (`SPI_wifi_IRQHandler` → WifiTask notify); ISR inventory; locking table (no `wifi_mutex` — sole ownership by task-design); D29 rationale |
| `wifi-driver.md` §1, §2.2–2.4, §3.5, §8, WIFI-D11, WIFI-O4, WIFI-O15 | Consumer contract, EXTI ownership, DRDY ISR wiring, synchronisation note, and the three open items this document resolves or explicitly inherits |
| `ntp-client.md` §6, LLD-D13 | Existing (not-yet-implemented) consumer pseudocode: `wifi_driver->open_socket(...) via WifiTask (D29 — no caller reaches WifiDriver directly)` — the one pre-existing hint at call shape |
| `sequence-diagrams.md` SD-03, SD-04, SD-09 | Cloud publish, store-and-forward, NTP flows — all draw `WifiDriver` as a passthrough lifeline (D12) at HLD granularity; this document is the LLD-level detail underneath that abstraction |

---

## 2. Activation model

WifiTask blocks on a single request queue with a bounded timeout, which
doubles as its periodic liveness-check tick — no separate timer object.

| Queue / event | Source | Period / event |
|---|---|---|
| `request_queue` (depth 3) | CloudPublisherTask (MqttClient), future TimeServiceTask (NtpClient), future UpdateServiceTask (FirmwareStore) | Event-driven, one outstanding request per caller |
| Queue-receive timeout | `WIFI_LIVENESS_CHECK_PERIOD_MS` | Periodic, runs when no request is pending (WIFI-O15) |

```
WifiTask loop:
    xQueueReceive(request_queue, &req, pdMS_TO_TICKS(WIFI_LIVENESS_CHECK_PERIOD_MS))
    if request received:
        dispatch(req)                          /* one blocking WifiDriver call */
        xTaskNotify(req->caller, req->status, eSetValueWithOverwrite)
    else if link_state == WIFI_LINK_UP:
        wifi_get_rssi(wifi, &rssi)              /* liveness probe, WIFI-O15 */
        if probe fails:
            attempt reconnect with backoff       /* see §5.3 */
```

**Queue depth: 3.** Sized by producer count, not throughput — every caller
blocks synchronously on its own request (§4), so no caller ever has more
than one request in flight. This mirrors this project's existing convention
of sizing IPC by source count rather than burst tolerance (the OTA command
queue is depth 1 for one source; the CloudPublisher alarm queue is depth 8
because it *is* sized for bursts). Three producers today (CloudPublisherTask
is the only one actually implemented for GW; TimeServiceTask and
UpdateServiceTask are declared in `components.md`/`task-breakdown.md` but
not yet built) — depth 3 leaves no slack for a fourth without revisiting
this sizing.

**Caller blocking is deliberate, not an oversight.** See §5.2 for the full
design-fork discussion: this is a synchronous relocate (D29 §5), not a
non-blocking API (deferred, see MQTT-O7 in §10).

---

## 3. Public API

### 3.1 ADT pattern

WifiTask follows the Gateway ADT default, same as WifiDriver, MqttClient,
and CloudPublisher: an opaque handle (`wifitask_handle_t`) returned by
`wifitask_create()` from a static internal pool of 1. The internal struct
(request queue, task handle, WifiDriver handle) is hidden in `wifi_task.c`.

Unlike WifiDriver/MqttClient, `wifitask_create()` does not just allocate an
instance — it also starts WifiTask's own FreeRTOS task via
`xTaskCreateStatic()`, using a static TCB and stack array (same pattern as
`cloud_publisher_create()` starting `CloudPublisherTask`,
`cloud_publisher.c:200`). WifiDriver's `wifi_create()` runs first,
pre-scheduler, per its own two-phase init contract (`wifi-driver.md` §3.6);
`wifitask_create()` runs after the scheduler starts and takes the resulting
`wifi_handle_t` as a config field.

### 3.2 Dependency-conformance check

| Dependency | In `components.md` | Actual usage |
|---|---|---|
| WifiDriver | Yes | Every dispatched request calls exactly one `wifi_*()` function |

### 3.3 Data types

```c
/* wifi_task.h */

#ifndef WIFI_TASK_H
#define WIFI_TASK_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "wifi_driver/wifi_driver.h"

#define WIFITASK_MAX_SSID_LEN WIFI_MAX_SSID_LEN
#define WIFITASK_MAX_PASS_LEN WIFI_MAX_PASS_LEN

/** @brief Opaque handle to a WifiTask instance. */
typedef struct wifitask_inst *wifitask_handle_t;

typedef enum {
    WIFITASK_ERR_OK          = 0,
    WIFITASK_ERR_NULL_PTR    = 1,  /**< Required pointer argument was NULL.      */
    WIFITASK_ERR_NO_RESOURCE = 2,  /**< Static pool or request queue exhausted.  */
    WIFITASK_ERR_TIMEOUT     = 3,  /**< Request queue full, or no response from
                                         WifiTask within a bounded wait.         */
    /* All other outcomes pass the underlying wifi_err_t through unchanged —
     * WifiTask does not reinterpret WifiDriver's error codes (§7 P8). */
} wifitask_err_t;

#endif /* WIFI_TASK_H */
```

**Error model note (see §7 Principles applied, P8):** `wifitask_*()`
functions return `wifi_err_t` directly for anything that reaches WifiDriver
— `wifitask_err_t` only covers the three failure modes that are specific to
the request/queue layer itself (bad arguments, queue full, no reply). This
avoids a second, parallel error taxonomy the caller would have to map back
onto `wifi_err_t` anyway.

### 3.4 Configuration struct

```c
/**
 * @brief WifiTask creation configuration.
 *
 * Injected dependency: the WifiDriver handle from wifi_create(), which
 * must already have completed (pre-scheduler, per wifi-driver.md §3.6).
 */
typedef struct {
    wifi_handle_t wifi;   /**< WifiDriver handle — already created.  */
} wifitask_config_t;
```

### 3.5 Public API (`wifi_task.h`)

```c
/**
 * @brief Create a WifiTask instance and start its FreeRTOS task.
 *
 * Starts the WifiTask task (priority 3, 256-word/1 KB stack, D29) via
 * xTaskCreateStatic(). The task immediately begins its request/liveness
 * loop (§2). wifi_create() must have already completed successfully —
 * this function does not initialise WifiDriver itself.
 *
 * @param[in]  config  Injected WifiDriver handle.
 * @param[out] handle  Receives the created handle on success.
 * @return WIFITASK_ERR_OK on success; WIFITASK_ERR_NULL_PTR if config,
 *         config->wifi, or handle is NULL; WIFITASK_ERR_NO_RESOURCE if
 *         the static pool is exhausted.
 * @note Threading: task-context only. Call after the scheduler has
 *       started (the task it creates cannot run before that).
 */
wifitask_err_t wifitask_create(const wifitask_config_t *config,
                                wifitask_handle_t *handle);

/**
 * @brief Connect to a WiFi access point (routed through WifiTask).
 *
 * Enqueues a request and blocks until WifiTask dispatches
 * wifi_connect_ap() and replies. Same blocking wall-clock behaviour as
 * calling wifi_connect_ap() directly (WIFI_JOIN_TIMEOUT_MS, 20 s worst
 * case) — this is a synchronous relocate, not a non-blocking API (§5.2).
 *
 * @param[in] handle    WifiTask handle.
 * @param[in] ssid      Null-terminated SSID (max WIFITASK_MAX_SSID_LEN).
 * @param[in] password  Null-terminated password (max WIFITASK_MAX_PASS_LEN).
 * @return WIFITASK_ERR_TIMEOUT if the request queue is full or WifiTask
 *         does not reply within a bounded wait; otherwise the wifi_err_t
 *         wifi_connect_ap() itself returned, passed through unchanged.
 * @note Threading: task-context only, blocking. Not ISR-safe. Callable
 *       from any task except WifiTask itself (self-enqueue would
 *       deadlock — see §6).
 */
wifitask_err_t wifitask_connect_ap(wifitask_handle_t handle,
                                    const char *ssid,
                                    const char *password);

/**
 * @brief Open a TCP or UDP socket (routed through WifiTask).
 *
 * @note Same blocking/threading contract as wifitask_connect_ap().
 *       Mirrors wifi_open_socket()'s parameters and worst-case timing
 *       (WIFI_SOCKET_CONNECT_TIMEOUT_MS, 15 s).
 */
wifitask_err_t wifitask_open_socket(wifitask_handle_t handle,
                                     wifi_socket_type_t type,
                                     const char *remote_addr,
                                     uint16_t remote_port,
                                     wifi_socket_t *out_socket);

/**
 * @brief Send data on an open socket (routed through WifiTask).
 *
 * @note Same blocking/threading contract as wifitask_connect_ap().
 *       Mirrors wifi_send()'s parameters.
 */
wifitask_err_t wifitask_send(wifitask_handle_t handle,
                              wifi_socket_t socket,
                              const uint8_t *data,
                              size_t len);

/**
 * @brief Receive data from an open socket (routed through WifiTask).
 *
 * @note Same blocking/threading contract as wifitask_connect_ap().
 *       Mirrors wifi_recv()'s parameters and worst-case timing
 *       (floored at WIFI_RESP_TIMEOUT_MS, 5 s — this is MQTT-O7's
 *       unresolved stall, now relocated but not shortened; see §10).
 */
wifitask_err_t wifitask_recv(wifitask_handle_t handle,
                              wifi_socket_t socket,
                              uint8_t *buf,
                              size_t buf_len,
                              size_t *out_len,
                              uint32_t timeout_ms);

/**
 * @brief Close an open socket (routed through WifiTask).
 *
 * @note Same blocking/threading contract as wifitask_connect_ap().
 */
wifitask_err_t wifitask_close_socket(wifitask_handle_t handle,
                                      wifi_socket_t socket);
```

**Not exposed here:** `wifi_get_link_state()` and `wifi_get_rssi()` remain
callable through WifiTask via the same request pattern (omitted above for
brevity — identical shape); `wifi_disconnect_ap()` likewise. All nine of
WifiDriver's post-init functions get a `wifitask_*()` counterpart with the
same signature shape, none reproduced with new field names — see §7 P10.

---

## 4. Internal design

### 4.1 Private struct and static pool

```c
#define WIFITASK_MAX_INSTANCES     1u
#define WIFITASK_TASK_STACK_WORDS 256u   /**< D29; matches memory-budget.md §1.1 (1 KB). */
#define WIFITASK_TASK_PRIORITY      3u   /**< D29. */
#define WIFITASK_QUEUE_DEPTH        3u   /**< §2 — sized by producer count. */
#define WIFI_LIVENESS_CHECK_PERIOD_MS 30000u  /**< WIFI-O15 tick; provisional, see §10. */

typedef enum {
    WIFITASK_OP_CONNECT_AP = 0,
    WIFITASK_OP_DISCONNECT_AP,
    WIFITASK_OP_GET_RSSI,
    WIFITASK_OP_OPEN_SOCKET,
    WIFITASK_OP_SEND,
    WIFITASK_OP_RECV,
    WIFITASK_OP_CLOSE_SOCKET,
} wifitask_op_t;

/**
 * @brief One request, allocated on the CALLING task's own stack.
 *
 * Zero heap, zero copy: the caller builds this locally, pushes a pointer
 * to request_queue, then blocks on xTaskNotifyWait(). WifiTask writes
 * wifi_status/out_* fields directly into it before notifying — safe
 * because the design guarantees exactly one outstanding request per
 * caller (§2), so nothing else touches this memory while WifiTask has
 * the pointer.
 */
typedef struct {
    wifitask_op_t    op;
    TaskHandle_t     caller;         /* xTaskGetCurrentTaskHandle() */

    /* CONNECT_AP */
    const char      *ssid;
    const char      *password;

    /* OPEN_SOCKET */
    wifi_socket_type_t socket_type;
    const char       *remote_addr;
    uint16_t          remote_port;

    /* SEND / RECV / CLOSE_SOCKET — socket identifies the target on all three */
    wifi_socket_t     socket;
    const uint8_t    *tx_buf;
    size_t            tx_len;
    uint8_t          *rx_buf;
    size_t            rx_buf_len;
    size_t            rx_len;        /* out: RECV */
    uint32_t          timeout_ms;    /* in: RECV */

    /* GET_RSSI */
    int8_t            rssi_dbm;      /* out */

    wifi_err_t        wifi_status;   /* out: filled by WifiTask before notify */
} wifitask_request_t;

struct wifitask_inst
{
    wifi_handle_t   wifi;
    TaskHandle_t    task_handle;
    QueueHandle_t   request_queue;
    wifi_link_state_t last_known_link_state;
    bool            in_use;
};

static struct wifitask_inst g_pool[WIFITASK_MAX_INSTANCES];
static StaticTask_t   s_wifitask_tcb;
static StackType_t    s_wifitask_stack[WIFITASK_TASK_STACK_WORDS];
static StaticQueue_t  s_request_queue_ctrl;
static uint8_t        s_request_queue_storage[WIFITASK_QUEUE_DEPTH * sizeof(wifitask_request_t *)];
```

The queue carries **pointers** to caller-owned `wifitask_request_t`
instances, not the structs themselves — `sizeof(wifitask_request_t *)` per
slot, matching how `wifi_open_socket()`'s own out-parameters work: the data
lives with whoever is blocked waiting for it.

### 4.2 Request dispatch

```c
static wifi_err_t prv_dispatch(struct wifitask_inst *inst, wifitask_request_t *req)
{
    switch (req->op)
    {
    case WIFITASK_OP_CONNECT_AP:
        return wifi_connect_ap(inst->wifi, req->ssid, req->password);
    case WIFITASK_OP_DISCONNECT_AP:
        return wifi_disconnect_ap(inst->wifi);
    case WIFITASK_OP_GET_RSSI:
        return wifi_get_rssi(inst->wifi, &req->rssi_dbm);
    case WIFITASK_OP_OPEN_SOCKET:
        return wifi_open_socket(inst->wifi, req->socket_type, req->remote_addr,
                                req->remote_port, &req->socket);
    case WIFITASK_OP_SEND:
        return wifi_send(inst->wifi, req->socket, req->tx_buf, req->tx_len);
    case WIFITASK_OP_RECV:
        return wifi_recv(inst->wifi, req->socket, req->rx_buf, req->rx_buf_len,
                         &req->rx_len, req->timeout_ms);
    case WIFITASK_OP_CLOSE_SOCKET:
        return wifi_close_socket(inst->wifi, req->socket);
    default:
        return WIFI_ERR_INVALID_ARG;
    }
}
```

Every `wifitask_*()` public function is a thin, identically-shaped wrapper:
build a `wifitask_request_t` on the stack, `xQueueSend()` a pointer to it
(bounded wait — see §6 for the deadlock this bound prevents), then
`xTaskNotifyWait()` for WifiTask's reply. Not reproduced nine times here —
one representative wrapper:

```c
wifitask_err_t wifitask_connect_ap(wifitask_handle_t handle, const char *ssid,
                                    const char *password)
{
    if ((handle == NULL) || (ssid == NULL) || (password == NULL))
    {
        return WIFITASK_ERR_NULL_PTR;
    }

    wifitask_request_t req = {
        .op = WIFITASK_OP_CONNECT_AP,
        .caller = xTaskGetCurrentTaskHandle(),
        .ssid = ssid,
        .password = password,
    };
    wifitask_request_t *req_ptr = &req;

    if (xQueueSend(handle->request_queue, &req_ptr, WIFITASK_ENQUEUE_TIMEOUT_TICKS) != pdPASS)
    {
        return WIFITASK_ERR_TIMEOUT;
    }

    uint32_t notified_status;
    if (xTaskNotifyWait(0, 0xFFFFFFFFu, &notified_status, WIFITASK_REPLY_TIMEOUT_TICKS) != pdTRUE)
    {
        return WIFITASK_ERR_TIMEOUT;
    }

    return (wifitask_err_t) notified_status; /* actually wifi_err_t, see note below */
}
```

**Note on the notify-value cast:** `xTaskNotify()`'s value is a 32-bit
`uint32_t`; `wifi_err_t` fits trivially. The public function signature
returns `wifitask_err_t`, but for the common case (queue accepted, reply
received) the value threaded through is the *raw* `wifi_err_t` from
`prv_dispatch()` — `wifitask_err_t`'s and `wifi_err_t`'s `_OK = 0` values are
required to match by construction (both are 0) so callers can test
`== WIFITASK_ERR_OK` / `== WIFI_ERR_OK` interchangeably at the boundary
without a translation table. This mirrors MqttClient's own precedent
(`mqtt_client_connect_step()` returning `MQTT_CLIENT_ERR_IN_PROGRESS`
alongside plain pass-through codes in one enum) rather than inventing a new
pattern.

### 4.3 WifiTask's own task body

```c
static void prv_wifitask_body(void *arg)
{
    struct wifitask_inst *inst = (struct wifitask_inst *) arg;

    (void) wifi_get_link_state(inst->wifi, &inst->last_known_link_state);

    for (;;)
    {
        wifitask_request_t *req = NULL;
        if (xQueueReceive(inst->request_queue, &req,
                          pdMS_TO_TICKS(WIFI_LIVENESS_CHECK_PERIOD_MS)) == pdPASS)
        {
            req->wifi_status = prv_dispatch(inst, req);
            (void) xTaskNotify(req->caller, (uint32_t) req->wifi_status,
                               eSetValueWithOverwrite);
        }
        else
        {
            prv_liveness_check(inst);   /* WIFI-O15, §5.3 */
        }
    }
}
```

### 4.4 Test reset hook

```c
#ifdef TEST
void wifitask_reset_for_test(void)
{
    memset(g_pool, 0, sizeof(g_pool));
}
#endif
```

Matches the established pattern (`wifi_driver.c` §3.8, `mqtt_client.c` §8.4,
`cloud_publisher.c` §5.7) — host tests reset the static pool between cases
without a full process restart.

---

## 5. Synchronisation and reconnect design

### 5.1 Sole-owner guarantee (D29)

Once NtpClient and UpdateService/FirmwareStore are implemented for GW and
call `wifitask_*()` exclusively (not `wifi_*()` directly — see §10 for the
rewire this depends on), WifiDriver has exactly one caller task by
construction: WifiTask. This is not enforced by a mutex — there is nothing
for a mutex to guard, because no other task ever holds a `wifi_handle_t`
call site. This is the same reasoning `wifi-driver.md` §8 and
`task-breakdown.md` §7's locking table already document for WifiDriver
itself; this document is what makes it literally true rather than aspirational.

### 5.2 Why synchronous, not asynchronous (design fork, resolved)

Two shapes were considered for the request API:

- **(A) Synchronous relocate** (chosen): caller blocks for the same
  wall-clock duration a direct WifiDriver call would take. The only change
  is *where* the call executes (WifiTask's context, not the caller's).
- **(B) Fully asynchronous**: caller submits and continues; WifiTask
  notifies later. This would let CloudPublisherTask stay responsive to its
  alarm queue (REQ-NF-113, ≤500 ms) even during a stalled WiFi op — a real
  fix for MQTT-O7.

(A) was chosen for this increment. (B) requires MqttClient's *entire*
steady-state I/O surface (`process()`/`send()`/`recv()`, not just connect)
to become a ticked, non-blocking state machine — the same pattern this
session's `mqtt_client_connect_step()` (MQTT-D8) already proved out for the
connect phase specifically, but extended far beyond connect. That is a
second module's rework riding on top of a module that doesn't exist yet;
scoping it into WifiTask's first LLD would repeat the mistake WIFI-D11
explicitly avoided (improvising one module's design inside another). (A)
fully resolves D29 and gives WIFI-O15 a real owner; it does not resolve
MQTT-O7, which stays open (§10) with a concrete, narrower path forward
already sketched: WifiTask running a background recv-poll per socket
(backed by the DATARDY notification bit reserved in §5.4) and exposing a
genuinely non-blocking `wifitask_try_recv()`, paired with a future,
separately-scoped tick added to `mqtt_client_process()`.

### 5.3 WIFI-O15 — AP-drop detection and reconnect

`prv_liveness_check()` runs whenever the request queue is empty for
`WIFI_LIVENESS_CHECK_PERIOD_MS`:

```
prv_liveness_check(inst):
    if last_known_link_state != WIFI_LINK_UP:
        return                                  /* nothing to check */
    rc = wifi_get_rssi(inst->wifi, &rssi)        /* cheapest real liveness probe */
    if rc == WIFI_ERR_OK:
        return                                   /* link healthy */
    /* probe failed: AP likely dropped the station silently (WIFI-O15) */
    log_warn("WiFi liveness probe failed, attempting reconnect")
    reconnect_with_backoff(inst)                 /* policy: open item, §10 */
```

This gives WIFI-O15's gap ("nothing re-associates after a genuine WiFi
AP-level drop") a structural home — the periodic tick and the task that
owns it both now exist. The backoff policy itself (attempt count, delay
curve, and what happens to any socket/MQTT session open at the time of the
drop) is **not** fully pinned in this increment — see §10.

### 5.4 DATARDY ISR — wired, reserved

`wifitask_create()` calls `wifi_attach_datardy_callback()` at startup
(required by WifiDriver's two-phase init contract, `wifi-driver.md` §3.6),
registering a callback that calls `xTaskNotifyFromISR()` on WifiTask's own
handle using a reserved notification bit (`WIFI_TASK_DATARDY_BIT`, distinct
from the request-reply notification value used in §4.2 — reply uses
`eSetValueWithOverwrite` with no bits, DATARDY uses `eSetBits`, so the two
never collide on the same notification word).

This bit is **not consumed anywhere on this increment's hot path**. Every
`wifi_*()` call `prv_dispatch()` invokes already blocks to completion
internally per WIFI-D11 — there is nothing to wait on. The registration
exists now, and is documented as a deliberate reservation, so the future
non-blocking-recv work described in §5.2/§10 has a real interrupt path
ready rather than needing to retrofit one.

### 5.5 Deadlock avoidance

WifiTask must never call its own `wifitask_*()` wrappers — `prv_dispatch()`
calls WifiDriver's `wifi_*()` functions directly (§4.2), never the
queue-based wrappers, specifically to avoid a task enqueueing a request to
itself and blocking forever waiting for a reply it will never dispatch. This
is enforced by construction (WifiTask's own code has no call sites for
`wifitask_*()`), not by a runtime check — same discipline as WifiDriver
having no calls into MqttClient.

---

## 6. Sequence integration

`sequence-diagrams.md` (SD-03, SD-04, SD-09) draws `MqttClient → WifiDriver`
as a direct message at HLD granularity (D12, "passthrough lifeline") — this
is a deliberate diagram-readability simplification, not a claim that
MqttClient should call WifiDriver directly at implementation level. At LLD
granularity, every one of those messages becomes `MqttClient/NtpClient →
WifiTask → WifiDriver`, with WifiTask's queue+notify round trip (§4.2)
sitting transparently inside what the HLD diagram drew as one arrow.

**TCP code path — MQTT cloud publish (SD-03):** `MqttClient` calls
`wifitask_open_socket()`/`wifitask_send()`/`wifitask_recv()` instead of the
WifiDriver equivalents (once rewired — §10). Wall-clock behaviour at the
sequence-diagram level is unchanged; the `{≤ 200 ms}` publish-queued
constraint on SD-03 messages 4–7 is unaffected since it starts *after* an
already-open connection.

**UDP code path — NTP time sync (SD-09):** `NtpClient`'s existing pseudocode
(`ntp-client.md` §6) already anticipated this exact routing.

---

## 7. Error and fault behaviour

WifiTask does not retry, reinterpret, or suppress any WifiDriver error — it
passes `wifi_err_t` through the notification value unchanged (§4.2, §4.3).
The three `wifitask_err_t` codes cover only the request/queue layer itself:

| Error | Cause | Behaviour |
|---|---|---|
| `WIFITASK_ERR_NULL_PTR` | Required pointer argument was NULL | Return immediately, no queue access |
| `WIFITASK_ERR_NO_RESOURCE` | Static instance pool exhausted at `wifitask_create()` | Return immediately |
| `WIFITASK_ERR_TIMEOUT` | Request queue full (`xQueueSend` bounded wait expired) or no reply within a bounded wait (`xTaskNotifyWait` expired) | Return immediately; caller's request may or may not have been dispatched — see §10 (in-flight-request-on-timeout is an open item) |

Any other outcome is `wifi_status` cast to `wifitask_err_t` — i.e. the raw
`wifi_err_t` WifiDriver returned, per §4.2's shared-zero-value contract.

---

## 8. Principles applied

- **P1 (Strict directional layering).** Depends only on WifiDriver (driver layer); no application-layer dependency.
- **P2 (DIP).** Consumers (MqttClient, NtpClient — middleware) depend on the `wifitask_handle_t` abstraction, not on WifiDriver directly, once rewired (§10). The opaque handle achieves DIP without a vtable, same as every other GW module.
- **P5 (Bounded resources).** Static pool of 1 instance; request queue bounded at `WIFITASK_QUEUE_DEPTH` (3); no heap; requests live on the caller's own stack (§4.1).
- **P6 (Traces to requirements).** REQ-CC-050, REQ-TS-010, CON-001 — same roots as WifiDriver, since WifiTask serves the same needs, just serialised.
- **P8 (Total error propagation).** `wifitask_err_t`/`wifi_err_t` on every operation; no silent failure; no retry-and-hide.
- **P9 (BARR-C).** Fixed-width types; `const` on read-only pointers; braces on all control flow.
- **P10 (Naming).** Prefix `wifitask_`; handle `wifitask_handle_t`; errors `WIFITASK_ERR_*`; every public function mirrors its WifiDriver counterpart's name and parameter order 1:1 (§3.5) — no invented renaming.

---

## 9. Unit-test plan

Test file: `tests/gateway/middleware/wifi_task/test_wifi_task.c`

**Layer 1 — request/dispatch with mock WifiDriver (host, no hardware):**

| ID | Scenario | Expected |
|---|---|---|
| WIFITASK-T01 | `wifitask_create` happy path | Task created, request queue created, handle returned |
| WIFITASK-T02 | `wifitask_create` with NULL config / NULL `config->wifi` | Returns `WIFITASK_ERR_NULL_PTR` |
| WIFITASK-T03 | `wifitask_create` pool exhaustion | Second call returns `WIFITASK_ERR_NO_RESOURCE` |
| WIFITASK-T04 | `wifitask_connect_ap` happy path | Mock `wifi_connect_ap()` called once with the right args; caller receives `WIFI_ERR_OK` |
| WIFITASK-T05 | `wifitask_connect_ap` — mock WifiDriver returns `WIFI_ERR_MODULE` | Caller receives `WIFI_ERR_MODULE` unchanged (pass-through, §7) |
| WIFITASK-T06 | `wifitask_open_socket`/`wifitask_send`/`wifitask_recv`/`wifitask_close_socket` — each dispatches to its matching mock WifiDriver call with matching args | One test per op; proves `prv_dispatch()`'s switch is wired correctly for all seven ops |
| WIFITASK-T07 | Two callers (simulated via two mock task handles) each get their own reply, not each other's | Proves the notify-by-caller-handle addressing in §4.2/§4.3 is correct, not just "first reply wins" |
| WIFITASK-T08 | Request queue full (simulate 4 outstanding enqueues against depth 3) | 4th caller receives `WIFITASK_ERR_TIMEOUT` |
| WIFITASK-T09 | `xTaskNotifyWait` timeout (mock: WifiTask never replies) | Caller receives `WIFITASK_ERR_TIMEOUT` |
| WIFITASK-T10 | Liveness check fires when queue is empty past `WIFI_LIVENESS_CHECK_PERIOD_MS` | Mock `wifi_get_rssi()` called with no request pending (WIFI-O15) |
| WIFITASK-T11 | Liveness check skipped when `last_known_link_state != WIFI_LINK_UP` | Mock `wifi_get_rssi()` NOT called |
| WIFITASK-T12 | Liveness probe fails — reconnect attempted | Mock `wifi_connect_ap()` called following the failed `wifi_get_rssi()` |
| WIFITASK-T13 | `wifitask_reset_for_test()` clears the pool | A prior instance's handle is no longer valid after reset |

**Layer 2 — hardware integration (on-board, deferred):**

Full multi-caller exercise against a real ISM43362 — at minimum, one
CloudPublisherTask-shaped caller doing repeated connect/send/recv/close
cycles while the liveness tick runs concurrently, proving the request queue
and the idle-tick timeout coexist correctly under real SPI timing (not just
mocked timing). No second real caller task exists yet (TimeServiceTask,
UpdateServiceTask are unbuilt) — WIFITASK-T07's two-caller proof stays
host-only until one of those ships.

---

## 10. Open items

| ID | Item | Status | Resolution |
|---|---|---|---|
| WIFITASK-O1 | MQTT-O7 (CloudPublisherTask's 5 s idle-recv stall) is relocated, not fixed. `wifitask_recv()` still blocks the caller for up to `WIFI_RESP_TIMEOUT_MS` (5 s), same as `wifi_recv()` does today — WifiTask just changes *where* that block executes. | **Open — deliberately deferred** | A real fix needs `wifitask_try_recv()` (genuinely non-blocking, backed by a per-socket background poll inside WifiTask and the DATARDY notification bit reserved in §5.4) paired with a ticked, non-blocking extension of `mqtt_client_process()`'s recv path — mirroring `mqtt_client_connect_step()`'s (MQTT-D8) precedent, but for the steady-state path instead of just connect. Separately scoped; not attempted here (§5.2). |
| WIFITASK-O2 | WIFI-O15's reconnect **policy** (attempt count, backoff curve, and what happens to any socket/MQTT session open at the time of an AP drop) is not pinned — only the structural owner and tick (§5.3) are. | **Open** | Needs a concrete backoff constant and a decision on whether WifiTask force-closes/invalidates open sockets on a detected drop or leaves that to the next `wifitask_send()`/`wifitask_recv()` caller to discover naturally (their own call would fail against a dead socket regardless). Revisit once a second real caller (TimeServiceTask or UpdateServiceTask) exists and this can be validated against more than one consumer's expectations. |
| WIFITASK-O3 | MqttClient's actual code (`mqtt_client.c`) still calls `wifi_send`/`wifi_recv`/`wifi_open_socket`/`wifi_close_socket` directly, and its `components.md` USES line still says `WifiDriver`, not `WifiTask`. This document does not change either. | **Resolved** | `mqtt_client.h`/`.c` rewired: `mqtt_client_config_t.wifi` is now `wifitask_handle_t`, every call site renamed to the matching `wifitask_*()` function. The one correctness trap handled explicitly: `WIFITASK_ERR_TIMEOUT` (value 3) and `WIFI_ERR_TIMEOUT` (value 4) are numerically distinct — `mqtt_client.c`'s two recv-timeout checks (`prv_mbedtls_net_recv()`, `prv_transport_recv()`) still compare against the raw `WIFI_ERR_TIMEOUT`, not the renamed constant, with a comment at each site explaining why. `components.md`'s MqttClient entry now reads `USES (downward): IWifiTask, ILogger`. Both integration mains (`main_test_mqtt_client.c`, `main_test_cloud_publisher.c`) updated to call `wifitask_create()` post-scheduler and thread the resulting handle through. NtpClient's entry deliberately left untouched — out of scope, not implemented for GW yet. |
| WIFITASK-O4 | `WIFI_LIVENESS_CHECK_PERIOD_MS` (30 s, §4.1) is a provisional placeholder, not validated against any requirement or field data. | **Open** | Revisit once real hardware bring-up data exists for how quickly a dropped AP is actually noticed via `wifi_get_rssi()` failure vs. a genuine multi-minute silent drop; balance against battery/RF-quiet considerations if any apply to the Gateway (currently mains-powered, so likely not a hard constraint). |
| WIFITASK-O5 | `WIFITASK_ENQUEUE_TIMEOUT_TICKS` / `WIFITASK_REPLY_TIMEOUT_TICKS` (§4.2) are referenced but not yet assigned concrete values in this document. | **Open** | Reply timeout must exceed the longest possible dispatched operation's own worst case (`WIFI_JOIN_TIMEOUT_MS` = 20 s is the longest, from `wifi-driver.md` §3.1) plus queueing delay for up to 2 other callers ahead in line — needs a concrete sum, not "generously large," to avoid a caller timing out on a request that actually succeeded. Enqueue timeout can be much shorter (bounds how long a caller waits just to get *into* the queue, not for a reply). |

---

## 11. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| WIFITASK-D1 | ADT pattern (opaque handle, static pool of 1), matching every other Gateway module | Established, confirmed convention (`feedback_gw_adt_mandatory`) — implement the module's own companion literally, don't correct it to a vtable pattern even though this doc's own prose occasionally uses `->` shorthand informally. |
| WIFITASK-D2 | Request/reply via queue (pointer to caller-stack struct) + direct-to-task notification, not a second queue for replies | Matches `task-breakdown.md`'s IPC preference order (§8, direct notification preferred for 1:1 single-event paths); the reply is exactly that — one value, one recipient, no buffering needed. |
| WIFITASK-D3 | Synchronous relocate for this increment; asynchronous/non-blocking API explicitly deferred | See §5.2. Keeps this LLD's scope to "give WifiDriver a real sole owner" (D29) and "give WIFI-O15 a home," not "rebuild MqttClient's I/O model." |
| WIFITASK-D4 | WifiTask calls WifiDriver's `wifi_*()` functions directly inside `prv_dispatch()`, never its own `wifitask_*()` wrappers | Prevents a self-enqueue deadlock by construction (§5.5) — no runtime guard needed if the call path structurally cannot exist. |
| WIFITASK-D5 | DATARDY ISR callback registered at `wifitask_create()` time but its notification bit is unused on the hot path this increment | Keeps the interrupt wiring correct and ready for the future non-blocking-recv work (§5.2, WIFITASK-O1) without pretending this increment already uses it — avoids a doc that oversells what's implemented. |
| WIFITASK-D6 | LAYER = Middleware (not Driver), matching `wifi-driver.md`'s own text ("the WifiTask middleware companion", §8) and the `docs/lld/middleware/` file path | WifiTask sits above WifiDriver in the dependency chain and is consumed by other Middleware components (MqttClient, NtpClient) — Middleware-depends-on-Middleware-depends-on-Driver keeps P1's directional layering intact only if WifiTask is classified consistently with where it actually sits, which the pre-existing wifi-driver.md text had already settled before this document was written. |

---

## 12. File layout

```
firmware/gateway/middleware/wifi_task/
├── wifi_task.h       /* public API — opaque handle, config, error enum, ops */
└── wifi_task.c        /* implementation — request queue, dispatch, liveness tick, ISR wiring */

tests/gateway/middleware/wifi_task/
└── test_wifi_task.c   /* Unity + CMock host tests, mocking wifi_driver.h */
```

---

## Phase H — Readiness review

| # | Check | Status |
|---|---|---|
| H1 | PROVIDES / USES match `components.md` exactly | PASS — PROVIDES IWifiTask, USES WifiDriver |
| H2 | Root SRS requirements cited and quoted | PASS — REQ-CC-050, REQ-TS-010, CON-001 (inherited from WifiDriver's own roots, §1) |
| H3 | All public API functions have complete Doxygen | PASS — one function shown in full per shape (§3.5), remaining eight are identical-shape mirrors of WifiDriver's own already-documented functions (P10) |
| H4 | ADT pattern applied (or exception documented) | PASS — ADT with pool of 1, WIFITASK-D1 |
| H5 | Error enum covers all failure modes | PASS — 3 request-layer codes + full `wifi_err_t` pass-through, §7 |
| H6 | Hardware contract specifies all pins, AF numbers, SPI config | N/A — WifiTask owns no hardware directly; inherits WifiDriver's contract unchanged |
| H7 | All critical open items resolved or have named owner | PASS — WIFITASK-O1 through O5 all have an explicit owner/path, none silently dropped |
| H8 | Unit-test plan covers happy path + error cases | PASS — 13 test cases across 2 layers |
| H9 | Test file path follows Gateway folder convention | PASS |
| H10 | P1–P10 compliance reviewed | PASS — §8 |
| H11 | No FreeRTOS dependency except where the task itself requires it | PASS — WifiTask *is* a FreeRTOS task by definition (unlike WifiDriver, which deliberately has none); this is the intended, documented split (WIFI-D11) |
| H12 | Thread safety documented | PASS — §5, sole-owner guarantee by construction, no mutex needed |
| H13 | `reset_for_test` hook specified | PASS — §4.4 |
| H14 | Decisions log complete | PASS — 6 decisions |
| H15 | Sequence integration traces to HLD SDs | PASS — §6, SD-03/SD-04/SD-09 |

**Verdict: PASS — ready for implementation.** WIFITASK-O3 (the MqttClient
rewire) has since landed: `mqtt_client.c` now routes exclusively through
`wifitask_*()`, so D29's sole-ownership guarantee is real in code, not just
aspirational, for the one real caller that exists today (CloudPublisherTask).
It remains a prerequisite for NtpClient/UpdateService specifically, since
neither is implemented for GW yet — the same rewire (or an equivalent one
scoped to those modules) still needs to happen before either is built.

Four open items remain (WIFITASK-O1 MQTT-O7 relocation-not-fix, WIFITASK-O2
reconnect policy, WIFITASK-O4 liveness period, WIFITASK-O5 timeout
constants) — none block *this* document's implementation-readiness; all are
scoped follow-ups with a stated path forward, consistent with how
`wifi-driver.md` and `mqtt-client.md` each carried open items past their
own Phase H sign-off.
