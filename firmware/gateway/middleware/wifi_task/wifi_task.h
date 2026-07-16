/**
 * @file wifi_task.h
 * @brief WifiTask (Gateway) — sole owning task for WifiDriver access (D29).
 *
 * Serialises all WiFi I/O behind a request queue dispatched from WifiTask's
 * own FreeRTOS task (priority 3, 256 words). Every public function here
 * mirrors a WifiDriver function 1:1 (same name minus prefix, same parameter
 * order): the caller still blocks for the same wall-clock duration the
 * underlying wifi_*() call takes — this is a synchronous relocate, not a
 * non-blocking API. See docs/lld/middleware/wifi-task.md for the full
 * design, including why (§5.2) and what stays open (§10, WIFITASK-O1..O5).
 *
 * @note See docs/lld/middleware/wifi-task.md for the full design
 *       specification.
 */

#ifndef WIFI_TASK_H
#define WIFI_TASK_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "wifi_driver/wifi_driver.h"

#define WIFITASK_MAX_SSID_LEN WIFI_MAX_SSID_LEN
#define WIFITASK_MAX_PASS_LEN WIFI_MAX_PASS_LEN

/** @brief Opaque handle to a WifiTask instance. */
typedef struct wifitask_inst *wifitask_handle_t;

typedef enum
{
    WIFITASK_ERR_OK = 0,
    WIFITASK_ERR_NULL_PTR = 1,    /**< Required pointer argument was NULL.     */
    WIFITASK_ERR_NO_RESOURCE = 2, /**< Static pool or request queue exhausted. */
    WIFITASK_ERR_TIMEOUT = 3,     /**< Request queue full, or no reply from
                                        WifiTask within a bounded wait.        */
    /* All other outcomes pass the underlying wifi_err_t through unchanged —
     * WifiTask does not reinterpret WifiDriver's error codes (companion §7). */
} wifitask_err_t;

typedef enum
{
    WIFITASK_OP_CONNECT_AP = 0,
    WIFITASK_OP_DISCONNECT_AP,
    WIFITASK_OP_GET_LINK_STATE,
    WIFITASK_OP_GET_RSSI,
    WIFITASK_OP_OPEN_SOCKET,
    WIFITASK_OP_SEND,
    WIFITASK_OP_RECV,
    WIFITASK_OP_CLOSE_SOCKET,
} wifitask_op_t;

/**
 * @brief One request, allocated on the CALLING task's own stack.
 *
 * Zero heap, zero copy: the caller builds this locally, pushes a pointer
 * to the request queue, then blocks on xTaskNotifyWait(). WifiTask writes
 * wifi_status/out_* fields directly into it before notifying — safe
 * because the design guarantees exactly one outstanding request per
 * caller (companion §2), so nothing else touches this memory while
 * WifiTask has the pointer. Defined here (not privately in wifi_task.c)
 * because tests construct these directly to drive wifitask_step_for_test()
 * (companion §9, Layer 2) — there is no real scheduler in host tests to
 * interleave a public wrapper's enqueue with WifiTask's own dispatch.
 */
typedef struct
{
    wifitask_op_t op;
    TaskHandle_t caller;

    /* CONNECT_AP */
    const char *ssid;
    const char *password;

    /* GET_LINK_STATE (out) */
    wifi_link_state_t link_state;

    /* GET_RSSI (out) */
    int8_t rssi_dbm;

    /* OPEN_SOCKET */
    wifi_socket_type_t socket_type;
    const char *remote_addr;
    uint16_t remote_port;

    /* SEND / RECV / CLOSE_SOCKET */
    wifi_socket_t socket;
    const uint8_t *tx_buf;
    size_t tx_len;
    uint8_t *rx_buf;
    size_t rx_buf_len;
    size_t rx_len;
    uint32_t timeout_ms;

    wifi_err_t wifi_status;
} wifitask_request_t;

/**
 * @brief Outcome of a wifitask_try_recv() poll (WIFITASK-O1).
 */
typedef enum
{
    WIFITASK_RECV_POLL_PENDING = 0, /**< Armed; WifiTask hasn't attempted it yet. */
    WIFITASK_RECV_POLL_READY,       /**< buf/out_len filled in by this call.      */
    WIFITASK_RECV_POLL_NONE,        /**< Last attempt: no data. Re-armed.         */
    WIFITASK_RECV_POLL_ERROR,       /**< Last attempt: real error. Re-armed.      */
} wifitask_recv_poll_t;

/**
 * @brief One "arm" request for the non-blocking recv path (WIFITASK-O1).
 *
 * Deliberately value-typed (no pointers into the caller's own memory,
 * unlike wifitask_request_t) — a wifitask_try_recv() caller returns
 * immediately, so nothing about it can be assumed to still be alive by
 * the time WifiTask gets around to processing this. Copied by value
 * through its own small queue. Defined here (not privately in
 * wifi_task.c) for the same reason wifitask_request_t is: tests construct
 * these directly to drive wifitask_step_for_test() (companion §9), since
 * there is no real scheduler in host tests to interleave a public
 * wrapper's enqueue with WifiTask's own dispatch.
 */
typedef struct
{
    wifi_socket_t socket;
    TaskHandle_t caller;
    uint32_t ready_notify_bit;
} wifitask_recv_arm_t;

/**
 * @brief WifiTask creation configuration.
 *
 * Injected dependency: the WifiDriver handle from wifi_create(), which
 * must already have completed (pre-scheduler, per wifi-driver.md §3.6).
 */
typedef struct
{
    wifi_handle_t wifi; /**< WifiDriver handle — already created. */
} wifitask_config_t;

/**
 * @brief Create a WifiTask instance and start its FreeRTOS task.
 *
 * Starts the WifiTask task (priority 3, 256-word/1 KB stack, D29) via
 * xTaskCreateStatic(). The task immediately begins its request/liveness
 * loop (companion §2). wifi_create() must have already completed
 * successfully — this function does not initialise WifiDriver itself.
 *
 * @param[in]  config  Injected WifiDriver handle.
 * @param[out] handle  Receives the created handle on success.
 * @return WIFITASK_ERR_OK on success; WIFITASK_ERR_NULL_PTR if config,
 *         config->wifi, or handle is NULL; WIFITASK_ERR_NO_RESOURCE if
 *         the static pool is exhausted.
 * @note Threading: task-context only. Call after the scheduler has
 *       started (the task it creates cannot run before that).
 */
wifitask_err_t wifitask_create(const wifitask_config_t *config, wifitask_handle_t *handle);

/**
 * @brief Connect to a WiFi access point (routed through WifiTask).
 *
 * Enqueues a request and blocks until WifiTask dispatches
 * wifi_connect_ap() and replies. Same blocking wall-clock behaviour as
 * calling wifi_connect_ap() directly (WIFI_JOIN_TIMEOUT_MS, 20 s worst
 * case) — this is a synchronous relocate, not a non-blocking API
 * (companion §5.2).
 *
 * @param[in] handle    WifiTask handle.
 * @param[in] ssid      Null-terminated SSID (max WIFITASK_MAX_SSID_LEN).
 * @param[in] password  Null-terminated password (max WIFITASK_MAX_PASS_LEN).
 * @return WIFITASK_ERR_TIMEOUT if the request queue is full or WifiTask
 *         does not reply within a bounded wait; otherwise the wifi_err_t
 *         wifi_connect_ap() itself returned, passed through unchanged.
 * @note Threading: task-context only, blocking. Not ISR-safe. Callable
 *       from any task except WifiTask itself (self-enqueue would
 *       deadlock — see companion §5.5).
 */
wifitask_err_t wifitask_connect_ap(wifitask_handle_t handle, const char *ssid,
                                   const char *password);

/**
 * @brief Disconnect from the current access point (routed through WifiTask).
 *
 * @note Same blocking/threading contract as wifitask_connect_ap().
 */
wifitask_err_t wifitask_disconnect_ap(wifitask_handle_t handle);

/**
 * @brief Query the current WiFi link state (routed through WifiTask).
 *
 * @note Same blocking/threading contract as wifitask_connect_ap(), though
 *       the underlying wifi_get_link_state() call itself is non-blocking
 *       (cached read) — only the queue round trip adds latency here.
 */
wifitask_err_t wifitask_get_link_state(wifitask_handle_t handle, wifi_link_state_t *state);

/**
 * @brief Read the current RSSI from the access point (routed through
 *        WifiTask).
 *
 * @note Same blocking/threading contract as wifitask_connect_ap().
 */
wifitask_err_t wifitask_get_rssi(wifitask_handle_t handle, int8_t *rssi_dbm);

/**
 * @brief Open a TCP or UDP socket (routed through WifiTask).
 *
 * @note Same blocking/threading contract as wifitask_connect_ap().
 *       Mirrors wifi_open_socket()'s parameters and worst-case timing
 *       (WIFI_SOCKET_CONNECT_TIMEOUT_MS, 15 s).
 */
wifitask_err_t wifitask_open_socket(wifitask_handle_t handle, wifi_socket_type_t type,
                                    const char *remote_addr, uint16_t remote_port,
                                    wifi_socket_t *out_socket);

/**
 * @brief Send data on an open socket (routed through WifiTask).
 *
 * @note Same blocking/threading contract as wifitask_connect_ap().
 *       Mirrors wifi_send()'s parameters.
 */
wifitask_err_t wifitask_send(wifitask_handle_t handle, wifi_socket_t socket, const uint8_t *data,
                             size_t len);

/**
 * @brief Receive data from an open socket (routed through WifiTask).
 *
 * @note Same blocking/threading contract as wifitask_connect_ap().
 *       Mirrors wifi_recv()'s parameters and worst-case timing (floored
 *       at WIFI_RESP_TIMEOUT_MS, 5 s — this is MQTT-O7's unresolved
 *       stall, now relocated but not shortened; see companion §10,
 *       WIFITASK-O1).
 */
wifitask_err_t wifitask_recv(wifitask_handle_t handle, wifi_socket_t socket, uint8_t *buf,
                            size_t buf_len, size_t *out_len, uint32_t timeout_ms);

/**
 * @brief Poll for received data without blocking (WIFITASK-O1, Phase 1).
 *
 * Never blocks more than a zero-timeout enqueue and a zero-timeout
 * notification check — safe to call from a task that cannot afford to be
 * parked for wifi_recv()'s real worst case (up to ~5 s per attempt,
 * WIFITASK_WIFI_RESP_TIMEOUT_MS). The first call for a given socket
 * arms a background attempt and returns WIFITASK_RECV_POLL_PENDING;
 * WifiTask's own task runs one full wifi_recv() attempt per poll cycle
 * from its own loop, re-arming automatically. A later call picks up
 * whatever that attempt resolved to (READY with buf/out_len filled in,
 * NONE, or ERROR) and re-arms for the next cycle. ready_notify_bit, if
 * non-zero, is set on the calling task's own default notification index
 * (0) — a distinct channel from WifiTask's own reply protocol on
 * WIFITASK_NOTIFY_INDEX (1) — when a background attempt completes, so a
 * caller blocked in its own xTaskNotifyWait() elsewhere doesn't have to
 * poll blindly. Coexists with the unchanged, still-blocking
 * wifitask_recv() above — this does not replace it.
 *
 * @param[in]  handle           WifiTask handle.
 * @param[in]  socket           Socket to poll.
 * @param[out] buf              Filled in only when *out_poll == READY.
 * @param[in]  buf_len          Capacity of buf.
 * @param[out] out_len          Bytes written to buf, only when READY.
 * @param[in]  ready_notify_bit Bit to set on the caller's own index-0 word
 *                              when a background attempt completes; 0 to
 *                              skip the wake notification.
 * @param[out] out_poll         Outcome of this specific call.
 * @return WIFITASK_ERR_OK once *out_poll has a valid outcome (including
 *         PENDING); WIFITASK_ERR_NULL_PTR if buf, out_len, or out_poll is
 *         NULL; WIFITASK_ERR_NO_RESOURCE if the arm queue is momentarily
 *         full (try again on the next poll — never blocks for it).
 * @note Threading: task-context only, never blocks. Not ISR-safe.
 */
wifitask_err_t wifitask_try_recv(wifitask_handle_t handle, wifi_socket_t socket, uint8_t *buf,
                                 size_t buf_len, size_t *out_len, uint32_t ready_notify_bit,
                                 wifitask_recv_poll_t *out_poll);

/**
 * @brief Close an open socket (routed through WifiTask).
 *
 * @note Same blocking/threading contract as wifitask_connect_ap().
 */
wifitask_err_t wifitask_close_socket(wifitask_handle_t handle, wifi_socket_t socket);

#ifdef TEST

/** @brief Reset all internal static state for unit testing. */
void wifitask_reset_for_test(void);

/**
 * @brief Runs exactly one iteration of WifiTask's loop (single-step
 * testing) — either one dispatched request, or one liveness check if the
 * queue was empty (companion §2, §4.3).
 */
void wifitask_step_for_test(wifitask_handle_t handle);

#endif /* TEST */

#endif /* WIFI_TASK_H */
