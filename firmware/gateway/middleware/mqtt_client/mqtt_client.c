/**
 * @file mqtt_client.c
 * @brief MQTT client (Gateway) implementation — coreMQTT + mbedTLS.
 *
 * @see docs/lld/middleware/mqtt-client.md for the design specification
 *      this file implements (companion §8).
 */

#include "mqtt_client.h"
#include "mqtt_topic_config.h"

#include <stddef.h>
#include <string.h>

#include "core_mqtt.h"
#include "transport_interface.h"

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/pk.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#include "FreeRTOS.h"
#include "task.h"

#include "logger/logger.h"
#include "stm32l475xx.h"

#define MQTT_CLIENT_LOG_MODULE "MqttClient"

#define MQTT_CLIENT_MAX_INSTANCES 1u
#define MQTT_PKT_BUF_SIZE 4096u /**< See MQTT-O3. */

/* wifi_recv() floors its own internal wait at WIFI_RESP_TIMEOUT_MS
 * (5000 ms, wifi_driver.c) regardless of the timeout_ms passed in — a
 * single R0 AT round trip can itself take a real multi-second wait
 * (WifiDriver's own WIFI-O11 finding), and retrying from a layer above
 * (as mbedtls_ssl_handshake()'s internal WANT_READ/WANT_WRITE retry
 * loop does here) multiplies that wait per attempt rather than
 * shortening it. So these budgets must give the retry loop enough
 * *attempts* at that ~5 s floor, not just a longer single wait.
 * Confirmed on hardware (pktmon capture) during MqttClient bring-up:
 * the broker's ServerHello + Certificate reply reached and was TCP-ACKed
 * by the module within ~15 ms of sending ClientHello, but
 * MQTT_CONNECT_TIMEOUT_MS's original 10 s budget allowed only 1-2
 * underlying wifi_recv() attempts before giving up — not enough margin
 * against that per-call floor. See MQTT-O2/O4/O6. */
/** MQTT-D8: bounds the TLS_HANDSHAKE phase's wall-clock deadline in
 *  mqtt_client_connect_step() — ticked across multiple calls (one
 *  mbedtls_ssl_handshake() round per call) rather than looped to
 *  completion in a single blocking call, so a stalled handshake no
 *  longer freezes CloudPublisherTask for the full budget in one shot
 *  (confirmed on hardware: a failed reconnect previously blocked
 *  CloudPublisherTask for ~19 s straight, TLS handshake failing with
 *  MBEDTLS_ERR_SSL_TIMEOUT — risks REQ-NF-113's 500 ms alarm-to-publish
 *  bound if an alarm fires mid-reconnect). */
#define MQTT_CONNECT_TIMEOUT_MS 30000u /**< ~6 retries at the 5 s floor. */
/** MQTT-D8: MQTT_Connect()'s own CONNACK-wait budget. Split out from
 *  MQTT_CONNECT_TIMEOUT_MS now that the two phases are ticked
 *  separately: MQTT_Connect() always (re)sends CONNECT on every call,
 *  so unlike the TLS handshake it cannot be resumed across
 *  connect_step() ticks — it stays one atomic call, but no longer needs
 *  to share the 30 s budget once meant to cover TLS + CONNECT combined.
 *  CONNACK RTT observed fast (sub-second) against a live broker once
 *  TLS is up (mirrors MQTT-O4's PUBACK/SUBACK observation); sized with
 *  margin for the same WifiDriver read floor as everything else here. */
#define MQTT_CONNACK_TIMEOUT_MS 10000u /**< ~2 retries at the 5 s floor. */
#define MQTT_PUBACK_TIMEOUT_MS 15000u  /**< ~3 retries at the 5 s floor. */
#define MQTT_SUBACK_TIMEOUT_MS                                                                     \
    15000u /**< Mirrors MQTT_PUBACK_TIMEOUT_MS; not yet a                                          \
            *  named open item — same integration-time                                           \
            *  validation applies. */

/** Outstanding QoS 1 record slots (MQTT_InitStatefulQoS). A handful is
 *  enough headroom for this project's single in-flight publish/subscribe
 *  usage pattern (CloudPublisherTask is the sole caller). */
#define MQTT_OUTGOING_PUBLISH_RECORDS 4u
#define MQTT_INCOMING_PUBLISH_RECORDS 4u

/** Timeout budget handed to the TEST-only transport recv path
 *  (prv_transport_recv()'s #ifdef TEST body, still wifitask_recv()-based
 *  — the production path, prv_mbedtls_net_recv(), moved to the
 *  non-blocking wifitask_try_recv() under WIFITASK-O1 Phase 2 and no
 *  longer uses this constant). Below WIFI_RESP_TIMEOUT_MS (5000 ms) this
 *  has no observable effect — wifi_recv() floors to that value regardless
 *  — but it is still the semantically-correct per-poll budget to pass. */
#define MQTT_TRANSPORT_POLL_TIMEOUT_MS 500u

/** WIFITASK-O1 Phase 2 regression fix: every tight "call MQTT_ProcessLoop()
 *  (or connect_step()) until a flag/status changes or a timeout elapses"
 *  loop in this file (mqtt_client_connect(), the QoS 1 PUBACK wait in
 *  mqtt_client_publish(), the SUBACK wait in mqtt_client_subscribe()) used
 *  to be implicitly paced by wifitask_recv()'s own ~5 s block per call — a
 *  real yield point, since that wait happens inside FreeRTOS's blocking
 *  primitives. Now that the underlying recv is wifitask_try_recv(), which
 *  never blocks, a bare tight loop starves WifiTask of any chance to run
 *  its own task and drain the arm queue it depends on — confirmed on
 *  hardware as an immediate, permanent WIFITASK_ERR_NO_RESOURCE spin (arm
 *  queue full forever, nothing ever dequeues it) in mqtt_client_connect().
 *  The QoS 1 publish/subscribe waits have the identical shape and would
 *  hit the same failure in production (not just bring-up) the first time
 *  either is called after Phase 2 — fixed alongside connect() rather than
 *  only where it was first observed. This delay is the yield point that
 *  replaces the one lost when the per-call block went away; matches
 *  WifiTask's own WIFITASK_RECV_ARM_POLL_TICKS cadence, so none of these
 *  loops poll faster than WifiTask itself re-checks the arm queue.
 *  CloudPublisher's own ticked reconnect path (prv_maybe_reconnect(), one
 *  connect_step() call per wake) is unaffected — it already yields via
 *  xTaskNotifyWait() between calls. */
#define MQTT_PROCESS_LOOP_POLL_MS 100u

#ifdef TEST
#define MQTT_CLIENT_TEST_VISIBLE
#else
#define MQTT_CLIENT_TEST_VISIBLE static
#endif

/**
 * @brief Concrete definition of coreMQTT's opaque NetworkContext_t.
 *
 * Holds the mbedTLS session state and the WifiDriver socket handle, per
 * companion §8.2. Embedded by value in mqtt_client_inst — no dynamic
 * allocation.
 */
struct NetworkContext
{
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca_cert;
    mbedtls_x509_crt client_cert;
    mbedtls_pk_context client_key;
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    wifitask_handle_t wifi;
    wifi_socket_t socket;
};

/**
 * @brief MQTT-D8: mqtt_client_connect_step()'s internal phase.
 *
 * Persisted on the instance so successive connect_step() calls resume
 * where the previous one left off, rather than restarting the whole
 * connect sequence.
 */
typedef enum
{
    MQTT_CONN_STATE_IDLE = 0,          /**< No attempt in progress. */
    MQTT_CONN_STATE_TLS_HANDSHAKE = 1, /**< Socket open; TLS handshake ticking. */
    MQTT_CONN_STATE_MQTT_CONNECT = 2,  /**< TLS complete; MQTT CONNECT/CONNACK pending. */
} mqtt_conn_state_t;

/** @brief Internal instance state — hidden from consumers (companion §8.1). */
struct mqtt_client_inst
{
    /* Injected dependencies */
    wifitask_handle_t wifi;
    mqtt_message_cb_t msg_cb;
    mqtt_disconnect_cb_t disconnect_cb;

    /* coreMQTT / mbedTLS contexts */
    MQTTContext_t mqtt_ctx;
    NetworkContext_t net_ctx;
    MQTTFixedBuffer_t fixed_buf;
    uint8_t pkt_buf[MQTT_PKT_BUF_SIZE];

    /* QoS 1 state engine records (MQTT_InitStatefulQoS) */
    MQTTPubAckInfo_t outgoing_records[MQTT_OUTGOING_PUBLISH_RECORDS];
    MQTTPubAckInfo_t incoming_records[MQTT_INCOMING_PUBLISH_RECORDS];

    /* Runtime state */
    bool connected;
    mqtt_stats_t stats;
    bool in_use;

    /* MQTT-D8: mqtt_client_connect_step() ticking state */
    mqtt_conn_state_t connect_state;
    uint32_t tls_handshake_deadline_ms;

    /* Blocking-wait bookkeeping for publish()/subscribe() */
    bool puback_received;
    bool suback_received;
    MQTTSubAckStatus_t suback_status;
};

static struct mqtt_client_inst g_pool[MQTT_CLIENT_MAX_INSTANCES];
static uint8_t g_count;

/* ========================================================================
 * Private helpers
 * ==================================================================== */

static uint32_t prv_get_time_ms(void)
{
    return (uint32_t) (xTaskGetTickCount() * (1000u / configTICK_RATE_HZ));
}

/**
 * @brief Select and start the RNG's 48 MHz kernel clock (PLLSAI1Q).
 *
 * RNG->CR.RNGEN alone is not sufficient: the RNG's *kernel* clock source
 * is a separate selection (RCC->CCIPR.CLK48SEL), distinct from the
 * AHB2ENR peripheral bus-clock gate enabled in prv_entropy_poll(). Reset
 * value does not select a running clock on this board, so without this,
 * RNG->SR.DRDY never asserts and prv_entropy_poll()'s poll loop hangs
 * forever — confirmed on hardware during MqttClient bring-up (see
 * MQTT-O6). PLLSAI1 provides an independent 48 MHz source from the same
 * 4 MHz VCO input CpuDriver already configures for the main PLL (cpu.c):
 * VCO = 4 MHz x N(48) = 192 MHz, /4 (Q) = exactly 48 MHz.
 *
 * Idempotent: safe to call every prv_entropy_poll() invocation, but only
 * does real work once (guarded by s_rng_clock_configured).
 *
 * @note Same documented middleware-register-access exception as
 *       prv_entropy_poll() below — this is the clock-enable half of that
 *       same RNG integration, not a new exception.
 */
static void prv_configure_rng_clock(void)
{
    static bool s_rng_clock_configured = false;
    if (s_rng_clock_configured)
    {
        return;
    }

    RCC->PLLSAI1CFGR = (48U << RCC_PLLSAI1CFGR_PLLSAI1N_Pos) | RCC_PLLSAI1CFGR_PLLSAI1Q_0 |
                       RCC_PLLSAI1CFGR_PLLSAI1QEN;
    RCC->CR |= RCC_CR_PLLSAI1ON;
    while ((RCC->CR & RCC_CR_PLLSAI1RDY) == 0u)
    {
        /* Bounded by hardware PLL lock time (~100 us typical) — same
         * no-software-timeout rationale as prv_entropy_poll() below. */
    }

    RCC->CCIPR = (RCC->CCIPR & ~RCC_CCIPR_CLK48SEL_Msk) | RCC_CCIPR_CLK48SEL_0; /* 01 = PLLSAI1Q */

    s_rng_clock_configured = true;
}

/**
 * @brief mbedTLS entropy source backed by the STM32L475 on-chip RNG.
 *
 * mbedtls_config_gateway.h defines MBEDTLS_NO_PLATFORM_ENTROPY, so
 * mbedTLS has no default entropy source — this function is registered
 * explicitly via mbedtls_entropy_add_source() in prv_tls_connect(). Not
 * described in the companion's §8 internal design (which predates the
 * concrete RNG choice); documented here as the integration glue needed
 * to make REQ-NF-300/301 (TLS 1.2, X.509 mutual auth) work at all. See
 * MQTT-O6 in the companion for the integration-time follow-up.
 *
 * @note Register-level CMSIS access from a middleware module is a
 *       deliberate, narrow exception — RNG has no dedicated driver in
 *       this project (unlike GpioDriver/SpiDriver etc.), and adding one
 *       for a single caller would widen the architecture beyond what
 *       the companion documents.
 */
MQTT_CLIENT_TEST_VISIBLE int prv_entropy_poll(void *data, unsigned char *output, size_t len,
                                              size_t *olen)
{
    (void) data;

    prv_configure_rng_clock();
    RCC->AHB2ENR |= RCC_AHB2ENR_RNGEN;
    RNG->CR |= RNG_CR_RNGEN;

    size_t produced = 0u;
    while (produced < len)
    {
        while ((RNG->SR & RNG_SR_DRDY) == 0u)
        {
            /* Bounded by hardware: the L475 RNG produces a new 32-bit word
             * every ~40 clock cycles once enabled. No timeout guard here —
             * mirrors WifiDriver's rationale (companion §3.5) that a
             * hardware peripheral with a documented worst-case latency
             * does not need a software timeout on top. */
        }

        uint32_t word = RNG->DR;
        size_t chunk = len - produced;
        if (chunk > sizeof(word))
        {
            chunk = sizeof(word);
        }
        (void) memcpy(&output[produced], &word, chunk);
        produced += chunk;
    }

    *olen = produced;
    return 0;
}

/** @brief mbedTLS BIO send callback — the lowest-level socket write. */
MQTT_CLIENT_TEST_VISIBLE int prv_mbedtls_net_send(void *ctx, const unsigned char *buf, size_t len)
{
    NetworkContext_t *net_ctx = (NetworkContext_t *) ctx;

    wifitask_err_t err = wifitask_send(net_ctx->wifi, net_ctx->socket, buf, len);
    if (err == WIFITASK_ERR_OK)
    {
        return (int) len;
    }
    LOG_WARN(MQTT_CLIENT_LOG_MODULE, "net_send: wifitask_send(%u B) failed, err=%d", (unsigned) len,
             (int) err);
    return MBEDTLS_ERR_SSL_WANT_WRITE;
}

/**
 * @brief mbedTLS BIO recv callback — the lowest-level socket read.
 *
 * WIFITASK-O1 Phase 2: routed through wifitask_try_recv() instead of the
 * blocking wifitask_recv(), so a TLS handshake round-trip no longer parks
 * CloudPublisherTask for WifiTask's ~5 s worst case per call (WIFI-O11) —
 * mbedtls_ssl_handshake() already treats MBEDTLS_ERR_SSL_WANT_READ as
 * "not done yet, call me again next tick" (that's what already powers
 * mqtt_client_connect_step()'s ticking, MQTT-D8), so no new state machine
 * is needed here. MQTT_CLIENT_WIFI_RECV_READY_BIT wakes CloudPublisherTask
 * promptly when a background attempt completes (see cloud_publisher.c's
 * prv_task_step()) instead of leaving it to the next 1 Hz stats tick.
 */
MQTT_CLIENT_TEST_VISIBLE int prv_mbedtls_net_recv(void *ctx, unsigned char *buf, size_t len)
{
    NetworkContext_t *net_ctx = (NetworkContext_t *) ctx;

    size_t out_len = 0u;
    wifitask_recv_poll_t poll = WIFITASK_RECV_POLL_PENDING;
    wifitask_err_t err = wifitask_try_recv(net_ctx->wifi, net_ctx->socket, buf, len, &out_len,
                                           MQTT_CLIENT_WIFI_RECV_READY_BIT, &poll);
    if (err == WIFITASK_ERR_NO_RESOURCE)
    {
        /* Arm queue momentarily full — transient, retry next call. */
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    if (err != WIFITASK_ERR_OK)
    {
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }

    switch (poll)
    {
    case WIFITASK_RECV_POLL_READY:
        return (int) out_len;
    case WIFITASK_RECV_POLL_PENDING:
    case WIFITASK_RECV_POLL_NONE:
        return MBEDTLS_ERR_SSL_WANT_READ;
    case WIFITASK_RECV_POLL_ERROR:
    default:
        return MBEDTLS_ERR_SSL_TIMEOUT;
    }
}

/**
 * @brief coreMQTT transport send callback.
 *
 * Production: writes through the TLS session (mbedtls_ssl_write).
 * TEST builds bypass TLS entirely so the real coreMQTT protocol engine
 * can be exercised against synthetic cleartext MQTT frames injected by
 * the (mocked) WifiDriver stub — see companion §13. TLS itself is
 * validated separately via mocked mbedTLS handshake calls (MQTT-T06).
 */
static int32_t prv_transport_send(NetworkContext_t *net_ctx, const void *buf, size_t len)
{
#ifdef TEST
    wifitask_err_t err = wifitask_send(net_ctx->wifi, net_ctx->socket, (const uint8_t *) buf, len);
    return (err == WIFITASK_ERR_OK) ? (int32_t) len : -1;
#else
    int ret = mbedtls_ssl_write(&net_ctx->ssl, (const unsigned char *) buf, len);
    if (ret >= 0)
    {
        return (int32_t) ret;
    }
    if ((ret == MBEDTLS_ERR_SSL_WANT_READ) || (ret == MBEDTLS_ERR_SSL_WANT_WRITE))
    {
        return 0;
    }
    return -1;
#endif
}

/** @brief coreMQTT transport recv callback. See prv_transport_send(). */
static int32_t prv_transport_recv(NetworkContext_t *net_ctx, void *buf, size_t len)
{
#ifdef TEST
    size_t out_len = 0u;
    wifitask_err_t err = wifitask_recv(net_ctx->wifi, net_ctx->socket, (uint8_t *) buf, len,
                                       &out_len, MQTT_TRANSPORT_POLL_TIMEOUT_MS);
    if (err == WIFITASK_ERR_OK)
    {
        return (int32_t) out_len;
    }
    /* This must stay the raw WifiDriver WIFI_ERR_TIMEOUT (4), not
     * WIFITASK_ERR_TIMEOUT (3, a different condition) — see wifi-task.md
     * §10 WIFITASK-O3. TEST-only: this path still uses the blocking
     * wifitask_recv(); the production recv (prv_mbedtls_net_recv() above)
     * moved to wifitask_try_recv()'s poll-enum contract under
     * WIFITASK-O1 Phase 2 and no longer compares against this value. Do
     * not rename. */
    if (err == (wifitask_err_t) WIFI_ERR_TIMEOUT)
    {
        return 0;
    }
    return -1;
#else
    int ret = mbedtls_ssl_read(&net_ctx->ssl, (unsigned char *) buf, len);
    if (ret >= 0)
    {
        return (int32_t) ret;
    }
    if ((ret == MBEDTLS_ERR_SSL_WANT_READ) || (ret == MBEDTLS_ERR_SSL_WANT_WRITE))
    {
        return 0;
    }
    return -1;
#endif
}

/**
 * @brief coreMQTT event callback — dispatches inbound acks and publishes.
 *
 * Registered once via MQTT_Init(). Invoked from MQTT_ProcessLoop() for
 * every incoming packet: PUBACK/SUBACK completion for the blocking waits
 * in mqtt_client_publish()/mqtt_client_subscribe(), and inbound PUBLISH
 * delivery via msg_cb (companion §1, §9.1 SD-07/SD-08).
 */
static void prv_event_callback(MQTTContext_t *ctx, MQTTPacketInfo_t *packet_info,
                               MQTTDeserializedInfo_t *deserialized_info)
{
    /* MQTTContext_t is not the first member of mqtt_client_inst (wifi,
     * msg_cb, disconnect_cb precede it) — a straight pointer cast would
     * silently read every field at the wrong offset. */
    struct mqtt_client_inst *inst =
        (struct mqtt_client_inst *) ((uint8_t *) ctx - offsetof(struct mqtt_client_inst, mqtt_ctx));

    if ((packet_info->type & 0xF0u) == MQTT_PACKET_TYPE_PUBLISH)
    {
        MQTTPublishInfo_t *publish_info = deserialized_info->pPublishInfo;
        if ((publish_info != NULL) && (inst->msg_cb != NULL))
        {
            inst->msg_cb(publish_info->pTopicName, publish_info->topicNameLength,
                         (const uint8_t *) publish_info->pPayload,
                         (uint32_t) publish_info->payloadLength);
        }
        return;
    }

    switch (packet_info->type)
    {
    case MQTT_PACKET_TYPE_PUBACK:
        inst->stats.publishes_acked++;
        inst->puback_received = true;
        break;

    case MQTT_PACKET_TYPE_SUBACK:
    {
        uint8_t *payload = NULL;
        size_t payload_size = 0u;
        if (MQTT_GetSubAckStatusCodes(packet_info, &payload, &payload_size) == MQTTSuccess &&
            (payload_size > 0u))
        {
            inst->suback_status = (MQTTSubAckStatus_t) payload[0];
        }
        else
        {
            inst->suback_status = MQTTSubAckFailure;
        }
        inst->suback_received = true;
        break;
    }

    case MQTT_PACKET_TYPE_PINGRESP:
    default:
        break;
    }
}

/**
 * @brief One-time TLS context setup: seed the RNG, load CA/client cert +
 *        key (DER, pointers only — REQ-NF-302), configure the
 *        ECDHE-RSA-AES128-GCM-SHA256 client session, and wire the BIO.
 *
 * No handshake I/O here — every step is local (parsing, config), so
 * this is safe to run in full within a single connect_step() tick
 * (MQTT-D8). The handshake itself is ticked separately by
 * prv_tls_handshake_step().
 */
static mqtt_client_err_t prv_tls_setup(struct mqtt_client_inst *inst, const mqtt_connect_cfg_t *cfg)
{
    NetworkContext_t *net_ctx = &inst->net_ctx;

    mbedtls_entropy_init(&net_ctx->entropy);
    (void) mbedtls_entropy_add_source(&net_ctx->entropy, prv_entropy_poll, NULL, 32u,
                                      MBEDTLS_ENTROPY_SOURCE_STRONG);
    mbedtls_ctr_drbg_init(&net_ctx->ctr_drbg);
    if (mbedtls_ctr_drbg_seed(&net_ctx->ctr_drbg, mbedtls_entropy_func, &net_ctx->entropy, NULL,
                              0u) != 0)
    {
        return MQTT_CLIENT_ERR_TLS_FAIL;
    }

    mbedtls_x509_crt_init(&net_ctx->ca_cert);
    if (mbedtls_x509_crt_parse_der(&net_ctx->ca_cert, cfg->ca_cert_der, cfg->ca_cert_len) != 0)
    {
        return MQTT_CLIENT_ERR_TLS_FAIL;
    }

    mbedtls_x509_crt_init(&net_ctx->client_cert);
    if (mbedtls_x509_crt_parse_der(&net_ctx->client_cert, cfg->client_cert_der,
                                   cfg->client_cert_len) != 0)
    {
        return MQTT_CLIENT_ERR_TLS_FAIL;
    }

    mbedtls_pk_init(&net_ctx->client_key);
    if (mbedtls_pk_parse_key(&net_ctx->client_key, cfg->client_key_der, cfg->client_key_len, NULL,
                             0u, mbedtls_ctr_drbg_random, &net_ctx->ctr_drbg) != 0)
    {
        return MQTT_CLIENT_ERR_TLS_FAIL;
    }

    mbedtls_ssl_config_init(&net_ctx->conf);
    if (mbedtls_ssl_config_defaults(&net_ctx->conf, MBEDTLS_SSL_IS_CLIENT,
                                    MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT) != 0)
    {
        return MQTT_CLIENT_ERR_TLS_FAIL;
    }
    mbedtls_ssl_conf_authmode(&net_ctx->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&net_ctx->conf, &net_ctx->ca_cert, NULL);
    if (mbedtls_ssl_conf_own_cert(&net_ctx->conf, &net_ctx->client_cert, &net_ctx->client_key) != 0)
    {
        return MQTT_CLIENT_ERR_TLS_FAIL;
    }
    mbedtls_ssl_conf_rng(&net_ctx->conf, mbedtls_ctr_drbg_random, &net_ctx->ctr_drbg);

    mbedtls_ssl_init(&net_ctx->ssl);
    if (mbedtls_ssl_setup(&net_ctx->ssl, &net_ctx->conf) != 0)
    {
        return MQTT_CLIENT_ERR_TLS_FAIL;
    }
    if (mbedtls_ssl_set_hostname(&net_ctx->ssl, cfg->broker_endpoint) != 0)
    {
        return MQTT_CLIENT_ERR_TLS_FAIL;
    }
    mbedtls_ssl_set_bio(&net_ctx->ssl, net_ctx, prv_mbedtls_net_send, prv_mbedtls_net_recv, NULL);

    return MQTT_CLIENT_ERR_OK;
}

/**
 * @brief Advance the TLS handshake by exactly one mbedtls_ssl_handshake()
 *        call (MQTT-D8).
 *
 * mbedTLS tracks handshake progress internally in net_ctx->ssl between
 * calls, so a single external call each tick correctly resumes where
 * the previous one left off — this is what makes the handshake
 * tickable at all without protocol-level bookkeeping of our own.
 * inst->tls_handshake_deadline_ms (set once when entering
 * MQTT_CONN_STATE_TLS_HANDSHAKE) bounds the *sequence* of ticks, not
 * any single call.
 */
static mqtt_client_err_t prv_tls_handshake_step(struct mqtt_client_inst *inst)
{
    int ret = mbedtls_ssl_handshake(&inst->net_ctx.ssl);
    if (ret == 0)
    {
        return MQTT_CLIENT_ERR_OK;
    }
    if ((ret == MBEDTLS_ERR_SSL_WANT_READ) || (ret == MBEDTLS_ERR_SSL_WANT_WRITE))
    {
        if (prv_get_time_ms() >= inst->tls_handshake_deadline_ms)
        {
            LOG_ERROR(MQTT_CLIENT_LOG_MODULE,
                      "TLS handshake timed out after %lu ms, still waiting on %s",
                      (unsigned long) MQTT_CONNECT_TIMEOUT_MS,
                      (ret == MBEDTLS_ERR_SSL_WANT_READ) ? "WANT_READ" : "WANT_WRITE");
            return MQTT_CLIENT_ERR_TLS_FAIL;
        }
        return MQTT_CLIENT_ERR_IN_PROGRESS;
    }

    LOG_ERROR(MQTT_CLIENT_LOG_MODULE, "TLS handshake failed: -0x%04x", (unsigned int) -ret);
    return MQTT_CLIENT_ERR_TLS_FAIL;
}

static void prv_tls_close(struct mqtt_client_inst *inst)
{
    (void) mbedtls_ssl_close_notify(&inst->net_ctx.ssl);
    mbedtls_ssl_free(&inst->net_ctx.ssl);
    mbedtls_ssl_config_free(&inst->net_ctx.conf);
    mbedtls_x509_crt_free(&inst->net_ctx.ca_cert);
    mbedtls_x509_crt_free(&inst->net_ctx.client_cert);
    mbedtls_pk_free(&inst->net_ctx.client_key);
    mbedtls_ctr_drbg_free(&inst->net_ctx.ctr_drbg);
    mbedtls_entropy_free(&inst->net_ctx.entropy);
}

/**
 * @brief Release the TLS session and underlying WifiDriver socket.
 *
 * Shared by the graceful (mqtt_client_disconnect()) and abnormal
 * (mqtt_client_process() keep-alive/recv/send failure) teardown paths.
 * Both must release the same two resources: WifiDriver's socket table
 * has only WIFI_MAX_SOCKETS (4) slots, and wifitask_close_socket() is the
 * only thing that frees one (wifi_driver.c §3.7) — skipping it on the
 * abnormal path leaks a slot per unexpected disconnect, exhausting the
 * table after a handful of broker drops and permanently blocking
 * reconnection until reboot (see mqtt-client.md MQTT-O8).
 */
static void prv_teardown_connection(struct mqtt_client_inst *inst)
{
    prv_tls_close(inst);
    (void) wifitask_close_socket(inst->wifi, inst->net_ctx.socket);
    inst->connected = false;
}

/* ========================================================================
 * IMqttClient
 * ==================================================================== */

mqtt_client_err_t mqtt_client_create(const mqtt_client_config_t *config,
                                     mqtt_client_handle_t *handle)
{
    if ((config == NULL) || (handle == NULL) || (config->msg_cb == NULL) ||
        (config->disconnect_cb == NULL))
    {
        return MQTT_CLIENT_ERR_NULL_PTR;
    }

    if (g_count >= MQTT_CLIENT_MAX_INSTANCES)
    {
        return MQTT_CLIENT_ERR_NO_RESOURCE;
    }

    struct mqtt_client_inst *inst = &g_pool[g_count];
    g_count++;

    (void) memset(inst, 0, sizeof(*inst));
    inst->wifi = config->wifi;
    inst->msg_cb = config->msg_cb;
    inst->disconnect_cb = config->disconnect_cb;
    inst->in_use = true;

    *handle = inst;
    return MQTT_CLIENT_ERR_OK;
}

/**
 * @brief MQTT_CONN_STATE_IDLE tick: open the TCP socket and run one-time
 *        TLS context setup (MQTT-D8).
 *
 * Atomic — wifitask_open_socket() bundles several AT commands (relayed
 * through WifiTask, still a synchronous relocate) with no way to report
 * partial progress (mqtt-client.md MQTT-O9) — but TLS setup itself is
 * local/non-blocking, so folding it into the same tick costs nothing
 * extra. Transitions to MQTT_CONN_STATE_TLS_HANDSHAKE on success; the
 * first actual mbedtls_ssl_handshake() call happens on the *next* tick,
 * keeping this tick's own worst-case block to wifitask_open_socket() alone.
 */
static mqtt_client_err_t prv_connect_step_idle(struct mqtt_client_inst *inst,
                                               const mqtt_connect_cfg_t *cfg)
{
    inst->stats.connect_attempts++;

    wifi_socket_t socket;
    wifitask_err_t wifi_status = wifitask_open_socket(
        inst->wifi, WIFI_SOCKET_TCP, cfg->broker_endpoint, cfg->broker_port, &socket);
    if (wifi_status != WIFITASK_ERR_OK)
    {
        LOG_ERROR(MQTT_CLIENT_LOG_MODULE, "wifitask_open_socket failed: %d", (int) wifi_status);
        return MQTT_CLIENT_ERR_CONNECT_FAIL;
    }

    inst->net_ctx.wifi = inst->wifi;
    inst->net_ctx.socket = socket;

    mqtt_client_err_t tls_result = prv_tls_setup(inst, cfg);
    if (tls_result != MQTT_CLIENT_ERR_OK)
    {
        /* Safe even on a partially-completed setup: every mbedTLS context
         * reached its _init() call before any step that can fail, and
         * mbedTLS guarantees _free() is safe on an _init()'d context
         * regardless of how much setup completed after that. */
        prv_tls_close(inst);
        (void) wifitask_close_socket(inst->wifi, socket);
        return tls_result;
    }

    inst->tls_handshake_deadline_ms = prv_get_time_ms() + MQTT_CONNECT_TIMEOUT_MS;
    inst->connect_state = MQTT_CONN_STATE_TLS_HANDSHAKE;
    return MQTT_CLIENT_ERR_IN_PROGRESS;
}

/**
 * @brief MQTT_CONN_STATE_TLS_HANDSHAKE tick (MQTT-D8).
 *
 * On handshake completion, transitions to MQTT_CONN_STATE_MQTT_CONNECT
 * and returns IN_PROGRESS rather than falling through into that phase
 * in the same tick — keeps every tick bounded to one phase's own
 * blocking work.
 */
static mqtt_client_err_t prv_connect_step_tls_handshake(struct mqtt_client_inst *inst)
{
    mqtt_client_err_t status = prv_tls_handshake_step(inst);
    if (status == MQTT_CLIENT_ERR_TLS_FAIL)
    {
        prv_tls_close(inst);
        (void) wifitask_close_socket(inst->wifi, inst->net_ctx.socket);
        inst->connect_state = MQTT_CONN_STATE_IDLE;
        return status;
    }
    if (status == MQTT_CLIENT_ERR_OK)
    {
        inst->connect_state = MQTT_CONN_STATE_MQTT_CONNECT;
    }
    return MQTT_CLIENT_ERR_IN_PROGRESS;
}

/**
 * @brief MQTT_CONN_STATE_MQTT_CONNECT tick (MQTT-D8).
 *
 * Atomic — coreMQTT's MQTT_Connect() always (re)sends a CONNECT packet
 * when called, so unlike the TLS handshake it cannot be resumed across
 * ticks without risking a duplicate CONNECT on the same session. Bounded
 * by MQTT_CONNACK_TIMEOUT_MS rather than the (now TLS-only)
 * MQTT_CONNECT_TIMEOUT_MS.
 */
static mqtt_client_err_t prv_connect_step_mqtt_connect(struct mqtt_client_inst *inst,
                                                       const mqtt_connect_cfg_t *cfg)
{
    static const TransportInterface_t s_transport_template = {
        .recv = prv_transport_recv,
        .send = prv_transport_send,
        .writev = NULL,
    };
    TransportInterface_t transport = s_transport_template;
    transport.pNetworkContext = &inst->net_ctx;

    inst->fixed_buf.pBuffer = inst->pkt_buf;
    inst->fixed_buf.size = MQTT_PKT_BUF_SIZE;

    if (MQTT_Init(&inst->mqtt_ctx, &transport, prv_get_time_ms, prv_event_callback,
                  &inst->fixed_buf) != MQTTSuccess)
    {
        prv_tls_close(inst);
        (void) wifitask_close_socket(inst->wifi, inst->net_ctx.socket);
        inst->connect_state = MQTT_CONN_STATE_IDLE;
        return MQTT_CLIENT_ERR_CONNECT_FAIL;
    }

    if (MQTT_InitStatefulQoS(&inst->mqtt_ctx, inst->outgoing_records, MQTT_OUTGOING_PUBLISH_RECORDS,
                             inst->incoming_records, MQTT_INCOMING_PUBLISH_RECORDS) != MQTTSuccess)
    {
        prv_tls_close(inst);
        (void) wifitask_close_socket(inst->wifi, inst->net_ctx.socket);
        inst->connect_state = MQTT_CONN_STATE_IDLE;
        return MQTT_CLIENT_ERR_CONNECT_FAIL;
    }

    MQTTConnectInfo_t connect_info = {0};
    connect_info.cleanSession = true;
    connect_info.keepAliveSeconds = cfg->keep_alive_s;
    connect_info.pClientIdentifier = cfg->client_id;
    connect_info.clientIdentifierLength = (uint16_t) strlen(cfg->client_id);

    bool session_present = false;
    MQTTStatus_t connect_status = MQTT_Connect(&inst->mqtt_ctx, &connect_info, NULL,
                                               MQTT_CONNACK_TIMEOUT_MS, &session_present);
    if (connect_status != MQTTSuccess)
    {
        LOG_ERROR(MQTT_CLIENT_LOG_MODULE, "MQTT_Connect failed: %d", (int) connect_status);
        prv_tls_close(inst);
        (void) wifitask_close_socket(inst->wifi, inst->net_ctx.socket);
        inst->connect_state = MQTT_CONN_STATE_IDLE;
        return MQTT_CLIENT_ERR_CONNECT_FAIL;
    }

    inst->connected = true;
    inst->stats.connect_ok++;
    if (inst->stats.connect_ok > 1u)
    {
        inst->stats.reconnect_count++;
    }
    inst->connect_state = MQTT_CONN_STATE_IDLE;

    LOG_INFO(MQTT_CLIENT_LOG_MODULE, "Connected to %s:%u", cfg->broker_endpoint,
             (unsigned int) cfg->broker_port);
    return MQTT_CLIENT_ERR_OK;
}

mqtt_client_err_t mqtt_client_connect_step(mqtt_client_handle_t handle,
                                           const mqtt_connect_cfg_t *cfg)
{
    if ((handle == NULL) || (cfg == NULL))
    {
        return MQTT_CLIENT_ERR_NULL_PTR;
    }

    switch (handle->connect_state)
    {
    case MQTT_CONN_STATE_TLS_HANDSHAKE:
        return prv_connect_step_tls_handshake(handle);
    case MQTT_CONN_STATE_MQTT_CONNECT:
        return prv_connect_step_mqtt_connect(handle, cfg);
    case MQTT_CONN_STATE_IDLE:
    default:
        return prv_connect_step_idle(handle, cfg);
    }
}

mqtt_client_err_t mqtt_client_connect(mqtt_client_handle_t handle, const mqtt_connect_cfg_t *cfg)
{
    if ((handle == NULL) || (cfg == NULL))
    {
        return MQTT_CLIENT_ERR_NULL_PTR;
    }

    mqtt_client_err_t status;
    do
    {
        status = mqtt_client_connect_step(handle, cfg);
        /* See MQTT_PROCESS_LOOP_POLL_MS's own doc comment — this yield is
         * required, not cosmetic, since WIFITASK-O1 Phase 2. #ifndef TEST:
         * no unit test drives connect_step() into more than a handful of
         * deterministic CMock-programmed IN_PROGRESS returns, so this is
         * never exercised host-side and needs no FreeRTOS mock. */
#ifndef TEST
        if (status == MQTT_CLIENT_ERR_IN_PROGRESS)
        {
            vTaskDelay(pdMS_TO_TICKS(MQTT_PROCESS_LOOP_POLL_MS));
        }
#endif
    } while (status == MQTT_CLIENT_ERR_IN_PROGRESS);

    return status;
}

mqtt_client_err_t mqtt_client_disconnect(mqtt_client_handle_t handle)
{
    if (handle == NULL)
    {
        return MQTT_CLIENT_ERR_NULL_PTR;
    }
    if (!handle->connected)
    {
        return MQTT_CLIENT_ERR_NOT_CONNECTED;
    }

    (void) MQTT_Disconnect(&handle->mqtt_ctx);
    prv_teardown_connection(handle);

    return MQTT_CLIENT_ERR_OK;
}

bool mqtt_client_is_connected(mqtt_client_handle_t handle)
{
    if (handle == NULL)
    {
        return false;
    }
    return handle->connected;
}

mqtt_client_err_t mqtt_client_publish(mqtt_client_handle_t handle, const char *topic,
                                      const uint8_t *payload, uint32_t len, mqtt_qos_t qos)
{
    if ((handle == NULL) || (topic == NULL))
    {
        return MQTT_CLIENT_ERR_NULL_PTR;
    }
    if (!handle->connected)
    {
        return MQTT_CLIENT_ERR_NOT_CONNECTED;
    }

    MQTTPublishInfo_t publish_info = {0};
    publish_info.qos = (qos == MQTT_QOS_1) ? MQTTQoS1 : MQTTQoS0;
    publish_info.pTopicName = topic;
    publish_info.topicNameLength = (uint16_t) strlen(topic);
    publish_info.pPayload = payload;
    publish_info.payloadLength = len;

    uint16_t packet_id = (qos == MQTT_QOS_1) ? MQTT_GetPacketId(&handle->mqtt_ctx) : 0u;
    handle->puback_received = false;

    if (MQTT_Publish(&handle->mqtt_ctx, &publish_info, packet_id) != MQTTSuccess)
    {
        handle->stats.publish_failures++;
        return MQTT_CLIENT_ERR_PUBLISH_FAIL;
    }
    handle->stats.publishes_sent++;

    if (qos == MQTT_QOS_0)
    {
        return MQTT_CLIENT_ERR_OK;
    }

    uint32_t start = prv_get_time_ms();
    while (!handle->puback_received)
    {
        (void) MQTT_ProcessLoop(&handle->mqtt_ctx);
        if (handle->puback_received)
        {
            break;
        }
        if ((prv_get_time_ms() - start) >= MQTT_PUBACK_TIMEOUT_MS)
        {
            handle->stats.publish_failures++;
            return MQTT_CLIENT_ERR_PUBLISH_FAIL;
        }
        /* See MQTT_PROCESS_LOOP_POLL_MS's own doc comment. */
#ifndef TEST
        vTaskDelay(pdMS_TO_TICKS(MQTT_PROCESS_LOOP_POLL_MS));
#endif
    }

    return MQTT_CLIENT_ERR_OK;
}

mqtt_client_err_t mqtt_client_subscribe(mqtt_client_handle_t handle, const char *topic,
                                        mqtt_qos_t qos)
{
    if ((handle == NULL) || (topic == NULL))
    {
        return MQTT_CLIENT_ERR_NULL_PTR;
    }
    if (!handle->connected)
    {
        return MQTT_CLIENT_ERR_NOT_CONNECTED;
    }

    MQTTSubscribeInfo_t sub_info = {0};
    sub_info.qos = (qos == MQTT_QOS_1) ? MQTTQoS1 : MQTTQoS0;
    sub_info.pTopicFilter = topic;
    sub_info.topicFilterLength = (uint16_t) strlen(topic);

    uint16_t packet_id = MQTT_GetPacketId(&handle->mqtt_ctx);
    handle->suback_received = false;

    if (MQTT_Subscribe(&handle->mqtt_ctx, &sub_info, 1u, packet_id) != MQTTSuccess)
    {
        handle->stats.subscribe_failures++;
        return MQTT_CLIENT_ERR_SUBSCRIBE_FAIL;
    }

    uint32_t start = prv_get_time_ms();
    while (!handle->suback_received)
    {
        (void) MQTT_ProcessLoop(&handle->mqtt_ctx);
        if (handle->suback_received)
        {
            break;
        }
        if ((prv_get_time_ms() - start) >= MQTT_SUBACK_TIMEOUT_MS)
        {
            handle->stats.subscribe_failures++;
            return MQTT_CLIENT_ERR_SUBSCRIBE_FAIL;
        }
        /* See MQTT_PROCESS_LOOP_POLL_MS's own doc comment. */
#ifndef TEST
        vTaskDelay(pdMS_TO_TICKS(MQTT_PROCESS_LOOP_POLL_MS));
#endif
    }

    if (handle->suback_status == MQTTSubAckFailure)
    {
        handle->stats.subscribe_failures++;
        return MQTT_CLIENT_ERR_SUBSCRIBE_FAIL;
    }

    return MQTT_CLIENT_ERR_OK;
}

mqtt_client_err_t mqtt_client_process(mqtt_client_handle_t handle)
{
    if (handle == NULL)
    {
        return MQTT_CLIENT_ERR_NULL_PTR;
    }
    if (!handle->connected)
    {
        return MQTT_CLIENT_ERR_OK;
    }

    MQTTStatus_t status = MQTT_ProcessLoop(&handle->mqtt_ctx);
    if ((status == MQTTKeepAliveTimeout) || (status == MQTTRecvFailed) ||
        (status == MQTTSendFailed))
    {
        LOG_WARN(MQTT_CLIENT_LOG_MODULE, "Connection lost: %d", (int) status);
        /* Release the socket and TLS session before notifying the caller —
         * see prv_teardown_connection()'s doc comment (MQTT-O8). */
        prv_teardown_connection(handle);
        if (handle->disconnect_cb != NULL)
        {
            handle->disconnect_cb();
        }
    }

    return MQTT_CLIENT_ERR_OK;
}

/* ========================================================================
 * IMqttStats
 * ==================================================================== */

mqtt_client_err_t mqtt_client_get_stats(mqtt_client_handle_t handle, mqtt_stats_t *stats_out)
{
    if ((handle == NULL) || (stats_out == NULL))
    {
        return MQTT_CLIENT_ERR_NULL_PTR;
    }
    *stats_out = handle->stats;
    return MQTT_CLIENT_ERR_OK;
}

mqtt_client_err_t mqtt_client_reset_stats(mqtt_client_handle_t handle)
{
    if (handle == NULL)
    {
        return MQTT_CLIENT_ERR_NULL_PTR;
    }
    (void) memset(&handle->stats, 0, sizeof(handle->stats));
    return MQTT_CLIENT_ERR_OK;
}

#ifdef TEST
void mqtt_client_reset_for_test(void)
{
    (void) memset(g_pool, 0, sizeof(g_pool));
    g_count = 0u;
}
#endif
