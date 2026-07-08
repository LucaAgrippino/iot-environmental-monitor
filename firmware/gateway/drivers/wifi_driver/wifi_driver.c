/**
 * @file wifi_driver.c
 * @brief WiFi driver implementation — AT-command engine, SPI protocol,
 *        socket table, DATARDY ISR (Gateway).
 *
 * AT command set: Inventek's IWIN command set (companion §3.3, WIFI-D12),
 * verified against the IWIN AT Command Set User Manual and quick
 * reference — NOT a Hayes "AT+" set. Every command is `<CODE>[=<value>]<CR>`
 * with no attention prefix. Command codes live in wifi_at_commands.h.
 *
 * @note See docs/lld/drivers/wifi-driver.md for the full design specification.
 */

#include "wifi_driver.h"

#include <stdio.h>
#include <string.h>

#include "cpu/cpu.h"
#include "wifi_at_commands.h"

#define WIFI_MAX_INSTANCES 1u
#define WIFI_AT_BUF_SIZE 512u
#define WIFI_DRDY_TIMEOUT_MS 100u  /**< DRDY assert wait (WIFI-O5). */
#define WIFI_RESP_TIMEOUT_MS 5000u /**< AT response wait (WIFI-O5). */
#define WIFI_RESET_PULSE_MS 10u
#define WIFI_BOOT_WAIT_MS 500u
/** Post-reset DRDY-high waits during the boot sequence only: the boot
 *  cursor's own wait, and the wait for the first genuine Command Phase
 *  right after it. Distinct from WIFI_DRDY_TIMEOUT_MS: that one bounds
 *  steady-state per-command turnaround once the module is already up, but
 *  the module's own reset-to-ready time isn't in the datasheet and
 *  measured well past WIFI_BOOT_WAIT_MS + WIFI_DRDY_TIMEOUT_MS at *both*
 *  of those post-reset transitions on real hardware (WIFI-O7) — hence the
 *  separate, generous bound here. */
#define WIFI_BOOT_DRDY_TIMEOUT_MS 3000u
#define WIFI_POLL_INTERVAL_US 100u
/** NAK — appended to an odd-length command to reach an even byte count.
 *  Confirmed against the quick reference's worked example (P0=0\r padded
 *  to "0P 0= 0x15\r"); NOT 0x0A as the general datasheet text suggested. */
#define WIFI_CMD_PAD_BYTE 0x15u
/** LF — clocked out by the host on MOSI while draining a Data Phase
 *  (datasheet §10.2.1/§10.2.4: "clock out 0x0A until DRDY lowers"). */
#define WIFI_READ_FILL_BYTE 0x0Au
#define WIFI_MAX_PACKET_SIZE 1460u /**< S1/R1/S3 packet-size ceiling (IWIN spec). */
#define WIFI_NUMSTR_MAX 6u         /**< Fits "65535\0". */

struct wifi_inst
{
    /* Injected dependencies */
    spi_handle_t spi;
    gpio_port_t nss_port;
    uint8_t nss_pin;
    gpio_port_t drdy_port;
    uint8_t drdy_pin;
    gpio_port_t rst_port;
    uint8_t rst_pin;
    gpio_port_t wakeup_port;
    uint8_t wakeup_pin;
    gpio_port_t boot0_port;
    uint8_t boot0_pin;

    /* Runtime state */
    wifi_link_state_t link_state;
    int8_t rssi_dbm;
    bool socket_open[WIFI_MAX_SOCKETS];
    wifi_datardy_cb_t datardy_cb;
    void *datardy_ctx;
    bool ready;
    bool in_use;

    /* AT command working buffer — reused for both TX formatting and RX. */
    char at_buf[WIFI_AT_BUF_SIZE];
};

static struct wifi_inst g_pool[WIFI_MAX_INSTANCES];
static uint8_t g_count;

/* Bring-up diagnostics (WIFI-O7 follow-up) — see wifi_driver.h. TEMPORARY. */
static wifi_bringup_diag_t g_diag;

#ifdef TEST
#define WIFI_TEST_VISIBLE
#else
#define WIFI_TEST_VISIBLE static
#endif

/* ====================================================================== */
/* Response parsing helpers                                                */
/* ====================================================================== */

static bool prv_contains(const char *haystack, size_t haystack_len, const char *needle)
{
    const size_t needle_len = strlen(needle);

    if (needle_len == 0u || haystack_len < needle_len)
    {
        return false;
    }

    for (size_t i = 0u; i <= (haystack_len - needle_len); i++)
    {
        if (memcmp(&haystack[i], needle, needle_len) == 0)
        {
            return true;
        }
    }
    return false;
}

WIFI_TEST_VISIBLE wifi_err_t prv_parse_response(const char *resp, size_t resp_len)
{
    if (prv_contains(resp, resp_len, WIFI_RESP_ERROR_MARKER))
    {
        return WIFI_ERR_MODULE;
    }
    if (prv_contains(resp, resp_len, WIFI_RESP_OK_MARKER))
    {
        return WIFI_ERR_OK;
    }
    return WIFI_ERR_TIMEOUT;
}

/**
 * @brief Parse a bare `CR` response: optional leading '-', then digits.
 *
 * The IWIN `CR` command returns the RSSI with no marker beyond the usual
 * response framing — every IWIN response is "\r\n<data>\r\nOK\r\n..."
 * (User Manual §1.4.2; confirmed on hardware for every response captured
 * so far), so the bare value itself starts after that leading "\r\n", not
 * at byte 0.
 */
WIFI_TEST_VISIBLE wifi_err_t prv_parse_rssi(const char *resp, size_t resp_len, int8_t *out_rssi)
{
    size_t pos = 0u;
    bool negative = false;
    int32_t value = 0;
    bool any_digit = false;

    if ((resp_len >= 2u) && (resp[0] == '\r') && (resp[1] == '\n'))
    {
        pos = 2u;
    }

    if ((pos < resp_len) && (resp[pos] == '-'))
    {
        negative = true;
        pos++;
    }

    while ((pos < resp_len) && (resp[pos] >= '0') && (resp[pos] <= '9'))
    {
        value = (value * 10) + (resp[pos] - '0');
        pos++;
        any_digit = true;
    }

    if (!any_digit)
    {
        return WIFI_ERR_MODULE;
    }

    *out_rssi = (int8_t) (negative ? -value : value);
    return WIFI_ERR_OK;
}

WIFI_TEST_VISIBLE wifi_err_t prv_check_firmware_version(const char *resp, size_t resp_len)
{
    if (prv_contains(resp, resp_len, WIFI_FIRMWARE_VERSION))
    {
        return WIFI_ERR_OK;
    }
    return WIFI_ERR_FIRMWARE;
}

/* ====================================================================== */
/* SPI / DRDY transaction engine                                           */
/* ====================================================================== */

static bool prv_wait_drdy(const struct wifi_inst *inst, bool want_high, uint32_t timeout_ms)
{
    const uint32_t timeout_us = timeout_ms * 1000u;
    uint32_t elapsed_us = 0u;

    for (;;)
    {
        gpio_level_t level;
        if (gpio_read_pin(inst->drdy_port, inst->drdy_pin, &level) != GPIO_OK)
        {
            return false;
        }

        const bool is_high = (level == GPIO_LEVEL_HIGH);
        if (is_high == want_high)
        {
            return true;
        }

        if (elapsed_us >= timeout_us)
        {
            return false;
        }

        cpu_delay_us(WIFI_POLL_INTERVAL_US);
        elapsed_us += WIFI_POLL_INTERVAL_US;
    }
}

/**
 * @brief Clock a byte buffer out as 16-bit SPI words, IWIN endian.
 *
 * The ISM43362 SPI shell is a 16-bit-wide window onto an internally
 * byte-oriented UART core: to keep two consecutive logical bytes in
 * their original order, the *second* byte of each pair must be clocked
 * out first (datasheet §10.2.3 endian example: "I?\r\x0A" -> wire bytes
 * 0x3F 0x49 0x0A 0x0D). Getting this backwards transposes every pair of
 * characters the module receives.
 */
static wifi_err_t prv_send_words(struct wifi_inst *inst, const uint8_t *data, size_t len)
{
    uint16_t word;
    size_t i = 0u;

    while (i < len)
    {
        const uint8_t first = data[i];
        const uint8_t second = ((i + 1u) < len) ? data[i + 1u] : (uint8_t) WIFI_CMD_PAD_BYTE;

        word = (uint16_t) (((uint16_t) second << 8) | first);

        if (spi_transceive(inst->spi, &word, NULL, 1u) != SPI_ERR_OK)
        {
            return WIFI_ERR_SPI;
        }

        i += 2u;
    }

    return WIFI_ERR_OK;
}

/**
 * @brief Drain a Data Phase: clock 0x0A on MOSI, decode IWIN-endian words.
 *
 * Used both for the post-reset boot cursor and for ordinary AT-command
 * responses — in both cases the module signals a Data Phase by holding
 * DRDY high and expects the host to keep clocking until DRDY falls
 * (datasheet §10.2.1/§10.2.4). The unswap is the mirror of prv_send_words.
 */
static size_t prv_recv_words(struct wifi_inst *inst, char *resp_buf, size_t resp_buf_len)
{
    static const uint16_t fill_word = ((uint16_t) WIFI_READ_FILL_BYTE << 8) | WIFI_READ_FILL_BYTE;
    size_t received = 0u;

    while (received < resp_buf_len)
    {
        gpio_level_t level;
        if (gpio_read_pin(inst->drdy_port, inst->drdy_pin, &level) != GPIO_OK)
        {
            break;
        }
        if (level == GPIO_LEVEL_LOW)
        {
            break;
        }

        uint16_t word = 0u;
        if (spi_transceive(inst->spi, &fill_word, &word, 1u) != SPI_ERR_OK)
        {
            break;
        }

        resp_buf[received] = (char) (word & 0xFFu);
        received++;
        if (received < resp_buf_len)
        {
            resp_buf[received] = (char) (word >> 8);
            received++;
        }
    }

    return received;
}

/**
 * @brief Drain the post-reset boot prompt before issuing any AT command.
 *
 * After reset the module immediately raises DRDY for a Data Phase holding
 * the boot cursor "\r\n> " — NOT a Command Phase (datasheet §10.2.1: "the
 * SPI Host must fetch the cursor... The next rising edge of the CMD/DATA
 * READY pin signals the Command Phase"). Sending a command on this first
 * DRDY-high wedges the module's phase state machine before the driver
 * ever gets a real reply, which is what produced the multi-second
 * wifi_create() timeout on hardware.
 */
static wifi_err_t prv_drain_boot_cursor(struct wifi_inst *inst)
{
    char scratch[8];

    g_diag.last_step = WIFI_DIAG_STEP_BOOT_CURSOR_WAIT;
    if (!prv_wait_drdy(inst, true, WIFI_BOOT_DRDY_TIMEOUT_MS))
    {
        return WIFI_ERR_TIMEOUT;
    }

    g_diag.last_step = WIFI_DIAG_STEP_BOOT_CURSOR_DRAIN;
    gpio_write_pin(inst->nss_port, inst->nss_pin, GPIO_LEVEL_LOW);
    g_diag.boot_cursor_bytes = prv_recv_words(inst, scratch, sizeof scratch);
    gpio_write_pin(inst->nss_port, inst->nss_pin, GPIO_LEVEL_HIGH);

    return WIFI_ERR_OK;
}

/**
 * @brief Send a raw, already-formatted command buffer and read the response.
 *
 * Full send/receive SPI cycle with DRDY handshake (companion §3.2). NSS is
 * deasserted on every path once the send phase completes (WIFI-D6).
 *
 * DRDY waits are bounded busy-polls in every phase (pre- and
 * post-scheduler): WifiDriver has no FreeRTOS dependency of its own
 * (companion §3.5, WIFI-D11) — task-level notification of DATARDY events
 * is WifiTask's responsibility, layered above this driver.
 */
static wifi_err_t prv_at_command(struct wifi_inst *inst, const uint8_t *cmd, size_t cmd_len,
                                 char *resp_buf, size_t resp_buf_len, size_t *out_resp_len)
{
    if (out_resp_len != NULL)
    {
        *out_resp_len = 0u;
    }

    gpio_write_pin(inst->nss_port, inst->nss_pin, GPIO_LEVEL_LOW);

    g_diag.last_step = WIFI_DIAG_STEP_INFO_WAIT_HIGH;
    if (!prv_wait_drdy(inst, true, WIFI_DRDY_TIMEOUT_MS))
    {
        gpio_write_pin(inst->nss_port, inst->nss_pin, GPIO_LEVEL_HIGH);
        return WIFI_ERR_TIMEOUT;
    }

    g_diag.last_step = WIFI_DIAG_STEP_INFO_SEND;
    const wifi_err_t send_err = prv_send_words(inst, cmd, cmd_len);

    gpio_write_pin(inst->nss_port, inst->nss_pin, GPIO_LEVEL_HIGH);

    if (send_err != WIFI_ERR_OK)
    {
        return send_err;
    }

    g_diag.last_step = WIFI_DIAG_STEP_INFO_WAIT_LOW;
    if (!prv_wait_drdy(inst, false, WIFI_DRDY_TIMEOUT_MS))
    {
        return WIFI_ERR_TIMEOUT;
    }

    g_diag.last_step = WIFI_DIAG_STEP_INFO_WAIT_RESP;
    if (!prv_wait_drdy(inst, true, WIFI_RESP_TIMEOUT_MS))
    {
        return WIFI_ERR_TIMEOUT;
    }

    gpio_write_pin(inst->nss_port, inst->nss_pin, GPIO_LEVEL_LOW);
    const size_t received = prv_recv_words(inst, resp_buf, resp_buf_len);
    gpio_write_pin(inst->nss_port, inst->nss_pin, GPIO_LEVEL_HIGH);

    if (out_resp_len != NULL)
    {
        *out_resp_len = received;
    }

    g_diag.last_step = WIFI_DIAG_STEP_INFO_PARSE;
    g_diag.info_resp_len = received;
    const size_t diag_copy_len =
        (received < sizeof(g_diag.info_resp_raw)) ? received : sizeof(g_diag.info_resp_raw) - 1u;
    memcpy(g_diag.info_resp_raw, resp_buf, diag_copy_len);
    g_diag.info_resp_raw[diag_copy_len] = '\0';

    return prv_parse_response(resp_buf, received);
}

/**
 * @brief Format and send a single IWIN command: `<code>[=<value>]<CR>`.
 *
 * @param[in]  inst          WifiDriver instance.
 * @param[in]  code          Command code, e.g. WIFI_AT_SET_SSID, WIFI_AT_GET_RSSI.
 * @param[in]  value         Value string for "<code>=<value>", or NULL for
 *                            a bare "<code>" command (e.g. WIFI_AT_JOIN).
 * @param[out] out_resp_len  Receives the response byte count, or NULL if
 *                            the caller only needs the OK/ERROR verdict.
 */
static wifi_err_t prv_send_kv(struct wifi_inst *inst, const char *code, const char *value,
                              size_t *out_resp_len)
{
    int written;

    if (value != NULL)
    {
        written = snprintf(inst->at_buf, WIFI_AT_BUF_SIZE, "%s=%s\r", code, value);
    }
    else
    {
        written = snprintf(inst->at_buf, WIFI_AT_BUF_SIZE, "%s\r", code);
    }

    if ((written < 0) || ((size_t) written >= WIFI_AT_BUF_SIZE))
    {
        return WIFI_ERR_INVALID_ARG;
    }

    return prv_at_command(inst, (const uint8_t *) inst->at_buf, (size_t) written, inst->at_buf,
                          WIFI_AT_BUF_SIZE, out_resp_len);
}

/* ====================================================================== */
/* Public API                                                               */
/* ====================================================================== */

wifi_err_t wifi_create(const wifi_config_t *config, wifi_handle_t *handle)
{
    if ((config == NULL) || (handle == NULL))
    {
        return WIFI_ERR_NULL_PTR;
    }

    if (g_count >= WIFI_MAX_INSTANCES)
    {
        return WIFI_ERR_NO_RESOURCE;
    }

    memset(&g_diag, 0, sizeof(g_diag));

    struct wifi_inst *inst = &g_pool[g_count];

    inst->spi = config->spi;
    inst->nss_port = config->nss_port;
    inst->nss_pin = config->nss_pin;
    inst->drdy_port = config->drdy_port;
    inst->drdy_pin = config->drdy_pin;
    inst->rst_port = config->rst_port;
    inst->rst_pin = config->rst_pin;
    inst->wakeup_port = config->wakeup_port;
    inst->wakeup_pin = config->wakeup_pin;
    inst->boot0_port = config->boot0_port;
    inst->boot0_pin = config->boot0_pin;
    inst->link_state = WIFI_LINK_DOWN;
    inst->rssi_dbm = 0;
    memset(inst->socket_open, 0, sizeof(inst->socket_open));
    inst->datardy_cb = NULL;
    inst->datardy_ctx = NULL;
    inst->ready = false;

    /* Hardware reset sequence (companion §3.6 Phase 1). */
    gpio_write_pin(inst->boot0_port, inst->boot0_pin, GPIO_LEVEL_LOW);
    gpio_write_pin(inst->rst_port, inst->rst_pin, GPIO_LEVEL_LOW);
    cpu_delay_ms(WIFI_RESET_PULSE_MS);
    gpio_write_pin(inst->rst_port, inst->rst_pin, GPIO_LEVEL_HIGH);
    cpu_delay_ms(WIFI_BOOT_WAIT_MS);
    gpio_write_pin(inst->wakeup_port, inst->wakeup_pin, GPIO_LEVEL_HIGH);
    gpio_write_pin(inst->nss_port, inst->nss_pin, GPIO_LEVEL_HIGH);

    (void) exti_configure(WIFI_DRDY_EXTI_LINE, EXTI_PORT_E, EXTI_EDGE_RISING);

    /* Drain the post-reset boot prompt before issuing any AT command
     * (datasheet §10.2.1). */
    wifi_err_t err = prv_drain_boot_cursor(inst);
    if (err != WIFI_ERR_OK)
    {
        return err;
    }

    /* The module needs more real settle time to reach the first genuine
     * Command Phase than steady-state per-command turnaround
     * (WIFI_DRDY_TIMEOUT_MS) allows for — confirmed on hardware (WIFI-O7):
     * the boot cursor drains fine, but DRDY doesn't rise again within
     * 100 ms afterward. Wait for it here, generously, before the first
     * command; prv_at_command()'s own (tight) pre-send wait then passes
     * near-instantly since DRDY is already high by the time it checks. */
    g_diag.last_step = WIFI_DIAG_STEP_COMMAND_PHASE_WAIT;
    if (!prv_wait_drdy(inst, true, WIFI_BOOT_DRDY_TIMEOUT_MS))
    {
        return WIFI_ERR_TIMEOUT;
    }

    /* WIFI_AT_INFO ("I?") — module info and liveness check in one step.
     * The quick-reference AT command doc marks "?" (print help) as "Not
     * available in SPI firmware", so it is never sent here. */
    size_t resp_len = 0u;
    err = prv_send_kv(inst, WIFI_AT_INFO, NULL, &resp_len);
    if (err != WIFI_ERR_OK)
    {
        return err;
    }

    err = prv_check_firmware_version(inst->at_buf, resp_len);
    if (err != WIFI_ERR_OK)
    {
        return err;
    }

    inst->ready = true;
    inst->in_use = true;
    g_count++;

    *handle = inst;
    return WIFI_ERR_OK;
}

const wifi_bringup_diag_t *wifi_get_bringup_diag(void)
{
    return &g_diag;
}

wifi_err_t wifi_attach_datardy_callback(wifi_handle_t handle, wifi_datardy_cb_t cb, void *ctx)
{
    if ((handle == NULL) || (cb == NULL))
    {
        return WIFI_ERR_NULL_PTR;
    }
    if (!handle->ready)
    {
        return WIFI_ERR_NOT_INIT;
    }

    handle->datardy_cb = cb;
    handle->datardy_ctx = ctx;

    (void) exti_enable(WIFI_DRDY_EXTI_LINE, WIFI_EXTI_NVIC_PRIORITY);

    return WIFI_ERR_OK;
}

wifi_err_t wifi_connect_ap(wifi_handle_t handle, const char *ssid, const char *password)
{
    if ((handle == NULL) || (ssid == NULL) || (password == NULL))
    {
        return WIFI_ERR_NULL_PTR;
    }
    if (!handle->ready)
    {
        return WIFI_ERR_NOT_INIT;
    }
    if ((strlen(ssid) > WIFI_MAX_SSID_LEN) || (strlen(password) > WIFI_MAX_PASS_LEN))
    {
        return WIFI_ERR_INVALID_ARG;
    }

    /* C1/C2/C3/C4/C0 — SSID, password, security (WPA2 Mixed), DHCP on, join. */
    wifi_err_t err = prv_send_kv(handle, WIFI_AT_SET_SSID, ssid, NULL);
    if (err != WIFI_ERR_OK)
    {
        return err;
    }
    err = prv_send_kv(handle, WIFI_AT_SET_PASSPHRASE, password, NULL);
    if (err != WIFI_ERR_OK)
    {
        return err;
    }
    err = prv_send_kv(handle, WIFI_AT_SET_SECURITY, WIFI_SECURITY_WPA2_MIXED, NULL);
    if (err != WIFI_ERR_OK)
    {
        return err;
    }
    err = prv_send_kv(handle, WIFI_AT_SET_DHCP, WIFI_DHCP_ENABLE, NULL);
    if (err != WIFI_ERR_OK)
    {
        return err;
    }
    err = prv_send_kv(handle, WIFI_AT_JOIN, NULL, NULL);
    if (err == WIFI_ERR_OK)
    {
        handle->link_state = WIFI_LINK_UP;
    }
    return err;
}

wifi_err_t wifi_disconnect_ap(wifi_handle_t handle)
{
    if (handle == NULL)
    {
        return WIFI_ERR_NULL_PTR;
    }
    if (!handle->ready)
    {
        return WIFI_ERR_NOT_INIT;
    }

    const wifi_err_t err = prv_send_kv(handle, WIFI_AT_DISCONNECT, NULL, NULL);
    if (err == WIFI_ERR_OK)
    {
        handle->link_state = WIFI_LINK_DOWN;
    }
    return err;
}

wifi_err_t wifi_get_link_state(wifi_handle_t handle, wifi_link_state_t *state)
{
    if ((handle == NULL) || (state == NULL))
    {
        return WIFI_ERR_NULL_PTR;
    }
    if (!handle->ready)
    {
        return WIFI_ERR_NOT_INIT;
    }

    *state = handle->link_state;
    return WIFI_ERR_OK;
}

wifi_err_t wifi_get_rssi(wifi_handle_t handle, int8_t *rssi_dbm)
{
    if ((handle == NULL) || (rssi_dbm == NULL))
    {
        return WIFI_ERR_NULL_PTR;
    }
    if (!handle->ready)
    {
        return WIFI_ERR_NOT_INIT;
    }
    if (handle->link_state != WIFI_LINK_UP)
    {
        return WIFI_ERR_NOT_CONNECTED;
    }

    size_t resp_len = 0u;
    const wifi_err_t err = prv_send_kv(handle, WIFI_AT_GET_RSSI, NULL, &resp_len);
    if (err != WIFI_ERR_OK)
    {
        return err;
    }

    const wifi_err_t parse_err = prv_parse_rssi(handle->at_buf, resp_len, &handle->rssi_dbm);
    if (parse_err != WIFI_ERR_OK)
    {
        return parse_err;
    }

    *rssi_dbm = handle->rssi_dbm;
    return WIFI_ERR_OK;
}

wifi_err_t wifi_open_socket(wifi_handle_t handle, wifi_socket_type_t type, const char *remote_addr,
                            uint16_t remote_port, wifi_socket_t *out_socket)
{
    if ((handle == NULL) || (remote_addr == NULL) || (out_socket == NULL))
    {
        return WIFI_ERR_NULL_PTR;
    }
    if (!handle->ready)
    {
        return WIFI_ERR_NOT_INIT;
    }

    wifi_socket_t slot = WIFI_INVALID_SOCKET;
    for (wifi_socket_t i = 0u; i < WIFI_MAX_SOCKETS; i++)
    {
        if (!handle->socket_open[i])
        {
            slot = i;
            break;
        }
    }
    if (slot == WIFI_INVALID_SOCKET)
    {
        return WIFI_ERR_NO_RESOURCE;
    }

    char slot_str[WIFI_NUMSTR_MAX];
    (void) snprintf(slot_str, sizeof(slot_str), "%u", (unsigned) slot);

    char port_str[WIFI_NUMSTR_MAX];
    (void) snprintf(port_str, sizeof(port_str), "%u", (unsigned) remote_port);

    const char *protocol_str = (type == WIFI_SOCKET_TCP) ? "0" : "1";

    /* P0/P1/P3/P4/P6=1 — select socket, protocol, remote host, remote port,
     * start client. There is no combined "open connection" command. */
    wifi_err_t err = prv_send_kv(handle, WIFI_AT_SET_SOCKET, slot_str, NULL);
    if (err != WIFI_ERR_OK)
    {
        return WIFI_ERR_SOCKET;
    }
    err = prv_send_kv(handle, WIFI_AT_SET_PROTOCOL, protocol_str, NULL);
    if (err != WIFI_ERR_OK)
    {
        return WIFI_ERR_SOCKET;
    }
    err = prv_send_kv(handle, WIFI_AT_SET_REMOTE_HOST, remote_addr, NULL);
    if (err != WIFI_ERR_OK)
    {
        return WIFI_ERR_SOCKET;
    }
    err = prv_send_kv(handle, WIFI_AT_SET_REMOTE_PORT, port_str, NULL);
    if (err != WIFI_ERR_OK)
    {
        return WIFI_ERR_SOCKET;
    }
    err = prv_send_kv(handle, WIFI_AT_SET_CLIENT, "1", NULL);
    if (err != WIFI_ERR_OK)
    {
        return WIFI_ERR_SOCKET;
    }

    handle->socket_open[slot] = true;
    *out_socket = slot;
    return WIFI_ERR_OK;
}

wifi_err_t wifi_send(wifi_handle_t handle, wifi_socket_t socket, const uint8_t *data, size_t len)
{
    if ((handle == NULL) || (data == NULL))
    {
        return WIFI_ERR_NULL_PTR;
    }
    if (!handle->ready)
    {
        return WIFI_ERR_NOT_INIT;
    }
    if (handle->link_state != WIFI_LINK_UP)
    {
        return WIFI_ERR_NOT_CONNECTED;
    }
    if ((socket >= WIFI_MAX_SOCKETS) || (!handle->socket_open[socket]) ||
        (len > WIFI_MAX_PACKET_SIZE))
    {
        return WIFI_ERR_INVALID_ARG;
    }

    char socket_str[WIFI_NUMSTR_MAX];
    (void) snprintf(socket_str, sizeof(socket_str), "%u", (unsigned) socket);

    /* P0 — select socket. */
    wifi_err_t err = prv_send_kv(handle, WIFI_AT_SET_SOCKET, socket_str, NULL);
    if (err != WIFI_ERR_OK)
    {
        return WIFI_ERR_SOCKET;
    }

    /* S3=<len> — set packet size and send in one command; payload follows
     * the <CR> directly (no combined string formatting: payload may be
     * binary and is not null-terminated). */
    const int header_len =
        snprintf(handle->at_buf, WIFI_AT_BUF_SIZE, WIFI_AT_SEND_DATA "=%u\r", (unsigned) len);
    if ((header_len < 0) || (((size_t) header_len + len) > WIFI_AT_BUF_SIZE))
    {
        return WIFI_ERR_INVALID_ARG;
    }
    memcpy(&handle->at_buf[header_len], data, len);
    const size_t total_len = (size_t) header_len + len;

    err = prv_at_command(handle, (const uint8_t *) handle->at_buf, total_len, handle->at_buf,
                         WIFI_AT_BUF_SIZE, NULL);
    if (err != WIFI_ERR_OK)
    {
        return WIFI_ERR_SOCKET;
    }

    return WIFI_ERR_OK;
}

wifi_err_t wifi_recv(wifi_handle_t handle, wifi_socket_t socket, uint8_t *buf, size_t buf_len,
                     size_t *out_len, uint32_t timeout_ms)
{
    (void) timeout_ms;

    if ((handle == NULL) || (buf == NULL) || (out_len == NULL))
    {
        return WIFI_ERR_NULL_PTR;
    }
    if (!handle->ready)
    {
        return WIFI_ERR_NOT_INIT;
    }
    if ((socket >= WIFI_MAX_SOCKETS) || (!handle->socket_open[socket]))
    {
        return WIFI_ERR_INVALID_ARG;
    }

    char socket_str[WIFI_NUMSTR_MAX];
    (void) snprintf(socket_str, sizeof(socket_str), "%u", (unsigned) socket);

    /* P0 — select socket. */
    wifi_err_t err = prv_send_kv(handle, WIFI_AT_SET_SOCKET, socket_str, NULL);
    if (err != WIFI_ERR_OK)
    {
        return (err == WIFI_ERR_TIMEOUT) ? WIFI_ERR_TIMEOUT : WIFI_ERR_SOCKET;
    }

    /* R1 — expected packet size, capped at the IWIN maximum. */
    const size_t req_len = (buf_len < WIFI_MAX_PACKET_SIZE) ? buf_len : WIFI_MAX_PACKET_SIZE;
    char len_str[WIFI_NUMSTR_MAX];
    (void) snprintf(len_str, sizeof(len_str), "%u", (unsigned) req_len);
    err = prv_send_kv(handle, WIFI_AT_SET_RECV_PACKET_SIZE, len_str, NULL);
    if (err != WIFI_ERR_OK)
    {
        return (err == WIFI_ERR_TIMEOUT) ? WIFI_ERR_TIMEOUT : WIFI_ERR_SOCKET;
    }

    /* R0 — receive one packet. */
    size_t resp_len = 0u;
    err = prv_send_kv(handle, WIFI_AT_RECV_DATA, NULL, &resp_len);
    if (err != WIFI_ERR_OK)
    {
        return (err == WIFI_ERR_TIMEOUT) ? WIFI_ERR_TIMEOUT : WIFI_ERR_SOCKET;
    }

    /* Every IWIN response starts with "\r\n" before the actual data (User
     * Manual §1.4.2) — strip it, then strip the trailing "\r\nOK\r\n" the
     * module appends after the payload. Both confirmed on hardware. */
    size_t payload_start = 0u;
    if ((resp_len >= 2u) && (handle->at_buf[0] == '\r') && (handle->at_buf[1] == '\n'))
    {
        payload_start = 2u;
    }

    size_t payload_len = resp_len - payload_start;
    const size_t ok_len = strlen(WIFI_RESP_OK_MARKER);

    /* The module also post-pads the whole response to an even byte count
     * with a trailing 0x15 (datasheet §10.2) — tolerate at most one such
     * byte after "\r\nOK\r\n" when locating where the real payload ends. */
    size_t search_len = payload_len;
    if ((search_len > 0u) && (handle->at_buf[payload_start + search_len - 1u] == (char) 0x15))
    {
        search_len -= 1u;
    }
    if ((search_len >= ok_len) && (memcmp(&handle->at_buf[payload_start + search_len - ok_len],
                                          WIFI_RESP_OK_MARKER, ok_len) == 0))
    {
        payload_len = search_len - ok_len;
    }

    const size_t copy_len = (payload_len < buf_len) ? payload_len : buf_len;
    memcpy(buf, &handle->at_buf[payload_start], copy_len);
    *out_len = copy_len;

    return WIFI_ERR_OK;
}

wifi_err_t wifi_close_socket(wifi_handle_t handle, wifi_socket_t socket)
{
    if (handle == NULL)
    {
        return WIFI_ERR_NULL_PTR;
    }
    if (!handle->ready)
    {
        return WIFI_ERR_NOT_INIT;
    }
    if ((socket >= WIFI_MAX_SOCKETS) || (!handle->socket_open[socket]))
    {
        return WIFI_ERR_INVALID_ARG;
    }

    char socket_str[WIFI_NUMSTR_MAX];
    (void) snprintf(socket_str, sizeof(socket_str), "%u", (unsigned) socket);

    /* P0 — select socket, then P6=0 — stop client. There is no dedicated
     * "close" command in the IWIN set. */
    wifi_err_t err = prv_send_kv(handle, WIFI_AT_SET_SOCKET, socket_str, NULL);
    if (err != WIFI_ERR_OK)
    {
        return WIFI_ERR_SOCKET;
    }
    err = prv_send_kv(handle, WIFI_AT_SET_CLIENT, "0", NULL);
    if (err != WIFI_ERR_OK)
    {
        return WIFI_ERR_SOCKET;
    }

    handle->socket_open[socket] = false;
    return WIFI_ERR_OK;
}

void wifi_datardy_irq_handler(void)
{
    /* g_pool[0] is safe here: single instance, single ISR. */
    struct wifi_inst *inst = &g_pool[0];
    if (inst->datardy_cb != NULL)
    {
        inst->datardy_cb(inst->datardy_ctx);
    }
}

#ifdef TEST
void wifi_reset_for_test(void)
{
    memset(g_pool, 0, sizeof(g_pool));
    g_count = 0u;
}
#endif
