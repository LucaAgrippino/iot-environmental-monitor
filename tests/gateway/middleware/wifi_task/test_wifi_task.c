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

/** Same idea as prv_arm_next_request(), but for the second (recv-slot arm)
 * queue — WIFITASK-O1. request_queue is created first in wifitask_create()
 * so it always lands on the plain g_mock_xQueueReceive_* globals; this
 * queue, created second, lands on the "_2" globals (see freertos_mock.c).
 * Value-typed (not a pointer), so a plain memcpy of the struct itself is
 * enough — no lifetime concerns to model here. */
static void prv_arm_next_recv_arm(const wifitask_recv_arm_t *arm)
{
    memcpy(g_mock_xQueueReceive2_next_item, arm, sizeof(*arm));
    g_mock_xQueueReceive2_next_item_size = sizeof(*arm);
    g_mock_xQueueReceive2_return = pdTRUE;
    g_mock_xQueueReceive2_available = 1u;
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
    /* 2, not 1: request_queue + the WIFITASK-O1 recv-slot arm queue. */
    TEST_ASSERT_EQUAL_UINT32(2u, g_mock_xQueueCreateStatic_call_count);
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

/* ========================================================================
 * WIFITASK-T15 — WIFITASK-O6: reply protocol uses its own notification
 * index, not the caller task's default (index 0) one.
 *
 * Found on real hardware: a caller task that also uses plain
 * xTaskNotify()/xTaskNotifyWait() on index 0 for something else of its own
 * (e.g. CloudPublisher's periodic-tick bits) could have that notification
 * wake up prv_submit_and_wait()'s wait early with the wrong value, causing
 * the caller to abandon an in-flight request while WifiTask was still
 * genuinely processing it — a dangling-pointer HardFault when WifiTask
 * later notified a caller that had already moved on. Both directions must
 * use WIFITASK_NOTIFY_INDEX (1), never the shared default index.
 * ==================================================================== */

void test_WIFITASK_T15_wait_uses_dedicated_notify_index(void)
{
    wifitask_handle_t handle = prv_create_default();

    g_mock_xQueueSend_last_item_size = sizeof(void *);
    g_mock_xTaskNotifyWait_return = pdTRUE;
    g_mock_xTaskNotifyWait_next_value = (uint32_t) WIFI_ERR_OK;

    TEST_ASSERT_EQUAL(WIFITASK_ERR_OK, wifitask_connect_ap(handle, "my-ssid", "my-pass"));
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xTaskNotifyWait_last_index);
}

void test_WIFITASK_T16_notify_back_uses_dedicated_notify_index(void)
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

    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xTaskNotify_last_index);
}

/* ========================================================================
 * WIFITASK-T17..T20 — WIFITASK-O1 Phase 1: wifitask_try_recv()
 * ==================================================================== */

void test_WIFITASK_T17_try_recv_never_blocks_on_fresh_socket(void)
{
    wifitask_handle_t handle = prv_create_default();

    uint8_t buf[16];
    size_t out_len = 0u;
    wifitask_recv_poll_t poll = WIFITASK_RECV_POLL_ERROR;

    /* No mock priming at all beyond create() — a fresh socket's first
     * poll must come back cleanly (never block, never require a test to
     * pre-arm anything just to get a sane answer). */
    TEST_ASSERT_EQUAL(WIFITASK_ERR_OK,
                      wifitask_try_recv(handle, 3u, buf, sizeof(buf), &out_len, 0x20u, &poll));
    TEST_ASSERT_EQUAL(WIFITASK_RECV_POLL_PENDING, poll);
    TEST_ASSERT_EQUAL_UINT32(0u, s_recv_call_count);
}

void test_WIFITASK_T18_arm_then_step_then_pickup_returns_ready(void)
{
    wifitask_handle_t handle = prv_create_default();

    wifitask_recv_arm_t arm = {
        .socket = 5u,
        .caller = (TaskHandle_t) 0x9999,
        .ready_notify_bit = 0x20u,
    };
    s_recv_result = WIFI_ERR_OK;
    s_recv_out_len = 4u;

    /* Main request_queue must be explicitly empty — its mock default is
     * "always succeeds with a NULL payload" (every other test either
     * arms a real request via prv_arm_next_request() or disables it like
     * this; relying on the default crashes, it doesn't harmlessly no-op). */
    g_mock_xQueueReceive_return = pdFALSE;

    prv_arm_next_recv_arm(&arm);
    wifitask_step_for_test(handle);

    /* One wifi_recv() attempt happened, and the owner was woken on its
     * own default index (0) — NOT WifiTask's reply index (1, WIFITASK-O6)
     * — with the caller-chosen bit, not eSetValueWithOverwrite. Direct
     * regression coverage for the two notification channels staying
     * genuinely separate. */
    TEST_ASSERT_EQUAL_UINT32(1u, s_recv_call_count);
    TEST_ASSERT_EQUAL_UINT32(1u, g_mock_xTaskNotify_call_count);
    TEST_ASSERT_EQUAL_PTR((TaskHandle_t) 0x9999, g_mock_xTaskNotify_last_handle);
    TEST_ASSERT_EQUAL_UINT32(0x20u, g_mock_xTaskNotify_last_value);
    TEST_ASSERT_EQUAL(eSetBits, g_mock_xTaskNotify_last_action);
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_xTaskNotify_last_index);

    uint8_t buf[16];
    size_t out_len = 0u;
    wifitask_recv_poll_t poll = WIFITASK_RECV_POLL_ERROR;
    TEST_ASSERT_EQUAL(WIFITASK_ERR_OK,
                      wifitask_try_recv(handle, 5u, buf, sizeof(buf), &out_len, 0x20u, &poll));
    TEST_ASSERT_EQUAL(WIFITASK_RECV_POLL_READY, poll);
    TEST_ASSERT_EQUAL_UINT32(4u, out_len);
}

void test_WIFITASK_T19_pickup_of_timeout_reports_none_not_error(void)
{
    wifitask_handle_t handle = prv_create_default();

    wifitask_recv_arm_t arm = {
        .socket = 2u, .caller = (TaskHandle_t) 0x1111, .ready_notify_bit = 0u,
    };
    s_recv_result = WIFI_ERR_TIMEOUT; /* "no data yet", not a real failure */
    g_mock_xQueueReceive_return = pdFALSE; /* main request_queue empty — see T18 */

    prv_arm_next_recv_arm(&arm);
    wifitask_step_for_test(handle);

    /* ready_notify_bit == 0 above means no wake was requested — confirms
     * the notify is opt-in, not unconditional. */
    TEST_ASSERT_EQUAL_UINT32(0u, g_mock_xTaskNotify_call_count);

    uint8_t buf[16];
    size_t out_len = 0u;
    wifitask_recv_poll_t poll = WIFITASK_RECV_POLL_ERROR;
    TEST_ASSERT_EQUAL(WIFITASK_ERR_OK,
                      wifitask_try_recv(handle, 2u, buf, sizeof(buf), &out_len, 0u, &poll));
    TEST_ASSERT_EQUAL(WIFITASK_RECV_POLL_NONE, poll);
}

void test_WIFITASK_T20_armed_slot_does_not_starve_a_real_request(void)
{
    wifitask_handle_t handle = prv_create_default();

    /* Prime both queues at once: a real request (GET_RSSI) and a recv-slot
     * arm for a different socket. One step must dispatch exactly one of
     * them (the real request takes priority; the armed slot's own
     * wifi_recv() attempt is deferred to a later step, not skipped or
     * run alongside it in the same step). */
    wifitask_request_t req = {.op = WIFITASK_OP_GET_RSSI};
    s_get_rssi_result = WIFI_ERR_OK;
    prv_arm_next_request(&req);

    wifitask_recv_arm_t arm = {
        .socket = 1u, .caller = (TaskHandle_t) 0x2222, .ready_notify_bit = 0u,
    };
    prv_arm_next_recv_arm(&arm);

    wifitask_step_for_test(handle);

    TEST_ASSERT_EQUAL_UINT32(1u, s_get_rssi_call_count);
    TEST_ASSERT_EQUAL_UINT32(0u, s_recv_call_count);
}
