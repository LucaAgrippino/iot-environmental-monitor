/**
 * @file wifi_driver.c
 * @brief WiFi driver implementation — AT-command engine, SPI protocol,
 *        socket table, DATARDY ISR (Gateway).
 *
 * @note See docs/lld/drivers/wifi-driver.md for the full design specification.
 */

#include "wifi_driver.h"

#include <stdio.h>
#include <string.h>

#include "cpu.h"

#define WIFI_MAX_INSTANCES 1u
#define WIFI_AT_BUF_SIZE 512u
#define WIFI_DRDY_TIMEOUT_MS 100u  /**< DRDY assert wait (WIFI-O5). */
#define WIFI_RESP_TIMEOUT_MS 5000u /**< AT response wait (WIFI-O5). */
#define WIFI_RESET_PULSE_MS 10u
#define WIFI_BOOT_WAIT_MS 500u
#define WIFI_POLL_INTERVAL_US 100u
#define WIFI_PAD_BYTE 0x0Au /**< LF — appended when a command has odd length. */
#define WIFI_FIRMWARE_VERSION "C3.5.2.3"
#define WIFI_RESP_OK_MARKER "\r\nOK\r\n"
#define WIFI_RESP_ERROR_MARKER "\r\nERROR\r\n"
#define WIFI_RSSI_MARKER "+WRSSI:"

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

WIFI_TEST_VISIBLE wifi_err_t prv_parse_rssi(const char *resp, size_t resp_len, int8_t *out_rssi)
{
    const size_t marker_len = strlen(WIFI_RSSI_MARKER);

    for (size_t i = 0u; (marker_len <= resp_len) && (i <= (resp_len - marker_len)); i++)
    {
        if (memcmp(&resp[i], WIFI_RSSI_MARKER, marker_len) != 0)
        {
            continue;
        }

        size_t pos = i + marker_len;
        bool negative = false;
        int32_t value = 0;
        bool any_digit = false;

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

    return WIFI_ERR_MODULE;
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

static wifi_err_t prv_send_words(struct wifi_inst *inst, const uint8_t *data, size_t len)
{
    uint16_t word;
    size_t i = 0u;

    while (i < len)
    {
        const uint8_t hi = data[i];
        const uint8_t lo = ((i + 1u) < len) ? data[i + 1u] : (uint8_t) WIFI_PAD_BYTE;

        word = (uint16_t) (((uint16_t) hi << 8) | lo);

        if (spi_transceive(inst->spi, &word, NULL, 1u) != SPI_ERR_OK)
        {
            return WIFI_ERR_SPI;
        }

        i += 2u;
    }

    return WIFI_ERR_OK;
}

static size_t prv_recv_words(struct wifi_inst *inst, char *resp_buf, size_t resp_buf_len)
{
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
        if (spi_transceive(inst->spi, NULL, &word, 1u) != SPI_ERR_OK)
        {
            break;
        }

        resp_buf[received] = (char) (word >> 8);
        received++;
        if (received < resp_buf_len)
        {
            resp_buf[received] = (char) (word & 0xFFu);
            received++;
        }
    }

    return received;
}

/**
 * @brief Send an AT command and read the response.
 *
 * Full send/receive SPI cycle with DRDY handshake (companion §3.2). NSS is
 * deasserted on every path once the send phase completes (WIFI-D6).
 *
 * DRDY waits are bounded busy-polls in every phase (pre- and
 * post-scheduler): WifiDriver has no FreeRTOS dependency of its own
 * (companion §3.5, H11) — task-level notification of DATARDY events is
 * WifiTask's responsibility, layered above this driver.
 */
static wifi_err_t prv_at_command(struct wifi_inst *inst, const uint8_t *cmd, size_t cmd_len,
                                 char *resp_buf, size_t resp_buf_len, size_t *out_resp_len)
{
    if (out_resp_len != NULL)
    {
        *out_resp_len = 0u;
    }

    gpio_write_pin(inst->nss_port, inst->nss_pin, GPIO_LEVEL_LOW);

    if (!prv_wait_drdy(inst, true, WIFI_DRDY_TIMEOUT_MS))
    {
        gpio_write_pin(inst->nss_port, inst->nss_pin, GPIO_LEVEL_HIGH);
        return WIFI_ERR_TIMEOUT;
    }

    const wifi_err_t send_err = prv_send_words(inst, cmd, cmd_len);

    gpio_write_pin(inst->nss_port, inst->nss_pin, GPIO_LEVEL_HIGH);

    if (send_err != WIFI_ERR_OK)
    {
        return send_err;
    }

    if (!prv_wait_drdy(inst, false, WIFI_DRDY_TIMEOUT_MS))
    {
        return WIFI_ERR_TIMEOUT;
    }

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

    return prv_parse_response(resp_buf, received);
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

    size_t resp_len = 0u;
    static const uint8_t at_handshake[] = "AT\r";
    wifi_err_t err = prv_at_command(inst, at_handshake, sizeof(at_handshake) - 1u, inst->at_buf,
                                    WIFI_AT_BUF_SIZE, &resp_len);
    if (err != WIFI_ERR_OK)
    {
        return err;
    }

    static const uint8_t at_version[] = "AT+GMR\r";
    err = prv_at_command(inst, at_version, sizeof(at_version) - 1u, inst->at_buf, WIFI_AT_BUF_SIZE,
                         &resp_len);
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

    const int written =
        snprintf(handle->at_buf, WIFI_AT_BUF_SIZE, "AT+WC=%s,%s,0\r", ssid, password);
    if ((written < 0) || ((size_t) written >= WIFI_AT_BUF_SIZE))
    {
        return WIFI_ERR_INVALID_ARG;
    }

    const wifi_err_t err = prv_at_command(handle, (const uint8_t *) handle->at_buf,
                                          (size_t) written, handle->at_buf, WIFI_AT_BUF_SIZE, NULL);
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

    static const uint8_t at_disconnect[] = "AT+WD\r";
    const wifi_err_t err = prv_at_command(handle, at_disconnect, sizeof(at_disconnect) - 1u,
                                          handle->at_buf, WIFI_AT_BUF_SIZE, NULL);
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
    static const uint8_t at_rssi[] = "AT+WRSSI\r";
    const wifi_err_t err = prv_at_command(handle, at_rssi, sizeof(at_rssi) - 1u, handle->at_buf,
                                          WIFI_AT_BUF_SIZE, &resp_len);
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

    const uint8_t transport = (type == WIFI_SOCKET_TCP) ? 0u : 1u;
    int written = snprintf(handle->at_buf, WIFI_AT_BUF_SIZE, "AT+P1=%u\r", (unsigned) transport);
    if ((written < 0) || ((size_t) written >= WIFI_AT_BUF_SIZE))
    {
        return WIFI_ERR_INVALID_ARG;
    }
    wifi_err_t err = prv_at_command(handle, (const uint8_t *) handle->at_buf, (size_t) written,
                                    handle->at_buf, WIFI_AT_BUF_SIZE, NULL);
    if (err != WIFI_ERR_OK)
    {
        return WIFI_ERR_SOCKET;
    }

    written = snprintf(handle->at_buf, WIFI_AT_BUF_SIZE, "AT+NCPX=%u,%s,%u,%u\r", (unsigned) slot,
                       remote_addr, (unsigned) remote_port, (unsigned) transport);
    if ((written < 0) || ((size_t) written >= WIFI_AT_BUF_SIZE))
    {
        return WIFI_ERR_INVALID_ARG;
    }
    err = prv_at_command(handle, (const uint8_t *) handle->at_buf, (size_t) written, handle->at_buf,
                         WIFI_AT_BUF_SIZE, NULL);
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
    if ((socket >= WIFI_MAX_SOCKETS) || (!handle->socket_open[socket]))
    {
        return WIFI_ERR_INVALID_ARG;
    }

    const int header_len = snprintf(handle->at_buf, WIFI_AT_BUF_SIZE, "AT+S.=%u,%u\r",
                                    (unsigned) socket, (unsigned) len);
    if ((header_len < 0) || (((size_t) header_len + len) > WIFI_AT_BUF_SIZE))
    {
        return WIFI_ERR_INVALID_ARG;
    }

    memcpy(&handle->at_buf[header_len], data, len);
    const size_t total_len = (size_t) header_len + len;

    const wifi_err_t err = prv_at_command(handle, (const uint8_t *) handle->at_buf, total_len,
                                          handle->at_buf, WIFI_AT_BUF_SIZE, NULL);
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

    const int written = snprintf(handle->at_buf, WIFI_AT_BUF_SIZE, "AT+R=%u,%u\r",
                                 (unsigned) socket, (unsigned) buf_len);
    if ((written < 0) || ((size_t) written >= WIFI_AT_BUF_SIZE))
    {
        return WIFI_ERR_INVALID_ARG;
    }

    size_t resp_len = 0u;
    const wifi_err_t err =
        prv_at_command(handle, (const uint8_t *) handle->at_buf, (size_t) written, handle->at_buf,
                       WIFI_AT_BUF_SIZE, &resp_len);
    if (err != WIFI_ERR_OK)
    {
        return (err == WIFI_ERR_TIMEOUT) ? WIFI_ERR_TIMEOUT : WIFI_ERR_SOCKET;
    }

    /* Strip the trailing "\r\nOK\r\n" the module appends after the payload. */
    size_t payload_len = resp_len;
    const size_t ok_len = strlen(WIFI_RESP_OK_MARKER);
    if ((payload_len >= ok_len) &&
        (memcmp(&handle->at_buf[payload_len - ok_len], WIFI_RESP_OK_MARKER, ok_len) == 0))
    {
        payload_len -= ok_len;
    }

    const size_t copy_len = (payload_len < buf_len) ? payload_len : buf_len;
    memcpy(buf, handle->at_buf, copy_len);
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

    const int written =
        snprintf(handle->at_buf, WIFI_AT_BUF_SIZE, "AT+NCLS=%u\r", (unsigned) socket);
    if ((written < 0) || ((size_t) written >= WIFI_AT_BUF_SIZE))
    {
        return WIFI_ERR_INVALID_ARG;
    }

    const wifi_err_t err = prv_at_command(handle, (const uint8_t *) handle->at_buf,
                                          (size_t) written, handle->at_buf, WIFI_AT_BUF_SIZE, NULL);
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
