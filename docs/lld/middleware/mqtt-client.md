# LLD Companion — MqttClient

**Document:** `docs/lld/middleware/mqtt-client.md`
**Version:** 0.2 (Phase H complete — ready for implementation)
**Board:** Gateway (B-L475E-IOT01A) only
**Layer:** Middleware
**Status:** Implementation-ready
**Date:** July 2026

**HLD anchor:** MqttClient in `components.md` (GW middleware layer)

---

## 1. Sources

| Attribute | Value | Source |
|---|---|---|
| Responsibility | Implements the MQTT client protocol over a TLS-secured connection. Maintains connection state and publish/subscribe reliability counters exposed via stats interface. | `components.md` |
| PROVIDES (upward) | IMqttClient, IMqttStats | `components.md` |
| USES (downward) | WifiDriver, ILogger | `components.md` |
| Root requirements | REQ-CC-050, REQ-CC-060 | `SRS.md` |
| Security requirements | REQ-NF-300 (TLS 1.2), REQ-NF-301 (X.509 mutual auth), REQ-NF-302 (no plaintext key logging), REQ-NF-305 (reject unencrypted) | `SRS.md` |
| HLD refs | `state-machines.md` Machine 3; `hld.md` §6.3; `sequence-diagrams.md` SD-03–SD-08 | HLD |

MqttClient owns the MQTT 3.1.1 protocol and the TLS 1.2 session beneath
it. Its two jobs:

1. **Publish:** encode and transmit MQTT PUBLISH frames at QoS 0 or QoS 1
   on behalf of CloudPublisher.
2. **Receive:** process inbound MQTT frames (SUBACK, PUBACK, PINGRESP,
   incoming PUBLISH on subscribed topics) and deliver them to CloudPublisher
   via a registered callback.

MqttClient does **not** own the Cloud Connectivity state machine (Machine 3)
or the reconnect timer — those belong to CloudPublisher. MqttClient emits
a `disconnect_callback` when it detects connection loss; CloudPublisher
reacts by transitioning Machine 3 to Disconnected.

MqttClient runs in `CloudPublisherTask` context. It has no thread of its
own. The `mqtt_client_process()` function is called from CloudPublisherTask's
main loop to drive the receive path and service keep-alive.

---

## 2. Library choice — coreMQTT + mbedTLS

| Concern | Library | Justification |
|---|---|---|
| MQTT protocol | coreMQTT (AWS FreeRTOS, MIT licence) | No dynamic allocation; transport-agnostic; designed for embedded; official AWS IoT recommendation |
| TLS 1.2 | mbedTLS | Widely deployed in embedded; supports X.509 mutual auth; integrates as coreMQTT transport callbacks |
| TCP transport | WifiDriver (IWifi) | ISM43362 AT-over-SPI; WifiDriver provides a plain TCP socket — mbedTLS wraps it |

**Why not ISM43362 native TLS?** Routing X.509 cert/key material through AT
commands (escaping binary in ASCII) is fragile and untestable on the host.
Running mbedTLS on the STM32L475 keeps the TLS stack visible, host-testable,
and under firmware control. The extra RAM cost (~35 KB for mbedTLS context)
is within budget — confirm at MQTT-O1.

---

## 3. TLS configuration

| Parameter | Value | Source |
|---|---|---|
| TLS version | 1.2 minimum | REQ-NF-300 |
| Cipher suites | TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 | AWS IoT Core default |
| Client auth | X.509 mutual auth | REQ-NF-301 |
| Server auth | AWS root CA verified | REQ-NF-305 |
| Certificate storage | Dedicated flash partition (CON-006) | See MQTT-O5 |
| Certificate format | PEM converted to DER at load time | mbedTLS requirement |

The TLS session is established inside `mqtt_client_connect()`. Certificates
are passed via the connect config struct as pointers. MqttClient stores
pointers for the session duration — it does not copy cert material
(REQ-NF-302).

---

## 4. Public API

### 4.1 ADT pattern

MqttClient follows the Gateway ADT default: an opaque handle
(`mqtt_client_handle_t`) is returned by `mqtt_client_create()` from a
static internal pool. Pool size is 1 (single MQTT connection).

Dependencies (WifiDriver handle, Logger) and callbacks (message, disconnect)
are injected via the config struct at creation time.

### 4.2 Data types

```c
/* mqtt_client.h */

#ifndef MQTT_CLIENT_H
#define MQTT_CLIENT_H

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "wifi_driver.h"

/** @brief Opaque handle to an MqttClient instance. */
typedef struct mqtt_client_inst *mqtt_client_handle_t;

typedef enum {
    MQTT_CLIENT_ERR_OK             = 0,
    MQTT_CLIENT_ERR_NOT_INIT       = 1,
    MQTT_CLIENT_ERR_NULL_PTR       = 2,
    MQTT_CLIENT_ERR_NO_RESOURCE    = 3,
    MQTT_CLIENT_ERR_CONNECT_FAIL   = 4,  /**< TLS or MQTT CONNECT rejected.         */
    MQTT_CLIENT_ERR_PUBLISH_FAIL   = 5,  /**< Send error or QoS 1 PUBACK timeout.   */
    MQTT_CLIENT_ERR_NOT_CONNECTED  = 6,
    MQTT_CLIENT_ERR_TLS_FAIL       = 7,
    MQTT_CLIENT_ERR_SUBSCRIBE_FAIL = 8,  /**< SUBACK with failure code.             */
} mqtt_client_err_t;

typedef enum {
    MQTT_QOS_0 = 0,  /**< Fire-and-forget (telemetry, health).  */
    MQTT_QOS_1 = 1,  /**< At-least-once (OTA result, config ACK, alarms). */
} mqtt_qos_t;

/** @brief MQTT connectivity statistics (Metric Producer Pattern — poll). */
typedef struct {
    uint32_t publishes_sent;      /**< Total PUBLISH frames transmitted.        */
    uint32_t publishes_acked;     /**< QoS 1 PUBACKs received.                 */
    uint32_t publish_failures;    /**< Send errors or PUBACK timeouts.          */
    uint32_t connect_attempts;    /**< Total mqtt_client_connect() calls.       */
    uint32_t connect_ok;          /**< Successful connections.                  */
    uint32_t reconnect_count;     /**< Connections after first.                 */
    uint32_t subscribe_failures;  /**< SUBACK with failure code.               */
} mqtt_stats_t;

/**
 * @brief Callback invoked when an inbound PUBLISH arrives on a subscribed
 *        topic.
 *
 * Called from CloudPublisherTask context (inside mqtt_client_process()).
 * The payload pointer is valid only for the duration of the callback.
 */
typedef void (*mqtt_message_cb_t)(const char    *topic,
                                   uint16_t       topic_len,
                                   const uint8_t *payload,
                                   uint32_t       payload_len);

/**
 * @brief Callback invoked when the MQTT connection is lost (keep-alive
 *        timeout, TCP error, or MQTT-level error).
 *
 * Called from CloudPublisherTask context. CloudPublisher transitions
 * Machine 3 to Disconnected on receipt.
 */
typedef void (*mqtt_disconnect_cb_t)(void);

/**
 * @brief MQTT connection parameters.
 *
 * Credentials are loaded by LifecycleController from the cert flash
 * partition at boot and passed as pointers. MqttClient stores
 * pointers for the session duration — it does not copy cert material.
 */
typedef struct {
    const char    *broker_endpoint;  /**< e.g. "xxxxx.iot.eu-west-1.amazonaws.com" */
    uint16_t       broker_port;      /**< 8883 (MQTT over TLS).                    */
    const char    *client_id;        /**< Device serial number.                    */
    const uint8_t *client_cert_der;  /**< DER-encoded client certificate.          */
    uint32_t       client_cert_len;
    const uint8_t *client_key_der;   /**< DER-encoded private key.                 */
    uint32_t       client_key_len;
    const uint8_t *ca_cert_der;      /**< DER-encoded AWS root CA certificate.     */
    uint32_t       ca_cert_len;
    uint16_t       keep_alive_s;     /**< MQTT keep-alive interval (seconds).      */
} mqtt_connect_cfg_t;

/**
 * @brief MqttClient creation configuration.
 */
typedef struct {
    wifi_handle_t        wifi;           /**< WifiDriver handle (injected).  */
    mqtt_message_cb_t    msg_cb;         /**< Inbound message callback.      */
    mqtt_disconnect_cb_t disconnect_cb;  /**< Connection-loss callback.      */
} mqtt_client_config_t;
```

### 4.3 IMqttClient

```c
/**
 * @brief Create and initialise an MqttClient instance.
 *
 * Registers callbacks and prepares the coreMQTT context. Does NOT
 * connect — connect is a separate call so LifecycleController can
 * gate it on WiFi association.
 *
 * @param[in]  config  Injected dependencies and callbacks.
 * @param[out] handle  Receives the created handle on success.
 * @return MQTT_CLIENT_ERR_OK on success; MQTT_CLIENT_ERR_NULL_PTR
 *         if config, handle, msg_cb, or disconnect_cb is NULL;
 *         MQTT_CLIENT_ERR_NO_RESOURCE if pool exhausted.
 * @note Threading: task-context only. Call before scheduler starts.
 */
mqtt_client_err_t mqtt_client_create(const mqtt_client_config_t *config,
                                      mqtt_client_handle_t *handle);

/**
 * @brief Establish TLS + MQTT connection to AWS IoT Core.
 *
 * Sequence: open TCP socket → TLS 1.2 handshake with X.509 mutual
 * auth → MQTT CONNECT → await CONNACK → update stats.
 * On failure at any step: close socket, return error.
 *
 * Blocking. Timeout: MQTT_CONNECT_TIMEOUT_MS (10 s, see MQTT-O2).
 * Called by CloudPublisher when Machine 3 enters Connecting.
 *
 * @param[in] handle  MqttClient handle.
 * @param[in] cfg     Connection parameters (broker, certs, keep-alive).
 * @return MQTT_CLIENT_ERR_OK on success; MQTT_CLIENT_ERR_CONNECT_FAIL
 *         on CONNACK rejection; MQTT_CLIENT_ERR_TLS_FAIL on TLS
 *         handshake failure.
 * @note Threading: task-context only, blocking. Not ISR-safe.
 */
mqtt_client_err_t mqtt_client_connect(mqtt_client_handle_t handle,
                                       const mqtt_connect_cfg_t *cfg);

/**
 * @brief Send MQTT DISCONNECT and close the TLS session.
 *
 * Graceful disconnect — does NOT invoke disconnect_cb.
 * Called by CloudPublisher on controlled shutdown (UC-17).
 *
 * @param[in] handle  MqttClient handle.
 * @return MQTT_CLIENT_ERR_OK on success.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
mqtt_client_err_t mqtt_client_disconnect(mqtt_client_handle_t handle);

/**
 * @brief Publish a message to a topic.
 *
 * QoS 0: fire-and-forget; returns after the frame is handed to
 * WifiDriver. Non-blocking.
 * QoS 1: blocks until PUBACK received or MQTT_PUBACK_TIMEOUT_MS
 * expires (5 s, see MQTT-O4).
 *
 * Returns MQTT_CLIENT_ERR_NOT_CONNECTED immediately if not connected.
 *
 * @param[in] handle   MqttClient handle.
 * @param[in] topic    Null-terminated topic string.
 * @param[in] payload  Message payload.
 * @param[in] len      Payload byte count.
 * @param[in] qos      MQTT_QOS_0 or MQTT_QOS_1.
 * @return MQTT_CLIENT_ERR_OK on success; MQTT_CLIENT_ERR_PUBLISH_FAIL
 *         on send error or PUBACK timeout.
 * @note Threading: task-context only. QoS 0 non-blocking; QoS 1
 *       blocks until PUBACK or timeout. Not ISR-safe.
 */
mqtt_client_err_t mqtt_client_publish(mqtt_client_handle_t handle,
                                       const char    *topic,
                                       const uint8_t *payload,
                                       uint32_t       len,
                                       mqtt_qos_t     qos);

/**
 * @brief Subscribe to an MQTT topic.
 *
 * Issues MQTT SUBSCRIBE and waits for SUBACK. Inbound messages on
 * the subscribed topic are delivered via the msg_cb registered at
 * creation time.
 *
 * @param[in] handle  MqttClient handle.
 * @param[in] topic   Null-terminated topic filter string.
 * @param[in] qos     Maximum QoS for messages on this subscription.
 * @return MQTT_CLIENT_ERR_OK on success; MQTT_CLIENT_ERR_SUBSCRIBE_FAIL
 *         if SUBACK contains failure code.
 * @note Threading: task-context only, blocking. Not ISR-safe.
 */
mqtt_client_err_t mqtt_client_subscribe(mqtt_client_handle_t handle,
                                         const char *topic,
                                         mqtt_qos_t qos);

/**
 * @brief Process inbound MQTT frames and service keep-alive.
 *
 * Must be called regularly from CloudPublisherTask's main loop
 * (recommended: once per 100 ms). Internally calls coreMQTT's
 * MQTT_ProcessLoop(). Delivers inbound messages via msg_cb.
 * Detects keep-alive timeout and invokes disconnect_cb.
 *
 * @param[in] handle  MqttClient handle.
 * @return MQTT_CLIENT_ERR_OK on success.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
mqtt_client_err_t mqtt_client_process(mqtt_client_handle_t handle);
```

### 4.4 IMqttStats

```c
/**
 * @brief Copy current stats snapshot.
 *
 * Polled by CloudPublisher on each health-report cycle (Metric Producer
 * Pattern — counters → poll). Thread-safe: stats updated only in
 * CloudPublisherTask context. No mutex required.
 *
 * @param[in]  handle     MqttClient handle.
 * @param[out] stats_out  Receives the stats snapshot.
 * @return MQTT_CLIENT_ERR_OK on success.
 */
mqtt_client_err_t mqtt_client_get_stats(mqtt_client_handle_t handle,
                                         mqtt_stats_t *stats_out);

/**
 * @brief Reset all counters to zero.
 *
 * @param[in] handle  MqttClient handle.
 * @return MQTT_CLIENT_ERR_OK on success.
 */
mqtt_client_err_t mqtt_client_reset_stats(mqtt_client_handle_t handle);

#endif /* MQTT_CLIENT_H */
```

---

## 5. QoS split rationale

| Message type | QoS | Requirement | Rationale |
|---|---|---|---|
| Sensor telemetry | 0 | REQ-NF-206 | High-frequency; occasional loss tolerable; StoreAndForward handles gaps |
| Device health | 0 | REQ-NF-216 | 10-minute interval makes loss negligible |
| Config change ACK | 1 | REQ-DM-002 | Cloud must know the command was processed |
| OTA command result | 1 | REQ-DM-055 | Guaranteed delivery prevents cloud re-sending OTA |
| Alarm events | 1 | REQ-CC-020 | Missed alarm notification is a functional failure |

QoS 2 is not used. Exactly-once overhead is not justified — QoS 1 with
idempotent cloud-side handling is sufficient.

---

## 6. MQTT topic map

Topics assembled at connect time from client ID and per-message suffix
defined in `mqtt_topic_config.h`.

```
Publish topics:
  dt/iotmonitor/<client_id>/telemetry
  dt/iotmonitor/<client_id>/health
  dt/iotmonitor/<client_id>/alarms
  dt/iotmonitor/<client_id>/ota/result

Subscribe topics:
  cmd/iotmonitor/<client_id>/config
  cmd/iotmonitor/<client_id>/ota
```

`dt/` = device-to-cloud, `cmd/` = cloud-to-device (AWS IoT Core convention).

---

## 7. Keep-alive and connection-loss detection

MQTT keep-alive interval: configurable, default 60 seconds.

coreMQTT sends PINGREQ after `keep_alive_s` seconds of inactivity. If
PINGRESP is not received within `keep_alive_s / 2` seconds, coreMQTT
reports timeout to the process loop, which invokes `disconnect_cb`.

`mqtt_client_process()` must be called at least once per `keep_alive_s / 2`
seconds. The recommended 100 ms call rate is well within this bound.

---

## 8. Internal design

### 8.1 Private struct and static pool

```c
/* mqtt_client.c */

#define MQTT_CLIENT_MAX_INSTANCES  1u
#define MQTT_PKT_BUF_SIZE         4096u  /**< See MQTT-O3. */
#define MQTT_CONNECT_TIMEOUT_MS  10000u  /**< See MQTT-O2. */
#define MQTT_PUBACK_TIMEOUT_MS    5000u  /**< See MQTT-O4. */

struct mqtt_client_inst {
    /* Injected dependencies */
    wifi_handle_t        wifi;
    mqtt_message_cb_t    msg_cb;
    mqtt_disconnect_cb_t disconnect_cb;

    /* coreMQTT / mbedTLS contexts */
    MQTTContext_t        mqtt_ctx;
    NetworkContext_t     net_ctx;
    MQTTFixedBuffer_t    fixed_buf;
    uint8_t              pkt_buf[MQTT_PKT_BUF_SIZE];

    /* Runtime state */
    bool                 connected;
    mqtt_stats_t         stats;
    bool                 in_use;
};

static struct mqtt_client_inst g_pool[MQTT_CLIENT_MAX_INSTANCES];
static uint8_t                 g_count;
```

No dynamic allocation. All coreMQTT and mbedTLS contexts are embedded
in the instance struct. The packet buffer is a static array.

### 8.2 coreMQTT integration points

coreMQTT requires three callbacks provided by the integration layer:

```c
/* Transport send — mbedTLS write → WifiDriver TCP send */
static int32_t prv_transport_send(NetworkContext_t *ctx,
                                   const void *buf, size_t len);

/* Transport receive — mbedTLS read → WifiDriver TCP recv */
static int32_t prv_transport_recv(NetworkContext_t *ctx,
                                   void *buf, size_t len);

/* Monotonic clock — maps to xTaskGetTickCount() */
static uint32_t prv_get_time_ms(void);
```

`NetworkContext_t` holds the mbedTLS context and the WifiDriver socket
handle (`wifi_socket_t`). It is embedded in the instance struct.

### 8.3 Init ordering

```
wifi_create(...)              ← driver ready
[WiFi associated with AP]    ← gated by LifecycleController
mqtt_client_create(...)      ← registers callbacks; does NOT connect
[CloudPublisherTask created] ← task handle valid
mqtt_client_connect(...)     ← called by CloudPublisher on Machine 3 → Connecting
mqtt_client_subscribe(...)   ← subscribe to command topics
```

### 8.4 Test reset hook

```c
#ifdef TEST
void mqtt_client_reset_for_test(void)
{
    memset(g_pool, 0, sizeof(g_pool));
    g_count = 0u;
}
#endif
```

---

## 9. Sequence integration

### 9.1 SD trace

| SD | Role | Key functions |
|---|---|---|
| SD-03 | Telemetry and health MQTT publish to AWS IoT Core | `mqtt_client_publish()` |
| SD-04 | Connection-loss detection; reconnect; drain store-and-forward | `disconnect_cb`, `mqtt_client_connect()`, `mqtt_client_publish()` |
| SD-05 | Alarm MQTT publish | `mqtt_client_publish()` |
| SD-06 | OTA command receive; progress/completion publish | `mqtt_client_subscribe()`, `mqtt_client_publish()` |
| SD-07 | Remote config command receive | `mqtt_client_subscribe()`, `msg_cb` |
| SD-08 | Remote restart command receive | `mqtt_client_subscribe()`, `msg_cb` |

---

## 10. Error and fault behaviour

All public functions return `mqtt_client_err_t`. No retry inside
MqttClient — CloudPublisher drives reconnect and retry strategy.

| Error | Cause | Behaviour |
|---|---|---|
| `MQTT_CLIENT_ERR_NOT_INIT` | Called before `mqtt_client_create()` | Return immediately |
| `MQTT_CLIENT_ERR_NULL_PTR` | Required pointer is NULL | Return immediately |
| `MQTT_CLIENT_ERR_NO_RESOURCE` | Pool exhausted | Return immediately |
| `MQTT_CLIENT_ERR_CONNECT_FAIL` | CONNACK rejection | Socket closed, stats updated |
| `MQTT_CLIENT_ERR_TLS_FAIL` | mbedTLS handshake or cert verification failure | TLS context freed, socket closed |
| `MQTT_CLIENT_ERR_PUBLISH_FAIL` | Send error or QoS 1 PUBACK timeout | `stats.publish_failures` incremented |
| `MQTT_CLIENT_ERR_NOT_CONNECTED` | Publish/disconnect when not connected | Return immediately |
| `MQTT_CLIENT_ERR_SUBSCRIBE_FAIL` | SUBACK with failure return code | `stats.subscribe_failures` incremented |

---

## 11. Principles applied

- **P1 (Strict directional layering).** Depends on WifiDriver (driver) and Logger (cross-cutting); no application dependencies.
- **P2 (DIP).** CloudPublisher and UpdateService (application) depend on the opaque `mqtt_client_handle_t`, not the concrete implementation.
- **P3 (ISP).** `IMqttClient` (publish/subscribe) and `IMqttStats` (counters) are separate interfaces — CloudPublisher consumes both; HealthMonitor reads only stats. Distinct consumer sets.
- **P4 (Cross-cutting exception).** Logger referenced concretely per convention.
- **P5 (Bounded resources).** Static pool; static packet buffer; no heap post-init.
- **P6 (Traces to requirements).** Publish/subscribe/connect trace to REQ-CC-050/060, REQ-NF-206/216/300–305.
- **P8 (Total error propagation).** 8 distinct error codes; TLS handshake, CONNACK rejection, PUBACK timeout each distinguished.
- **P9 (BARR-C).** Fixed-width types; `const` on read-only pointers.
- **P10 (Naming).** Prefix `mqtt_client_`; handle `mqtt_client_handle_t`; errors `MQTT_CLIENT_ERR_*`.

---

## 12. Synchronisation

Caller serialises. MqttClient runs exclusively in CloudPublisherTask
context. No FreeRTOS synchronisation primitives. The ISR path does not
involve MqttClient — all socket I/O is blocking within the task.

---

## 13. Unit-test plan

Test file: `tests/gateway/middleware/mqtt_client/test_mqtt_client.c`

Transport layer is stubbed: mock `transport_send/recv` inject synthetic
CONNACK, PUBACK, SUBACK, inbound PUBLISH, or error codes.

**Layer 1 — AT response parser / coreMQTT frame parsing:**

| ID | Scenario | Expected |
|---|---|---|
| MQTT-T01 | `mqtt_client_create` happy path | Handle returned, callbacks stored |
| MQTT-T02 | `mqtt_client_create` with NULL config | Returns `ERR_NULL_PTR` |
| MQTT-T03 | `mqtt_client_create` pool exhaustion | Returns `ERR_NO_RESOURCE` |
| MQTT-T04 | `mqtt_client_connect` with stub CONNACK=0 | Returns `ERR_OK`; `stats.connect_ok == 1` |
| MQTT-T05 | `mqtt_client_connect` with stub CONNACK≠0 | Returns `ERR_CONNECT_FAIL` |
| MQTT-T06 | `mqtt_client_connect` TLS handshake failure | Returns `ERR_TLS_FAIL` |
| MQTT-T07 | `mqtt_client_publish` QoS 0 | Frame transmitted; `stats.publishes_sent == 1` |
| MQTT-T08 | `mqtt_client_publish` QoS 1 + stub PUBACK | Returns `ERR_OK`; `stats.publishes_acked == 1` |
| MQTT-T09 | `mqtt_client_publish` QoS 1 no PUBACK (timeout) | Returns `ERR_PUBLISH_FAIL`; `stats.publish_failures == 1` |
| MQTT-T10 | `mqtt_client_publish` when not connected | Returns `ERR_NOT_CONNECTED` immediately |
| MQTT-T11 | `mqtt_client_subscribe` happy path | SUBACK with success |
| MQTT-T12 | `mqtt_client_subscribe` SUBACK failure | Returns `ERR_SUBSCRIBE_FAIL` |
| MQTT-T13 | `mqtt_client_process` with inbound PUBLISH | `msg_cb` invoked with correct topic and payload |
| MQTT-T14 | Keep-alive timeout in `mqtt_client_process` | `disconnect_cb` invoked |
| MQTT-T15 | `mqtt_client_disconnect` graceful | Connection closed; `disconnect_cb` NOT invoked |
| MQTT-T16 | `mqtt_client_reset_stats` | All counters zero |
| MQTT-T17 | `mqtt_client_get_stats` copies snapshot | Stats match internal state |

---

## 14. Open items

| ID | Item | Status | Resolution |
|---|---|---|---|
| MQTT-O1 | mbedTLS RAM: ~35–50 KB. Must verify against 128 KB SRAM budget at integration. | **Open** | Verify at integration. If insufficient, evaluate reduced cipher config or on-module TLS fallback. Also now covers bounding mbedTLS's internal calloc/free usage for RSA/ECC bignum scratch space (`MBEDTLS_PLATFORM_C`, required for host-test builds) — e.g. `mbedtls_memory_buffer_alloc_init()` over a fixed static buffer. |
| MQTT-O2 | `MQTT_CONNECT_TIMEOUT_MS` — provisional 10 000 ms proved insufficient on hardware (measured ~29 s for a full mutual-auth TLS 1.2 handshake: software RSA-2048/ECDHE on the L475's 80 MHz core with no crypto accelerator, compounded by the WifiDriver read floor in MQTT-O7). Bumped to 30 000 ms. | **Open** | 29 s against a 60 s keep-alive interval is uncomfortably close — revisit once MQTT-O7 (non-blocking transport) is resolved, since that removes the dominant compounding factor. |
| MQTT-O3 | `MQTT_PKT_BUF_SIZE` = 4096 (provisional). Must exceed largest payload. Health ~1–2 KB; OTA chunk TBD. | **Open** | Confirm max OTA chunk size at UpdateService LLD. |
| MQTT-O4 | `MQTT_PUBACK_TIMEOUT_MS` / `MQTT_SUBACK_TIMEOUT_MS` — provisional 5000 ms bumped to 15 000 ms for the same reason as MQTT-O2 (WifiDriver's per-call read floor, MQTT-O7), even though observed PUBACK/SUBACK round trips against the local test broker were fast (well under 1 s). | **Open** | Validate against observed AWS IoT Core RTT at integration; revisit alongside MQTT-O2/O7. |
| MQTT-O5 | Certificate storage partition address/format — depends on QspiFlashDriver/ConfigStore. MqttClient receives pointers only. | **Open** | Confirm at QspiFlashDriver LLD. |
| MQTT-O6 | mbedTLS's CTR-DRBG entropy source needs a real RNG. The STM32L475's on-chip RNG peripheral requires its *kernel* clock (`RCC->CCIPR.CLK48SEL`) selected separately from its bus-clock gate (`AHB2ENR.RNGEN`) — not covered by this companion's original §8 design, which predates the concrete RNG choice, and not obvious until it hung on hardware (the poll loop waiting on `RNG->SR.DRDY` spun forever with no running kernel clock selected). | **Resolved** | Fixed in `mqtt_client.c` (`prv_configure_rng_clock()`): configures PLLSAI1 for an independent 48 MHz source (4 MHz VCO input × 48, ÷4) and selects it via `CLK48SEL`. Confirmed working on hardware. RNG has no dedicated driver in this project; this is documented as a narrow, scoped exception to register-level access from a middleware module, not a new convention. |
| MQTT-O7 | `mqtt_client_process()` is not the fast/non-blocking ~100 ms call this companion's §7 recommended cadence assumes when idle. It calls `wifi_recv()` internally, which floors its own wait at `WIFI_RESP_TIMEOUT_MS` (5000 ms, `wifi_driver.c`) regardless of the timeout requested — so every idle poll can itself block for up to ~5 s. In production, `CloudPublisherTask` also owns telemetry timers, alarm-queue draining, and command-queue draining (§4 activation model) on the *same* task — a 5 s block on every idle `mqtt_client_process()` call stalls all of those, not just MQTT. Confirmed on hardware during bring-up (a diagnostic poll loop assuming ~100 ms/iteration measured ~5 s/iteration instead). | **Open** | Two candidate fixes, not yet chosen: (a) give WifiDriver a genuinely non-blocking "is data available" primitive instead of a floored blocking read, or (b) move `mqtt_client_process()` off `CloudPublisherTask` onto its own dedicated task. Deferred to CloudPublisher's design — do not resolve as a side effect of an unrelated change. |

---

## 15. Decisions log

| ID | Decision | Rationale |
|---|---|---|
| MQTT-D1 | coreMQTT as MQTT library | No dynamic allocation; transport-agnostic; official AWS IoT recommendation. |
| MQTT-D2 | TLS via mbedTLS, not ISM43362 on-module TLS | mbedTLS is host-testable, portable, and keeps cert management under firmware control. |
| MQTT-D3 | QoS 0 for telemetry/health; QoS 1 for alarms/OTA/config ACK | Balances throughput (QoS 0) against delivery guarantees (QoS 1) per message criticality. No QoS 2 — overhead not justified. |
| MQTT-D4 | `wifi_rssi_dbm` removed from `mqtt_stats_t` | RSSI is WifiDriver's domain, not MQTT's. CloudPublisher reads RSSI from WifiDriver directly and reports via IHealthReport. Keeps MqttClient stats scoped to MQTT-level concerns. |
| MQTT-D5 | ADT pattern (opaque handle, static pool of 1) | Gateway default. WifiDriver handle and callbacks injected via config struct. |
| MQTT-D6 | `mqtt_client_subscribe()` as explicit public function | Referenced in SD-06, SD-07, SD-08 for command/OTA topic subscription. Separation from connect allows CloudPublisher to control subscription timing. |
| MQTT-D7 | `mqtt_client_publish()` blocking behaviour: QoS 0 non-blocking, QoS 1 blocking until PUBACK or timeout | QoS 0 is fire-and-forget by MQTT spec. QoS 1 must wait for PUBACK to confirm delivery — blocking is the correct model for single-threaded CloudPublisherTask. |

---

## 16. File layout

```
firmware/gateway/middleware/mqtt_client/
├── mqtt_client.h           /* public API — handle, config, error enum, types  */
├── mqtt_client.c           /* implementation — coreMQTT/mbedTLS integration   */
└── mqtt_topic_config.h     /* topic string constants                          */

tests/gateway/middleware/mqtt_client/
└── test_mqtt_client.c      /* Unity + mock transport stubs                    */
```

---

## Phase H — Readiness review

| # | Check | Status |
|---|---|---|
| H1 | PROVIDES / USES match `components.md` exactly | PASS — PROVIDES IMqttClient + IMqttStats; USES WifiDriver + ILogger |
| H2 | Root SRS requirements cited | PASS — REQ-CC-050, REQ-CC-060, REQ-NF-206/216/300–305 |
| H3 | All public API functions have complete Doxygen | PASS |
| H4 | ADT pattern applied | PASS — pool of 1, MQTT-D5 |
| H5 | Error enum covers all failure modes | PASS — 8 error codes |
| H6 | Library choices documented with justification | PASS — §2 (coreMQTT, mbedTLS) |
| H7 | TLS configuration specified per security requirements | PASS — §3 |
| H8 | Open items have named owner and resolution path | PASS — all deferred to integration with clear trigger |
| H9 | Unit-test plan covers happy path + error cases | PASS — 17 test cases |
| H10 | Test file path follows Gateway convention | PASS |
| H11 | P1–P10 compliance reviewed | PASS — §11 |
| H12 | Thread safety documented | PASS — §12, single-task caller |
| H13 | `reset_for_test` hook specified | PASS — §8.4 |
| H14 | Decisions log complete | PASS — 7 decisions |
| H15 | Sequence integration traces to HLD SDs | PASS — §9.1 |
| H16 | QoS assignment per message type documented with requirement trace | PASS — §5 |
| H17 | Topic map documented | PASS — §6 |

**Verdict: PASS — ready for implementation.**

Five open items remain — all are integration-time validations (RAM budget,
timeouts, buffer size, cert storage) that do not block implementation.
