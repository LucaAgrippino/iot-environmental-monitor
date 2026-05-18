# LLD Companion — NtpClient

**Layer:** Middleware  
**Board:** Gateway (GW) only  
**Provides:** `INtpClient`  
**Consumes:** `IWifi` (WifiDriver), `IHealthReport`, `ILogger`  
**SRS traces:** REQ-TS-010  
**HLD ref:** `components.md` §Middleware — NtpClient; `sequence-diagrams.md` SD-09; `hld.md` §6.1
**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** NtpClient in `components.md` (GW middleware layer)

---

## 1. Sources

NtpClient issues a single SNTPv4 request to a configurable server list and
returns the server's time reference to its caller (`TimeService`). It owns
nothing beyond the wire exchange — no RTC write, no sync-state flag, no
retry scheduling. Those concerns belong to `TimeService`.

NtpClient reports a query-failure event via `IHealthReport` when every
server in the list fails to respond. It does not retry within a single
`ntp_client_query()` call; retrying is TimeService's policy (REQ-TS-0E1).

NtpClient is passive — it runs in `TimeServiceTask` context, called
synchronously. No thread of its own.

---

## 2. Protocol — SNTPv4

NtpClient implements the client side of SNTPv4 (RFC 4330 / RFC 5905 §14).
SNTPv4 is a one-shot subset of the full NTP protocol: send a 48-byte UDP
request, receive a 48-byte response, extract the Transmit Timestamp. No
clock discipline, no symmetric mode, no association management.

| Parameter | Value |
|-----------|-------|
| Protocol | UDP |
| Server port | 123 |
| Client port | Ephemeral (allocated by WifiDriver) |
| Packet size | 48 bytes |
| LI/VN/Mode byte (request) | `0x1B` — LI=0, VN=3, Mode=3 (client) |
| Epoch | NTP epoch: seconds since 1 Jan 1900 UTC |
| Unix conversion | subtract `2208988800` (70 years in seconds) |

---

## 3. NTP packet layout

```c
/* ntp_client.c — internal only, not in header */

typedef struct __attribute__((packed)) {
    uint8_t  li_vn_mode;       /* byte 0: LI(2)|VN(3)|Mode(3)           */
    uint8_t  stratum;          /* byte 1: clock stratum                  */
    uint8_t  poll;             /* byte 2: polling interval (log2 s)      */
    int8_t   precision;        /* byte 3: clock precision (log2 s)       */
    uint32_t root_delay;       /* bytes  4-7:  big-endian fixed-point    */
    uint32_t root_dispersion;  /* bytes  8-11: big-endian fixed-point    */
    uint32_t reference_id;     /* bytes 12-15                            */
    uint32_t ref_ts_sec;       /* bytes 16-19: reference timestamp (s)   */
    uint32_t ref_ts_frac;      /* bytes 20-23: reference timestamp (frac)*/
    uint32_t orig_ts_sec;      /* bytes 24-27: originate timestamp       */
    uint32_t orig_ts_frac;     /* bytes 28-31                            */
    uint32_t rx_ts_sec;        /* bytes 32-35: receive timestamp         */
    uint32_t rx_ts_frac;       /* bytes 36-39                            */
    uint32_t tx_ts_sec;        /* bytes 40-43: transmit timestamp (s)    */
    uint32_t tx_ts_frac;       /* bytes 44-47: transmit timestamp (frac) */
} ntp_packet_t;
```

The client sends a zeroed 48-byte packet with `li_vn_mode = 0x1B`. The
server fills all fields in the response. Only `tx_ts_sec` is used — it is
the server's best estimate of the current time at the moment the response
was sent, which is sufficient for the accuracy requirements of this system
(REQ-TS-010 does not mandate sub-second precision).

All multi-byte fields in the NTP packet are **big-endian** on the wire. The
Cortex-M4 is little-endian; byte-swap `tx_ts_sec` before use.

---

## 4. Data types

```c
/* ntp_client.h */

typedef enum {
    NTP_CLIENT_ERR_OK          = 0,
    NTP_CLIENT_ERR_NOT_INIT    = 1,
    NTP_CLIENT_ERR_NULL_ARG    = 2,
    NTP_CLIENT_ERR_ALL_FAILED  = 3,   /* no server in list responded       */
    NTP_CLIENT_ERR_BAD_RESPONSE= 4,   /* response failed sanity check      */
    NTP_CLIENT_ERR_WIFI        = 5,   /* WifiDriver returned error         */
} ntp_client_err_t;

#define NTP_CLIENT_MAX_SERVERS   4U   /* maximum entries in server list    */
#define NTP_CLIENT_QUERY_TIMEOUT_MS  3000U   /* per-server timeout (NTP-O1) */
#define NTP_UNIX_OFFSET          2208988800UL  /* NTP epoch → Unix epoch   */
```

---

## 2. Public API — `INtpClient`

```c
/**
 * @brief  Initialise NtpClient.
 *
 * @param  health  IHealthReport handle for failure event push.
 */
ntp_client_err_t ntp_client_init(IHealthReport *health);

/**
 * @brief  Query NTP servers and return a Unix epoch timestamp.
 *
 * Tries each server in server_list in order. On the first successful
 * response, converts the NTP Transmit Timestamp to a Unix epoch value and
 * returns it. If all servers fail (no response within NTP_CLIENT_QUERY_TIMEOUT_MS
 * or bad response), pushes HEALTH_EVENT_NTP_SYNC_FAILED and returns
 * NTP_CLIENT_ERR_ALL_FAILED.
 *
 * Blocking — holds TimeServiceTask for up to
 * server_count × NTP_CLIENT_QUERY_TIMEOUT_MS ms in the worst case.
 *
 * The server list is passed in on each call — NtpClient does not cache it.
 * This allows TimeService to update the list from ConfigService without
 * reinitialising NtpClient.
 *
 * @param  server_list    Array of null-terminated server hostnames or IPs.
 * @param  server_count   Number of entries; must be ≤ NTP_CLIENT_MAX_SERVERS.
 * @param[out] unix_epoch_out  Set to Unix epoch seconds on success.
 */
ntp_client_err_t ntp_client_query(const char * const *server_list,
                                   uint8_t             server_count,
                                   uint32_t           *unix_epoch_out);
```

---

## 6. Query execution — one server attempt

The per-server logic inside `ntp_client_query()`:

```
for each server in server_list:

1. Resolve hostname → IP address via wifi_driver_dns_lookup(server).
   On failure: log, continue to next server.

2. Open UDP socket to server_ip:123 via wifi_driver_udp_open().
   On failure: log, continue.

3. Build request packet:
   memset(&pkt, 0, sizeof(pkt));
   pkt.li_vn_mode = 0x1B;

4. Send packet via wifi_driver_udp_send(&pkt, 48).
   On failure: close socket; log; continue.

5. Receive response via wifi_driver_udp_receive(&pkt, 48,
                                                 NTP_CLIENT_QUERY_TIMEOUT_MS).
   On timeout: close socket; log; continue.

6. Sanity check response (§7).
   On failure: close socket; log; push HEALTH_EVENT_NTP_BAD_RESPONSE; continue.

7. Extract tx_ts_sec = ntohl(pkt.tx_ts_sec).
   Convert: *unix_epoch_out = tx_ts_sec - NTP_UNIX_OFFSET.

8. Close socket via wifi_driver_udp_close().
   Return NTP_CLIENT_ERR_OK.

if all servers exhausted:
    push HEALTH_EVENT_NTP_SYNC_FAILED;
    return NTP_CLIENT_ERR_ALL_FAILED.
```

`ntohl()` performs the big-endian to little-endian byte swap on
`tx_ts_sec`. Use the CMSIS `__REV()` intrinsic or a portable four-byte
swap — do not rely on compiler-specific builtins.

---

## 7. Response sanity checks

These are applied before accepting a timestamp (§6, step 6):

| Check | Condition to reject | Rationale |
|-------|--------------------|-----------| 
| Mode | `(pkt.li_vn_mode & 0x07) != 4` | Response must be Mode 4 (server) |
| Stratum | `pkt.stratum == 0` | Kiss-of-Death packet — server is refusing queries |
| Stratum | `pkt.stratum > 15` | Invalid stratum (16 = unsynchronised) |
| Transmit timestamp | `tx_ts_sec == 0` | Server did not fill the timestamp |
| Unix conversion | `tx_ts_sec < NTP_UNIX_OFFSET` | Result would underflow (timestamp before 1970) |

**NTP delta sanity check is not performed here.** Per `time-provider.md`
§10 (GW), the plausibility check (|new_epoch − current_rtc| >
`TIME_PROVIDER_SANITY_DELTA_S`) is TimeService's responsibility, executed
before calling `time_provider_set_time()`. NtpClient returns the raw
server timestamp without comparing it to the RTC.

---

## 8. IHealthReport events

| Event constant | Trigger |
|----------------|---------|
| `HEALTH_EVENT_NTP_SYNC_FAILED` | All servers exhausted without a valid response |
| `HEALTH_EVENT_NTP_BAD_RESPONSE` | A server responded but failed the sanity check (per attempt) |

Both are direct-push events (Metric Producer Pattern — events → push).
`HEALTH_EVENT_NTP_BAD_RESPONSE` may fire multiple times in one
`ntp_client_query()` call if several servers return invalid responses.

---

## 3. Internal design

```c
/* ntp_client.c */

typedef struct {
    bool          initialised;
    IHealthReport *health;
} NtpClientState;

static NtpClientState s_ntp;
```

No mutex needed — `ntp_client_query()` is called only from
`TimeServiceTask`, never concurrently.

---

## 10. Init ordering

```
wifi_driver_init()      ← driver ready (two-phase init complete; WiFi associated)
ntp_client_init(health) ← registers health handle
```

NtpClient must not be called before the WiFi link is up (the ISM43362 must
have joined the AP). `TimeService` enforces this — it calls
`ntp_client_query()` only from within its boot and periodic sync flow,
which is gated on the Cloud Connectivity state machine reaching Connected.

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

## 6. Error and fault behaviour

Error codes and propagation policy are defined in the Public API section above. All public functions return an error code; callers must not ignore non-OK returns.

## 7. Unit-test plan

```c
#ifdef UNIT_TEST
/* Replace WifiDriver UDP calls with a loopback stub that injects
   synthetic NTP responses or simulates timeouts */
#define wifi_driver_dns_lookup(host, ip_out)       stub_dns_lookup(host, ip_out)
#define wifi_driver_udp_open(ip, port)             stub_udp_open(ip, port)
#define wifi_driver_udp_send(buf, len)             stub_udp_send(buf, len)
#define wifi_driver_udp_receive(buf, len, timeout) stub_udp_receive(buf, len, timeout)
#define wifi_driver_udp_close()                    stub_udp_close()
#endif
```

Minimum test cases:
- Single server responds with valid timestamp → returns `ERR_OK`, correct Unix epoch.
- NTP epoch conversion: `tx_ts_sec = 3913215600` (2024-01-01 00:00:00 UTC) → Unix `1704067200`.
- Server responds with Mode != 4 → sanity check rejects; `HEALTH_EVENT_NTP_BAD_RESPONSE` pushed.
- Server responds with stratum = 0 → rejected.
- Server responds with stratum = 16 → rejected.
- Server responds with `tx_ts_sec = 0` → rejected.
- First server times out, second responds → `ERR_OK` from second server.
- All servers time out → `NTP_CLIENT_ERR_ALL_FAILED`; `HEALTH_EVENT_NTP_SYNC_FAILED` pushed exactly once.
- `server_count = 0` → `NTP_CLIENT_ERR_NULL_ARG` (or `ERR_ALL_FAILED` if zero-length list is acceptable — decide at implementation).
- Big-endian byte swap: inject `tx_ts_sec = 0x01020304` on wire → verify `ntohl()` gives `0x04030201` before subtraction.

---

## 8. Open items

| ID     | Item |
|--------|------|
| NTP-O1 | `NTP_CLIENT_QUERY_TIMEOUT_MS = 3000` is provisional. Validate against worst-case WiFi + internet RTT during integration. Increase to 5000 ms if public NTP servers prove slow. |
| NTP-O2 | DNS resolution via WifiDriver — confirm that `wifi_driver_dns_lookup()` exists in the WifiDriver API surface (WIFI-O4 from session summary). If not, NtpClient must accept IP addresses only and DNS resolution must be done above (TimeService or ConfigService). |
| NTP-O3 | IPv6 support — SNTPv4 supports IPv6. WifiDriver (ISM43362) is IPv4-only per AT command set. No action needed; document the constraint. |
