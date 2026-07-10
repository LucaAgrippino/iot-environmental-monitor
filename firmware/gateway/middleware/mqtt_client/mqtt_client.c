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
#define MQTT_CONNECT_TIMEOUT_MS 30000u /**< ~6 retries at the 5 s floor. */
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

/** Timeout budget handed to each individual transport-level socket
 *  call. Below WIFI_RESP_TIMEOUT_MS (5000 ms) this has no observable
 *  effect — wifi_recv() floors to that value regardless — but it is
 *  still the semantically-correct per-poll budget to pass (a future
 *  WifiDriver revision, or swapping the transport, might honour it
 *  directly), and it bounds mqtt_client_process()'s own non-blocking
 *  contract when data genuinely is available immediately. */
#define MQTT_TRANSPORT_POLL_TIMEOUT_MS 500u

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
    wifi_handle_t wifi;
    wifi_socket_t socket;
};

/** @brief Internal instance state — hidden from consumers (companion §8.1). */
struct mqtt_client_inst
{
    /* Injected dependencies */
    wifi_handle_t wifi;
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

    wifi_err_t err = wifi_send(net_ctx->wifi, net_ctx->socket, buf, len);
    if (err == WIFI_ERR_OK)
    {
        return (int) len;
    }
    return MBEDTLS_ERR_SSL_WANT_WRITE;
}

/** @brief mbedTLS BIO recv callback — the lowest-level socket read. */
MQTT_CLIENT_TEST_VISIBLE int prv_mbedtls_net_recv(void *ctx, unsigned char *buf, size_t len)
{
    NetworkContext_t *net_ctx = (NetworkContext_t *) ctx;

    size_t out_len = 0u;
    wifi_err_t err = wifi_recv(net_ctx->wifi, net_ctx->socket, buf, len, &out_len,
                               MQTT_TRANSPORT_POLL_TIMEOUT_MS);
    if (err == WIFI_ERR_OK)
    {
        return (int) out_len;
    }
    if (err == WIFI_ERR_TIMEOUT)
    {
        return MBEDTLS_ERR_SSL_WANT_READ;
    }
    return MBEDTLS_ERR_SSL_TIMEOUT;
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
    wifi_err_t err = wifi_send(net_ctx->wifi, net_ctx->socket, (const uint8_t *) buf, len);
    return (err == WIFI_ERR_OK) ? (int32_t) len : -1;
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
    wifi_err_t err = wifi_recv(net_ctx->wifi, net_ctx->socket, (uint8_t *) buf, len, &out_len,
                               MQTT_TRANSPORT_POLL_TIMEOUT_MS);
    if (err == WIFI_ERR_OK)
    {
        return (int32_t) out_len;
    }
    if (err == WIFI_ERR_TIMEOUT)
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
 * @brief Establish the TLS 1.2 session with X.509 mutual auth.
 *
 * Sequence per companion §3: seed the RNG, load CA/client cert + key
 * (DER, pointers only — REQ-NF-302), configure the ECDHE-RSA-AES128-
 * GCM-SHA256 client session, and run the handshake bounded by
 * MQTT_CONNECT_TIMEOUT_MS.
 */
static mqtt_client_err_t prv_tls_connect(struct mqtt_client_inst *inst,
                                         const mqtt_connect_cfg_t *cfg)
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

    uint32_t start = prv_get_time_ms();
    int ret = mbedtls_ssl_handshake(&net_ctx->ssl);
    while ((ret == MBEDTLS_ERR_SSL_WANT_READ) || (ret == MBEDTLS_ERR_SSL_WANT_WRITE))
    {
        if ((prv_get_time_ms() - start) >= MQTT_CONNECT_TIMEOUT_MS)
        {
            LOG_ERROR(MQTT_CLIENT_LOG_MODULE,
                      "TLS handshake timed out after %lu ms, still waiting on %s",
                      (unsigned long) MQTT_CONNECT_TIMEOUT_MS,
                      (ret == MBEDTLS_ERR_SSL_WANT_READ) ? "WANT_READ" : "WANT_WRITE");
            return MQTT_CLIENT_ERR_TLS_FAIL;
        }
        ret = mbedtls_ssl_handshake(&net_ctx->ssl);
    }
    if (ret != 0)
    {
        LOG_ERROR(MQTT_CLIENT_LOG_MODULE, "TLS handshake failed: -0x%04x", (unsigned int) -ret);
        return MQTT_CLIENT_ERR_TLS_FAIL;
    }

    return MQTT_CLIENT_ERR_OK;
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
 * has only WIFI_MAX_SOCKETS (4) slots, and wifi_close_socket() is the
 * only thing that frees one (wifi_driver.c §3.7) — skipping it on the
 * abnormal path leaks a slot per unexpected disconnect, exhausting the
 * table after a handful of broker drops and permanently blocking
 * reconnection until reboot (see mqtt-client.md MQTT-O8).
 */
static void prv_teardown_connection(struct mqtt_client_inst *inst)
{
    prv_tls_close(inst);
    (void) wifi_close_socket(inst->wifi, inst->net_ctx.socket);
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

mqtt_client_err_t mqtt_client_connect(mqtt_client_handle_t handle, const mqtt_connect_cfg_t *cfg)
{
    if ((handle == NULL) || (cfg == NULL))
    {
        return MQTT_CLIENT_ERR_NULL_PTR;
    }

    handle->stats.connect_attempts++;

    wifi_socket_t socket;
    wifi_err_t wifi_status = wifi_open_socket(handle->wifi, WIFI_SOCKET_TCP, cfg->broker_endpoint,
                                              cfg->broker_port, &socket);
    if (wifi_status != WIFI_ERR_OK)
    {
        LOG_ERROR(MQTT_CLIENT_LOG_MODULE, "wifi_open_socket failed: %d", (int) wifi_status);
        return MQTT_CLIENT_ERR_CONNECT_FAIL;
    }

    handle->net_ctx.wifi = handle->wifi;
    handle->net_ctx.socket = socket;

    mqtt_client_err_t tls_result = prv_tls_connect(handle, cfg);
    if (tls_result != MQTT_CLIENT_ERR_OK)
    {
        /* Safe even on a partially-completed handshake: every mbedTLS
         * context reached its _init() call before any step that can fail,
         * and mbedTLS guarantees _free() is safe on an _init()'d context
         * regardless of how much setup completed after that. Skipping this
         * would leave stale TLS state behind for the next connect attempt. */
        prv_tls_close(handle);
        (void) wifi_close_socket(handle->wifi, socket);
        return tls_result;
    }

    static const TransportInterface_t s_transport_template = {
        .recv = prv_transport_recv,
        .send = prv_transport_send,
        .writev = NULL,
    };
    TransportInterface_t transport = s_transport_template;
    transport.pNetworkContext = &handle->net_ctx;

    handle->fixed_buf.pBuffer = handle->pkt_buf;
    handle->fixed_buf.size = MQTT_PKT_BUF_SIZE;

    if (MQTT_Init(&handle->mqtt_ctx, &transport, prv_get_time_ms, prv_event_callback,
                  &handle->fixed_buf) != MQTTSuccess)
    {
        prv_tls_close(handle);
        (void) wifi_close_socket(handle->wifi, socket);
        return MQTT_CLIENT_ERR_CONNECT_FAIL;
    }

    if (MQTT_InitStatefulQoS(&handle->mqtt_ctx, handle->outgoing_records,
                             MQTT_OUTGOING_PUBLISH_RECORDS, handle->incoming_records,
                             MQTT_INCOMING_PUBLISH_RECORDS) != MQTTSuccess)
    {
        prv_tls_close(handle);
        (void) wifi_close_socket(handle->wifi, socket);
        return MQTT_CLIENT_ERR_CONNECT_FAIL;
    }

    MQTTConnectInfo_t connect_info = {0};
    connect_info.cleanSession = true;
    connect_info.keepAliveSeconds = cfg->keep_alive_s;
    connect_info.pClientIdentifier = cfg->client_id;
    connect_info.clientIdentifierLength = (uint16_t) strlen(cfg->client_id);

    bool session_present = false;
    MQTTStatus_t connect_status = MQTT_Connect(&handle->mqtt_ctx, &connect_info, NULL,
                                               MQTT_CONNECT_TIMEOUT_MS, &session_present);
    if (connect_status != MQTTSuccess)
    {
        LOG_ERROR(MQTT_CLIENT_LOG_MODULE, "MQTT_Connect failed: %d", (int) connect_status);
        prv_tls_close(handle);
        (void) wifi_close_socket(handle->wifi, socket);
        return MQTT_CLIENT_ERR_CONNECT_FAIL;
    }

    handle->connected = true;
    handle->stats.connect_ok++;
    if (handle->stats.connect_ok > 1u)
    {
        handle->stats.reconnect_count++;
    }

    LOG_INFO(MQTT_CLIENT_LOG_MODULE, "Connected to %s:%u", cfg->broker_endpoint,
             (unsigned int) cfg->broker_port);
    return MQTT_CLIENT_ERR_OK;
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
