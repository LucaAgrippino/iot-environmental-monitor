/**
 * @file test_mqtt_client.c
 * @brief Unity unit tests for MqttClient — MQTT-T01 through MQTT-T19.
 *
 * mbedTLS's entropy.h and ctr_drbg.h are CMock-mocked directly from the
 * real vendor headers. ssl.h, pk.h, and x509_crt.h cannot: they either
 * hide struct fields behind an MBEDTLS_PRIVATE() macro or declare
 * functions with opaque-type parameters (e.g. mbedtls_pk_setup()'s
 * mbedtls_pk_info_t), both of which defeat CMock's parser or its
 * generated sizeof()-based comparison. tests/support/mbedtls_ssl_mockable.h,
 * mbedtls_pk_mockable.h, and mbedtls_x509_crt_mockable.h stand in as
 * CMock-generatable proxies declaring only the functions mqtt_client.c
 * actually calls, with identical signatures — see those files for the
 * full rationale.
 *
 * WifiDriver is hand-stubbed by defining wifi_open_socket/send/recv/
 * close_socket directly against the real wifi_driver.h prototypes
 * (pulled in transitively via mqtt_client.h); Logger is hand-stubbed per
 * tests/support/logger_stub.h (Ceedling auto-link avoidance — see the
 * comment above the WifiDriver stub section below, and logger_stub.h,
 * for why each takes a different shape). Crucially, prv_transport_send()/
 * prv_transport_recv() in
 * mqtt_client.c bypass mbedTLS entirely under #ifdef TEST, so the real
 * coreMQTT protocol engine parses cleartext MQTT frames injected here
 * through the WifiDriver stub's recv queue (companion §13: "Transport
 * layer is stubbed ... inject synthetic CONNACK, PUBACK, SUBACK").
 * TLS itself is exercised separately by mocking the mbedTLS handshake
 * call (MQTT-T06).
 */

#include "unity.h"

#include <string.h>

#include "mock_ctr_drbg.h"
#include "mock_entropy.h"
#include "mock_mbedtls_pk_mockable.h"
#include "mock_mbedtls_ssl_mockable.h"
#include "mock_mbedtls_x509_crt_mockable.h"

#include "freertos_mock.h"
#include "logger_stub.h"
#include "stm32l475_cmsis_mock.h"

/* Ceedling auto-links a .c file only when the *test file itself* (not
 * transitively, through the SUT) #includes a header whose basename
 * matches. mqtt_client.c pulls in core_mqtt.h/FreeRTOS.h/stm32l475xx.h
 * for its own compilation, but that alone does not link core_mqtt.c,
 * freertos_mock.c, or stm32l475_cmsis_mock.c into this test executable
 * — these direct includes do that. */
#include "core_mqtt.h"
#include "core_mqtt_serializer.h"
#include "core_mqtt_state.h"

#include "mqtt_client.h"

/* ========================================================================
 * WifiDriver stub — inline bodies only.
 *
 * mqtt_client.h's public API embeds wifi_handle_t (the injected WifiDriver
 * dependency), so it transitively #includes the real wifi_driver.h for
 * that type — unlike Logger's rtc/debug_uart dependencies, which are
 * private to logger.c and never appear in logger.h. A duplicate type
 * declaration here (the usual tests/support/<dep>_stub.h pattern) would
 * therefore collide with the real header already visible in this
 * translation unit. Only the function *bodies* are provided below,
 * against the real prototypes; Ceedling's auto-link still does not pull
 * in the real wifi_driver.c because that decision is driven by this test
 * file's own #include list, not the SUT's transitive includes — and this
 * file never writes #include "wifi_driver.h" itself.
 * ==================================================================== */

#define MOCK_RECV_BUF_MAX 512u

static uint8_t s_recv_buf[MOCK_RECV_BUF_MAX];
static size_t s_recv_len;
static size_t s_recv_pos;
static wifi_err_t s_recv_idle_result; /* returned once the queue is drained */
static wifi_err_t s_open_socket_result;
static wifi_err_t s_send_result;
static size_t s_send_call_count;
static size_t s_send_total_bytes;
/* Mirrors wifi_driver.c's real socket table (WIFI_MAX_SOCKETS slots): lets
 * MQTT-T19 prove that a leaked socket slot on abnormal disconnect (MQTT-O8)
 * would actually exhaust WifiDriver's socket table after a few reconnect
 * cycles, not just a synthetic counter divorced from real behaviour. */
static uint8_t s_open_sockets;
static size_t s_close_socket_call_count;

static void prv_reset_wifi_stub(void)
{
    memset(s_recv_buf, 0, sizeof(s_recv_buf));
    s_recv_len = 0u;
    s_recv_pos = 0u;
    s_recv_idle_result = WIFI_ERR_TIMEOUT;
    s_open_socket_result = WIFI_ERR_OK;
    s_send_result = WIFI_ERR_OK;
    s_send_call_count = 0u;
    s_send_total_bytes = 0u;
    s_open_sockets = 0u;
    s_close_socket_call_count = 0u;
}

static void prv_queue_recv_bytes(const uint8_t *data, size_t len)
{
    TEST_ASSERT_TRUE_MESSAGE((s_recv_len + len) <= MOCK_RECV_BUF_MAX, "recv queue overflow");
    memcpy(&s_recv_buf[s_recv_len], data, len);
    s_recv_len += len;
}

wifi_err_t wifi_open_socket(wifi_handle_t handle, wifi_socket_type_t type, const char *remote_addr,
                            uint16_t remote_port, wifi_socket_t *out_socket)
{
    (void) handle;
    (void) type;
    (void) remote_addr;
    (void) remote_port;

    if (s_open_socket_result != WIFI_ERR_OK)
    {
        return s_open_socket_result;
    }
    if (s_open_sockets >= WIFI_MAX_SOCKETS)
    {
        return WIFI_ERR_NO_RESOURCE;
    }
    s_open_sockets++;
    *out_socket = 0u;
    return WIFI_ERR_OK;
}

wifi_err_t wifi_send(wifi_handle_t handle, wifi_socket_t socket, const uint8_t *data, size_t len)
{
    (void) handle;
    (void) socket;
    (void) data;
    s_send_call_count++;
    s_send_total_bytes += len;
    return s_send_result;
}

wifi_err_t wifi_recv(wifi_handle_t handle, wifi_socket_t socket, uint8_t *buf, size_t buf_len,
                     size_t *out_len, uint32_t timeout_ms)
{
    (void) handle;
    (void) socket;
    (void) timeout_ms;

    if (s_recv_pos >= s_recv_len)
    {
        *out_len = 0u;
        /* Simulate elapsed time while polling for data with nothing
         * available, so a bounded blocking wait (mqtt_client_publish()'s
         * QoS 1 PUBACK loop, MQTT_Connect()'s CONNACK wait) can actually
         * cross its timeout threshold in a host test without a real
         * sleep(). Tests that queue a reply before the first poll never
         * observe this path. */
        g_mock_tick_count += 250u;
        return s_recv_idle_result;
    }

    size_t avail = s_recv_len - s_recv_pos;
    size_t n = (avail < buf_len) ? avail : buf_len;
    memcpy(buf, &s_recv_buf[s_recv_pos], n);
    s_recv_pos += n;
    *out_len = n;
    return WIFI_ERR_OK;
}

wifi_err_t wifi_close_socket(wifi_handle_t handle, wifi_socket_t socket)
{
    (void) handle;
    (void) socket;
    s_close_socket_call_count++;
    if (s_open_sockets > 0u)
    {
        s_open_sockets--;
    }
    return WIFI_ERR_OK;
}

/* ========================================================================
 * Logger stub — inline no-op body.
 * ==================================================================== */

void logger_log(log_level_t level, const char *module, const char *msg)
{
    (void) level;
    (void) module;
    (void) msg;
}

/* ========================================================================
 * mbedTLS mock plumbing helpers
 * ==================================================================== */

static void prv_mock_tls_handshake(int handshake_ret)
{
    mbedtls_entropy_init_Ignore();
    mbedtls_entropy_add_source_IgnoreAndReturn(0);
    mbedtls_ctr_drbg_init_Ignore();
    mbedtls_ctr_drbg_seed_IgnoreAndReturn(0);
    mbedtls_x509_crt_init_Ignore();
    mbedtls_x509_crt_parse_der_IgnoreAndReturn(0);
    mbedtls_pk_init_Ignore();
    mbedtls_pk_parse_key_IgnoreAndReturn(0);
    mbedtls_ssl_config_init_Ignore();
    mbedtls_ssl_config_defaults_IgnoreAndReturn(0);
    mbedtls_ssl_conf_authmode_Ignore();
    mbedtls_ssl_conf_ca_chain_Ignore();
    mbedtls_ssl_conf_own_cert_IgnoreAndReturn(0);
    mbedtls_ssl_conf_rng_Ignore();
    mbedtls_ssl_init_Ignore();
    mbedtls_ssl_setup_IgnoreAndReturn(0);
    mbedtls_ssl_set_hostname_IgnoreAndReturn(0);
    mbedtls_ssl_set_bio_Ignore();
    mbedtls_ssl_handshake_IgnoreAndReturn(handshake_ret);
}

static void prv_mock_tls_teardown(void)
{
    mbedtls_ssl_close_notify_IgnoreAndReturn(0);
    mbedtls_ssl_free_Ignore();
    mbedtls_ssl_config_free_Ignore();
    mbedtls_x509_crt_free_Ignore();
    mbedtls_pk_free_Ignore();
    mbedtls_ctr_drbg_free_Ignore();
    mbedtls_entropy_free_Ignore();
}

/* ========================================================================
 * MQTT wire-frame builders (MQTT 3.1.1 fixed + variable header)
 * ==================================================================== */

static void prv_queue_connack(uint8_t return_code)
{
    uint8_t frame[4] = {0x20u, 0x02u, 0x00u, return_code};
    prv_queue_recv_bytes(frame, sizeof(frame));
}

static void prv_queue_puback(uint16_t packet_id)
{
    uint8_t frame[4] = {0x40u, 0x02u, (uint8_t) (packet_id >> 8), (uint8_t) (packet_id & 0xFFu)};
    prv_queue_recv_bytes(frame, sizeof(frame));
}

static void prv_queue_suback(uint16_t packet_id, uint8_t return_code)
{
    uint8_t frame[5] = {0x90u, 0x03u, (uint8_t) (packet_id >> 8), (uint8_t) (packet_id & 0xFFu),
                        return_code};
    prv_queue_recv_bytes(frame, sizeof(frame));
}

static void prv_queue_publish(const char *topic, const uint8_t *payload, uint16_t payload_len)
{
    uint16_t topic_len = (uint16_t) strlen(topic);
    uint32_t remaining_len = 2u + topic_len + payload_len;
    TEST_ASSERT_TRUE_MESSAGE(remaining_len < 128u,
                             "test helper only encodes 1-byte remaining length");

    uint8_t header[4] = {0x30u, (uint8_t) remaining_len, (uint8_t) (topic_len >> 8),
                         (uint8_t) (topic_len & 0xFFu)};
    prv_queue_recv_bytes(header, sizeof(header));
    prv_queue_recv_bytes((const uint8_t *) topic, topic_len);
    prv_queue_recv_bytes(payload, payload_len);
}

/* ========================================================================
 * Test-side callback capture
 * ==================================================================== */

static bool s_msg_cb_called;
static char s_msg_cb_topic[64];
static uint16_t s_msg_cb_topic_len;
static uint8_t s_msg_cb_payload[64];
static uint32_t s_msg_cb_payload_len;

static void prv_msg_cb(const char *topic, uint16_t topic_len, const uint8_t *payload,
                       uint32_t payload_len)
{
    s_msg_cb_called = true;
    s_msg_cb_topic_len = topic_len;
    memcpy(s_msg_cb_topic, topic, topic_len);
    s_msg_cb_payload_len = payload_len;
    memcpy(s_msg_cb_payload, payload, payload_len);
}

static bool s_disconnect_cb_called;

static void prv_disconnect_cb(void)
{
    s_disconnect_cb_called = true;
}

/* ========================================================================
 * Fixture helpers
 * ==================================================================== */

static mqtt_client_handle_t prv_create_default(void)
{
    mqtt_client_config_t config = {
        .wifi = (wifi_handle_t) 0x1234,
        .msg_cb = prv_msg_cb,
        .disconnect_cb = prv_disconnect_cb,
    };
    mqtt_client_handle_t handle = NULL;
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_create(&config, &handle));
    TEST_ASSERT_NOT_NULL(handle);
    return handle;
}

static mqtt_connect_cfg_t prv_default_connect_cfg(void)
{
    static const uint8_t cert[] = {0x01, 0x02, 0x03};
    mqtt_connect_cfg_t cfg = {
        .broker_endpoint = "test.iot.example.com",
        .broker_port = 8883u,
        .client_id = "gw-001",
        .client_cert_der = cert,
        .client_cert_len = sizeof(cert),
        .client_key_der = cert,
        .client_key_len = sizeof(cert),
        .ca_cert_der = cert,
        .ca_cert_len = sizeof(cert),
        .keep_alive_s = 60u,
    };
    return cfg;
}

/** @brief Establishes a connection with a successful TLS handshake and a
 *         CONNACK(rc=0) reply queued ahead of time. Used by tests that need
 *         a connected handle (MQTT-T07..T15). */
static mqtt_client_handle_t prv_connect_success(void)
{
    mqtt_client_handle_t handle = prv_create_default();
    prv_mock_tls_handshake(0);
    prv_queue_connack(0u);

    mqtt_connect_cfg_t cfg = prv_default_connect_cfg();
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_connect(handle, &cfg));
    return handle;
}

/* ========================================================================
 * setUp / tearDown
 * ==================================================================== */

void setUp(void)
{
    mqtt_client_reset_for_test();
    prv_reset_wifi_stub();
    g_mock_tick_count = 0u;
    s_msg_cb_called = false;
    memset(s_msg_cb_topic, 0, sizeof(s_msg_cb_topic));
    s_msg_cb_topic_len = 0u;
    memset(s_msg_cb_payload, 0, sizeof(s_msg_cb_payload));
    s_msg_cb_payload_len = 0u;
    s_disconnect_cb_called = false;
}

void tearDown(void)
{
}

/* ========================================================================
 * MQTT-T01..T03 — mqtt_client_create()
 * ==================================================================== */

void test_MQTT_T01_create_happy_path(void)
{
    mqtt_client_handle_t handle = prv_create_default();
    (void) handle;
}

void test_MQTT_T02_create_null_config(void)
{
    mqtt_client_handle_t handle = NULL;
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_NULL_PTR, mqtt_client_create(NULL, &handle));

    mqtt_client_config_t config = {
        .wifi = (wifi_handle_t) 0x1234,
        .msg_cb = NULL,
        .disconnect_cb = prv_disconnect_cb,
    };
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_NULL_PTR, mqtt_client_create(&config, &handle));

    config.msg_cb = prv_msg_cb;
    config.disconnect_cb = NULL;
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_NULL_PTR, mqtt_client_create(&config, &handle));
}

void test_MQTT_T03_create_pool_exhaustion(void)
{
    (void) prv_create_default(); /* MQTT_CLIENT_MAX_INSTANCES == 1 */

    mqtt_client_config_t config = {
        .wifi = (wifi_handle_t) 0x1234,
        .msg_cb = prv_msg_cb,
        .disconnect_cb = prv_disconnect_cb,
    };
    mqtt_client_handle_t handle2 = NULL;
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_NO_RESOURCE, mqtt_client_create(&config, &handle2));
}

/* ========================================================================
 * MQTT-T04..T06 — mqtt_client_connect()
 * ==================================================================== */

void test_MQTT_T04_connect_connack_accepted(void)
{
    mqtt_client_handle_t handle = prv_create_default();
    prv_mock_tls_handshake(0);
    prv_queue_connack(0u);

    mqtt_connect_cfg_t cfg = prv_default_connect_cfg();
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_connect(handle, &cfg));

    mqtt_stats_t stats;
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_get_stats(handle, &stats));
    TEST_ASSERT_EQUAL_UINT32(1u, stats.connect_ok);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.connect_attempts);
}

void test_MQTT_T05_connect_connack_rejected(void)
{
    mqtt_client_handle_t handle = prv_create_default();
    prv_mock_tls_handshake(0);
    prv_mock_tls_teardown();  /* connect() unwinds TLS on CONNACK rejection */
    prv_queue_connack(0x05u); /* MQTT 3.1.1: 5 = not authorized */

    mqtt_connect_cfg_t cfg = prv_default_connect_cfg();
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_CONNECT_FAIL, mqtt_client_connect(handle, &cfg));
}

void test_MQTT_T06_connect_tls_handshake_failure(void)
{
    mqtt_client_handle_t handle = prv_create_default();
    prv_mock_tls_handshake(-0x7200); /* MBEDTLS_ERR_X509_CERT_VERIFY_FAILED, any negative code */
    prv_mock_tls_teardown();

    mqtt_connect_cfg_t cfg = prv_default_connect_cfg();
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_TLS_FAIL, mqtt_client_connect(handle, &cfg));
}

/* ========================================================================
 * MQTT-T07..T10 — mqtt_client_publish()
 * ==================================================================== */

void test_MQTT_T07_publish_qos0(void)
{
    mqtt_client_handle_t handle = prv_connect_success();

    const uint8_t payload[] = "22.5";
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK,
                      mqtt_client_publish(handle, "dt/iotmonitor/gw-001/telemetry", payload,
                                          sizeof(payload) - 1u, MQTT_QOS_0));

    mqtt_stats_t stats;
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_get_stats(handle, &stats));
    TEST_ASSERT_EQUAL_UINT32(1u, stats.publishes_sent);
    TEST_ASSERT_TRUE(s_send_call_count > 0u);
}

void test_MQTT_T08_publish_qos1_with_puback(void)
{
    mqtt_client_handle_t handle = prv_connect_success();
    prv_queue_puback(1u);

    const uint8_t payload[] = "ALARM";
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK,
                      mqtt_client_publish(handle, "dt/iotmonitor/gw-001/alarms", payload,
                                          sizeof(payload) - 1u, MQTT_QOS_1));

    mqtt_stats_t stats;
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_get_stats(handle, &stats));
    TEST_ASSERT_EQUAL_UINT32(1u, stats.publishes_acked);
}

void test_MQTT_T09_publish_qos1_puback_timeout(void)
{
    mqtt_client_handle_t handle = prv_connect_success();
    /* No PUBACK queued: every recv poll returns "no data" and advances the
     * mock clock (see wifi_recv() stub) until MQTT_PUBACK_TIMEOUT_MS trips. */

    const uint8_t payload[] = "ALARM";
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_PUBLISH_FAIL,
                      mqtt_client_publish(handle, "dt/iotmonitor/gw-001/alarms", payload,
                                          sizeof(payload) - 1u, MQTT_QOS_1));

    mqtt_stats_t stats;
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_get_stats(handle, &stats));
    TEST_ASSERT_EQUAL_UINT32(1u, stats.publish_failures);
}

void test_MQTT_T10_publish_not_connected(void)
{
    mqtt_client_handle_t handle = prv_create_default(); /* never connected */

    const uint8_t payload[] = "x";
    TEST_ASSERT_EQUAL(
        MQTT_CLIENT_ERR_NOT_CONNECTED,
        mqtt_client_publish(handle, "dt/iotmonitor/gw-001/telemetry", payload, 1u, MQTT_QOS_0));
    TEST_ASSERT_EQUAL_UINT32(0u, s_send_call_count);
}

/* ========================================================================
 * MQTT-T11..T12 — mqtt_client_subscribe()
 * ==================================================================== */

/* Raw SUBACK return codes per MQTT 3.1.1 §3.9.3 — not part of MqttClient's
 * public API (mqtt_client.h does not expose coreMQTT's MQTTSubAckStatus_t),
 * so the wire values are spelled out here directly. */
#define MQTT_TEST_SUBACK_RC_SUCCESS_QOS1 0x01u
#define MQTT_TEST_SUBACK_RC_FAILURE 0x80u

void test_MQTT_T11_subscribe_happy_path(void)
{
    mqtt_client_handle_t handle = prv_connect_success();
    prv_queue_suback(1u, MQTT_TEST_SUBACK_RC_SUCCESS_QOS1);

    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK,
                      mqtt_client_subscribe(handle, "cmd/iotmonitor/gw-001/config", MQTT_QOS_1));
}

void test_MQTT_T12_subscribe_suback_failure(void)
{
    mqtt_client_handle_t handle = prv_connect_success();
    prv_queue_suback(1u, MQTT_TEST_SUBACK_RC_FAILURE);

    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_SUBSCRIBE_FAIL,
                      mqtt_client_subscribe(handle, "cmd/iotmonitor/gw-001/config", MQTT_QOS_1));

    mqtt_stats_t stats;
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_get_stats(handle, &stats));
    TEST_ASSERT_EQUAL_UINT32(1u, stats.subscribe_failures);
}

/* ========================================================================
 * MQTT-T13..T14 — mqtt_client_process()
 * ==================================================================== */

void test_MQTT_T13_process_inbound_publish(void)
{
    mqtt_client_handle_t handle = prv_connect_success();
    const uint8_t payload[] = {0xDEu, 0xADu, 0xBEu, 0xEFu};
    prv_queue_publish("cmd/iotmonitor/gw-001/config", payload, sizeof(payload));

    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_process(handle));

    TEST_ASSERT_TRUE(s_msg_cb_called);
    TEST_ASSERT_EQUAL_UINT16(strlen("cmd/iotmonitor/gw-001/config"), s_msg_cb_topic_len);
    TEST_ASSERT_EQUAL_MEMORY("cmd/iotmonitor/gw-001/config", s_msg_cb_topic, s_msg_cb_topic_len);
    TEST_ASSERT_EQUAL_UINT32(sizeof(payload), s_msg_cb_payload_len);
    TEST_ASSERT_EQUAL_MEMORY(payload, s_msg_cb_payload, sizeof(payload));
}

void test_MQTT_T14_process_keepalive_timeout(void)
{
    mqtt_client_handle_t handle = prv_connect_success();
    /* mqtt_client_process()'s abnormal-disconnect path now tears down the
     * TLS session before invoking disconnect_cb() (MQTT-O8) — these mocks
     * must be armed or the unexpected mbedTLS calls fail the test. */
    prv_mock_tls_teardown();

    /* No PINGRESP is ever queued. Call process() repeatedly, exactly as
     * CloudPublisherTask does in production (~every 100 ms) — each idle
     * poll advances the mock clock by 250 ms (see wifi_recv() stub), so
     * coreMQTT's own keep-alive state machine (send PINGREQ once idle
     * exceeds keep_alive_s, then wait up to keep_alive_s / 2 for
     * PINGRESP) runs its natural course across calls rather than relying
     * on a single artificially-jumped call. keep_alive_s == 60 here, so
     * this must trip within 60 + 30 = 90 s of simulated time. */
    bool timed_out = false;
    for (uint32_t i = 0u; (i < 500u) && !timed_out; ++i)
    {
        TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_process(handle));
        timed_out = s_disconnect_cb_called;
    }

    TEST_ASSERT_TRUE_MESSAGE(timed_out, "disconnect_cb was not invoked within 500 process() polls");
    /* MQTT-O8: the WifiDriver socket must be released on this path too,
     * not just on the graceful mqtt_client_disconnect() path — otherwise
     * every unexpected drop leaks a socket-table slot. */
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1u, s_close_socket_call_count,
                                     "wifi_close_socket() was not called on keep-alive timeout");
}

/* ========================================================================
 * MQTT-T15 — mqtt_client_disconnect()
 * ==================================================================== */

void test_MQTT_T15_disconnect_graceful(void)
{
    mqtt_client_handle_t handle = prv_connect_success();
    prv_mock_tls_teardown();

    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_disconnect(handle));
    TEST_ASSERT_FALSE(s_disconnect_cb_called);

    /* Now disconnected: publish must be rejected immediately. */
    const uint8_t payload[] = "x";
    TEST_ASSERT_EQUAL(
        MQTT_CLIENT_ERR_NOT_CONNECTED,
        mqtt_client_publish(handle, "dt/iotmonitor/gw-001/telemetry", payload, 1u, MQTT_QOS_0));
}

/* ========================================================================
 * MQTT-T18..T19 — Resource cleanup on abnormal disconnect (MQTT-O8)
 * ==================================================================== */

/** @brief A single unexpected disconnect must not prevent a subsequent
 *         mqtt_client_connect() from succeeding — proves the WifiDriver
 *         socket slot and TLS session are actually released on the
 *         keep-alive-timeout path, not just marked logically disconnected. */
void test_MQTT_T18_reconnect_after_keepalive_timeout(void)
{
    mqtt_client_handle_t handle = prv_create_default();
    prv_mock_tls_handshake(0);
    prv_mock_tls_teardown();

    mqtt_connect_cfg_t cfg = prv_default_connect_cfg();
    prv_queue_connack(0u);
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_connect(handle, &cfg));

    bool timed_out = false;
    for (uint32_t i = 0u; (i < 500u) && !timed_out; ++i)
    {
        (void) mqtt_client_process(handle);
        timed_out = s_disconnect_cb_called;
    }
    TEST_ASSERT_TRUE_MESSAGE(timed_out, "disconnect_cb was not invoked");
    TEST_ASSERT_EQUAL_UINT32(1u, s_close_socket_call_count);

    s_recv_pos = 0u;
    s_recv_len = 0u;
    s_disconnect_cb_called = false;
    prv_queue_connack(0u);
    TEST_ASSERT_EQUAL_MESSAGE(
        MQTT_CLIENT_ERR_OK, mqtt_client_connect(handle, &cfg),
        "reconnect failed after keep-alive timeout — socket/TLS state leaked");

    mqtt_stats_t stats;
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_get_stats(handle, &stats));
    TEST_ASSERT_EQUAL_UINT32(2u, stats.connect_attempts);
    TEST_ASSERT_EQUAL_UINT32(2u, stats.connect_ok);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.reconnect_count);
}

/** @brief Repeated drop/reconnect cycles, one more than WifiDriver's real
 *         socket-table depth (WIFI_MAX_SOCKETS), must all succeed. Without
 *         releasing the socket slot on every abnormal disconnect, this
 *         loop would start failing with ERR_CONNECT_FAIL once the table's
 *         4 slots are all leaked (MQTT-O8) — this is the scenario that
 *         permanently strands a device against a broker that drops the
 *         connection more than WIFI_MAX_SOCKETS times over its uptime. */
void test_MQTT_T19_multi_cycle_disconnect_reconnect_no_socket_exhaustion(void)
{
    mqtt_client_handle_t handle = prv_create_default();
    prv_mock_tls_handshake(0);
    prv_mock_tls_teardown();

    mqtt_connect_cfg_t cfg = prv_default_connect_cfg();
    const uint32_t cycles = (uint32_t) WIFI_MAX_SOCKETS + 1u;

    for (uint32_t cycle = 0u; cycle < cycles; ++cycle)
    {
        s_recv_pos = 0u;
        s_recv_len = 0u;
        s_disconnect_cb_called = false;
        prv_queue_connack(0u);

        TEST_ASSERT_EQUAL_MESSAGE(MQTT_CLIENT_ERR_OK, mqtt_client_connect(handle, &cfg),
                                  "connect failed - socket table likely exhausted");

        bool timed_out = false;
        for (uint32_t i = 0u; (i < 500u) && !timed_out; ++i)
        {
            (void) mqtt_client_process(handle);
            timed_out = s_disconnect_cb_called;
        }
        TEST_ASSERT_TRUE_MESSAGE(timed_out, "disconnect_cb was not invoked this cycle");
    }

    TEST_ASSERT_EQUAL_UINT32(cycles, s_close_socket_call_count);

    mqtt_stats_t stats;
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_get_stats(handle, &stats));
    TEST_ASSERT_EQUAL_UINT32(cycles, stats.connect_ok);
    TEST_ASSERT_EQUAL_UINT32(cycles - 1u, stats.reconnect_count);
}

/* ========================================================================
 * MQTT-T16..T17 — IMqttStats
 * ==================================================================== */

void test_MQTT_T16_reset_stats(void)
{
    mqtt_client_handle_t handle = prv_connect_success();

    mqtt_stats_t stats;
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_get_stats(handle, &stats));
    TEST_ASSERT_EQUAL_UINT32(1u, stats.connect_ok); /* non-zero before reset */

    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_reset_stats(handle));
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_get_stats(handle, &stats));

    mqtt_stats_t zero = {0};
    TEST_ASSERT_EQUAL_MEMORY(&zero, &stats, sizeof(zero));
}

void test_MQTT_T17_get_stats_matches_internal_state(void)
{
    mqtt_client_handle_t handle = prv_connect_success();
    prv_queue_puback(1u);

    const uint8_t payload[] = "x";
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_publish(handle, "dt/iotmonitor/gw-001/alarms",
                                                              payload, 1u, MQTT_QOS_1));

    mqtt_stats_t stats;
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_get_stats(handle, &stats));
    TEST_ASSERT_EQUAL_UINT32(1u, stats.connect_attempts);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.connect_ok);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.publishes_sent);
    TEST_ASSERT_EQUAL_UINT32(1u, stats.publishes_acked);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.publish_failures);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.subscribe_failures);
    TEST_ASSERT_EQUAL_UINT32(0u, stats.reconnect_count);
}

/* ========================================================================
 * MQTT-T20..T21 — mqtt_client_is_connected()
 * ==================================================================== */

void test_MQTT_T20_is_connected_reflects_state(void)
{
    mqtt_client_handle_t handle = prv_create_default();
    TEST_ASSERT_FALSE(mqtt_client_is_connected(handle));

    prv_mock_tls_handshake(0);
    prv_queue_connack(0u);
    mqtt_connect_cfg_t cfg = prv_default_connect_cfg();
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_connect(handle, &cfg));
    TEST_ASSERT_TRUE(mqtt_client_is_connected(handle));

    prv_mock_tls_teardown();
    TEST_ASSERT_EQUAL(MQTT_CLIENT_ERR_OK, mqtt_client_disconnect(handle));
    TEST_ASSERT_FALSE(mqtt_client_is_connected(handle));
}

void test_MQTT_T21_is_connected_null_handle(void)
{
    TEST_ASSERT_FALSE(mqtt_client_is_connected(NULL));
}
