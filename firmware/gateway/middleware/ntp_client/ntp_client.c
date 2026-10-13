/**
 * @file ntp_client.c
 * @brief NtpClient implementation (Gateway).
 *
 * @see docs/lld/middleware/ntp-client.md
 */

#include "ntp_client.h"

#include <stddef.h>
#include <string.h>

#ifdef TEST
#define LOG_WARN(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

#define MODULE_TAG "NTP"

#define NTP_CLIENT_MAX_INSTANCES (1U)
#define NTP_SERVER_PORT (123U)
#define NTP_PACKET_BYTES (48U)
#define NTP_REQUEST_LI_VN_MODE (0x1BU) /* LI=0, VN=3, Mode=3 (client) */
#define NTP_MODE_MASK (0x07U)
#define NTP_MODE_SERVER (4U)
#define NTP_STRATUM_MAX (15U)      /* 0 is Kiss-of-Death, 16 is unsynchronised */
#define NTP_TX_TS_SEC_OFFSET (40U) /* transmit timestamp, seconds */

#define BITS_PER_BYTE (8U)
#define BYTE_MASK (0xFFU)

/* Documents the wire layout; only li_vn_mode and stratum are read by name. */
typedef struct __attribute__((packed))
{
    uint8_t li_vn_mode;       /* byte 0: LI(2)|VN(3)|Mode(3) */
    uint8_t stratum;          /* byte 1 */
    uint8_t poll;             /* byte 2 */
    int8_t precision;         /* byte 3 */
    uint32_t root_delay;      /* bytes 4-7 */
    uint32_t root_dispersion; /* bytes 8-11 */
    uint32_t reference_id;    /* bytes 12-15 */
    uint32_t ref_ts_sec;      /* bytes 16-19 */
    uint32_t ref_ts_frac;     /* bytes 20-23 */
    uint32_t orig_ts_sec;     /* bytes 24-27 */
    uint32_t orig_ts_frac;    /* bytes 28-31 */
    uint32_t rx_ts_sec;       /* bytes 32-35 */
    uint32_t rx_ts_frac;      /* bytes 36-39 */
    uint32_t tx_ts_sec;       /* bytes 40-43: the only field used */
    uint32_t tx_ts_frac;      /* bytes 44-47 */
} ntp_packet_t;

struct ntp_client_inst
{
    wifitask_handle_t wifi;
    health_monitor_handle_t health;
    bool in_use;
};

static struct ntp_client_inst g_pool[NTP_CLIENT_MAX_INSTANCES];
static uint8_t g_count;

/** Read a big-endian 32-bit field from the wire bytes; independent of host byte order. */
static uint32_t read_be32(const uint8_t *bytes)
{
    return ((uint32_t) bytes[0] << (3U * BITS_PER_BYTE)) |
           ((uint32_t) bytes[1] << (2U * BITS_PER_BYTE)) | ((uint32_t) bytes[2] << BITS_PER_BYTE) |
           (uint32_t) bytes[3];
}

/** True if the reply is a usable server answer (companion §5.3). */
static bool reply_is_sane(const ntp_packet_t *pkt, uint32_t tx_ts_sec)
{
    if ((pkt->li_vn_mode & NTP_MODE_MASK) != NTP_MODE_SERVER)
    {
        return false;
    }
    if ((pkt->stratum == 0U) || (pkt->stratum > NTP_STRATUM_MAX))
    {
        return false;
    }
    return (tx_ts_sec != 0U) && (tx_ts_sec >= NTP_UNIX_OFFSET);
}

/**
 * Ask one server.  Returns true and sets *unix_epoch when a valid reply arrived.
 * Pushes NTP_BAD_RESPONSE itself when a reply was received but rejected.
 */
static bool query_one(const struct ntp_client_inst *inst, const char *server, uint32_t *unix_epoch)
{
    wifi_socket_t sock = WIFI_INVALID_SOCKET;
    ntp_packet_t pkt;
    size_t rcvd = 0U;
    bool ok = false;

    if (server == NULL)
    {
        return false;
    }

    if (wifitask_open_socket(inst->wifi, WIFI_SOCKET_UDP, server, NTP_SERVER_PORT, &sock) !=
        (wifitask_err_t) WIFI_ERR_OK)
    {
        LOG_WARN(MODULE_TAG, "open_socket failed for %s", server);
        return false;
    }

    (void) memset(&pkt, 0, sizeof(pkt));
    pkt.li_vn_mode = NTP_REQUEST_LI_VN_MODE;

    if (wifitask_send(inst->wifi, sock, (const uint8_t *) &pkt, NTP_PACKET_BYTES) !=
        (wifitask_err_t) WIFI_ERR_OK)
    {
        LOG_WARN(MODULE_TAG, "send failed for %s", server);
    }
    else if ((wifitask_recv(inst->wifi, sock, (uint8_t *) &pkt, NTP_PACKET_BYTES, &rcvd,
                            NTP_CLIENT_QUERY_TIMEOUT_MS) != (wifitask_err_t) WIFI_ERR_OK) ||
             (rcvd != NTP_PACKET_BYTES))
    {
        LOG_WARN(MODULE_TAG, "no reply from %s", server);
    }
    else
    {
        const uint32_t tx_ts_sec = read_be32(&((const uint8_t *) &pkt)[NTP_TX_TS_SEC_OFFSET]);

        if (reply_is_sane(&pkt, tx_ts_sec))
        {
            *unix_epoch = tx_ts_sec - NTP_UNIX_OFFSET;
            ok = true;
        }
        else
        {
            LOG_WARN(MODULE_TAG, "bad reply from %s", server);
            (void) health_monitor_push_event(inst->health, HEALTH_EVENT_NTP_BAD_RESPONSE, 0U);
        }
    }

    (void) wifitask_close_socket(inst->wifi, sock);
    return ok;
}

ntp_client_err_t ntp_client_create(const ntp_client_config_t *config, ntp_client_handle_t *handle)
{
    if ((config == NULL) || (handle == NULL) || (config->wifi == NULL) || (config->health == NULL))
    {
        return NTP_CLIENT_ERR_NULL_PTR;
    }
    if (g_count >= NTP_CLIENT_MAX_INSTANCES)
    {
        return NTP_CLIENT_ERR_NO_RESOURCE;
    }

    struct ntp_client_inst *inst = &g_pool[g_count];

    g_count++;
    inst->wifi = config->wifi;
    inst->health = config->health;
    inst->in_use = true;
    *handle = inst;
    return NTP_CLIENT_ERR_OK;
}

ntp_client_err_t ntp_client_query(ntp_client_handle_t handle, const char *const *server_list,
                                  uint8_t server_count, uint32_t *unix_epoch_out)
{
    if ((handle == NULL) || (server_list == NULL) || (unix_epoch_out == NULL) ||
        (server_count == 0U))
    {
        return NTP_CLIENT_ERR_NULL_PTR;
    }
    if ((server_count > NTP_CLIENT_MAX_SERVERS) || !handle->in_use)
    {
        return NTP_CLIENT_ERR_INVALID_ARG;
    }

    for (uint8_t i = 0U; i < server_count; i++)
    {
        if (query_one(handle, server_list[i], unix_epoch_out))
        {
            return NTP_CLIENT_ERR_OK;
        }
    }

    (void) health_monitor_push_event(handle->health, HEALTH_EVENT_NTP_SYNC_FAILED, 0U);
    return NTP_CLIENT_ERR_ALL_FAILED;
}

#ifdef TEST
void ntp_client_reset_for_test(void)
{
    (void) memset(g_pool, 0, sizeof(g_pool));
    g_count = 0U;
}
#endif /* TEST */
