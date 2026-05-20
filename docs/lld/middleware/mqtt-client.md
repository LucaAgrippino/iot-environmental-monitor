# LLD Companion — MqttClient

**Layer:** Middleware  
**Board:** Gateway (GW) only  
**Provides:** `IMqttClient`, `IMqttStats`  
**Consumes:** `IWifi` (WifiDriver), `ILogger`  
**SRS traces:** REQ-CC-050, REQ-CC-060, REQ-NF-206, REQ-NF-216, REQ-NF-300, REQ-NF-301, REQ-NF-302, REQ-NF-305  
**HLD ref:** `components.md` §Middleware — MqttClient; `state-machines.md` Machine 3; `hld.md` §6.3; `sequence-diagrams.md` SD-03, SD-04, SD-05, SD-06a–d
**Version:** 0.1
**Date:** May 2026
**Status:** Draft

**HLD anchor:** MqttClient in `components.md` (GW middleware layer)

---

## 1. Sources

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
reacts by transitioning Machine 3 to Disconnected and starting the 1 Hz
reconnect timer (REQ-NF-209).

MqttClient runs in `CloudPublisherTask` context. It has no thread of its
own. The `mqtt_client_process()` function is called from CloudPublisherTask's
main loop to drive the receive path.

---

## 2. Library choice — coreMQTT + mbedTLS

| Concern | Library | Justification |
|---------|---------|---------------|
| MQTT protocol | coreMQTT (AWS FreeRTOS, MIT licence) | No dynamic allocation; transport-agnostic; designed for embedded; official AWS IoT recommendation |
| TLS 1.2 | mbedTLS | Widely deployed in embedded; supports X.509 mutual auth; integrates as coreMQTT transport callbacks |
| TCP transport | WifiDriver (`IWifi`) | ISM43362 AT-over-SPI; WifiDriver provides a plain TCP socket — mbedTLS wraps it |

**Why not ISM43362 native TLS?** The ISM43362 AT command set does support
TLS, but routing X.509 certificate and key material through AT commands
(escaping binary data in ASCII frames) is fragile and untestable on the host.
Running mbedTLS on the STM32L475 keeps the TLS stack visible, host-testable,
and under firmware control. The extra RAM cost (~35 KB for mbedTLS context)
is within budget given REQ-NF-400 (128 KB SRAM) — confirm at MQTT-O1.

---

## 3. TLS configuration (REQ-NF-300, REQ-NF-301, REQ-NF-302)

| Parameter | Value |
|-----------|-------|
| TLS version | 1.2 minimum (REQ-NF-300) |
| Cipher suites | TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256 (AWS IoT Core default) |
| Client auth | X.509 mutual auth (REQ-NF-301) |
| Server auth | AWS root CA verified (REQ-NF-305 — reject unencrypted / unauthenticated connections) |
| Certificate storage | Dedicated flash partition (CON-006); loaded at boot, never logged (REQ-NF-302) |
| Certificate type | PEM, converted to DER at load time for mbedTLS |

The TLS session is established inside `mqtt_client_connect()`. Certificates
are passed in from ConfigService (loaded from the cert partition at boot).
MqttClient stores a pointer to the cert material for the duration of the
session — it does not copy it.

---

## 4. Data types

```c
/* mqtt_client.h */

typedef enum {
    MQTT_CLIENT_ERR_OK             = 0,
    MQTT_CLIENT_ERR_NOT_INIT       = 1,
    MQTT_CLIENT_ERR_NULL_ARG       = 2,
    MQTT_CLIENT_ERR_CONNECT_FAIL   = 3,   /* TLS or MQTT CONNECT rejected  */
    MQTT_CLIENT_ERR_PUBLISH_FAIL   = 4,   /* send error or QoS1 PUBACK timeout */
    MQTT_CLIENT_ERR_NOT_CONNECTED  = 5,
    MQTT_CLIENT_ERR_TLS_FAIL       = 6,
} mqtt_client_err_t;

typedef enum {
    MQTT_QOS_0 = 0,   /* fire-and-forget (telemetry, health — REQ-NF-206, NF-216) */
    MQTT_QOS_1 = 1,   /* at-least-once (OTA result, config ACK — REQ-DM-055)      */
} mqtt_qos_t;

typedef struct {
    uint32_t publishes_sent;       /* total PUBLISH frames transmitted        */
    uint32_t publishes_acked;      /* QoS 1 PUBACKs received                  */
    uint32_t publish_failures;     /* send errors or PUBACK timeouts           */
    uint32_t connect_attempts;     /* total mqtt_client_connect() calls        */
    uint32_t connect_ok;           /* successful connections                   */
    uint32_t reconnect_count;      /* connections after first                  */
    uint32_t subscribe_failures;   /* SUBACK with failure code                 */
    int32_t  wifi_rssi_dbm;        /* last RSSI reading from WifiDriver        */
} mqtt_stats_t;

/**
 * @brief  Callback invoked by MqttClient when an inbound PUBLISH arrives
 *         on a subscribed topic.
 *
 * Called from CloudPublisherTask context (inside mqtt_client_process()).
 * The payload pointer is valid only for the duration of the callback.
 *
 * @param  topic      Null-terminated topic string.
 * @param  topic_len  Topic string length in bytes.
 * @param  payload    Message payload (not null-terminated).
 * @param  payload_len  Payload byte count.
 */
typedef void (*mqtt_message_cb_t)(const char    *topic,
                                   uint16_t       topic_len,
                                   const uint8_t *payload,
                                   uint32_t       payload_len);

/**
 * @brief  Callback invoked when the MQTT connection is lost (keep-alive
 *         timeout, TCP error, or MQTT-level error).
 *
 * Called from CloudPublisherTask context. CloudPublisher transitions
 * Machine 3 to Disconnected on receipt.
 */
typedef void (*mqtt_disconnect_cb_t)(void);
```

---

## 2. Public API

### 5.1 `IMqttClient`

```c
/**
 * @brief  Initialise MqttClient.
 *
 * Registers callbacks. Does NOT connect — connect is a separate call so
 * LifecycleController can gate it on WiFi association.
 *
 * @param  msg_cb         Inbound message callback (subscribed topics).
 * @param  disconnect_cb  Connection-loss callback.
 */
mqtt_client_err_t mqtt_client_init(mqtt_message_cb_t    msg_cb,
                                    mqtt_disconnect_cb_t disconnect_cb);

/**
 * @brief  Establish TLS + MQTT connection to AWS IoT Core.
 *
 * Sequence:
 *   1. Open TCP socket via WifiDriver.
 *   2. Perform TLS 1.2 handshake with X.509 mutual auth via mbedTLS.
 *   3. Send MQTT CONNECT packet; await CONNACK.
 *   4. On success: subscribe to command topics; update stats.
 *   5. On failure at any step: close socket; return error.
 *
 * Blocking. Timeout: MQTT_CONNECT_TIMEOUT_MS (see MQTT-O2).
 * Called by CloudPublisher when Machine 3 enters Connecting.
 *
 * @param  cfg  Connection parameters (broker endpoint, port, client ID,
 *              cert/key pointers, CA cert pointer).
 */
mqtt_client_err_t mqtt_client_connect(const mqtt_connect_cfg_t *cfg);

/**
 * @brief  Send MQTT DISCONNECT and close the TLS session.
 *
 * Graceful disconnect — does not invoke disconnect_cb.
 * Called by CloudPublisher on controlled shutdown (UC-17).
 */
mqtt_client_err_t mqtt_client_disconnect(void);

/**
 * @brief  Publish a message.
 *
 * QoS 0: fire-and-forget; returns MQTT_CLIENT_ERR_OK after the frame
 * is handed to WifiDriver.
 * QoS 1: blocks until PUBACK received or MQTT_PUBACK_TIMEOUT_MS expires.
 *
 * Returns MQTT_CLIENT_ERR_NOT_CONNECTED immediately if not connected —
 * CloudPublisher is responsible for re-routing to StoreAndForward.
 *
 * @param  topic      Null-terminated topic string.
 * @param  payload    Message payload.
 * @param  len        Payload byte count.
 * @param  qos        MQTT_QOS_0 or MQTT_QOS_1.
 */
mqtt_client_err_t mqtt_client_publish(const char    *topic,
                                       const uint8_t *payload,
                                       uint32_t       len,
                                       mqtt_qos_t     qos);

/**
 * @brief  Process inbound MQTT frames and service keep-alive.
 *
 * Must be called regularly from CloudPublisherTask's main loop
 * (recommended: once per 100 ms, or after each publish).
 * Internally calls coreMQTT's MQTT_ProcessLoop().
 * Delivers inbound messages via msg_cb.
 * Detects keep-alive timeout → invokes disconnect_cb.
 */
mqtt_client_err_t mqtt_client_process(void);
```

### 5.2 `IMqttStats`

```c
/**
 * @brief  Copy current stats snapshot.
 *
 * Polled by CloudPublisher on each health-report cycle (Metric Producer
 * Pattern — counters → poll). Includes RSSI sampled from WifiDriver.
 * Thread-safe: all stats updated only in CloudPublisherTask context.
 * No mutex required — single-task caller model.
 */
mqtt_client_err_t mqtt_client_get_stats(mqtt_stats_t *stats_out);

/** @brief  Reset all counters (triggered by CMD_RESET_METRICS). */
mqtt_client_err_t mqtt_client_reset_stats(void);
```

---

## 6. Connection parameters struct

```c
/* mqtt_client.h */

typedef struct {
    const char    *broker_endpoint;   /* e.g. "xxxxxx.iot.eu-west-1.amazonaws.com" */
    uint16_t       broker_port;       /* 8883 (MQTT over TLS) */
    const char    *client_id;         /* device serial number */
    const uint8_t *client_cert_der;   /* DER-encoded client certificate           */
    uint32_t       client_cert_len;
    const uint8_t *client_key_der;    /* DER-encoded private key                  */
    uint32_t       client_key_len;
    const uint8_t *ca_cert_der;       /* DER-encoded AWS root CA certificate       */
    uint32_t       ca_cert_len;
    uint16_t       keep_alive_s;      /* MQTT keep-alive interval in seconds       */
} mqtt_connect_cfg_t;
```

Credentials are loaded by LifecycleController from the dedicated cert flash
partition at boot and passed to `mqtt_client_connect()` as pointers.
MqttClient never copies them (REQ-NF-302: no plaintext storage beyond what
is already in the cert partition, which is a separate concern).

---

## 7. QoS split rationale

| Message type | QoS | Requirement | Rationale |
|-------------|-----|------------|-----------|
| Sensor telemetry | 0 | REQ-NF-206 | High-frequency; occasional loss tolerable; StoreAndForward handles gaps |
| Device health | 0 | REQ-NF-216 | Same reasoning; 10-minute interval makes loss negligible |
| Config change ACK | 1 | REQ-DM-002 | Cloud must know the command was processed |
| OTA command result | 1 | REQ-DM-055 | Guaranteed delivery prevents cloud from re-sending the OTA command |
| Alarm events | 1 | REQ-CC-020 | Alarms require confirmed delivery; missed alarm notification is a functional failure |

QoS 2 is not used. The network overhead and state complexity of exactly-once
delivery is not justified for this system — QoS 1 with idempotent handling
on the cloud side is sufficient.

---

## 8. MQTT topic map

Topics are assembled at connect time from the client ID and a per-message
suffix defined in `mqtt_topic_config.h`. This keeps topic strings out of
the application code.

```
Publish topics:
  Telemetry:  dt/iotmonitor/<client_id>/telemetry
  Health:     dt/iotmonitor/<client_id>/health
  Alarms:     dt/iotmonitor/<client_id>/alarms
  OTA result: dt/iotmonitor/<client_id>/ota/result

Subscribe topics:
  Commands:   cmd/iotmonitor/<client_id>/config
  OTA:        cmd/iotmonitor/<client_id>/ota
```

The `dt/` (device-to-cloud) and `cmd/` (cloud-to-device) prefixes follow
AWS IoT Core topic namespacing convention. Topic strings are `const char *`
constants — no dynamic string construction at publish time.

---

## 9. Keep-alive and connection-loss detection

MQTT keep-alive interval: configurable, default 60 seconds
(`mqtt_connect_cfg_t.keep_alive_s`).

coreMQTT sends PINGREQ after `keep_alive_s` seconds of inactivity.
If PINGRESP is not received within `keep_alive_s / 2` seconds, coreMQTT
reports a timeout to the process loop, which invokes `disconnect_cb`.

`mqtt_client_process()` must be called at least once per
`keep_alive_s / 2` seconds by CloudPublisherTask to ensure the keep-alive
is serviced. The recommended call rate of 100 ms is well within this bound.

---

## 10. coreMQTT integration points

coreMQTT requires three callbacks provided by the integration layer:

```c
/* Transport send — maps to mbedTLS write, which maps to WifiDriver TCP send */
static int32_t transport_send(NetworkContext_t *ctx,
                               const void *buf, size_t len);

/* Transport receive — maps to mbedTLS read, which maps to WifiDriver TCP recv */
static int32_t transport_recv(NetworkContext_t *ctx,
                               void *buf, size_t len);

/* Monotonic clock in milliseconds — maps to xTaskGetTickCount() */
static uint32_t get_time_ms(void);
```

`NetworkContext_t` holds the mbedTLS context and the WifiDriver socket
handle. It is a static struct within `mqtt_client.c` — no dynamic
allocation.

---

## 3. Internal design

```c
/* mqtt_client.c */

typedef struct {
    bool                 initialised;
    bool                 connected;
    MQTTContext_t        mqtt_ctx;          /* coreMQTT context              */
    NetworkContext_t     net_ctx;           /* mbedTLS + WifiDriver socket   */
    MQTTFixedBuffer_t    fixed_buf;         /* coreMQTT packet buffer        */
    uint8_t              pkt_buf[MQTT_PKT_BUF_SIZE];   /* static buffer      */
    mqtt_message_cb_t    msg_cb;
    mqtt_disconnect_cb_t disconnect_cb;
    mqtt_stats_t         stats;
} MqttClientState;

static MqttClientState s_mqtt;

#define MQTT_PKT_BUF_SIZE   4096U   /* covers max expected payload; see MQTT-O3 */
```

No dynamic allocation. All coreMQTT and mbedTLS contexts are embedded in
`s_mqtt`. The packet buffer is a static array.

---

### Principles applied

- **P1 (Strict directional layering).** Depends on IWifi (driver layer) and Logger (cross-cutting); no application-layer dependencies.
- **P2 (Dependency Inversion).** Exposes `imqtt_client_t` vtable; CloudPublisher depends on `IMqttClient` and `IMqttStats`.
- **P3 (Interface Segregation).** `IMqttClient` (publish/subscribe operations) and `IMqttStats` (connectivity counters) are separate interfaces because CloudPublisher needs the former while HealthMonitor reads the latter — distinct, non-overlapping consumer sets.
- **P4 (Cross-cutting concern exception).** Logger referenced concretely per the cross-cutting exception; documented in §1 Sources.
- **P5 (Bounded resources, no dynamic allocation post-init).** Static session state and packet buffer; TLS context allocated from a fixed-size static pool; no heap after scheduler start.
- **P6 (Responsibility traces to requirements).** Publish / subscribe / receive functions trace to REQ-CC-050/060 / REQ-NF-206/216/300-305 cloud connectivity requirements.
- **P8 (Total error propagation, no silent failures).** All operations return `mqtt_client_err_t`; TLS handshake failure, CONNACK rejection, and timeout are distinct error codes.
- **P9 (BARR-C coding standard).** Topic length `uint16_t`; payload length `uint16_t`; QoS level `uint8_t`; no floating-point.
- **P10 (Naming conventions).** Prefix `mqtt_client_`; interface `IMqttClient` -> `imqtt_client_t`; errors `MQTT_CLIENT_ERR_*`.


## 12. Init ordering

```
wifi_driver_init()           ← driver ready
[WiFi associated with AP]    ← gated by LifecycleController
mqtt_client_init(msg_cb, dc) ← registers callbacks; does NOT connect
[CloudPublisherTask created] ← task handle valid
mqtt_client_connect(&cfg)    ← called by CloudPublisher on Machine 3 → Connecting
```

No two-phase init beyond ordering above. The ISR path does not involve
MqttClient — all socket I/O is blocking within CloudPublisherTask.

---

## 5. Sequence integration

See the HLD sequence diagrams for inter-component flows. This component is called synchronously; no task-level sequencing diagram is required beyond the HLD.

## 6. Error and fault behaviour

Error codes and propagation policy are defined in the Public API section above. All public functions return an error code; callers must not ignore non-OK returns.

## 7. Unit-test plan

```c
#ifdef UNIT_TEST
/* Replace WifiDriver TCP calls and mbedTLS with loopback stubs */
static int32_t stub_transport_send(NetworkContext_t *ctx,
                                    const void *buf, size_t len);
static int32_t stub_transport_recv(NetworkContext_t *ctx,
                                    void *buf, size_t len);
/* Inject synthetic CONNACK, PUBACK, SUBACK, inbound PUBLISH, or error codes */
#endif
```

Minimum test cases:
- `mqtt_client_connect()` with stub CONNACK = 0 → `ERR_OK`; `stats.connect_ok == 1`.
- `mqtt_client_connect()` with stub CONNACK ≠ 0 → `ERR_CONNECT_FAIL`.
- `mqtt_client_publish()` QoS 0 → frame transmitted; `stats.publishes_sent == 1`.
- `mqtt_client_publish()` QoS 1 + stub PUBACK → `ERR_OK`; `stats.publishes_acked == 1`.
- `mqtt_client_publish()` QoS 1 + no PUBACK (timeout) → `ERR_PUBLISH_FAIL`; `stats.publish_failures == 1`.
- `mqtt_client_publish()` when not connected → `ERR_NOT_CONNECTED` immediately.
- `mqtt_client_process()` with inbound PUBLISH → `msg_cb` invoked with correct topic and payload.
- Keep-alive timeout in `mqtt_client_process()` → `disconnect_cb` invoked.
- `mqtt_client_reset_stats()` → all counters zero.

---

## 8. Open items

| ID | Item | Resolution path | Status |
|--------|------|-----------------|--------|
| MQTT-O1 | mbedTLS RAM requirement: mbedTLS SSL context ≈ 36 KB + TX/RX buffers (configurable, typically 16 KB each). At minimal config (reduced cipher suite, small buffers) total is ~35–50 KB. Must be verified against the 128 KB SRAM budget (REQ-NF-400) during integration. WIFI-O1 from session summary. | Verify mbedTLS SRAM footprint against REQ-NF-400 budget at integration | Open |
| MQTT-O2 | `MQTT_CONNECT_TIMEOUT_MS` — provisional value: 10 000 ms. TLS handshake with AWS IoT Core is the bottleneck (~2–5 s on typical WiFi). Validate at integration. | Validate TLS handshake timing at integration; adjust timeout if needed | Open |
| MQTT-O3 | `MQTT_PKT_BUF_SIZE = 4096` is provisional. Must exceed the largest expected payload. Health payload is the largest (full metric set, JSON, ~1–2 KB estimated). Firmware download chunks (SD-06b) are the upper bound — confirm max OTA chunk size from UpdateService LLD. | Confirm max OTA chunk size at UpdateService LLD — must fit MQTT_PKT_BUF_SIZE | Open |
| MQTT-O4 | QoS 1 PUBACK timeout — not yet defined. Provisional: 5 000 ms. Validate at integration. | Validate PUBACK timeout at integration against observed AWS IoT Core RTT | Open |
| MQTT-O5 | Certificate storage partition address and format — depends on QspiFlashDriver LLD. MqttClient receives cert pointers from LifecycleController; it does not access flash directly. | Confirm cert partition address/format at QspiFlashDriver LLD | Open |
