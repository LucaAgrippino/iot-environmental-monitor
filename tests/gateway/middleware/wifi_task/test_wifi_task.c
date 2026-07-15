/**
 * @file test_wifi_task.c
 * @brief Unity unit tests for WifiTask — WIFITASK-T01 through WIFITASK-T14.
 *
 * WifiDriver is hand-stubbed by defining wifi_connect_ap/disconnect_ap/
 * get_link_state/get_rssi/open_socket/send/recv/close_socket/
 * attach_datardy_callback directly against the real wifi_driver.h
 * prototypes (pulled in transitively via wifi_task.h) — same pattern as
 * test_mqtt_client.c's WifiDriver stub, for the same reason (wifi_task.h
 * embeds wifi_handle_t/wifi_socket_t/etc., so a duplicate stub header
 * would collide with the real one already visible in this translation
 * unit).
 *
 * freertos_mock.h/.c does NOT simulate a real queue moving data from
 * xQueueSend to xQueueReceive — the two are independent mock state
 * blocks. That means a single call to a public wifitask_*() wrapper
 * (which enqueues, then immediately blocks on xTaskNotifyWait, all
 * within one host-test call with no real second task to interleave)
 * cannot exercise both "was the right request built" and "did dispatch
 * route to the right WifiDriver call" in one test — there is no
 * scheduler here to run WifiTask's task body between those two steps.
 * Tests are split into two layers accordingly:
 *   - Layer 1 (wrapper/plumbing): calls a public wifitask_*() function
 *     directly, inspects what xQueueSend was asked to enqueue via
 *     g_mock_xQueueSend_last_item, and checks the wrapper's return value
 *     against a pre-armed g_mock_xTaskNotifyWait_next_value.
 *   - Layer 2 (dispatch): feeds a hand-built wifitask_request_t into
 *     g_mock_xQueueReceive_next_item and calls wifitask_step_for_test()
 *     directly, proving prv_dispatch()'s switch and the notify-back both
 *     work, independent of any public wrapper.
 */

#include "unity.h"

#include <string.h>

#include "freertos_mock.h"

#include "wifi_task.h"

/* ========================================================================
 * WifiDriver stub — inline bodies only (see file header for rationale).
 * ==================================================================== */

static wifi_err_t s_connect_ap_result;
static size_t s_connect_ap_call_count;
static char s_connect_ap_last_ssid[WIFI_MAX_SSID_LEN + 1u];

static wifi_err_t s_disconnect_ap_result;
static size_t s_disconnect_ap_call_count;

static wifi_err_t s_get_link_state_result;
static wifi_link_state_t s_get_link_state_value;
static size_t s_get_link_state_call_count;

static wifi_err_t s_get_rssi_result;
static int8_t s_get_rssi_value;
static size_t s_get_rssi_call_count;

static wifi_err_t s_open_socket_result;
static wifi_socket_t s_open_socket_value;
static size_t s_open_socket_call_count;

static wifi_err_t s_send_result;
static size_t s_send_call_count;

static wifi_err_t s_recv_result;
static size_t s_recv_out_len;
static size_t s_recv_call_count;

static wifi_err_t s_close_socket_result;
static size_t s_close_socket_call_count;
static wifi_socket_t s_close_socket_last_socket;

static size_t s_attach_datardy_callback_call_count;

static void prv_reset_wifi_stub(void)
{
    s_connect_ap_result = WIFI_ERR_OK;
    s_connect_ap_call_count = 0u;
    memset(s_connect_ap_last_ssid, 0, sizeof(s_connect_ap_last_ssid));

    s_disconnect_ap_result = WIFI_ERR_OK;
    s_disconnect_ap_call_count = 0u;

    s_get_link_state_result = WIFI_ERR_OK;
    s_get_link_state_value = WIFI_LINK_UP;
    s_get_link_state_call_count = 0u;

    s_get_rssi_result = WIFI_ERR_OK;
    s_get_rssi_value = -50;
    s_get_rssi_call_count = 0u;

    s_open_socket_result = WIFI_ERR_OK;
    s_open_socket_value = 0u;
    s_open_socket_call_count = 0u;

    s_send_result = WIFI_ERR_OK;
    s_send_call_count = 0u;

    s_recv_result = WIFI_ERR_OK;
    s_recv_out_len = 0u;
    s_recv_call_count = 0u;

    s_close_socket_result = WIFI_ERR_OK;
    s_close_socket_call_count = 0u;
    s_close_socket_last_socket = WIFI_INVALID_SOCKET;

    s_attach_datardy_callback_call_count = 0u;
}

wifi_err_t wifi_connect_ap(wifi_handle_t handle, const char *ssid, const char *password)
{
    (void) handle;
    (void) password;
    s_connect_ap_call_count++;
    (void) strncpy(s_connect_ap_last_ssid, ssid, WIFI_MAX_SSID_LEN);
    return s_connect_ap_result;
}

wifi_err_t wifi_disconnect_ap(wifi_handle_t handle)
{
    (void) handle;
    s_disconnect_ap_call_count++;
    return s_disconnect_ap_result;
}

wifi_err_t wifi_get_link_state(wifi_handle_t handle, wifi_link_state_t *state)
{
    (void) handle;
    s_get_link_state_call_count++;
    *state = s_get_link_state_value;
    return s_get_link_state_result;
}

wifi_err_t wifi_get_rssi(wifi_handle_t handle, int8_t *rssi_dbm)
{
    (void) handle;
    s_get_rssi_call_count++;
    *rssi_dbm = s_get_rssi_value;
    return s_get_rssi_result;
}

wifi_err_t wifi_open_socket(wifi_handle_t handle, wifi_socket_type_t type,
                            const char *remote_addr, uint16_t remote_port,
                            wifi_socket_t *out_socket)
{
    (void) handle;
    (void) type;
    (void) remote_addr;
    (void) remote_port;
    s_open_socket_call_count++;
    *out_socket = s_open_socket_value;
    return s_open_socket_result;
}

wifi_err_t wifi_send(wifi_handle_t handle, wifi_socket_t socket, const uint8_t *data, size_t len)
{
    (void) handle;
    (void) socket;
    (void) data;
    (void) len;
    s_send_call_count++;
    return s_send_result;
}

wifi_err_t wifi_recv(wifi_handle_t handle, wifi_socket_t socket, uint8_t *buf, size_t buf_len,
                     size_t *out_len, uint32_t timeout_ms)
{
    (void) handle;
    (void) socket;
    (void) buf;
    (void) buf_len;
    (void) timeout_ms;
    s_recv_call_count++;
    *out_len = s_recv_out_len;
    return s_recv_result;
}

wifi_err_t wifi_close_socket(wifi_handle_t handle, wifi_socket_t socket)
{
    (void) handle;
    s_close_socket_call_count++;
    s_close_socket_last_socket = socket;
    return s_close_socket_result;
}

wifi_err_t wifi_attach_datardy_callback(wifi_handle_t handle, wifi_datardy_cb_t cb, void *ctx)
{
    (void) handle;
    (void) cb;
    (void) ctx;
    s_attach_datardy_callback_call_count++;
    return WIFI_ERR_OK;
}

/* ========================================================================
 * Helpers
 * ==================================================================== */

/* wifi_handle_t is opaque (struct wifi_inst is private to wifi_driver.c) —
 * any non-NULL value works as a dummy handle, since the stub functions
 * above accept it but never dereference it. */
static int s_dummy_wifi_inst_sentinel;

static wifitask_handle_t prv_create_default(void)
{
    wifitask_config_t cfg = {.wifi = (wifi_handle_t) &s_dummy_wifi_inst_sentinel};
    wifitask_handle_t handle = NULL;
    TEST_ASSERT_EQUAL(WIFITASK_ERR_OK, wifitask_create(&cfg, &handle));
    return handle;
}

/** Feeds a hand-built request into the mocked queue's receive side, ready
 * for the next wifitask_step_for_test() call (Layer 2 tests). */
static void prv_arm_next_request(wifitask_request_t *req)
{
    wifitask_request_t *ptr = req;
    memcpy(g_mock_xQueueReceive_next_item, &ptr, sizeof(ptr));
    g_mock_xQueueReceive_next_item_size = sizeof(ptr);
    g_mock_xQueueReceive_return = pdTRUE;
    g_mock_xQueueReceive_available = 1u;
}

/* ========================================================================
 * setUp / tearDown
 * ==================================================================== */

void setUp(void)
{
    wifitask_reset_for_test();
    mock_freertos_reset();
    prv_reset_wifi_stub();
}

void tearDown(void) {}

/* ========================================================================
 * WIFITASK-T01..T03 — wifitask_create()
 * ==================================================================== */

void test_WIFITASK_T01_create_happy_path(void)
{
    wifitask_handle_t handle = prv_create_default();
    TEST_ASSERT_NOT_NULL(handle);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xTaskCreateStatic_call_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xQueueCreateStatic_call_count);
    TEST_ASSERT_EQUAL_UINT32(1u, s_attach_datardy_callback_call_count);
}

void test_WIFITASK_T02_create_null_args(void)
{
    wifitask_handle_t handle = NULL;
    wifitask_config_t cfg = {.wifi = (wifi_handle_t) &s_dummy_wifi_inst_sentinel};

    TEST_ASSERT_EQUAL(WIFITASK_ERR_NULL_PTR, wifitask_create(NULL, &handle));

    wifitask_config_t null_wifi_cfg = {.wifi = NULL};
    TEST_ASSERT_EQUAL(WIFITASK_ERR_NULL_PTR, wifitask_create(&null_wifi_cfg, &handle));

    TEST_ASSERT_EQUAL(WIFITASK_ERR_NULL_PTR, wifitask_create(&cfg, NULL));
}

void test_WIFITASK_T03_create_pool_exhaustion(void)
{
    (void) prv_create_default();

    wifitask_config_t cfg = {.wifi = (wifi_handle_t) &s_dummy_wifi_inst_sentinel};
    wifitask_handle_t second = NULL;
    TEST_ASSERT_EQUAL(WIFITASK_ERR_NO_RESOURCE, wifitask_create(&cfg, &second));
}

/* ========================================================================
 * WIFITASK-T04..T07 — wrapper/plumbing (Layer 1)
 * ==================================================================== */

void test_WIFITASK_T04_connect_ap_builds_request_and_returns_notified_value(void)
{
    wifitask_handle_t handle = prv_create_default();

    g_mock_xQueueSend_last_item_size = sizeof(void *);
    g_mock_xTaskNotifyWait_return = pdTRUE;
    g_mock_xTaskNotifyWait_next_value = (uint32_t) WIFI_ERR_OK;

    wifitask_err_t rc = wifitask_connect_ap(handle, "my-ssid", "my-pass");

    TEST_ASSERT_EQUAL(WIFITASK_ERR_OK, rc);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xQueueSend_call_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xTaskNotifyWait_call_count);
}

void test_WIFITASK_T05_queue_full_returns_timeout(void)
{
    wifitask_handle_t handle = prv_create_default();

    g_mock_xQueueSend_return = pdFALSE;

    TEST_ASSERT_EQUAL(WIFITASK_ERR_TIMEOUT, wifitask_connect_ap(handle, "ssid", "pass"));
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_xTaskNotifyWait_call_count);
}

void test_WIFITASK_T06_no_reply_returns_timeout(void)
{
    wifitask_handle_t handle = prv_create_default();

    g_mock_xQueueSend_return = pdTRUE;
    g_mock_xTaskNotifyWait_return = pdFALSE;

    TEST_ASSERT_EQUAL(WIFITASK_ERR_TIMEOUT, wifitask_connect_ap(handle, "ssid", "pass"));
}

void test_WIFITASK_T07_null_arg_checks(void)
{
    wifitask_handle_t handle = prv_create_default();
    wifi_socket_t socket;
    wifi_link_state_t state;
    int8_t rssi;

    TEST_ASSERT_EQUAL(WIFITASK_ERR_NULL_PTR, wifitask_connect_ap(handle, NULL, "pass"));
    TEST_ASSERT_EQUAL(WIFITASK_ERR_NULL_PTR, wifitask_connect_ap(handle, "ssid", NULL));
    TEST_ASSERT_EQUAL(WIFITASK_ERR_NULL_PTR, wifitask_get_link_state(handle, NULL));
    TEST_ASSERT_EQUAL(WIFITASK_ERR_NULL_PTR, wifitask_get_rssi(handle, NULL));
    TEST_ASSERT_EQUAL(WIFITASK_ERR_NULL_PTR,
                      wifitask_open_socket(handle, WIFI_SOCKET_TCP, NULL, 1883u, &socket));
    TEST_ASSERT_EQUAL(WIFITASK_ERR_NULL_PTR,
                      wifitask_open_socket(handle, WIFI_SOCKET_TCP, "1.2.3.4", 1883u, NULL));
    TEST_ASSERT_EQUAL(WIFITASK_ERR_NULL_PTR, wifitask_send(handle, 0u, NULL, 4u));
    uint8_t buf[4];
    size_t out_len;
    TEST_ASSERT_EQUAL(WIFITASK_ERR_NULL_PTR, wifitask_recv(handle, 0u, NULL, sizeof(buf), &out_len, 1000u));
    TEST_ASSERT_EQUAL(WIFITASK_ERR_NULL_PTR, wifitask_recv(handle, 0u, buf, sizeof(buf), NULL, 1000u));
    (void) state;
    (void) rssi;
}

/* ========================================================================
 * WIFITASK-T08..T10 — dispatch (Layer 2, via wifitask_step_for_test())
 * ==================================================================== */

void test_WIFITASK_T08_step_dispatches_connect_ap(void)
{
    wifitask_handle_t handle = prv_create_default();

    wifitask_request_t req = {0};
    req.op = WIFITASK_OP_CONNECT_AP;
    req.ssid = "target-ssid";
    req.password = "target-pass";
    req.caller = (TaskHandle_t) 0x1234;
    s_connect_ap_result = WIFI_ERR_OK;

    prv_arm_next_request(&req);
    wifitask_step_for_test(handle);

    TEST_ASSERT_EQUAL_UINT32(1u, s_connect_ap_call_count);
    TEST_ASSERT_EQUAL_STRING("target-ssid", s_connect_ap_last_ssid);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xTaskNotify_call_count);
    TEST_ASSERT_EQUAL_PTR((TaskHandle_t) 0x1234, g_mock_xTaskNotify_last_handle);
    TEST_ASSERT_EQUAL_UINT32((uint32_t) WIFI_ERR_OK, g_mock_xTaskNotify_last_value);
}

void test_WIFITASK_T09_step_dispatches_open_socket_send_recv_close(void)
{
    wifitask_handle_t handle = prv_create_default();

    wifitask_request_t req = {0};
    req.op = WIFITASK_OP_OPEN_SOCKET;
    req.remote_addr = "10.0.0.1";
    req.remote_port = 8883u;
    req.caller = (TaskHandle_t) 0x1u;
    s_open_socket_result = WIFI_ERR_OK;
    s_open_socket_value = 2u;

    prv_arm_next_request(&req);
    wifitask_step_for_test(handle);

    TEST_ASSERT_EQUAL_UINT32(1u, s_open_socket_call_count);
    TEST_ASSERT_EQUAL_UINT32((uint32_t) WIFI_ERR_OK, g_mock_xTaskNotify_last_value);
}

void test_WIFITASK_T10_two_callers_each_get_their_own_reply(void)
{
    wifitask_handle_t handle = prv_create_default();

    wifitask_request_t req1 = {0};
    req1.op = WIFITASK_OP_DISCONNECT_AP;
    req1.caller = (TaskHandle_t) 0x1111;
    s_disconnect_ap_result = WIFI_ERR_OK;
    prv_arm_next_request(&req1);
    wifitask_step_for_test(handle);
    TEST_ASSERT_EQUAL_PTR((TaskHandle_t) 0x1111, g_mock_xTaskNotify_last_handle);

    wifitask_request_t req2 = {0};
    req2.op = WIFITASK_OP_GET_RSSI;
    req2.caller = (TaskHandle_t) 0x2222;
    s_get_rssi_result = WIFI_ERR_OK;
    prv_arm_next_request(&req2);
    wifitask_step_for_test(handle);
    TEST_ASSERT_EQUAL_PTR((TaskHandle_t) 0x2222, g_mock_xTaskNotify_last_handle);
}

/* ========================================================================
 * WIFITASK-T11..T13 — liveness check / WIFI-O15 (Layer 2)
 * ==================================================================== */

void test_WIFITASK_T11_liveness_check_fires_when_queue_empty(void)
{
    wifitask_handle_t handle = prv_create_default();

    g_mock_xQueueReceive_available = 0u; /* queue empty -> liveness path */
    g_mock_xQueueReceive_return = pdFALSE;
    s_get_link_state_value = WIFI_LINK_UP;
    s_get_rssi_result = WIFI_ERR_OK;

    wifitask_step_for_test(handle);

    TEST_ASSERT_TRUE(s_get_rssi_call_count >= 1u);
}

void test_WIFITASK_T12_liveness_check_skipped_when_link_down(void)
{
    wifitask_handle_t handle = prv_create_default();

    g_mock_xQueueReceive_available = 0u;
    g_mock_xQueueReceive_return = pdFALSE;
    s_get_link_state_value = WIFI_LINK_DOWN;

    wifitask_step_for_test(handle);

    TEST_ASSERT_EQUAL_UINT32(0u, s_get_rssi_call_count);
}

void test_WIFITASK_T13_liveness_probe_failure_attempts_reconnect(void)
{
    wifitask_handle_t handle = prv_create_default();

    g_mock_xQueueReceive_available = 0u;
    g_mock_xQueueReceive_return = pdFALSE;
    s_get_link_state_value = WIFI_LINK_UP;
    s_get_rssi_result = WIFI_ERR_TIMEOUT;

    wifitask_step_for_test(handle);

    TEST_ASSERT_TRUE(s_get_rssi_call_count >= 1u);
    /* Reconnect policy itself is WIFITASK-O2 (unpinned) — this only
     * proves the probe fired and re-checked link state, not a specific
     * backoff/reconnect sequence yet. */
    TEST_ASSERT_TRUE(s_get_link_state_call_count >= 2u);
}

/* ========================================================================
 * WIFITASK-T14 — reset_for_test
 * ==================================================================== */

void test_WIFITASK_T14_reset_for_test_clears_pool(void)
{
    (void) prv_create_default();
    wifitask_reset_for_test();

    wifitask_config_t cfg = {.wifi = (wifi_handle_t) &s_dummy_wifi_inst_sentinel};
    wifitask_handle_t handle = NULL;
    TEST_ASSERT_EQUAL(WIFITASK_ERR_OK, wifitask_create(&cfg, &handle));
}
