# LLD Companion — NtpClient

**Document:** `docs/lld/middleware/ntp-client.md`
**Version:** 0.3 (implemented and hardware-validated 2026-10-13; as-built deviations in NTP-D6)
**Board:** Gateway (B-L475E-IOT01A) only
**Layer:** Middleware
**Status:** Implementation-ready — GW implemented and hardware-validated
**Date:** July 2026

**HLD anchor:** NtpClient in `components.md` (GW middleware layer)

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Queries NTP servers and returns time references. Reports NTP query failure events. | `components.md` |
| PROVIDES (upward) | INtpClient | `components.md` |
| USES (downward) | WifiDriver (reached through WifiTask — NTP-D6), IHealthReport, ILogger | `components.md` |
| Root requirement | REQ-TS-010 | `SRS.md` |
| HLD refs | `sequence-diagrams.md` SD-09; `hld.md` §6.1 | HLD |

NtpClient issues a single SNTPv4 request to a configurable server list
and returns the server's time reference to its caller (`TimeService`).
It owns nothing beyond the wire exchange — no RTC write, no sync-state
flag, no retry scheduling. Those concerns belong to `TimeService`.

NtpClient reports a query-failure event via IHealthReport when every
server in the list fails to respond. It does not retry within a single
`ntp_client_query()` call; retrying is TimeService's policy.

NtpClient is passive — it runs in TimeServiceTask context, called
synchronously. No thread of its own.

---

## 2. Protocol — SNTPv4

NtpClient implements the client side of SNTPv4 (RFC 4330 / RFC 5905 §14).
SNTPv4 is a one-shot subset of NTP: send a 48-byte UDP request, receive
a 48-byte response, extract the Transmit Timestamp.

| Parameter | Value |
|---|---|
| Protocol | UDP |
| Server port | 123 |
| Client port | Ephemeral (allocated by WifiDriver) |
| Packet size | 48 bytes |
| LI/VN/Mode byte (request) | `0x1B` — LI=0, VN=3, Mode=3 (client) |
| Epoch | NTP: seconds since 1 Jan 1900 UTC |
| Unix conversion | subtract `2208988800` (70 years in seconds) |

---

## 3. NTP packet layout

```c
/* ntp_client.c — internal only, not in header */

typedef struct __attribute__((packed)) {
    uint8_t  li_vn_mode;       /* byte 0: LI(2)|VN(3)|Mode(3)            */
    uint8_t  stratum;          /* byte 1: clock stratum                   */
    uint8_t  poll;             /* byte 2: polling interval (log2 s)       */
    int8_t   precision;        /* byte 3: clock precision (log2 s)        */
    uint32_t root_delay;       /* bytes  4–7:  big-endian fixed-point     */
    uint32_t root_dispersion;  /* bytes  8–11: big-endian fixed-point     */
    uint32_t reference_id;     /* bytes 12–15                             */
    uint32_t ref_ts_sec;       /* bytes 16–19: reference timestamp (s)    */
    uint32_t ref_ts_frac;      /* bytes 20–23: reference timestamp (frac) */
    uint32_t orig_ts_sec;      /* bytes 24–27: originate timestamp        */
    uint32_t orig_ts_frac;     /* bytes 28–31                             */
    uint32_t rx_ts_sec;        /* bytes 32–35: receive timestamp          */
    uint32_t rx_ts_frac;       /* bytes 36–39                             */
    uint32_t tx_ts_sec;        /* bytes 40–43: transmit timestamp (s)     */
    uint32_t tx_ts_frac;       /* bytes 44–47: transmit timestamp (frac)  */
} ntp_packet_t;
```

Client sends a zeroed 48-byte packet with `li_vn_mode = 0x1B`. Server
fills all fields. Only `tx_ts_sec` is used — sufficient for REQ-TS-010
(sub-second precision not required).

All multi-byte fields are **big-endian** on the wire. Cortex-M4 is
little-endian; byte-swap `tx_ts_sec` via `__REV()` intrinsic or a
portable four-byte swap.

---

## 4. Public API

### 4.1 ADT pattern

NtpClient follows the Gateway ADT default: an opaque handle returned by
`ntp_client_create()` from a static pool of 1. Both dependencies
(WifiDriver, IHealthReport) are injected via the config struct.

### 4.2 Data types

```c
/* ntp_client.h */

#ifndef NTP_CLIENT_H
#define NTP_CLIENT_H

#include <stdint.h>
#include <stdbool.h>
#include "health_monitor/health_monitor.h"
#include "wifi_task/wifi_task.h"

/** @brief Opaque handle to an NtpClient instance. */
typedef struct ntp_client_inst *ntp_client_handle_t;

typedef enum {
    NTP_CLIENT_ERR_OK           = 0,
    NTP_CLIENT_ERR_NULL_PTR     = 1,
    NTP_CLIENT_ERR_NO_RESOURCE  = 2,
    NTP_CLIENT_ERR_ALL_FAILED   = 3, /**< No server responded.               */
    NTP_CLIENT_ERR_BAD_RESPONSE = 4, /**< Reserved: a reply failed a sanity check. */
    NTP_CLIENT_ERR_WIFI         = 5, /**< Reserved: WiFi layer returned error. */
    NTP_CLIENT_ERR_INVALID_ARG  = 6, /**< server_count > MAX, or stale handle (NTP-D6). */
} ntp_client_err_t;

#define NTP_CLIENT_MAX_SERVERS        4u
#define NTP_CLIENT_QUERY_TIMEOUT_MS   3000u  /**< Per-server timeout; see NTP-O1. */
#define NTP_UNIX_OFFSET               2208988800ul

/**
 * @brief NtpClient creation configuration.
 */
typedef struct {
    wifitask_handle_t       wifi;    /**< WifiTask handle (for UDP sockets). */
    health_monitor_handle_t health;  /**< IHealthReport handle (for push events). */
} ntp_client_config_t;
```

### 4.3 Public API — INtpClient

```c
/**
 * @brief Create and initialise an NtpClient instance.
 *
 * Stores injected dependencies. Does not perform any network I/O.
 *
 * @param[in]  config  Injected dependencies (WifiDriver + IHealthReport).
 * @param[out] handle  Receives the created handle on success.
 * @return NTP_CLIENT_ERR_OK on success; NTP_CLIENT_ERR_NULL_PTR if
 *         config or handle is NULL; NTP_CLIENT_ERR_NO_RESOURCE if
 *         pool exhausted.
 * @note Threading: task-context only. Call before scheduler starts.
 */
ntp_client_err_t ntp_client_create(const ntp_client_config_t *config,
                                    ntp_client_handle_t *handle);

/**
 * @brief Query NTP servers and return a Unix epoch timestamp.
 *
 * Tries each server in server_list in order. On the first successful
 * response, converts the NTP Transmit Timestamp to a Unix epoch value
 * and returns it via unix_epoch_out.
 *
 * If all servers fail (no response within NTP_CLIENT_QUERY_TIMEOUT_MS
 * or bad response), pushes HEALTH_EVENT_NTP_SYNC_FAILED and returns
 * NTP_CLIENT_ERR_ALL_FAILED.
 *
 * The server list is passed on each call — NtpClient does not cache it.
 * This allows TimeService to update the list from ConfigService without
 * reinitialising NtpClient.
 *
 * NTP servers must be supplied as IPv4 addresses (dotted-decimal).
 * WifiDriver (ISM43362) has no DNS resolver. TimeService / ConfigService
 * must ensure server_list contains IP addresses, not hostnames.
 *
 * @param[in]  handle          NtpClient handle.
 * @param[in]  server_list     Array of null-terminated IPv4 address strings.
 * @param[in]  server_count    Number of entries (≤ NTP_CLIENT_MAX_SERVERS).
 * @param[out] unix_epoch_out  Receives Unix epoch seconds on success.
 * @return NTP_CLIENT_ERR_OK on success; NTP_CLIENT_ERR_ALL_FAILED if
 *         no server responded; NTP_CLIENT_ERR_NULL_PTR if handle,
 *         server_list, or unix_epoch_out is NULL.
 * @note Threading: task-context only, **blocking**. Worst case:
 *       server_count × NTP_CLIENT_QUERY_TIMEOUT_MS. Not ISR-safe.
 */
ntp_client_err_t ntp_client_query(ntp_client_handle_t handle,
                                   const char * const *server_list,
                                   uint8_t             server_count,
                                   uint32_t           *unix_epoch_out);

#endif /* NTP_CLIENT_H */
```

---

## 5. Internal design

### 5.1 Private struct and static pool

```c
/* ntp_client.c */

#define NTP_CLIENT_MAX_INSTANCES  1u

struct ntp_client_inst {
    wifitask_handle_t       wifi;
    health_monitor_handle_t health;
    bool                    in_use;
};

static struct ntp_client_inst g_pool[NTP_CLIENT_MAX_INSTANCES];
static uint8_t                g_count;
```

No mutex — `ntp_client_query()` is called only from TimeServiceTask.

### 5.2 Query execution — per-server logic

```
for each server in server_list:

1. IPv4 addresses required (no DNS on ISM43362).

2. wifitask_open_socket(inst->wifi, WIFI_SOCKET_UDP, server_ip, 123, &sock)
   On failure: log, continue to next server.

3. Build request:
   memset(&pkt, 0, sizeof(pkt));
   pkt.li_vn_mode = 0x1B;

4. wifitask_send(inst->wifi, sock, (uint8_t *)&pkt, 48)
   On failure: wifitask_close_socket(inst->wifi, sock); log; continue.

5. wifitask_recv(inst->wifi, sock, (uint8_t *)&pkt, 48, &rcvd,
             NTP_CLIENT_QUERY_TIMEOUT_MS)
   On timeout or rcvd != 48: close socket; log; continue.

6. Sanity check response (§5.3).
   On failure: close socket; log;
   health_monitor_push_event(inst->health, HEALTH_EVENT_NTP_BAD_RESPONSE, 0);
   continue.

7. tx_ts_sec = read_be32(&pkt_bytes[40])  /* big-endian wire bytes, host-order independent */
   *unix_epoch_out = tx_ts_sec - NTP_UNIX_OFFSET

8. wifitask_close_socket(inst->wifi, sock)
   return NTP_CLIENT_ERR_OK

if all servers exhausted:
    health_monitor_push_event(inst->health, HEALTH_EVENT_NTP_SYNC_FAILED, 0);
    return NTP_CLIENT_ERR_ALL_FAILED
```

### 5.3 Response sanity checks

| Check | Reject if | Rationale |
|---|---|---|
| Mode | `(li_vn_mode & 0x07) != 4` | Response must be Mode 4 (server) |
| Stratum | `stratum == 0` | Kiss-of-Death — server refusing queries |
| Stratum | `stratum > 15` | 16 = unsynchronised |
| Timestamp | `tx_ts_sec == 0` | Server did not fill the timestamp |
| Underflow | `tx_ts_sec < NTP_UNIX_OFFSET` | Unix epoch would underflow |

**Delta sanity check** (|new_epoch − current_rtc|) is TimeService's
responsibility, not NtpClient's. NtpClient returns the raw server
timestamp.

### 5.4 IHealthReport events

| Event | Trigger |
|---|---|
| `HEALTH_EVENT_NTP_SYNC_FAILED` | All servers exhausted without valid response |
| `HEALTH_EVENT_NTP_BAD_RESPONSE` | A server responded but failed sanity check (per attempt). Added to HealthMonitor with this module (value 15; log-only, no snapshot field) |

Both are direct-push events (Metric Producer Pattern — events → push).

### 5.5 Test reset hook

```c
#ifdef TEST
void ntp_client_reset_for_test(void)
{
    memset(g_pool, 0, sizeof(g_pool));
    g_count = 0u;
}
#endif
```

---

## 6. Init ordering

```
wifi_create(...)              ← driver ready
[WiFi associated with AP]    ← gated by LifecycleController
ntp_client_create(...)       ← stores WiFi + IHealthReport handles
[TimeServiceTask created]
ntp_client_query(...)        ← called by TimeService on sync schedule
```

NtpClient must not be called before the WiFi link is up. TimeService
enforces this — it calls `ntp_client_query()` only when the Cloud
Connectivity state machine has reached Connected.

---

## 7. Sequence integration

| SD | Role | Key function |
|---|---|---|
| SD-09 | TimeService calls `ntp_client_query()` — UDP NTP exchange via WifiDriver, returns synchronised Unix timestamp | `ntp_client_query()` |

---

## 8. Error and fault behaviour

| Error | Cause | Behaviour |
|---|---|---|
| `NTP_CLIENT_ERR_NULL_PTR` | Required pointer is NULL | Return immediately |
| `NTP_CLIENT_ERR_NO_RESOURCE` | Pool exhausted | Return immediately |
| `NTP_CLIENT_ERR_ALL_FAILED` | No server responded within timeout | `HEALTH_EVENT_NTP_SYNC_FAILED` pushed; TimeService retries with back-off |
| `NTP_CLIENT_ERR_BAD_RESPONSE` | Sanity check failed | `HEALTH_EVENT_NTP_BAD_RESPONSE` pushed; try next server |
| `NTP_CLIENT_ERR_WIFI` | WifiDriver returned error on socket/send/recv | Log; try next server |

No retry inside NtpClient — TimeService drives the retry policy.

---

## 9. Principles applied

- **P1 (Strict directional layering).** Depends on WifiDriver (driver layer via UDP socket); IHealthReport and ILogger are cross-cutting exceptions (P4).
- **P2 (DIP).** TimeService (application) depends on the opaque `ntp_client_handle_t`, not the concrete implementation.
- **P4 (Cross-cutting exception).** Logger and IHealthReport referenced per convention.
- **P5 (Bounded resources).** Static pool; 48-byte NTP packet on stack; no heap.
- **P6 (Traces to requirements).** Query and parse trace to REQ-TS-010.
- **P7 (Pull-based).** TimeService calls `ntp_client_query()` on its own schedule; NtpClient does not push.
- **P8 (Total error propagation).** 5 distinct error codes; socket failure, timeout, and invalid-stratum each distinguished.
- **P9 (BARR-C).** `uint32_t` for NTP timestamps; explicit integer arithmetic; no floating-point.
- **P10 (Naming).** Prefix `ntp_client_`; handle `ntp_client_handle_t`; errors `NTP_CLIENT_ERR_*`.

---

## 10. Synchronisation

Caller serialises. NtpClient runs exclusively in TimeServiceTask context.
No FreeRTOS synchronisation primitives. `ntp_client_create()` is called
before the scheduler starts; `ntp_client_query()` is called only from
TimeServiceTask.

---

## 11. Unit-test plan

Test file: `tests/gateway/middleware/ntp_client/test_ntp_client.c`

WifiTask and HealthMonitor are replaced by inline spies in the test TU (the repo idiom for Gateway middleware tests, NTP-D6) instead of CMock.

| ID | Scenario | Expected |
|---|---|---|
| NTP-T01 | `ntp_client_create` happy path | Handle returned, deps stored |
| NTP-T02 | `ntp_client_create` NULL config | Returns `ERR_NULL_PTR` |
| NTP-T03 | `ntp_client_create` pool exhaustion | Returns `ERR_NO_RESOURCE` |
| NTP-T04 | Single server, valid response | Returns `ERR_OK`; correct Unix epoch |
| NTP-T05 | NTP epoch conversion: `tx_ts_sec = 3913215600` → Unix `1704226800` | Arithmetic verified |
| NTP-T06 | Response Mode != 4 | Rejected; `HEALTH_EVENT_NTP_BAD_RESPONSE` pushed |
| NTP-T07 | Response stratum = 0 (KoD) | Rejected |
| NTP-T08 | Response stratum = 16 (unsynchronised) | Rejected |
| NTP-T09 | Response `tx_ts_sec = 0` | Rejected |
| NTP-T10 | `tx_ts_sec < NTP_UNIX_OFFSET` (pre-1970) | Rejected |
| NTP-T11 | First server times out, second responds | Returns `ERR_OK` from second |
| NTP-T12 | All servers time out | Returns `ERR_ALL_FAILED`; `HEALTH_EVENT_NTP_SYNC_FAILED` pushed once |
| NTP-T13 | `server_count = 0` | Returns `ERR_NULL_PTR` |
| NTP-T14 | Big-endian read: wire bytes `E9 6B A1 F0` | Epoch `0xE96BA1F0 − NTP_UNIX_OFFSET`; the code reads the four bytes directly, so the result does not depend on host byte order |
| NTP-T15 | NULL `unix_epoch_out` | Returns `ERR_NULL_PTR` |
| NTP-T16 | `wifitask_open_socket` fails on first, succeeds on second | First logged, second proceeds; no close for the failed open |
| NTP-T17 | `create` with NULL handle / NULL dependency handle | `ERR_NULL_PTR`; pool not consumed |
| NTP-T18 | Request packet | 48 bytes, `0x1B` then zeros; UDP, port 123, per-server timeout passed on |
| NTP-T19 | Stratum 15 | Accepted (boundary) |
| NTP-T20 | Bad reply, then a good one | `ERR_OK`; one `BAD_RESPONSE`, no `SYNC_FAILED` |
| NTP-T21 | Send fails | Socket closed; next server tried |
| NTP-T22 | Reply of 47 bytes | A miss, not a bad response: no `BAD_RESPONSE` event |
| NTP-T23 | NULL entry in the server list | Skipped |
| NTP-T24 | NULL handle / NULL list | `ERR_NULL_PTR` |
| NTP-T25 | `server_count` 5 | `ERR_INVALID_ARG` |
| NTP-T26 | Stale handle after reset | `ERR_INVALID_ARG` |

---

## 12. Open items

| ID | Item | Status | Resolution |
|---|---|---|---|
| NTP-O1 | `NTP_CLIENT_QUERY_TIMEOUT_MS = 3000` provisional. Validate against worst-case WiFi + internet RTT. | **Open** | Bench 2026-10-13 (home router): a good server answers in 90–130 ms; an unreachable server costs about 3.1 s. The 3 s default is ample for the working case. Keep open until measured on a slower link. |
| NTP-O4 | `HEALTH_EVENT_NTP_SYNC_OK` is described in the HealthMonitor companion as pushed by NtpClient, but this companion (and `time-service-lld.md`) leave it to TimeService, which alone knows the delta check passed. | **Open** | Decide when TimeService is built; NtpClient pushes only failures today. |
| NTP-O2 | DNS resolution | **Resolved** | ISM43362 has no DNS. Server list must be IPv4 addresses. Documented in §4.3. |
| NTP-O3 | IPv6 support | **Resolved** | ISM43362 is IPv4-only. No action needed. |

---

## 13. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| NTP-D1 | ADT pattern (opaque handle, static pool of 1) | Gateway default. Both deps (WifiDriver, IHealthReport) injected via config struct. |
| NTP-D2 | IPv4 addresses only, no hostname/DNS | ISM43362 has no DNS resolver. Callers must supply dotted-decimal addresses. |
| NTP-D3 | Only `tx_ts_sec` used from response | REQ-TS-010 does not require sub-second precision. Full NTP clock discipline is unnecessary. |
| NTP-D4 | No retry inside NtpClient | TimeService owns the retry policy with exponential back-off. NtpClient is a stateless one-shot query. |
| NTP-D5 | Server list passed per-call, not cached | Allows TimeService to update servers from ConfigService without reinitialising NtpClient. |
| NTP-D6 | As-built deviations: (1) WiFi I/O goes through `wifitask_*` (config field `wifitask_handle_t`), not `wifi_*`: WifiTask is the sole owner of WifiDriver (D29, `task-breakdown.md`) and MqttClient already does the same; the calls mirror WifiDriver's 1:1. (2) `health_monitor_push_event(handle, event, 0)` replaces the abstract `health_report_push_event`; `HEALTH_EVENT_NTP_BAD_RESPONSE` was added to HealthMonitor. (3) `ERR_INVALID_ARG` added for `server_count > 4` and stale handles; `ERR_BAD_RESPONSE` and `ERR_WIFI` stay reserved because every per-server failure moves on to the next server. (4) The timestamp is read byte by byte, not with `ntohl()`. (5) Inline spies instead of CMock. | (1) avoids two tasks driving the ISM43362 SPI channel at once; the rest follow from the real APIs on `main`. |

---

## 14. File layout

```
firmware/gateway/middleware/ntp_client/
├── ntp_client.h    /* public API — handle, config, error enum */
└── ntp_client.c    /* implementation — SNTP query, sanity checks */

tests/gateway/middleware/ntp_client/
└── test_ntp_client.c  /* Unity + CMock */
```

---

## Phase H — Readiness review

| # | Check | Status |
|---|---|---|
| H1 | PROVIDES / USES match `components.md` | PASS — PROVIDES INtpClient; USES WifiDriver, IHealthReport, ILogger |
| H2 | Root SRS requirement cited | PASS — REQ-TS-010 |
| H3 | All public API functions have complete Doxygen | PASS |
| H4 | ADT pattern applied | PASS — NTP-D1 |
| H5 | Error enum covers all failure modes | PASS — 5 error codes |
| H6 | Protocol spec documented (SNTPv4, packet layout, sanity checks) | PASS — §2, §3, §5.3 |
| H7 | Open items resolved or have resolution path | PASS — O2, O3 resolved; O1 deferred to integration |
| H8 | Unit-test plan covers happy + error cases | PASS — 16 test cases |
| H9 | Test file path follows Gateway convention | PASS |
| H10 | P1–P10 compliance reviewed | PASS — §9 |
| H11 | Thread safety documented | PASS — §10, single-task access |
| H12 | `reset_for_test` hook specified | PASS — §5.5 |
| H13 | Decisions log complete | PASS — 5 decisions |
| H14 | Both dependencies injected via config struct | PASS — wifi + health |
| H15 | Hardware bring-up | PASS — TC-HW-NTP-001..006 on the B-L475E-IOT01A, 2026-10-13 (5 of 5 runs after an unexplained one-off association failure on the very first boot) |

**Verdict: PASS — ready for implementation.**
