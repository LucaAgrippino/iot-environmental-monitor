/**
 * @file ntp_client.h
 * @brief NtpClient (Gateway middleware) — one-shot SNTPv4 query over a UDP socket.
 *
 * Stateless apart from its injected dependencies: no RTC write, no sync flag,
 * no retry scheduling — those belong to TimeService.  Runs in TimeServiceTask
 * context and has no thread of its own.
 *
 * WiFi access goes through WifiTask, the sole owner of WifiDriver (D29), not
 * through WifiDriver directly (NTP-D6).
 *
 * @note Companion: docs/lld/middleware/ntp-client.md
 */

#ifndef NTP_CLIENT_H
#define NTP_CLIENT_H

#include <stdbool.h>
#include <stdint.h>

#include "health_monitor/health_monitor.h"
#include "wifi_task/wifi_task.h"

/** @brief Opaque handle to an NtpClient instance. */
typedef struct ntp_client_inst *ntp_client_handle_t;

/** @brief Error codes returned by NtpClient operations. */
typedef enum
{
    NTP_CLIENT_ERR_OK = 0,           /**< Success. */
    NTP_CLIENT_ERR_NULL_PTR = 1,     /**< A required pointer is NULL, or server_count is 0. */
    NTP_CLIENT_ERR_NO_RESOURCE = 2,  /**< Instance pool exhausted. */
    NTP_CLIENT_ERR_ALL_FAILED = 3,   /**< No server returned a usable reply. */
    NTP_CLIENT_ERR_BAD_RESPONSE = 4, /**< Reserved: reply failed a sanity check. */
    NTP_CLIENT_ERR_WIFI = 5,         /**< Reserved: WiFi layer returned an error. */
    NTP_CLIENT_ERR_INVALID_ARG = 6,  /**< server_count too large, or a stale handle (NTP-D6). */
} ntp_client_err_t;

/** @brief Longest server list a single query accepts. */
#define NTP_CLIENT_MAX_SERVERS (4U)
/** @brief Per-server reply timeout; provisional, see NTP-O1. */
#define NTP_CLIENT_QUERY_TIMEOUT_MS (3000U)
/** @brief Seconds between the NTP epoch (1900) and the Unix epoch (1970). */
#define NTP_UNIX_OFFSET (2208988800UL)

/** @brief NtpClient creation configuration. */
typedef struct
{
    wifitask_handle_t wifi;         /**< WifiTask handle used for UDP sockets. */
    health_monitor_handle_t health; /**< HealthMonitor handle for failure events. */
} ntp_client_config_t;

/**
 * @brief Create and initialise an NtpClient instance.
 *
 * Stores the injected dependencies.  Performs no network I/O.
 *
 * @param[in]  config  Injected dependencies; both handles must be non-NULL.
 * @param[out] handle  Receives the created handle on success.
 * @return NTP_CLIENT_ERR_OK on success; NTP_CLIENT_ERR_NULL_PTR if @p config,
 *         @p handle or either dependency handle is NULL;
 *         NTP_CLIENT_ERR_NO_RESOURCE if the pool is exhausted.
 * @note Threading: task-context only.  Call before the scheduler starts.
 */
ntp_client_err_t ntp_client_create(const ntp_client_config_t *config, ntp_client_handle_t *handle);

/**
 * @brief Query NTP servers and return a Unix epoch timestamp.
 *
 * Tries each server in order and returns on the first valid reply.  A reply
 * that fails the sanity checks pushes HEALTH_EVENT_NTP_BAD_RESPONSE and the next
 * server is tried.  If every server fails, pushes HEALTH_EVENT_NTP_SYNC_FAILED
 * once and returns NTP_CLIENT_ERR_ALL_FAILED.  The server list is passed on every
 * call and never cached.  Servers must be dotted-decimal IPv4 addresses: the
 * ISM43362 has no DNS resolver.
 *
 * The plausibility check against the RTC is TimeService's job; the raw server
 * time is returned.
 *
 * @param[in]  handle          NtpClient handle.
 * @param[in]  server_list     Array of null-terminated IPv4 address strings.
 * @param[in]  server_count    Entries in @p server_list, 1..NTP_CLIENT_MAX_SERVERS.
 * @param[out] unix_epoch_out  Receives Unix epoch seconds on success.
 * @return NTP_CLIENT_ERR_OK; NTP_CLIENT_ERR_ALL_FAILED;
 *         NTP_CLIENT_ERR_NULL_PTR if @p handle, @p server_list or @p unix_epoch_out
 *         is NULL or @p server_count is 0; NTP_CLIENT_ERR_INVALID_ARG if
 *         @p server_count exceeds NTP_CLIENT_MAX_SERVERS or the handle is stale.
 * @note Threading: task-context only, blocking.  Worst case is
 *       server_count x (socket open + NTP_CLIENT_QUERY_TIMEOUT_MS + close) and the
 *       open alone can take WIFI_SOCKET_CONNECT_TIMEOUT_MS.  Not ISR-safe.
 */
ntp_client_err_t ntp_client_query(ntp_client_handle_t handle, const char *const *server_list,
                                  uint8_t server_count, uint32_t *unix_epoch_out);

#ifdef TEST
/**
 * @brief Reset all module state (unit tests only).
 */
void ntp_client_reset_for_test(void);
#endif /* TEST */

#endif /* NTP_CLIENT_H */
