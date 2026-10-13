/**
 * @file test_ntp_client.c
 * @brief Unity tests for NtpClient (Gateway).
 *
 * WifiTask and HealthMonitor are replaced by spies defined in this file; the test
 * never includes their headers directly (ntp_client.h does), so Ceedling links
 * neither.  The wifitask_recv() spy fills the caller's buffer with a scripted
 * 48-byte reply, built byte by byte exactly as it would arrive on the wire.
 *
 * Test IDs follow companion §11 (NTP-T01..T16); NTP-T17.. are additions.
 */

#include <string.h>

#include "ntp_client.h"
#include "unity.h"

#define NTP_PACKET_BYTES (48U)
#define TX_TS_OFFSET (40U)
#define MAX_SERVERS_SCRIPTED (6U)
#define MAX_EVENTS (8U)
#define WIFI_HANDLE ((wifitask_handle_t) 0x10)
#define HEALTH_HANDLE ((health_monitor_handle_t) 0x20)

/* 2024-01-02 20:20:00 UTC: NTP seconds and the Unix seconds they must convert to. */
#define NTP_SECONDS_2024 (3913215600UL)
#define UNIX_SECONDS_2024 (1704226800UL)

/* ===================================================================== */
/* WifiTask spy                                                          */
/* ===================================================================== */

typedef struct
{
    wifitask_err_t open_err;
    wifitask_err_t send_err;
    wifitask_err_t recv_err;
    size_t recv_len;
    uint8_t reply[NTP_PACKET_BYTES];
} server_script_t;

static server_script_t s_script[MAX_SERVERS_SCRIPTED];
static uint32_t s_open_calls;
static uint32_t s_send_calls;
static uint32_t s_recv_calls;
static uint32_t s_close_calls;
static char s_open_addr[MAX_SERVERS_SCRIPTED][20];
static uint16_t s_open_port;
static wifi_socket_type_t s_open_type;
static uint8_t s_sent[NTP_PACKET_BYTES];
static size_t s_sent_len;
static uint32_t s_recv_timeout;

wifitask_err_t wifitask_open_socket(wifitask_handle_t handle, wifi_socket_type_t type,
                                    const char *remote_addr, uint16_t remote_port,
                                    wifi_socket_t *out_socket)
{
    (void) handle;
    if (s_open_calls < MAX_SERVERS_SCRIPTED)
    {
        (void) strncpy(s_open_addr[s_open_calls], remote_addr, sizeof(s_open_addr[0]) - 1U);
    }
    s_open_port = remote_port;
    s_open_type = type;
    *out_socket = (wifi_socket_t) s_open_calls;
    return s_script[s_open_calls++].open_err;
}

wifitask_err_t wifitask_send(wifitask_handle_t handle, wifi_socket_t socket, const uint8_t *data,
                             size_t len)
{
    (void) handle;
    s_send_calls++;
    s_sent_len = len;
    (void) memcpy(s_sent, data, (len <= sizeof(s_sent)) ? len : sizeof(s_sent));
    return s_script[socket].send_err;
}

wifitask_err_t wifitask_recv(wifitask_handle_t handle, wifi_socket_t socket, uint8_t *buf,
                             size_t buf_len, size_t *out_len, uint32_t timeout_ms)
{
    (void) handle;
    (void) buf_len;
    s_recv_calls++;
    s_recv_timeout = timeout_ms;
    if (s_script[socket].recv_err == (wifitask_err_t) WIFI_ERR_OK)
    {
        (void) memcpy(buf, s_script[socket].reply, NTP_PACKET_BYTES);
        *out_len = s_script[socket].recv_len;
    }
    return s_script[socket].recv_err;
}

wifitask_err_t wifitask_close_socket(wifitask_handle_t handle, wifi_socket_t socket)
{
    (void) handle;
    (void) socket;
    s_close_calls++;
    return (wifitask_err_t) WIFI_ERR_OK;
}

/* ===================================================================== */
/* HealthMonitor spy                                                     */
/* ===================================================================== */

static uint32_t s_event_count;
static health_event_t s_event_kind[MAX_EVENTS];
static health_monitor_handle_t s_event_handle;

health_monitor_err_t health_monitor_push_event(health_monitor_handle_t handle, health_event_t event,
                                               uint32_t param)
{
    (void) param;
    if (s_event_count < MAX_EVENTS)
    {
        s_event_kind[s_event_count] = event;
    }
    s_event_count++;
    s_event_handle = handle;
    return HEALTH_MONITOR_ERR_OK;
}

static uint32_t count_events(health_event_t kind)
{
    uint32_t n = 0U;

    for (uint32_t i = 0U; (i < s_event_count) && (i < MAX_EVENTS); i++)
    {
        if (s_event_kind[i] == kind)
        {
            n++;
        }
    }
    return n;
}

/* ===================================================================== */
/* Helpers                                                               */
/* ===================================================================== */

static ntp_client_handle_t s_h;
static const char *const k_servers[] = {"10.0.0.1", "10.0.0.2", "10.0.0.3", "10.0.0.4"};

/** Store `value` big-endian at `at`, the way a server sends it. */
static void put_be32(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t) (value >> 24U);
    at[1] = (uint8_t) (value >> 16U);
    at[2] = (uint8_t) (value >> 8U);
    at[3] = (uint8_t) value;
}

/** Script server `n` to answer with a well-formed reply carrying NTP time `ntp_seconds`. */
static void script_good(uint32_t n, uint32_t ntp_seconds)
{
    (void) memset(&s_script[n], 0, sizeof(s_script[n]));
    s_script[n].reply[0] = 0x24U; /* LI 0, VN 4, Mode 4 (server) */
    s_script[n].reply[1] = 2U;    /* stratum 2 */
    put_be32(&s_script[n].reply[TX_TS_OFFSET], ntp_seconds);
    s_script[n].recv_len = NTP_PACKET_BYTES;
}

static void script_timeout(uint32_t n)
{
    (void) memset(&s_script[n], 0, sizeof(s_script[n]));
    s_script[n].recv_err = (wifitask_err_t) WIFI_ERR_TIMEOUT;
}

void setUp(void)
{
    ntp_client_reset_for_test();
    (void) memset(s_script, 0, sizeof(s_script));
    (void) memset(s_open_addr, 0, sizeof(s_open_addr));
    (void) memset(s_sent, 0xEE, sizeof(s_sent));
    s_open_calls = 0U;
    s_send_calls = 0U;
    s_recv_calls = 0U;
    s_close_calls = 0U;
    s_open_port = 0U;
    s_sent_len = 0U;
    s_recv_timeout = 0U;
    s_event_count = 0U;
    s_event_handle = NULL;

    const ntp_client_config_t config = {.wifi = WIFI_HANDLE, .health = HEALTH_HANDLE};

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_OK, ntp_client_create(&config, &s_h));
}

void tearDown(void)
{
}

/* ===================================================================== */
/* create                                                                */
/* ===================================================================== */

void test_NTP_T01_create_returns_a_handle(void)
{
    TEST_ASSERT_NOT_NULL(s_h);
}

void test_NTP_T02_create_null_config_is_rejected(void)
{
    ntp_client_handle_t h = NULL;

    ntp_client_reset_for_test();
    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_NULL_PTR, ntp_client_create(NULL, &h));
    TEST_ASSERT_NULL(h);
}

void test_NTP_T03_create_pool_exhaustion(void)
{
    const ntp_client_config_t config = {.wifi = WIFI_HANDLE, .health = HEALTH_HANDLE};
    ntp_client_handle_t second = NULL;

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_NO_RESOURCE, ntp_client_create(&config, &second));
    TEST_ASSERT_NULL(second);
}

void test_NTP_T17_create_null_handle_or_dependency_is_rejected(void)
{
    ntp_client_handle_t h = NULL;
    ntp_client_config_t config = {.wifi = WIFI_HANDLE, .health = HEALTH_HANDLE};

    ntp_client_reset_for_test();
    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_NULL_PTR, ntp_client_create(&config, NULL));
    config.wifi = NULL;
    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_NULL_PTR, ntp_client_create(&config, &h));
    config.wifi = WIFI_HANDLE;
    config.health = NULL;
    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_NULL_PTR, ntp_client_create(&config, &h));
    /* None of the failures used up the pool. */
    config.health = HEALTH_HANDLE;
    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_OK, ntp_client_create(&config, &h));
}

/* ===================================================================== */
/* happy path                                                            */
/* ===================================================================== */

void test_NTP_T04_single_valid_reply_returns_ok(void)
{
    uint32_t epoch = 0U;

    script_good(0U, NTP_SECONDS_2024);

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_OK, ntp_client_query(s_h, k_servers, 1U, &epoch));
    TEST_ASSERT_EQUAL_UINT32(UNIX_SECONDS_2024, epoch);
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
}

void test_NTP_T05_epoch_conversion(void)
{
    uint32_t epoch = 0U;

    script_good(0U, NTP_UNIX_OFFSET); /* exactly 1 Jan 1970 */
    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_OK, ntp_client_query(s_h, k_servers, 1U, &epoch));
    TEST_ASSERT_EQUAL_UINT32(0U, epoch);

    s_open_calls = 0U;
    script_good(0U, NTP_SECONDS_2024);
    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_OK, ntp_client_query(s_h, k_servers, 1U, &epoch));
    TEST_ASSERT_EQUAL_UINT32(1704226800UL, epoch);
}

void test_NTP_T14_timestamp_is_read_as_big_endian_wire_bytes(void)
{
    uint32_t epoch = 0U;

    script_good(0U, 0U);
    /* Wire bytes E9 6B A1 F0 are NTP second 0xE96BA1F0 = 3916505584, whatever the host order. */
    s_script[0].reply[TX_TS_OFFSET + 0U] = 0xE9U;
    s_script[0].reply[TX_TS_OFFSET + 1U] = 0x6BU;
    s_script[0].reply[TX_TS_OFFSET + 2U] = 0xA1U;
    s_script[0].reply[TX_TS_OFFSET + 3U] = 0xF0U;

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_OK, ntp_client_query(s_h, k_servers, 1U, &epoch));
    TEST_ASSERT_EQUAL_UINT32(0xE96BA1F0UL - NTP_UNIX_OFFSET, epoch);
}

void test_NTP_T18_request_is_a_zeroed_48_byte_client_packet_to_udp_123(void)
{
    uint32_t epoch = 0U;

    script_good(0U, NTP_SECONDS_2024);
    (void) ntp_client_query(s_h, k_servers, 1U, &epoch);

    TEST_ASSERT_EQUAL_UINT32(NTP_PACKET_BYTES, s_sent_len);
    TEST_ASSERT_EQUAL_HEX8(0x1BU, s_sent[0]);
    for (uint32_t i = 1U; i < NTP_PACKET_BYTES; i++)
    {
        TEST_ASSERT_EQUAL_HEX8(0x00U, s_sent[i]);
    }
    TEST_ASSERT_EQUAL(WIFI_SOCKET_UDP, s_open_type);
    TEST_ASSERT_EQUAL_UINT16(123U, s_open_port);
    TEST_ASSERT_EQUAL_STRING("10.0.0.1", s_open_addr[0]);
    TEST_ASSERT_EQUAL_UINT32(NTP_CLIENT_QUERY_TIMEOUT_MS, s_recv_timeout);
}

/* ===================================================================== */
/* sanity checks                                                         */
/* ===================================================================== */

static void expect_rejected_with_bad_response_event(void)
{
    uint32_t epoch = 0xDEADBEEFUL;

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_ALL_FAILED, ntp_client_query(s_h, k_servers, 1U, &epoch));
    TEST_ASSERT_EQUAL_UINT32(1U, count_events(HEALTH_EVENT_NTP_BAD_RESPONSE));
    TEST_ASSERT_EQUAL_UINT32(1U, count_events(HEALTH_EVENT_NTP_SYNC_FAILED));
    TEST_ASSERT_EQUAL_UINT32(0xDEADBEEFUL, epoch); /* untouched on failure */
    TEST_ASSERT_EQUAL_UINT32(1U, s_close_calls);
}

void test_NTP_T06_mode_other_than_server_is_rejected(void)
{
    script_good(0U, NTP_SECONDS_2024);
    s_script[0].reply[0] = 0x23U; /* Mode 3: a client packet echoed back */
    expect_rejected_with_bad_response_event();
}

void test_NTP_T07_stratum_zero_kiss_of_death_is_rejected(void)
{
    script_good(0U, NTP_SECONDS_2024);
    s_script[0].reply[1] = 0U;
    expect_rejected_with_bad_response_event();
}

void test_NTP_T08_stratum_sixteen_is_rejected(void)
{
    script_good(0U, NTP_SECONDS_2024);
    s_script[0].reply[1] = 16U;
    expect_rejected_with_bad_response_event();
}

void test_NTP_T09_zero_transmit_timestamp_is_rejected(void)
{
    script_good(0U, 0U);
    expect_rejected_with_bad_response_event();
}

void test_NTP_T10_pre_1970_timestamp_is_rejected(void)
{
    script_good(0U, NTP_UNIX_OFFSET - 1UL);
    expect_rejected_with_bad_response_event();
}

void test_NTP_T19_stratum_fifteen_is_accepted(void)
{
    uint32_t epoch = 0U;

    script_good(0U, NTP_SECONDS_2024);
    s_script[0].reply[1] = 15U;

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_OK, ntp_client_query(s_h, k_servers, 1U, &epoch));
}

void test_NTP_T20_bad_reply_then_good_reply_succeeds_with_one_bad_event(void)
{
    uint32_t epoch = 0U;

    script_good(0U, NTP_SECONDS_2024);
    s_script[0].reply[1] = 0U;
    script_good(1U, NTP_SECONDS_2024);

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_OK, ntp_client_query(s_h, k_servers, 2U, &epoch));
    TEST_ASSERT_EQUAL_UINT32(UNIX_SECONDS_2024, epoch);
    TEST_ASSERT_EQUAL_UINT32(1U, count_events(HEALTH_EVENT_NTP_BAD_RESPONSE));
    TEST_ASSERT_EQUAL_UINT32(0U, count_events(HEALTH_EVENT_NTP_SYNC_FAILED));
}

/* ===================================================================== */
/* failover                                                              */
/* ===================================================================== */

void test_NTP_T11_first_server_times_out_second_answers(void)
{
    uint32_t epoch = 0U;

    script_timeout(0U);
    script_good(1U, NTP_SECONDS_2024);

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_OK, ntp_client_query(s_h, k_servers, 2U, &epoch));
    TEST_ASSERT_EQUAL_UINT32(UNIX_SECONDS_2024, epoch);
    TEST_ASSERT_EQUAL_UINT32(2U, s_open_calls);
    TEST_ASSERT_EQUAL_STRING("10.0.0.2", s_open_addr[1]);
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
}

void test_NTP_T12_all_servers_time_out(void)
{
    uint32_t epoch = 0U;

    for (uint32_t i = 0U; i < NTP_CLIENT_MAX_SERVERS; i++)
    {
        script_timeout(i);
    }

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_ALL_FAILED,
                      ntp_client_query(s_h, k_servers, NTP_CLIENT_MAX_SERVERS, &epoch));
    TEST_ASSERT_EQUAL_UINT32(NTP_CLIENT_MAX_SERVERS, s_open_calls);
    TEST_ASSERT_EQUAL_UINT32(NTP_CLIENT_MAX_SERVERS, s_close_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_event_count);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_NTP_SYNC_FAILED, s_event_kind[0]);
    TEST_ASSERT_EQUAL_PTR(HEALTH_HANDLE, s_event_handle);
}

void test_NTP_T16_open_fails_on_first_succeeds_on_second(void)
{
    uint32_t epoch = 0U;

    script_good(0U, NTP_SECONDS_2024);
    s_script[0].open_err = (wifitask_err_t) WIFI_ERR_SOCKET;
    script_good(1U, NTP_SECONDS_2024);

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_OK, ntp_client_query(s_h, k_servers, 2U, &epoch));
    TEST_ASSERT_EQUAL_UINT32(1U, s_send_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_close_calls); /* nothing to close for the failed open */
}

void test_NTP_T21_send_failure_closes_the_socket_and_moves_on(void)
{
    uint32_t epoch = 0U;

    script_good(0U, NTP_SECONDS_2024);
    s_script[0].send_err = (wifitask_err_t) WIFI_ERR_SOCKET;
    script_good(1U, NTP_SECONDS_2024);

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_OK, ntp_client_query(s_h, k_servers, 2U, &epoch));
    TEST_ASSERT_EQUAL_UINT32(2U, s_close_calls);
    TEST_ASSERT_EQUAL_UINT32(1U, s_recv_calls);
}

void test_NTP_T22_short_reply_is_a_miss_not_a_bad_response(void)
{
    uint32_t epoch = 0U;

    script_good(0U, NTP_SECONDS_2024);
    s_script[0].recv_len = NTP_PACKET_BYTES - 1U;

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_ALL_FAILED, ntp_client_query(s_h, k_servers, 1U, &epoch));
    TEST_ASSERT_EQUAL_UINT32(0U, count_events(HEALTH_EVENT_NTP_BAD_RESPONSE));
    TEST_ASSERT_EQUAL_UINT32(1U, count_events(HEALTH_EVENT_NTP_SYNC_FAILED));
    TEST_ASSERT_EQUAL_UINT32(1U, s_close_calls);
}

void test_NTP_T23_null_list_entry_is_skipped(void)
{
    uint32_t epoch = 0U;
    const char *const list[] = {NULL, "10.0.0.2"};

    script_good(0U, NTP_SECONDS_2024);

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_OK, ntp_client_query(s_h, list, 2U, &epoch));
    TEST_ASSERT_EQUAL_UINT32(1U, s_open_calls);
    TEST_ASSERT_EQUAL_STRING("10.0.0.2", s_open_addr[0]);
}

/* ===================================================================== */
/* argument checks                                                       */
/* ===================================================================== */

void test_NTP_T13_zero_server_count_is_rejected(void)
{
    uint32_t epoch = 0U;

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_NULL_PTR, ntp_client_query(s_h, k_servers, 0U, &epoch));
    TEST_ASSERT_EQUAL_UINT32(0U, s_open_calls);
    TEST_ASSERT_EQUAL_UINT32(0U, s_event_count);
}

void test_NTP_T15_null_output_is_rejected(void)
{
    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_NULL_PTR, ntp_client_query(s_h, k_servers, 1U, NULL));
    TEST_ASSERT_EQUAL_UINT32(0U, s_open_calls);
}

void test_NTP_T24_null_handle_or_list_is_rejected(void)
{
    uint32_t epoch = 0U;

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_NULL_PTR, ntp_client_query(NULL, k_servers, 1U, &epoch));
    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_NULL_PTR, ntp_client_query(s_h, NULL, 1U, &epoch));
}

void test_NTP_T25_too_many_servers_is_rejected(void)
{
    uint32_t epoch = 0U;
    const char *const list[NTP_CLIENT_MAX_SERVERS + 1U] = {"1", "2", "3", "4", "5"};

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_INVALID_ARG,
                      ntp_client_query(s_h, list, NTP_CLIENT_MAX_SERVERS + 1U, &epoch));
    TEST_ASSERT_EQUAL_UINT32(0U, s_open_calls);
}

void test_NTP_T26_stale_handle_after_reset_is_rejected(void)
{
    uint32_t epoch = 0U;

    ntp_client_reset_for_test();

    TEST_ASSERT_EQUAL(NTP_CLIENT_ERR_INVALID_ARG, ntp_client_query(s_h, k_servers, 1U, &epoch));
    TEST_ASSERT_EQUAL_UINT32(0U, s_open_calls);
}
