/**
 * @file test_wifi_driver.c
 * @brief Unity unit tests for WifiDriver — WIFI-T01 through WIFI-T18.
 *
 * Layer 1 (WIFI-T01..T06) calls the response-parsing helpers directly with
 * hand-built buffers; no mocks involved.
 *
 * Layer 2 (WIFI-T07..T18) mocks SpiDriver, GpioDriver, ExtiDriver, and
 * CpuDriver via CMock. gpio_read_pin and spi_transceive are driven by
 * hand-written stub callbacks (not plain CMock expectations) because the
 * DRDY handshake needs a precise, ordered multi-call sequence per AT
 * command: helper_script_at_command() queues exactly the DRDY levels and
 * response words prv_at_command() will consume, in the order it consumes
 * them (companion §3.2). helper_script_boot_cursor() does the same for
 * the post-reset boot-cursor Data Phase that wifi_create() now drains
 * before it ever sends an AT command (datasheet §10.2.1). Response words
 * are built in IWIN's swapped 16-bit byte order (datasheet §10.2.3), the
 * same order the fixed prv_recv_words() expects to decode.
 *
 * Build: STM32L475xx and TEST must be defined
 *        (tests/project_gateway.yml :test_wifi_driver:). This test runs
 *        under project_gateway.yml, not the shared tests/project.yml,
 *        because it is the first module to consume GpioDriver — whose
 *        Gateway header collides by filename with the Field Device one
 *        (see tests/project_gateway.yml's header comment).
 */

#include "unity.h"

#include <string.h>

#include "mock_cpu.h"
#include "mock_exti_driver.h"
#include "mock_gpio_driver.h"
#include "mock_spi.h"

#include "wifi_driver.h"

/* ====================================================================== */
/* DRDY sequence + SPI word queues driving the stub callbacks              */
/* ====================================================================== */

/* Generous headroom: WIFI-T17 alone opens 4 sockets x 5 IWIN commands each. */
#define MAX_SCRIPTED_LEVELS 512u
#define MAX_SCRIPTED_WORDS 256u
#define MAX_CAPTURED_BYTES 512u
#define MAX_CAPTURED_WRITES 256u

static gpio_level_t s_drdy_seq[MAX_SCRIPTED_LEVELS];
static size_t s_drdy_seq_len;
static size_t s_drdy_seq_idx;

static uint16_t s_rx_words[MAX_SCRIPTED_WORDS];
static size_t s_rx_words_len;
static size_t s_rx_words_idx;

static uint8_t s_tx_captured[MAX_CAPTURED_BYTES];
static size_t s_tx_captured_len;

static spi_err_t s_spi_forced_err;

static gpio_port_t s_write_port[MAX_CAPTURED_WRITES];
static uint8_t s_write_pin[MAX_CAPTURED_WRITES];
static gpio_level_t s_write_level[MAX_CAPTURED_WRITES];
static size_t s_write_count;

static struct spi_inst_dummy_tag
{
    int unused;
} s_spi_backing;
static const spi_handle_t DUMMY_SPI = (spi_handle_t) &s_spi_backing;

static const gpio_port_t NSS_PORT = GPIO_PORT_E;
static const uint8_t NSS_PIN = 0u;
static const gpio_port_t DRDY_PORT = GPIO_PORT_E;
static const uint8_t DRDY_PIN = 1u;
static const gpio_port_t RST_PORT = GPIO_PORT_E;
static const uint8_t RST_PIN = 8u;
static const gpio_port_t WAKEUP_PORT = GPIO_PORT_B;
static const uint8_t WAKEUP_PIN = 13u;
static const gpio_port_t BOOT0_PORT = GPIO_PORT_B;
static const uint8_t BOOT0_PIN = 12u;

static void drdy_seq_push(gpio_level_t level, size_t repeat)
{
    for (size_t i = 0u; i < repeat; i++)
    {
        TEST_ASSERT_TRUE(s_drdy_seq_len < MAX_SCRIPTED_LEVELS);
        s_drdy_seq[s_drdy_seq_len] = level;
        s_drdy_seq_len++;
    }
}

static void rx_words_push(uint16_t word)
{
    TEST_ASSERT_TRUE(s_rx_words_len < MAX_SCRIPTED_WORDS);
    s_rx_words[s_rx_words_len] = word;
    s_rx_words_len++;
}

/**
 * @brief Queue the DRDY levels + words one prv_recv_words() call consumes.
 *
 * One HIGH per 16-bit word (the loop's own per-iteration DRDY check),
 * then a final LOW to end the Data Phase. Words are built in IWIN's
 * swapped byte order (second logical byte in the high half) to match
 * the driver's corrected decode in prv_recv_words() — the mirror image
 * of the datasheet's own endian example (§10.2.3).
 */
static void helper_script_data_phase(const char *text)
{
    const size_t text_len = strlen(text);
    const size_t word_count = (text_len + 1u) / 2u;

    for (size_t i = 0u; i < word_count; i++)
    {
        drdy_seq_push(GPIO_LEVEL_HIGH, 1u);
        const uint16_t first = (uint8_t) text[2u * i];
        const uint16_t second = ((2u * i + 1u) < text_len) ? (uint8_t) text[2u * i + 1u] : 0x15u;
        rx_words_push((uint16_t) ((second << 8) | first));
    }
    drdy_seq_push(GPIO_LEVEL_LOW, 1u); /* end of Data Phase */
}

/** Queue the exact DRDY levels and response words one prv_at_command()
 *  call will consume for a given response string. */
static void helper_script_at_command(const char *resp)
{
    drdy_seq_push(GPIO_LEVEL_HIGH, 1u); /* pre-send: DRDY high (Command Phase) */
    drdy_seq_push(GPIO_LEVEL_LOW, 1u);  /* post-send ack: DRDY low */
    drdy_seq_push(GPIO_LEVEL_HIGH, 1u); /* response ready: DRDY high (Data Phase) */
    helper_script_data_phase(resp);
}

/** Queue the DRDY levels + words one prv_drain_boot_cursor() call
 *  consumes: the leading DRDY-high satisfies its own prv_wait_drdy(true)
 *  before the boot-cursor Data Phase itself (datasheet §10.2.1). */
static void helper_script_boot_cursor(void)
{
    drdy_seq_push(GPIO_LEVEL_HIGH, 1u); /* post-reset: DRDY high (Data Phase begins) */
    helper_script_data_phase("\r\n> ");
}

static gpio_level_t last_write_level(gpio_port_t port, uint8_t pin)
{
    for (size_t i = s_write_count; i > 0u; i--)
    {
        if ((s_write_port[i - 1u] == port) && (s_write_pin[i - 1u] == pin))
        {
            return s_write_level[i - 1u];
        }
    }
    TEST_FAIL_MESSAGE("no write recorded for requested port/pin");
    return GPIO_LEVEL_LOW;
}

/* ====================================================================== */
/* Stub callbacks (CMock :callback plugin)                                 */
/* ====================================================================== */

static gpio_err_t stub_gpio_write_pin(gpio_port_t port, uint8_t pin, gpio_level_t level,
                                      int cmock_num_calls)
{
    (void) cmock_num_calls;
    TEST_ASSERT_TRUE(s_write_count < MAX_CAPTURED_WRITES);
    s_write_port[s_write_count] = port;
    s_write_pin[s_write_count] = pin;
    s_write_level[s_write_count] = level;
    s_write_count++;
    return GPIO_OK;
}

static gpio_err_t stub_gpio_read_pin(gpio_port_t port, uint8_t pin, gpio_level_t *out_level,
                                     int cmock_num_calls)
{
    (void) port;
    (void) pin;
    (void) cmock_num_calls;

    if (s_drdy_seq_idx < s_drdy_seq_len)
    {
        *out_level = s_drdy_seq[s_drdy_seq_idx];
        s_drdy_seq_idx++;
    }
    else
    {
        *out_level = GPIO_LEVEL_LOW;
    }
    return GPIO_OK;
}

static spi_err_t stub_spi_transceive(spi_handle_t handle, const uint16_t *tx_buf, uint16_t *rx_buf,
                                     uint16_t len, int cmock_num_calls)
{
    (void) handle;
    (void) cmock_num_calls;

    if (s_spi_forced_err != SPI_ERR_OK)
    {
        return s_spi_forced_err;
    }

    for (uint16_t i = 0u; i < len; i++)
    {
        if (tx_buf != NULL)
        {
            TEST_ASSERT_TRUE((s_tx_captured_len + 2u) <= MAX_CAPTURED_BYTES);
            s_tx_captured[s_tx_captured_len] = (uint8_t) (tx_buf[i] >> 8);
            s_tx_captured_len++;
            s_tx_captured[s_tx_captured_len] = (uint8_t) (tx_buf[i] & 0xFFu);
            s_tx_captured_len++;
        }
        if (rx_buf != NULL)
        {
            rx_buf[i] = (s_rx_words_idx < s_rx_words_len) ? s_rx_words[s_rx_words_idx] : 0u;
            if (s_rx_words_idx < s_rx_words_len)
            {
                s_rx_words_idx++;
            }
        }
    }
    return SPI_ERR_OK;
}

/* ====================================================================== */
/* setUp / tearDown                                                       */
/* ====================================================================== */

static wifi_config_t helper_make_config(void)
{
    wifi_config_t config = {
        .spi = DUMMY_SPI,
        .nss_port = NSS_PORT,
        .nss_pin = NSS_PIN,
        .drdy_port = DRDY_PORT,
        .drdy_pin = DRDY_PIN,
        .rst_port = RST_PORT,
        .rst_pin = RST_PIN,
        .wakeup_port = WAKEUP_PORT,
        .wakeup_pin = WAKEUP_PIN,
        .boot0_port = BOOT0_PORT,
        .boot0_pin = BOOT0_PIN,
    };
    return config;
}

static wifi_handle_t helper_create_ready(void)
{
    helper_script_boot_cursor();
    drdy_seq_push(GPIO_LEVEL_HIGH, 1u);                   /* wifi_create()'s own wait for the first
                                                           * Command Phase, before sending I? (WIFI-O7) */
    helper_script_at_command("C3.5.2.3.BETA9\r\nOK\r\n"); /* I? */

    wifi_config_t config = helper_make_config();
    wifi_handle_t handle = NULL;
    TEST_ASSERT_EQUAL(WIFI_ERR_OK, wifi_create(&config, &handle));
    TEST_ASSERT_NOT_NULL(handle);
    return handle;
}

void setUp(void)
{
    wifi_reset_for_test();

    s_drdy_seq_len = 0u;
    s_drdy_seq_idx = 0u;
    s_rx_words_len = 0u;
    s_rx_words_idx = 0u;
    s_tx_captured_len = 0u;
    s_write_count = 0u;
    s_spi_forced_err = SPI_ERR_OK;

    gpio_write_pin_StubWithCallback(stub_gpio_write_pin);
    gpio_read_pin_StubWithCallback(stub_gpio_read_pin);
    spi_transceive_StubWithCallback(stub_spi_transceive);
    exti_configure_IgnoreAndReturn(EXTI_ERR_OK);
    exti_enable_IgnoreAndReturn(EXTI_ERR_OK);
    cpu_delay_us_Ignore();
    cpu_delay_ms_Ignore();
}

void tearDown(void)
{
}

/* ====================================================================== */
/* Layer 1 — AT response parser                                           */
/* ====================================================================== */

void test_WIFI_T01_parse_response_ok(void)
{
    const char *resp = "\r\nOK\r\n";
    TEST_ASSERT_EQUAL(WIFI_ERR_OK, prv_parse_response(resp, strlen(resp)));
}

void test_WIFI_T02_parse_response_error(void)
{
    const char *resp = "\r\nERROR\r\n";
    TEST_ASSERT_EQUAL(WIFI_ERR_MODULE, prv_parse_response(resp, strlen(resp)));
}

void test_WIFI_T03_parse_response_truncated(void)
{
    const char *resp = "garbage, no marker here";
    TEST_ASSERT_EQUAL(WIFI_ERR_TIMEOUT, prv_parse_response(resp, strlen(resp)));
}

void test_WIFI_T04_parse_rssi(void)
{
    /* IWIN "CR" response is a bare value, no "+WRSSI:" marker. */
    const char *resp = "-67\r\nOK\r\n";
    int8_t rssi = 0;
    TEST_ASSERT_EQUAL(WIFI_ERR_OK, prv_parse_rssi(resp, strlen(resp), &rssi));
    TEST_ASSERT_EQUAL_INT8(-67, rssi);
}

void test_WIFI_T05_firmware_version_match(void)
{
    const char *resp = "C3.5.2.3.BETA9\r\nOK\r\n";
    TEST_ASSERT_EQUAL(WIFI_ERR_OK, prv_check_firmware_version(resp, strlen(resp)));
}

void test_WIFI_T06_firmware_version_mismatch(void)
{
    const char *resp = "C2.0.0.0\r\nOK\r\n";
    TEST_ASSERT_EQUAL(WIFI_ERR_FIRMWARE, prv_check_firmware_version(resp, strlen(resp)));
}

/* ====================================================================== */
/* Layer 2 — full API with mocked SPI/GPIO/EXTI/CPU                       */
/* ====================================================================== */

void test_WIFI_T07_create_happy_path(void)
{
    wifi_handle_t handle = helper_create_ready();

    wifi_link_state_t state;
    TEST_ASSERT_EQUAL(WIFI_ERR_OK, wifi_get_link_state(handle, &state));
    TEST_ASSERT_EQUAL(WIFI_LINK_DOWN, state);

    /* NSS must be idle (high) once creation settles. */
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, last_write_level(NSS_PORT, NSS_PIN));
}

void test_WIFI_T08_create_null_config(void)
{
    wifi_handle_t handle = NULL;
    wifi_config_t config = helper_make_config();

    TEST_ASSERT_EQUAL(WIFI_ERR_NULL_PTR, wifi_create(NULL, &handle));
    TEST_ASSERT_EQUAL(WIFI_ERR_NULL_PTR, wifi_create(&config, NULL));
}

void test_WIFI_T09_create_pool_exhaustion(void)
{
    (void) helper_create_ready();

    wifi_config_t config = helper_make_config();
    wifi_handle_t second = NULL;
    TEST_ASSERT_EQUAL(WIFI_ERR_NO_RESOURCE, wifi_create(&config, &second));
}

void test_WIFI_T10_connect_ap_nominal(void)
{
    wifi_handle_t handle = helper_create_ready();

    helper_script_at_command("\r\nOK\r\n"); /* C1=ssid */
    helper_script_at_command("\r\nOK\r\n"); /* C2=password */
    helper_script_at_command("\r\nOK\r\n"); /* C3=4 (security) */
    helper_script_at_command("\r\nOK\r\n"); /* C4=1 (DHCP) */
    helper_script_at_command("\r\nOK\r\n"); /* C0 (join) */
    TEST_ASSERT_EQUAL(WIFI_ERR_OK, wifi_connect_ap(handle, "myssid", "mypassword"));

    wifi_link_state_t state;
    TEST_ASSERT_EQUAL(WIFI_ERR_OK, wifi_get_link_state(handle, &state));
    TEST_ASSERT_EQUAL(WIFI_LINK_UP, state);
}

void test_WIFI_T11_connect_ap_wrong_ssid(void)
{
    wifi_handle_t handle = helper_create_ready();

    helper_script_at_command("\r\nOK\r\n");    /* C1=ssid */
    helper_script_at_command("\r\nOK\r\n");    /* C2=password */
    helper_script_at_command("\r\nOK\r\n");    /* C3=4 (security) */
    helper_script_at_command("\r\nOK\r\n");    /* C4=1 (DHCP) */
    helper_script_at_command("\r\nERROR\r\n"); /* C0 (join) fails */
    TEST_ASSERT_EQUAL(WIFI_ERR_MODULE, wifi_connect_ap(handle, "myssid", "wrongpass"));

    wifi_link_state_t state;
    TEST_ASSERT_EQUAL(WIFI_ERR_OK, wifi_get_link_state(handle, &state));
    TEST_ASSERT_EQUAL(WIFI_LINK_DOWN, state);
}

void test_WIFI_T12_open_socket_tcp_nominal(void)
{
    wifi_handle_t handle = helper_create_ready();

    helper_script_at_command("\r\nOK\r\n"); /* P0=0 (select) */
    helper_script_at_command("\r\nOK\r\n"); /* P1=0 (TCP) */
    helper_script_at_command("\r\nOK\r\n"); /* P3=<host> */
    helper_script_at_command("\r\nOK\r\n"); /* P4=<port> */
    helper_script_at_command("\r\nOK\r\n"); /* P6=1 (start client) */

    wifi_socket_t sock = WIFI_INVALID_SOCKET;
    TEST_ASSERT_EQUAL(WIFI_ERR_OK,
                      wifi_open_socket(handle, WIFI_SOCKET_TCP, "10.0.0.1", 8883, &sock));
    TEST_ASSERT_EQUAL(0u, sock);
}

void test_WIFI_T13_open_socket_udp_nominal(void)
{
    wifi_handle_t handle = helper_create_ready();

    helper_script_at_command("\r\nOK\r\n"); /* P0=0 (select) */
    helper_script_at_command("\r\nOK\r\n"); /* P1=1 (UDP) */
    helper_script_at_command("\r\nOK\r\n"); /* P3=<host> */
    helper_script_at_command("\r\nOK\r\n"); /* P4=<port> */
    helper_script_at_command("\r\nOK\r\n"); /* P6=1 (start client) */

    wifi_socket_t sock = WIFI_INVALID_SOCKET;
    TEST_ASSERT_EQUAL(WIFI_ERR_OK,
                      wifi_open_socket(handle, WIFI_SOCKET_UDP, "pool.ntp.org", 123, &sock));
    TEST_ASSERT_EQUAL(0u, sock);
}

void test_WIFI_T14_send_link_down(void)
{
    wifi_handle_t handle = helper_create_ready();

    const uint8_t payload[] = {0x01u, 0x02u};
    /* No AT command scripted: link is DOWN, so wifi_send must bail out
     * before touching SPI. socket 0 is not open either way. */
    TEST_ASSERT_EQUAL(WIFI_ERR_NOT_CONNECTED, wifi_send(handle, 0u, payload, sizeof(payload)));
}

void test_WIFI_T15_drdy_timeout(void)
{
    wifi_handle_t handle = helper_create_ready();

    helper_script_at_command("\r\nOK\r\n"); /* C1 */
    helper_script_at_command("\r\nOK\r\n"); /* C2 */
    helper_script_at_command("\r\nOK\r\n"); /* C3 */
    helper_script_at_command("\r\nOK\r\n"); /* C4 */
    helper_script_at_command("\r\nOK\r\n"); /* C0 */
    TEST_ASSERT_EQUAL(WIFI_ERR_OK, wifi_connect_ap(handle, "myssid", "mypassword"));

    /* No DRDY levels scripted for this call: stub falls back to LOW
     * forever, so the pre-send "wait DRDY high" wait times out. */
    int8_t rssi = 0;
    TEST_ASSERT_EQUAL(WIFI_ERR_TIMEOUT, wifi_get_rssi(handle, &rssi));
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, last_write_level(NSS_PORT, NSS_PIN));
}

void test_WIFI_T16_nss_deasserted_on_spi_error(void)
{
    wifi_handle_t handle = helper_create_ready();

    helper_script_at_command("\r\nOK\r\n"); /* C1 */
    helper_script_at_command("\r\nOK\r\n"); /* C2 */
    helper_script_at_command("\r\nOK\r\n"); /* C3 */
    helper_script_at_command("\r\nOK\r\n"); /* C4 */
    helper_script_at_command("\r\nOK\r\n"); /* C0 */
    TEST_ASSERT_EQUAL(WIFI_ERR_OK, wifi_connect_ap(handle, "myssid", "mypassword"));

    drdy_seq_push(GPIO_LEVEL_HIGH, 1u); /* pre-send DRDY high succeeds ... */
    s_spi_forced_err = SPI_ERR_TIMEOUT; /* ... then the SPI exchange itself fails. */

    int8_t rssi = 0;
    TEST_ASSERT_EQUAL(WIFI_ERR_SPI, wifi_get_rssi(handle, &rssi));
    TEST_ASSERT_EQUAL(GPIO_LEVEL_HIGH, last_write_level(NSS_PORT, NSS_PIN));
}

void test_WIFI_T17_socket_table_exhaustion(void)
{
    wifi_handle_t handle = helper_create_ready();

    for (uint8_t i = 0u; i < WIFI_MAX_SOCKETS; i++)
    {
        helper_script_at_command("\r\nOK\r\n"); /* P0= */
        helper_script_at_command("\r\nOK\r\n"); /* P1= */
        helper_script_at_command("\r\nOK\r\n"); /* P3= */
        helper_script_at_command("\r\nOK\r\n"); /* P4= */
        helper_script_at_command("\r\nOK\r\n"); /* P6=1 */

        wifi_socket_t sock = WIFI_INVALID_SOCKET;
        TEST_ASSERT_EQUAL(WIFI_ERR_OK,
                          wifi_open_socket(handle, WIFI_SOCKET_TCP, "10.0.0.1", 8883, &sock));
        TEST_ASSERT_EQUAL(i, sock);
    }

    wifi_socket_t overflow_sock = WIFI_INVALID_SOCKET;
    TEST_ASSERT_EQUAL(WIFI_ERR_NO_RESOURCE,
                      wifi_open_socket(handle, WIFI_SOCKET_TCP, "10.0.0.1", 8883, &overflow_sock));
}

void test_WIFI_T18_close_socket_frees_slot(void)
{
    wifi_handle_t handle = helper_create_ready();

    helper_script_at_command("\r\nOK\r\n"); /* P0= */
    helper_script_at_command("\r\nOK\r\n"); /* P1= */
    helper_script_at_command("\r\nOK\r\n"); /* P3= */
    helper_script_at_command("\r\nOK\r\n"); /* P4= */
    helper_script_at_command("\r\nOK\r\n"); /* P6=1 */
    wifi_socket_t sock = WIFI_INVALID_SOCKET;
    TEST_ASSERT_EQUAL(WIFI_ERR_OK,
                      wifi_open_socket(handle, WIFI_SOCKET_TCP, "10.0.0.1", 8883, &sock));
    TEST_ASSERT_EQUAL(0u, sock);

    helper_script_at_command("\r\nOK\r\n"); /* P0= (select) */
    helper_script_at_command("\r\nOK\r\n"); /* P6=0 (stop client) */
    TEST_ASSERT_EQUAL(WIFI_ERR_OK, wifi_close_socket(handle, sock));

    helper_script_at_command("\r\nOK\r\n"); /* P0= */
    helper_script_at_command("\r\nOK\r\n"); /* P1= */
    helper_script_at_command("\r\nOK\r\n"); /* P3= */
    helper_script_at_command("\r\nOK\r\n"); /* P4= */
    helper_script_at_command("\r\nOK\r\n"); /* P6=1 */
    wifi_socket_t reopened = WIFI_INVALID_SOCKET;
    TEST_ASSERT_EQUAL(WIFI_ERR_OK,
                      wifi_open_socket(handle, WIFI_SOCKET_TCP, "10.0.0.1", 8883, &reopened));
    TEST_ASSERT_EQUAL(0u, reopened);
}
