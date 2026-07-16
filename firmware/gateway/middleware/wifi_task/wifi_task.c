/**
 * @file wifi_task.c
 * @brief WifiTask (Gateway) implementation — request queue, dispatch, liveness tick.
 *
 * @see docs/lld/middleware/wifi-task.md for the design specification this
 *      file implements (companion §4).
 */

#include "wifi_task.h"

#include <string.h>

#include "FreeRTOS.h"
#include "queue.h"
#include "task.h"

#define WIFITASK_MAX_INSTANCES 1u
#define WIFITASK_TASK_STACK_WORDS 256u /**< D29; matches memory-budget.md §1.1 (1 KB). */
#define WIFITASK_TASK_PRIORITY 3u      /**< D29. */
#define WIFITASK_QUEUE_DEPTH 3u        /**< Sized by producer count (companion §2), not throughput. */

/** WIFI_LIVENESS_CHECK_PERIOD_MS: provisional, not yet validated against
 *  field data (WIFITASK-O4, companion §10). Doubles as the queue-receive
 *  timeout — no separate timer object needed (companion §2, §4.3). */
#define WIFI_LIVENESS_CHECK_PERIOD_MS 30000u

#define WIFITASK_ENQUEUE_TIMEOUT_TICKS pdMS_TO_TICKS(1000u)

/** WIFITASK-O6: dedicated task-notification index for WifiTask's own
 * request/reply protocol (requires configTASK_NOTIFICATION_ARRAY_ENTRIES
 * >= 2, set in FreeRTOSConfig.h). Every task has one notification word per
 * index, and plain xTaskNotify()/xTaskNotifyWait() both implicitly target
 * index 0 (tskDEFAULT_INDEX_TO_NOTIFY). A caller task that also uses
 * task notifications for its own purposes on index 0 (e.g. CloudPublisher's
 * periodic-tick bits, sent from the Timer Service task while
 * CloudPublisherTask is blocked here waiting for WifiTask's reply) would
 * otherwise wake this wait early with the wrong value: the caller then
 * returns believing WifiTask replied, abandons its (still-in-flight)
 * request, and reuses that stack memory — so when WifiTask's own blocking
 * wifi_*() call eventually finishes and this module notifies the caller
 * for real, it writes into a dangling stack pointer. Confirmed on real
 * hardware: a HardFault inside xTaskGenericNotify, at different call
 * sites and different times depending on exact tick/reply timing,
 * eventually traced to prv_wifitask_step()'s notify racing
 * CloudPublisher's stats-tick notify on the shared default index. Index 1
 * gives WifiTask's protocol its own private channel, isolated from
 * whatever else the calling task uses notifications for. */
#define WIFITASK_NOTIFY_INDEX 1u

/** WifiDriver's own worst-case timing constants are private to
 *  wifi_driver.c (wifi-driver.md §3.1) — not part of its public header,
 *  so they can't be #included here. Mirrored from the published values
 *  in that companion doc; must be kept in sync if WifiDriver's change. */
#define WIFITASK_WIFI_RESP_TIMEOUT_MS 5000u
#define WIFITASK_WIFI_JOIN_TIMEOUT_MS 20000u
#define WIFITASK_WIFI_SOCKET_CONNECT_TIMEOUT_MS 15000u

/** WIFITASK-O5 resolution: reply-wait bound is this request's own
 *  worst-case dispatch time plus queueing margin for up to 2 other
 *  callers ahead of it (queue depth 3), each up to the longest single
 *  op (WIFITASK_WIFI_JOIN_TIMEOUT_MS). A single blanket constant doesn't
 *  fit — wifitask_recv()'s own worst case is the caller-supplied
 *  timeout_ms, not a fixed driver constant. */
#define WIFITASK_QUEUE_MARGIN_MS (2u * WIFITASK_WIFI_JOIN_TIMEOUT_MS)
#define WIFITASK_REPLY_TIMEOUT_TICKS(op_worst_case_ms) \
    pdMS_TO_TICKS((op_worst_case_ms) + WIFITASK_QUEUE_MARGIN_MS)

/* wifitask_op_t and wifitask_request_t are defined in wifi_task.h, not
 * here — tests construct wifitask_request_t directly to drive
 * wifitask_step_for_test() (companion §9, Layer 2 dispatch tests), since
 * there is no real scheduler in host tests to interleave a public
 * wrapper's enqueue with WifiTask's own dispatch. */

struct wifitask_inst
{
    wifi_handle_t wifi;
    TaskHandle_t task_handle;
    QueueHandle_t request_queue;
    wifi_link_state_t last_known_link_state;
    bool in_use;
};

static struct wifitask_inst g_pool[WIFITASK_MAX_INSTANCES];
static uint32_t g_count;

static StaticTask_t s_wifitask_tcb;
static StackType_t s_wifitask_stack[WIFITASK_TASK_STACK_WORDS];
static StaticQueue_t s_request_queue_ctrl;
static uint8_t s_request_queue_storage[WIFITASK_QUEUE_DEPTH * sizeof(wifitask_request_t *)];

static wifi_err_t prv_dispatch(struct wifitask_inst *inst, wifitask_request_t *req)
{
    switch (req->op)
    {
    case WIFITASK_OP_CONNECT_AP:
        return wifi_connect_ap(inst->wifi, req->ssid, req->password);
    case WIFITASK_OP_DISCONNECT_AP:
        return wifi_disconnect_ap(inst->wifi);
    case WIFITASK_OP_GET_LINK_STATE:
        return wifi_get_link_state(inst->wifi, &req->link_state);
    case WIFITASK_OP_GET_RSSI:
        return wifi_get_rssi(inst->wifi, &req->rssi_dbm);
    case WIFITASK_OP_OPEN_SOCKET:
        return wifi_open_socket(inst->wifi, req->socket_type, req->remote_addr, req->remote_port,
                                &req->socket);
    case WIFITASK_OP_SEND:
        return wifi_send(inst->wifi, req->socket, req->tx_buf, req->tx_len);
    case WIFITASK_OP_RECV:
        return wifi_recv(inst->wifi, req->socket, req->rx_buf, req->rx_buf_len, &req->rx_len,
                         req->timeout_ms);
    case WIFITASK_OP_CLOSE_SOCKET:
        return wifi_close_socket(inst->wifi, req->socket);
    default:
        return WIFI_ERR_INVALID_ARG;
    }
}

/**
 * @brief WIFI-O15: liveness probe, run whenever the request queue is
 * empty for WIFI_LIVENESS_CHECK_PERIOD_MS. Backoff policy (attempt
 * count, delay curve) is deliberately unpinned — WIFITASK-O2.
 */
static void prv_liveness_check(struct wifitask_inst *inst)
{
    (void) wifi_get_link_state(inst->wifi, &inst->last_known_link_state);
    if (inst->last_known_link_state != WIFI_LINK_UP)
    {
        return;
    }

    int8_t rssi_dbm;
    if (wifi_get_rssi(inst->wifi, &rssi_dbm) != WIFI_ERR_OK)
    {
        (void) wifi_get_link_state(inst->wifi, &inst->last_known_link_state);
    }
}

static void prv_wifitask_step(struct wifitask_inst *inst)
{
    wifitask_request_t *req = NULL;
    if (xQueueReceive(inst->request_queue, &req, pdMS_TO_TICKS(WIFI_LIVENESS_CHECK_PERIOD_MS)) ==
        pdPASS)
    {
        req->wifi_status = prv_dispatch(inst, req);
        (void) xTaskNotifyIndexed(req->caller, WIFITASK_NOTIFY_INDEX, (uint32_t) req->wifi_status,
                                  eSetValueWithOverwrite);
    }
    else
    {
        prv_liveness_check(inst);
    }
}

static void prv_wifitask_body(void *arg)
{
    struct wifitask_inst *inst = (struct wifitask_inst *) arg;

    (void) wifi_get_link_state(inst->wifi, &inst->last_known_link_state);

    for (;;)
    {
        prv_wifitask_step(inst);
    }
}

/**
 * @brief Enqueue req and block for WifiTask's reply. Shared by every
 * public wrapper (companion §4.2) — not repeated nine times.
 */
static wifitask_err_t prv_submit_and_wait(wifitask_handle_t handle, wifitask_request_t *req,
                                          uint32_t reply_timeout_ticks)
{
    if (handle == NULL)
    {
        return WIFITASK_ERR_NULL_PTR;
    }

    req->caller = xTaskGetCurrentTaskHandle();
    wifitask_request_t *req_ptr = req;

    if (xQueueSend(handle->request_queue, &req_ptr, WIFITASK_ENQUEUE_TIMEOUT_TICKS) != pdPASS)
    {
        return WIFITASK_ERR_TIMEOUT;
    }

    uint32_t notified_status;
    if (xTaskNotifyWaitIndexed(WIFITASK_NOTIFY_INDEX, 0u, 0xFFFFFFFFu, &notified_status,
                               reply_timeout_ticks) != pdTRUE)
    {
        return WIFITASK_ERR_TIMEOUT;
    }

    /* wifitask_err_t/wifi_err_t share _OK = 0 by construction (companion
     * §4.2) — the raw wifi_err_t from prv_dispatch() passes straight
     * through the wifitask_err_t return type without a translation table. */
    return (wifitask_err_t) notified_status;
}

wifitask_err_t wifitask_create(const wifitask_config_t *config, wifitask_handle_t *handle)
{
    if ((config == NULL) || (config->wifi == NULL) || (handle == NULL))
    {
        return WIFITASK_ERR_NULL_PTR;
    }

    if (g_count >= WIFITASK_MAX_INSTANCES)
    {
        return WIFITASK_ERR_NO_RESOURCE;
    }

    struct wifitask_inst *inst = &g_pool[g_count];
    g_count++;

    (void) memset(inst, 0, sizeof(*inst));
    inst->wifi = config->wifi;

    inst->request_queue = xQueueCreateStatic(WIFITASK_QUEUE_DEPTH,
                                             (UBaseType_t) sizeof(wifitask_request_t *),
                                             s_request_queue_storage, &s_request_queue_ctrl);

    inst->task_handle = xTaskCreateStatic(prv_wifitask_body, "WifiTask", WIFITASK_TASK_STACK_WORDS,
                                          inst, WIFITASK_TASK_PRIORITY, s_wifitask_stack,
                                          &s_wifitask_tcb);

    /* DATARDY ISR is wired but its notification bit is reserved, unused
     * on this increment's hot path (companion §5.4, WIFITASK-D5) — every
     * wifi_*() call prv_dispatch() invokes already blocks to completion
     * internally per WIFI-D11, so there is nothing to wait on yet. */
    (void) wifi_attach_datardy_callback(inst->wifi, NULL, NULL);

    inst->in_use = true;
    *handle = inst;
    return WIFITASK_ERR_OK;
}

wifitask_err_t wifitask_connect_ap(wifitask_handle_t handle, const char *ssid,
                                   const char *password)
{
    if ((ssid == NULL) || (password == NULL))
    {
        return WIFITASK_ERR_NULL_PTR;
    }

    wifitask_request_t req = {
        .op = WIFITASK_OP_CONNECT_AP,
        .ssid = ssid,
        .password = password,
    };
    return prv_submit_and_wait(handle, &req, WIFITASK_REPLY_TIMEOUT_TICKS(WIFITASK_WIFI_JOIN_TIMEOUT_MS));
}

wifitask_err_t wifitask_disconnect_ap(wifitask_handle_t handle)
{
    wifitask_request_t req = {.op = WIFITASK_OP_DISCONNECT_AP};
    return prv_submit_and_wait(handle, &req, WIFITASK_REPLY_TIMEOUT_TICKS(WIFITASK_WIFI_RESP_TIMEOUT_MS));
}

wifitask_err_t wifitask_get_link_state(wifitask_handle_t handle, wifi_link_state_t *state)
{
    if (state == NULL)
    {
        return WIFITASK_ERR_NULL_PTR;
    }

    wifitask_request_t req = {.op = WIFITASK_OP_GET_LINK_STATE};
    wifitask_err_t rc = prv_submit_and_wait(handle, &req,
                                            WIFITASK_REPLY_TIMEOUT_TICKS(WIFITASK_WIFI_RESP_TIMEOUT_MS));
    if (rc == WIFITASK_ERR_OK)
    {
        *state = req.link_state;
    }
    return rc;
}

wifitask_err_t wifitask_get_rssi(wifitask_handle_t handle, int8_t *rssi_dbm)
{
    if (rssi_dbm == NULL)
    {
        return WIFITASK_ERR_NULL_PTR;
    }

    wifitask_request_t req = {.op = WIFITASK_OP_GET_RSSI};
    wifitask_err_t rc = prv_submit_and_wait(handle, &req,
                                            WIFITASK_REPLY_TIMEOUT_TICKS(WIFITASK_WIFI_RESP_TIMEOUT_MS));
    if (rc == WIFITASK_ERR_OK)
    {
        *rssi_dbm = req.rssi_dbm;
    }
    return rc;
}

wifitask_err_t wifitask_open_socket(wifitask_handle_t handle, wifi_socket_type_t type,
                                    const char *remote_addr, uint16_t remote_port,
                                    wifi_socket_t *out_socket)
{
    if ((remote_addr == NULL) || (out_socket == NULL))
    {
        return WIFITASK_ERR_NULL_PTR;
    }

    wifitask_request_t req = {
        .op = WIFITASK_OP_OPEN_SOCKET,
        .socket_type = type,
        .remote_addr = remote_addr,
        .remote_port = remote_port,
    };
    wifitask_err_t rc = prv_submit_and_wait(
        handle, &req, WIFITASK_REPLY_TIMEOUT_TICKS(WIFITASK_WIFI_SOCKET_CONNECT_TIMEOUT_MS));
    if (rc == WIFITASK_ERR_OK)
    {
        *out_socket = req.socket;
    }
    return rc;
}

wifitask_err_t wifitask_send(wifitask_handle_t handle, wifi_socket_t socket, const uint8_t *data,
                             size_t len)
{
    if (data == NULL)
    {
        return WIFITASK_ERR_NULL_PTR;
    }

    wifitask_request_t req = {
        .op = WIFITASK_OP_SEND,
        .socket = socket,
        .tx_buf = data,
        .tx_len = len,
    };
    return prv_submit_and_wait(handle, &req, WIFITASK_REPLY_TIMEOUT_TICKS(WIFITASK_WIFI_RESP_TIMEOUT_MS));
}

wifitask_err_t wifitask_recv(wifitask_handle_t handle, wifi_socket_t socket, uint8_t *buf,
                            size_t buf_len, size_t *out_len, uint32_t timeout_ms)
{
    if ((buf == NULL) || (out_len == NULL))
    {
        return WIFITASK_ERR_NULL_PTR;
    }

    wifitask_request_t req = {
        .op = WIFITASK_OP_RECV,
        .socket = socket,
        .rx_buf = buf,
        .rx_buf_len = buf_len,
        .timeout_ms = timeout_ms,
    };
    /* Reply bound uses THIS request's own caller-supplied timeout_ms, not a
     * fixed driver constant — wifitask_recv()'s worst case is caller-
     * controlled (WIFITASK-O5). */
    wifitask_err_t rc = prv_submit_and_wait(handle, &req, WIFITASK_REPLY_TIMEOUT_TICKS(timeout_ms));
    if (rc == WIFITASK_ERR_OK)
    {
        *out_len = req.rx_len;
    }
    return rc;
}

wifitask_err_t wifitask_close_socket(wifitask_handle_t handle, wifi_socket_t socket)
{
    wifitask_request_t req = {
        .op = WIFITASK_OP_CLOSE_SOCKET,
        .socket = socket,
    };
    return prv_submit_and_wait(handle, &req, WIFITASK_REPLY_TIMEOUT_TICKS(WIFITASK_WIFI_RESP_TIMEOUT_MS));
}

#ifdef TEST
void wifitask_reset_for_test(void)
{
    (void) memset(g_pool, 0, sizeof(g_pool));
    g_count = 0u;
}

void wifitask_step_for_test(wifitask_handle_t handle)
{
    prv_wifitask_step(handle);
}
#endif
