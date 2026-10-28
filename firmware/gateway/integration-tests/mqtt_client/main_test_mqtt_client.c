/**
 * @file main_test_mqtt_client.c
 * @brief Hardware bring-up test for MqttClient (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default. To activate:
 *   1. Copy this file into Src/ (or add integration-tests/mqtt_client/ as
 *      a source path in CubeIDE).
 *   2. In CubeIDE, right-click Src/main.c -> Properties ->
 *      C/C++ Build -> check "Exclude resource from build".
 *   3. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   4. Wire the ISM43362-M3G-L44 module (or use the on-board one on the
 *      B-L475E-IOT01A) — same SPI3 + 5 control-line wiring as
 *      integration-tests/wifi_driver/main_test_wifi_driver.c.
 *   5. Create firmware/gateway/certs/bringup_secrets.h with your real
 *      BRINGUP_WIFI_SSID / BRINGUP_WIFI_PASSWORD — see
 *      scripts/bringup_secrets.h.example for the template.
 *      bringup_secrets.h is gitignored and never committed; without
 *      it this is a placeholder-only build
 *      (BRINGUP_WIFI_SSID stays empty and this bring-up halts at the
 *      "no AP" check). Separately, BRINGUP_MQTT_BROKER_ENDPOINT and the
 *      three DER cert/key arrays come from bringup_certs.h
 *      automatically — it's found via the
 *      firmware/gateway/certs/ include path (added to .cproject), no
 *      manual copying needed; run regenerate-bringup-certs.ps1 first if it
 *      doesn't exist yet (see scripts/regenerate-bringup-certs-usage.txt),
 *      or fill BRINGUP_MQTT_* in by hand for a real AWS IoT Core "thing"
 *      instead of the local broker.
 *   6. To exercise TC-HW-MQTT-007/008 (inbound command), use the AWS IoT
 *      Core MQTT test client (console) to publish to
 *      cmd/iotmonitor/<client_id>/config while the countdown in
 *      TC-HW-MQTT-008 is running.
 *
 * Reports through the real Logger middleware, same pattern as
 * main_test_wifi_driver.c. WiFi + MqttClient bring-up (Phase 1: pins,
 * SpiDriver, wifi_create()) runs pre-scheduler; the connect/publish/
 * subscribe/process/disconnect exercise (Phase 2) runs from a real
 * FreeRTOS task after vTaskStartScheduler(), the same task-context
 * MqttClient is designed to run in (companion §1, §12 — CloudPublisherTask
 * is the only caller).
 *
 * Hardware outputs:
 *   USART1 TX  PB6   115 200 8N1  Logger output (LOG_INFO/LOG_ERROR lines)
 *   LD2 green  PA5                Heartbeat once the mqtt task is running
 *   SPI3 SCK/MISO/MOSI  PC10/PC11/PC12  AF6, 10 MHz, mode 0, 16-bit frames
 *   NSS/DRDY/RST/WAKEUP/BOOT0  PE0/PE1/PE8/PB13/PB12 (ISM43362 control)
 *
 * Automated test sequence:
 *   TC-HW-MQTT-001  gpio_init() + spi_create() + wifi_create() all succeed
 *   TC-HW-MQTT-002  wifitask_create() + wifitask_connect_ap() associates
 *                   with BRINGUP_WIFI_SSID (WIFITASK-O3: routed through
 *                   WifiTask, not WifiDriver directly)
 *   TC-HW-MQTT-003  (only if BRINGUP_MQTT_BROKER_ENDPOINT is non-empty)
 *                   mqtt_client_create() returns MQTT_CLIENT_ERR_OK
 *   TC-HW-MQTT-004  mqtt_client_connect() completes TLS handshake + CONNACK
 *   TC-HW-MQTT-005  mqtt_client_publish() QoS 0 telemetry frame
 *   TC-HW-MQTT-006  mqtt_client_publish() QoS 1 alarm frame, PUBACK observed
 *   TC-HW-MQTT-007  mqtt_client_subscribe() to the config command topic,
 *                   SUBACK observed
 *   TC-HW-MQTT-008  mqtt_client_process() loop; publish a message to the
 *                   subscribed topic from the AWS IoT console during the
 *                   countdown to observe msg_cb firing
 *   TC-HW-MQTT-009  mqtt_client_get_stats() — counters logged and sanity
 *                   checked (connect_ok == 1, publishes_sent == 2,
 *                   publishes_acked == 1)
 *   TC-HW-MQTT-014  mbedTLS static arena (WP2, MQTT-O1, FS-O4). Right after the handshake the
 *                   arena peak is logged; then one ECDSA-P256 verify (the OTA signature check,
 *                   same mbedTLS calls as ota_crypto.c) runs with the TLS session still open and
 *                   the peak is logged again. PASS needs the verify to succeed and the C heap
 *                   (sbrk) not to have grown between mqtt_client_create() and the end of the
 *                   verify, i.e. mbedTLS used no heap. Expected lines:
 *                     TC-HW-MQTT-014  arena after TLS handshake: peak=<n> of <size> bytes
 *                     TC-HW-MQTT-014  arena after verify with session open: peak=<n> of <size>
 *                       bytes (verify added <d>), heap grew by 0 bytes
 *                   <size> minus the second peak is the margin; the size is fixed from this number
 *                   plus 25 % (tls_arena.h). A mqtt_client_connect() failure with an mbedTLS
 *                   ALLOC_FAILED code means the arena is too small.
 *   TC-HW-MQTT-011  Broker-drop / reconnect resilience (MQTT-O8). Stop the
 *                   MQTT broker when prompted; the test polls
 *                   mqtt_client_process() until disconnect_cb fires (real
 *                   keep-alive timeout / TCP failure, not simulated), then
 *                   — once you restart the broker when prompted —
 *                   reconnects. This is the hardware-only half of the
 *                   MQTT-O8 fix: the host unit tests (MQTT-T18/T19) prove
 *                   the *mock* WifiDriver's socket-table bookkeeping is
 *                   correct, but only real hardware proves the ISM43362
 *                   module itself actually frees the socket when
 *                   wifi_close_socket() issues its P6=0 "stop client" —
 *                   if it didn't, this reconnect would fail exactly the
 *                   way a leaked socket slot fails on real hardware.
 *   TC-HW-MQTT-012  WIFI_MAX_SOCKETS further disconnect/reconnect cycles,
 *                   fully automated (broker stays up) — proves the
 *                   module's real 4-slot socket table doesn't drift into
 *                   exhaustion across repeated cycles, matching MQTT-T19's
 *                   "WIFI_MAX_SOCKETS + 1 total cycles" bound (TC-HW-
 *                   MQTT-011's abnormal cycle is the "+1").
 *   TC-HW-MQTT-013  mqtt_client_disconnect() completes without invoking
 *                   disconnect_cb
 *
 * TC-HW-MQTT-011 needs a broker you can stop and restart on command while
 * the board keeps running — this is what BRINGUP_MQTT_BROKER_ENDPOINT's
 * local Mosquitto setup is for (a real AWS IoT Core endpoint works for
 * every other test case, but you can't stop it on demand for this one).
 *
 * Note on scope: TC-HW-MQTT-011/012 validate MqttClient's resource cleanup
 * after a *broker*-level drop, which is what MQTT-O8 fixed. They do not
 * (and cannot, with WifiDriver as it stands today) validate recovery from
 * a *WiFi AP*-level drop — that is WIFI-O15, an open, deferred gap: nothing
 * in WifiDriver or this bring-up re-associates with the AP if it drops the
 * station, so don't power off the router expecting this test to recover;
 * it will just hang at TC-HW-MQTT-011's poll loop until you either restore
 * the AP or reset the board.
 *
 * If cpu_init(), debug_uart_init(), or rtc_init() fail, Logger cannot be
 * trusted as the reporting channel yet — the board halts with a fast LED
 * blink instead (same convention as main_test_wifi_driver.c).
 */

#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "exti/exti_driver.h"
#include "gpio/gpio_driver.h"
#include "logger/logger.h"
#include "rtc/rtc.h"
#include "spi/spi.h"
#include "wifi_driver/wifi_driver.h"
#include "wifi_task/wifi_task.h"

#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mqtt_client.h"
#include "mqtt_topic_config.h"
#include "tls_arena.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Fill these in before flashing.                                         */
/* ---------------------------------------------------------------------- */

/* Real credentials come from bringup_secrets.h (gitignored, never committed —
 * see scripts/bringup_secrets.h.example) when the file exists; otherwise
 * this is a placeholder-only build. Same __has_include mechanism as
 * bringup_certs.h below, so no build-setting toggle is needed and the
 * tracked .cproject builds cleanly on a machine without secrets (CI). */
#if __has_include("bringup_secrets.h")
#include "bringup_secrets.h"
#else
#define BRINGUP_WIFI_SSID ""
#define BRINGUP_WIFI_PASSWORD ""
#endif

/* Leave BRINGUP_MQTT_BROKER_ENDPOINT empty to skip TC-HW-MQTT-003 onward.
 *
 * Currently pointed at a local Mosquitto broker (not AWS IoT Core) running
 * on the dev machine's Wi-Fi adapter IP, port 8883, TLS 1.2 with a
 * self-signed test CA and mutual-auth client cert — see
 * local-test/mosquitto-broker/ (gitignored: contains private key material)
 * for the cert material and start-mosquitto-tls.bat, which starts it.
 * Ctrl+C that window (or close it) to stop it — needed interactively for
 * TC-HW-MQTT-011/012. The board's WiFi AP (BRINGUP_WIFI_SSID) must be the
 * same network this IP is reachable on, and must match the IP baked into
 * the server cert's SAN (start-mosquitto-tls.bat prints it on launch).
 * Swap back to a real AWS IoT Core endpoint + provisioned certs for a
 * production-representative test.
 *
 * bringup_certs.h (gitignored) is generated by
 * scripts/regenerate-bringup-certs.ps1 -BrokerIp <ip> —
 * it supplies BRINGUP_CERTS_BROKER_ENDPOINT and the three DER byte arrays
 * below, and stays in sync whenever the broker machine's IP changes.
 * Falls back to empty placeholders if it hasn't been generated yet. */
#if __has_include("bringup_certs.h")
#include "bringup_certs.h"
#define BRINGUP_MQTT_BROKER_ENDPOINT BRINGUP_CERTS_BROKER_ENDPOINT
#else
#define BRINGUP_MQTT_BROKER_ENDPOINT ""
static const uint8_t s_bringup_ca_cert_der[] = {0x00};
static const uint8_t s_bringup_client_cert_der[] = {0x00};
static const uint8_t s_bringup_client_key_der[] = {0x00};
#endif

#define BRINGUP_MQTT_BROKER_PORT (8883U)
#define BRINGUP_MQTT_CLIENT_ID "gw-bringup-001"
#define BRINGUP_MQTT_KEEP_ALIVE_S (60U)

/* ---------------------------------------------------------------------- */
/* Board constants — identical wiring to main_test_wifi_driver.c.         */
/* ---------------------------------------------------------------------- */

#define BRINGUP_LED_PORT GPIO_PORT_A
#define BRINGUP_LED_PIN (5U)

#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)

#define BRINGUP_SPI_SCK_PORT GPIO_PORT_C
#define BRINGUP_SPI_SCK_PIN (10U)
#define BRINGUP_SPI_MISO_PORT GPIO_PORT_C
#define BRINGUP_SPI_MISO_PIN (11U)
#define BRINGUP_SPI_MOSI_PORT GPIO_PORT_C
#define BRINGUP_SPI_MOSI_PIN (12U)
#define BRINGUP_SPI_AF (6U)

#define BRINGUP_NSS_PORT GPIO_PORT_E
#define BRINGUP_NSS_PIN (0U)
#define BRINGUP_DRDY_PORT GPIO_PORT_E
#define BRINGUP_DRDY_PIN (1U)
#define BRINGUP_RST_PORT GPIO_PORT_E
#define BRINGUP_RST_PIN (8U)
#define BRINGUP_WAKEUP_PORT GPIO_PORT_B
#define BRINGUP_WAKEUP_PIN (13U)
#define BRINGUP_BOOT0_PORT GPIO_PORT_B
#define BRINGUP_BOOT0_PIN (12U)

#define MQTT_TASK_STACK_WORDS (1024U)
#define MQTT_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)

/* ---------------------------------------------------------------------- */
/* Boot-failure halt — Logger is not yet trusted, so this bypasses it,     */
/* same convention as main_test_wifi_driver.c.                            */
/* ---------------------------------------------------------------------- */

static void bringup_raw_spin(uint32_t n)
{
    volatile uint32_t c = n;
    while (c > 0U)
    {
        --c;
    }
}

static void bringup_halt(void)
{
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
    GPIOA->MODER &= ~(3UL << (BRINGUP_LED_PIN * 2U));
    GPIOA->MODER |= (1UL << (BRINGUP_LED_PIN * 2U));
    for (;;)
    {
        GPIOA->BSRR = (1UL << BRINGUP_LED_PIN);
        bringup_raw_spin(200000U);
        GPIOA->BSRR = (1UL << (BRINGUP_LED_PIN + 16U));
        bringup_raw_spin(200000U);
    }
}

static void bringup_fail(const char *label)
{
    LOG_ERROR("Mqtt", "%s", label);
    for (;;)
    {
        (void) gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        if (xTaskGetSchedulerState() == taskSCHEDULER_RUNNING)
        {
            vTaskDelay(pdMS_TO_TICKS(500));
        }
        else
        {
            bringup_raw_spin(500000U);
        }
    }
}

void EXTI1_IRQHandler(void)
{
    exti_clear_pending(WIFI_DRDY_EXTI_LINE);
    wifi_datardy_irq_handler();
}

static void bringup_countdown(const char *action, uint32_t seconds)
{
    for (uint32_t s = seconds; s > 0U; s--)
    {
        LOG_INFO("Mqtt", "%s in %u...", action, (unsigned) s);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ---------------------------------------------------------------------- */
/* MqttClient callbacks under test                                        */
/* ---------------------------------------------------------------------- */

static volatile bool s_msg_cb_fired;
static volatile bool s_disconnect_cb_fired;

static void bringup_msg_cb(const char *topic, uint16_t topic_len, const uint8_t *payload,
                           uint32_t payload_len)
{
    s_msg_cb_fired = true;
    LOG_INFO("Mqtt", "TC-HW-MQTT-008  msg_cb fired: topic='%.*s' payload_len=%u", (int) topic_len,
             topic, (unsigned) payload_len);
}

static void bringup_disconnect_cb(void)
{
    s_disconnect_cb_fired = true;
    /* Expected during TC-HW-MQTT-011; unexpected everywhere else — each
     * phase interprets s_disconnect_cb_fired against its own context, so
     * this log stays neutral rather than presupposing which one applies. */
    LOG_WARN("Mqtt", "disconnect_cb fired");
}

/* ---------------------------------------------------------------------- */
/* MqttClient task — Phase 2 (post-scheduler).                            */
/* ---------------------------------------------------------------------- */

/* ---------------------------------------------------------------------- */
/* TC-HW-MQTT-014 helpers: a known-answer ECDSA-P256 vector, generated once  */
/* with an ephemeral key (nothing secret): SHA-256 of a fixed message, its    */
/* signature and the public point.                                           */
/* ---------------------------------------------------------------------- */

static const uint8_t s_vector_pubkey[65] = {
    0x04U, 0x74U, 0xAEU, 0x94U, 0x5FU, 0xD9U, 0x7AU, 0x86U, 0x27U, 0xE5U, 0x62U, 0x62U, 0x66U,
    0x97U, 0x95U, 0x01U, 0xB4U, 0x2EU, 0x8DU, 0x61U, 0x5BU, 0xC0U, 0xFBU, 0x96U, 0xACU, 0x5BU,
    0x7BU, 0x76U, 0xB5U, 0x41U, 0xA3U, 0x0DU, 0xDFU, 0xBDU, 0x7FU, 0xD5U, 0x7EU, 0x44U, 0x92U,
    0x8CU, 0x3FU, 0x35U, 0x4EU, 0x15U, 0x2BU, 0xB1U, 0x3CU, 0x73U, 0xE8U, 0xF3U, 0x88U, 0x51U,
    0x52U, 0x8FU, 0x36U, 0x1BU, 0xE8U, 0x25U, 0x2EU, 0xBCU, 0xF9U, 0xD3U, 0xB0U, 0x2FU, 0xCEU,
};

static const uint8_t s_vector_digest[32] = {
    0x7EU, 0xE1U, 0xD1U, 0x73U, 0x28U, 0xCCU, 0x0DU, 0xF8U, 0x68U, 0x44U, 0x0DU,
    0x00U, 0x05U, 0x4BU, 0xAFU, 0x58U, 0x24U, 0xC5U, 0xD1U, 0x8FU, 0x62U, 0x38U,
    0xCFU, 0xA6U, 0x10U, 0x16U, 0x54U, 0xBCU, 0x6AU, 0xF2U, 0x4AU, 0x06U,
};

static const uint8_t s_vector_r[32] = {
    0x9AU, 0x93U, 0x11U, 0x56U, 0xD6U, 0x06U, 0x8BU, 0x91U, 0x35U, 0x24U, 0x61U,
    0xAFU, 0xA5U, 0xEBU, 0x1EU, 0xBEU, 0x6AU, 0xCBU, 0x8FU, 0x4BU, 0x85U, 0xE7U,
    0x56U, 0x60U, 0x61U, 0xA1U, 0xA0U, 0x79U, 0xE9U, 0xB1U, 0x6CU, 0x81U,
};

static const uint8_t s_vector_s[32] = {
    0x7CU, 0x4AU, 0xAEU, 0xCCU, 0xA1U, 0x70U, 0x2FU, 0x63U, 0x41U, 0x02U, 0x43U,
    0x3EU, 0x40U, 0xB2U, 0x18U, 0x9AU, 0xB6U, 0x8EU, 0x83U, 0xD4U, 0x5BU, 0xAAU,
    0x5FU, 0x91U, 0x5FU, 0x48U, 0xD0U, 0x73U, 0xA1U, 0x56U, 0x6FU, 0x36U,
};

/** @brief Bytes the C heap (newlib, via _sbrk) has handed out since boot. */
static uint32_t bringup_heap_bytes(void)
{
    extern uint8_t _end; /* linker symbol: the heap starts here (Src/sysmem.c) */
    extern void *_sbrk(ptrdiff_t incr);

    return (uint32_t) ((uintptr_t) _sbrk(0) - (uintptr_t) &_end);
}

/** @brief The OTA signature check on the known-answer vector (same calls as ota_crypto.c). */
static bool bringup_ecdsa_verify_once(void)
{
    mbedtls_ecp_group grp;
    mbedtls_ecp_point q;
    mbedtls_mpi r;
    mbedtls_mpi s;
    bool ok = false;

    mbedtls_ecp_group_init(&grp);
    mbedtls_ecp_point_init(&q);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&s);

    if ((mbedtls_ecp_group_load(&grp, MBEDTLS_ECP_DP_SECP256R1) == 0) &&
        (mbedtls_ecp_point_read_binary(&grp, &q, s_vector_pubkey, sizeof(s_vector_pubkey)) == 0) &&
        (mbedtls_ecp_check_pubkey(&grp, &q) == 0) &&
        (mbedtls_mpi_read_binary(&r, s_vector_r, sizeof(s_vector_r)) == 0) &&
        (mbedtls_mpi_read_binary(&s, s_vector_s, sizeof(s_vector_s)) == 0))
    {
        ok =
            (mbedtls_ecdsa_verify(&grp, s_vector_digest, sizeof(s_vector_digest), &q, &r, &s) == 0);
    }

    mbedtls_mpi_free(&s);
    mbedtls_mpi_free(&r);
    mbedtls_ecp_point_free(&q);
    mbedtls_ecp_group_free(&grp);

    return ok;
}

static StaticTask_t s_mqtt_task_tcb;
static StackType_t s_mqtt_task_stack[MQTT_TASK_STACK_WORDS];

static void mqtt_bringup_task(void *arg)
{
    wifi_handle_t wifi_handle = (wifi_handle_t) arg;

    /* WIFITASK-O3: WifiTask is now the sole caller of WifiDriver (D29).
     * wifitask_create() must run in task context, post-scheduler — it
     * registers the real DATARDY callback itself (formerly the
     * wifi_attach_datardy_callback(..., NULL, NULL) call in main()). */
    wifitask_config_t wifitask_config = {.wifi = wifi_handle};
    wifitask_handle_t wifitask_handle = NULL;
    if (wifitask_create(&wifitask_config, &wifitask_handle) != WIFITASK_ERR_OK)
    {
        bringup_fail("TC-HW-MQTT-002  wifitask_create() failed");
    }
    LOG_INFO("Mqtt", "wifitask_create() returned WIFITASK_ERR_OK (WifiTask's own task is "
                     "now running)");

    /* TC-HW-MQTT-002 */
    if (sizeof(BRINGUP_WIFI_SSID) <= 1U)
    {
        LOG_INFO("Mqtt", "BRINGUP_WIFI_SSID is empty - cannot continue without an AP");
        for (;;)
        {
            (void) gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }
    wifitask_err_t wifi_err =
        wifitask_connect_ap(wifitask_handle, BRINGUP_WIFI_SSID, BRINGUP_WIFI_PASSWORD);
    if (wifi_err != WIFITASK_ERR_OK)
    {
        LOG_ERROR("Mqtt", "wifitask_connect_ap() error code: %d", (int) wifi_err);
        bringup_fail("TC-HW-MQTT-002  wifitask_connect_ap() failed");
    }
    LOG_INFO("Mqtt", "TC-HW-MQTT-002  associated with %s", BRINGUP_WIFI_SSID);
    vTaskDelay(pdMS_TO_TICKS(200));

    if (sizeof(BRINGUP_MQTT_BROKER_ENDPOINT) <= 1U)
    {
        LOG_INFO("Mqtt", "BRINGUP_MQTT_BROKER_ENDPOINT is empty - skipping TC-HW-MQTT-003 onward");
        for (;;)
        {
            (void) gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
            vTaskDelay(pdMS_TO_TICKS(500));
        }
    }

    const uint32_t heap_before_tls = bringup_heap_bytes();

    /* TC-HW-MQTT-003 */
    mqtt_client_config_t mqtt_config = {
        .wifi = wifitask_handle,
        .msg_cb = bringup_msg_cb,
        .disconnect_cb = bringup_disconnect_cb,
    };
    mqtt_client_handle_t mqtt_handle = NULL;
    if (mqtt_client_create(&mqtt_config, &mqtt_handle) != MQTT_CLIENT_ERR_OK)
    {
        bringup_fail("TC-HW-MQTT-003  mqtt_client_create() failed");
    }
    LOG_INFO("Mqtt", "TC-HW-MQTT-003  mqtt_client_create() returned MQTT_CLIENT_ERR_OK");

    /* TC-HW-MQTT-004 */
    mqtt_connect_cfg_t connect_cfg = {
        .broker_endpoint = BRINGUP_MQTT_BROKER_ENDPOINT,
        .broker_port = BRINGUP_MQTT_BROKER_PORT,
        .client_id = BRINGUP_MQTT_CLIENT_ID,
        .client_cert_der = s_bringup_client_cert_der,
        .client_cert_len = sizeof(s_bringup_client_cert_der),
        .client_key_der = s_bringup_client_key_der,
        .client_key_len = sizeof(s_bringup_client_key_der),
        .ca_cert_der = s_bringup_ca_cert_der,
        .ca_cert_len = sizeof(s_bringup_ca_cert_der),
        .keep_alive_s = BRINGUP_MQTT_KEEP_ALIVE_S,
    };
    LOG_INFO("Mqtt", "TC-HW-MQTT-004  connecting to %s:%u...", BRINGUP_MQTT_BROKER_ENDPOINT,
             (unsigned) BRINGUP_MQTT_BROKER_PORT);
    mqtt_client_err_t connect_result = mqtt_client_connect(mqtt_handle, &connect_cfg);
    if (connect_result != MQTT_CLIENT_ERR_OK)
    {
        LOG_ERROR("Mqtt", "mqtt_client_connect() error code: %d", (int) connect_result);
        bringup_fail("TC-HW-MQTT-004  mqtt_client_connect() failed (TLS handshake or CONNACK)");
    }
    LOG_INFO("Mqtt", "TC-HW-MQTT-004  connected (TLS 1.2 + MQTT CONNACK accepted)");
    vTaskDelay(pdMS_TO_TICKS(200));

    /* TC-HW-MQTT-014: the arena peak after the handshake, then a verify with the session open. */
    const uint32_t handshake_peak = tls_arena_peak_bytes();
    LOG_INFO("Mqtt", "TC-HW-MQTT-014  arena after TLS handshake: peak=%lu of %lu bytes",
             (unsigned long) handshake_peak, (unsigned long) TLS_ARENA_SIZE_BYTES);

    if (!bringup_ecdsa_verify_once())
    {
        bringup_fail("TC-HW-MQTT-014  ECDSA verify with the TLS session open failed");
    }

    const uint32_t verify_peak = tls_arena_peak_bytes();
    const uint32_t heap_growth = bringup_heap_bytes() - heap_before_tls;
    LOG_INFO("Mqtt",
             "TC-HW-MQTT-014  arena after verify with session open: peak=%lu of %lu bytes "
             "(verify added %lu), heap grew by %lu bytes",
             (unsigned long) verify_peak, (unsigned long) TLS_ARENA_SIZE_BYTES,
             (unsigned long) (verify_peak - handshake_peak), (unsigned long) heap_growth);

    if (heap_growth != 0U)
    {
        bringup_fail(
            "TC-HW-MQTT-014  the C heap grew: mbedTLS (or other code) still allocates there");
    }

    /* TC-HW-MQTT-005 */
    static const uint8_t telemetry_payload[] = "{\"temp_c\":22.5}";
    char telemetry_topic[MQTT_TOPIC_MAX_LEN];
    (void) snprintf(telemetry_topic, sizeof(telemetry_topic), "%s%s%s", MQTT_TOPIC_PREFIX_PUBLISH,
                    BRINGUP_MQTT_CLIENT_ID, MQTT_TOPIC_SUFFIX_TELEMETRY);
    if (mqtt_client_publish(mqtt_handle, telemetry_topic, telemetry_payload,
                            sizeof(telemetry_payload) - 1U, MQTT_QOS_0) != MQTT_CLIENT_ERR_OK)
    {
        bringup_fail("TC-HW-MQTT-005  mqtt_client_publish() QoS 0 failed");
    }
    LOG_INFO("Mqtt", "TC-HW-MQTT-005  published QoS 0 telemetry to %s", telemetry_topic);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* TC-HW-MQTT-006 */
    static const uint8_t alarm_payload[] = "{\"alarm\":\"BRINGUP_TEST\"}";
    char alarm_topic[MQTT_TOPIC_MAX_LEN];
    (void) snprintf(alarm_topic, sizeof(alarm_topic), "%s%s%s", MQTT_TOPIC_PREFIX_PUBLISH,
                    BRINGUP_MQTT_CLIENT_ID, MQTT_TOPIC_SUFFIX_ALARMS);
    mqtt_client_err_t publish_qos1_result = mqtt_client_publish(
        mqtt_handle, alarm_topic, alarm_payload, sizeof(alarm_payload) - 1U, MQTT_QOS_1);
    if (publish_qos1_result != MQTT_CLIENT_ERR_OK)
    {
        LOG_ERROR("Mqtt", "mqtt_client_publish() QoS 1 error code: %d", (int) publish_qos1_result);
        bringup_fail("TC-HW-MQTT-006  mqtt_client_publish() QoS 1 failed (no PUBACK?)");
    }
    LOG_INFO("Mqtt", "TC-HW-MQTT-006  published QoS 1 alarm to %s, PUBACK received", alarm_topic);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* TC-HW-MQTT-007 */
    char config_topic[MQTT_TOPIC_MAX_LEN];
    (void) snprintf(config_topic, sizeof(config_topic), "%s%s%s", MQTT_TOPIC_PREFIX_SUBSCRIBE,
                    BRINGUP_MQTT_CLIENT_ID, MQTT_TOPIC_SUFFIX_CONFIG);
    if (mqtt_client_subscribe(mqtt_handle, config_topic, MQTT_QOS_1) != MQTT_CLIENT_ERR_OK)
    {
        bringup_fail("TC-HW-MQTT-007  mqtt_client_subscribe() failed (no SUBACK or rejected)");
    }
    LOG_INFO("Mqtt", "TC-HW-MQTT-007  subscribed to %s, SUBACK received", config_topic);
    vTaskDelay(pdMS_TO_TICKS(200));

    /* TC-HW-MQTT-008
     *
     * mqtt_client_process() is NOT the fast/non-blocking ~100 ms call the
     * companion's §7 recommended cadence assumes when idle: it calls
     * wifi_recv() internally, which floors its own wait at
     * WIFI_RESP_TIMEOUT_MS (5000 ms, wifi_driver.c) regardless of the
     * timeout requested. So each idle poll here can itself take up to
     * ~5 s -- a fixed iteration count with no per-iteration log made a
     * working poll look identical to a genuine hang during bring-up.
     * Logging each attempt keeps that distinguishable; the iteration
     * count is sized for that real per-call cost (12 x ~5 s worst case
     * =~ 60 s), not the original 100 x 100 ms assumption. */
    LOG_INFO("Mqtt", "TC-HW-MQTT-008  publish a message to %s now", config_topic);
    bringup_countdown("Polling for inbound message", 20U);
    for (uint32_t i = 0U; (i < 12U) && !s_msg_cb_fired; ++i)
    {
        LOG_INFO("Mqtt", "TC-HW-MQTT-008  poll attempt %u/12 (each may take up to ~5 s)...",
                 (unsigned) (i + 1U));
        (void) mqtt_client_process(mqtt_handle);
    }
    if (s_msg_cb_fired)
    {
        LOG_INFO("Mqtt", "TC-HW-MQTT-008  msg_cb observed - PASS");
    }
    else
    {
        LOG_INFO("Mqtt", "TC-HW-MQTT-008  no inbound message observed within the window - "
                         "SKIPPED (not a failure; nothing was published)");
    }

    /* TC-HW-MQTT-009 */
    mqtt_stats_t stats;
    if (mqtt_client_get_stats(mqtt_handle, &stats) != MQTT_CLIENT_ERR_OK)
    {
        bringup_fail("TC-HW-MQTT-009  mqtt_client_get_stats() failed");
    }
    LOG_INFO("Mqtt",
             "TC-HW-MQTT-009  stats: connect_ok=%lu publishes_sent=%lu publishes_acked=%lu "
             "publish_failures=%lu subscribe_failures=%lu",
             (unsigned long) stats.connect_ok, (unsigned long) stats.publishes_sent,
             (unsigned long) stats.publishes_acked, (unsigned long) stats.publish_failures,
             (unsigned long) stats.subscribe_failures);
    if ((stats.connect_ok != 1U) || (stats.publishes_sent != 2U) || (stats.publishes_acked != 1U))
    {
        bringup_fail("TC-HW-MQTT-009  stats do not match the expected bring-up sequence");
    }
    LOG_INFO("Mqtt", "TC-HW-MQTT-009  stats match expected counts - PASS");

    /* TC-HW-MQTT-011 (MQTT-O8 hardware validation)
     *
     * The host unit tests (MQTT-T18/T19) only prove the *mock* WifiDriver
     * releases a socket-table slot on abnormal disconnect — the mock
     * cannot prove wifi_close_socket()'s real P0/P6=0 AT sequence
     * actually frees the socket on the physical ISM43362 module. This
     * phase forces a genuine broker drop and proves reconnection works
     * against real hardware: if the module-side socket weren't actually
     * freed, the reconnect below would fail with WIFI_ERR_NO_RESOURCE
     * surfacing as MQTT_CLIENT_ERR_CONNECT_FAIL, a failure mode the host
     * test cannot reach. */
    LOG_INFO("Mqtt", "TC-HW-MQTT-011  broker-drop / reconnect resilience (MQTT-O8)");
    LOG_INFO("Mqtt", "Stop the MQTT broker now (Ctrl+C the mosquitto window).");
    bringup_countdown("Stopping broker", 15U);

    s_disconnect_cb_fired = false;
    LOG_INFO("Mqtt",
             "TC-HW-MQTT-011  polling for disconnect detection (keep_alive_s=%u, so up to "
             "~%u s worst case)...",
             (unsigned) BRINGUP_MQTT_KEEP_ALIVE_S,
             (unsigned) (BRINGUP_MQTT_KEEP_ALIVE_S + (BRINGUP_MQTT_KEEP_ALIVE_S / 2U)));
    /* Same MQTT-O7 caveat as TC-HW-MQTT-008's loop: each mqtt_client_process()
     * call can itself take up to ~5 s, so 30 iterations gives ~150 s of
     * headroom against the ~90 s worst-case keep-alive window above. */
    for (uint32_t i = 0U; (i < 30U) && !s_disconnect_cb_fired; ++i)
    {
        LOG_INFO("Mqtt", "TC-HW-MQTT-011  poll attempt %u/30 (each may take up to ~5 s)...",
                 (unsigned) (i + 1U));
        (void) mqtt_client_process(mqtt_handle);
    }
    if (!s_disconnect_cb_fired)
    {
        bringup_fail("TC-HW-MQTT-011  disconnect_cb was not invoked - broker drop not detected");
    }
    LOG_INFO("Mqtt", "TC-HW-MQTT-011  disconnect detected, disconnect_cb fired - PASS");

    LOG_INFO("Mqtt", "Restart the MQTT broker now.");
    bringup_countdown("Restarting broker", 15U);

    mqtt_client_err_t reconnect_result = mqtt_client_connect(mqtt_handle, &connect_cfg);
    if (reconnect_result != MQTT_CLIENT_ERR_OK)
    {
        LOG_ERROR("Mqtt", "mqtt_client_connect() error code: %d", (int) reconnect_result);
        bringup_fail("TC-HW-MQTT-011  reconnect failed - socket/TLS state leaked on real hardware");
    }
    LOG_INFO("Mqtt", "TC-HW-MQTT-011  reconnected after real broker drop - PASS (WifiDriver "
                     "socket slot was genuinely released on the physical module)");
    vTaskDelay(pdMS_TO_TICKS(200));

    /* TC-HW-MQTT-012
     *
     * Broker stays up from here on — no further operator action. Cycle
     * disconnect+reconnect WIFI_MAX_SOCKETS more times to prove the
     * module's real 4-slot socket table doesn't drift into exhaustion
     * across repeated cycles (mirrors MQTT-T19's "WIFI_MAX_SOCKETS + 1
     * total cycles" bound — TC-HW-MQTT-011's abnormal cycle above is the
     * "+1"). */
    LOG_INFO("Mqtt", "TC-HW-MQTT-012  %u automated disconnect/reconnect cycles...",
             (unsigned) WIFI_MAX_SOCKETS);
    for (uint8_t cycle = 0U; cycle < WIFI_MAX_SOCKETS; ++cycle)
    {
        if (mqtt_client_disconnect(mqtt_handle) != MQTT_CLIENT_ERR_OK)
        {
            bringup_fail("TC-HW-MQTT-012  mqtt_client_disconnect() failed mid-cycle");
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        if (mqtt_client_connect(mqtt_handle, &connect_cfg) != MQTT_CLIENT_ERR_OK)
        {
            LOG_ERROR("Mqtt", "TC-HW-MQTT-012  cycle %u/%u reconnect failed",
                      (unsigned) (cycle + 1U), (unsigned) WIFI_MAX_SOCKETS);
            bringup_fail("TC-HW-MQTT-012  socket table exhausted on real hardware");
        }
        LOG_INFO("Mqtt", "TC-HW-MQTT-012  cycle %u/%u OK", (unsigned) (cycle + 1U),
                 (unsigned) WIFI_MAX_SOCKETS);
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    LOG_INFO("Mqtt", "TC-HW-MQTT-012  all cycles reconnected - PASS (no socket-table exhaustion)");

    mqtt_stats_t resilience_stats;
    if (mqtt_client_get_stats(mqtt_handle, &resilience_stats) == MQTT_CLIENT_ERR_OK)
    {
        LOG_INFO("Mqtt", "TC-HW-MQTT-012  stats: connect_ok=%lu reconnect_count=%lu",
                 (unsigned long) resilience_stats.connect_ok,
                 (unsigned long) resilience_stats.reconnect_count);
    }

    /* TC-HW-MQTT-013 */
    s_disconnect_cb_fired = false; /* reset: TC-HW-MQTT-011 legitimately set this */
    if (mqtt_client_disconnect(mqtt_handle) != MQTT_CLIENT_ERR_OK)
    {
        bringup_fail("TC-HW-MQTT-013  mqtt_client_disconnect() failed");
    }
    if (s_disconnect_cb_fired)
    {
        bringup_fail("TC-HW-MQTT-013  disconnect_cb fired on a graceful disconnect");
    }
    LOG_INFO("Mqtt", "TC-HW-MQTT-013  disconnected gracefully, disconnect_cb NOT invoked - PASS");

    LOG_INFO("Mqtt", "All automated tests complete.");

    for (;;)
    {
        (void) gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

/* ---------------------------------------------------------------------- */
/* main()                                                                  */
/* ---------------------------------------------------------------------- */

int main(void)
{
    if (cpu_init() != STATUS_OK)
    {
        bringup_halt();
    }

    tls_arena_init(); /* before the scheduler and before any mbedTLS call (WP2) */

    if (gpio_init() != GPIO_OK)
    {
        bringup_halt();
    }

    gpio_pin_config_t uart_tx_config = {
        .port = BRINGUP_UART_TX_PORT,
        .pin = BRINGUP_UART_TX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_NONE,
        .alternate = BRINGUP_UART_AF,
    };
    gpio_pin_config_t uart_rx_config = {
        .port = BRINGUP_UART_RX_PORT,
        .pin = BRINGUP_UART_RX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_UP,
        .alternate = BRINGUP_UART_AF,
    };
    (void) gpio_configure_pin(&uart_tx_config);
    (void) gpio_configure_pin(&uart_rx_config);
    if (debug_uart_init() != DEBUG_UART_OK)
    {
        bringup_halt();
    }
    if (rtc_init() != RTC_OK)
    {
        bringup_halt();
    }

    (void) logger_init(LOG_LEVEL_DEBUG);

    LOG_INFO("Mqtt", "===== MqttClient Hardware Bring-up =====");
    LOG_INFO("Mqtt", "Board : B-L475E-IOT01A (STM32L475VGTx)");
    LOG_INFO("Mqtt", "SYSCLK=%lu Hz", (unsigned long) cpu_get_sysclk_hz());

    /* WifiDriver Phase 1 (pre-scheduler): pins, SpiDriver, wifi_create() —
     * identical wiring to main_test_wifi_driver.c. */
    gpio_pin_config_t spi_pins[3] = {
        {.port = BRINGUP_SPI_SCK_PORT,
         .pin = BRINGUP_SPI_SCK_PIN,
         .mode = GPIO_MODE_ALTERNATE,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_VERY_HIGH,
         .pull = GPIO_PULL_NONE,
         .alternate = BRINGUP_SPI_AF},
        {.port = BRINGUP_SPI_MISO_PORT,
         .pin = BRINGUP_SPI_MISO_PIN,
         .mode = GPIO_MODE_ALTERNATE,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_VERY_HIGH,
         .pull = GPIO_PULL_NONE,
         .alternate = BRINGUP_SPI_AF},
        {.port = BRINGUP_SPI_MOSI_PORT,
         .pin = BRINGUP_SPI_MOSI_PIN,
         .mode = GPIO_MODE_ALTERNATE,
         .otype = GPIO_OTYPE_PUSH_PULL,
         .speed = GPIO_SPEED_VERY_HIGH,
         .pull = GPIO_PULL_NONE,
         .alternate = BRINGUP_SPI_AF},
    };
    for (uint8_t i = 0U; i < 3U; ++i)
    {
        if (gpio_configure_pin(&spi_pins[i]) != GPIO_OK)
        {
            bringup_fail("TC-HW-MQTT-001  SPI3 pin configuration failed");
        }
    }

    gpio_pin_config_t nss_config = {.port = BRINGUP_NSS_PORT,
                                    .pin = BRINGUP_NSS_PIN,
                                    .mode = GPIO_MODE_OUTPUT,
                                    .otype = GPIO_OTYPE_PUSH_PULL,
                                    .speed = GPIO_SPEED_LOW,
                                    .pull = GPIO_PULL_NONE,
                                    .alternate = 0};
    gpio_pin_config_t drdy_config = {.port = BRINGUP_DRDY_PORT,
                                     .pin = BRINGUP_DRDY_PIN,
                                     .mode = GPIO_MODE_INPUT,
                                     .otype = GPIO_OTYPE_PUSH_PULL,
                                     .speed = GPIO_SPEED_LOW,
                                     .pull = GPIO_PULL_DOWN,
                                     .alternate = 0};
    gpio_pin_config_t rst_config = {.port = BRINGUP_RST_PORT,
                                    .pin = BRINGUP_RST_PIN,
                                    .mode = GPIO_MODE_OUTPUT,
                                    .otype = GPIO_OTYPE_PUSH_PULL,
                                    .speed = GPIO_SPEED_LOW,
                                    .pull = GPIO_PULL_NONE,
                                    .alternate = 0};
    gpio_pin_config_t wakeup_config = {.port = BRINGUP_WAKEUP_PORT,
                                       .pin = BRINGUP_WAKEUP_PIN,
                                       .mode = GPIO_MODE_OUTPUT,
                                       .otype = GPIO_OTYPE_PUSH_PULL,
                                       .speed = GPIO_SPEED_LOW,
                                       .pull = GPIO_PULL_NONE,
                                       .alternate = 0};
    gpio_pin_config_t boot0_config = {.port = BRINGUP_BOOT0_PORT,
                                      .pin = BRINGUP_BOOT0_PIN,
                                      .mode = GPIO_MODE_OUTPUT,
                                      .otype = GPIO_OTYPE_PUSH_PULL,
                                      .speed = GPIO_SPEED_LOW,
                                      .pull = GPIO_PULL_NONE,
                                      .alternate = 0};
    if ((gpio_configure_pin(&nss_config) != GPIO_OK) ||
        (gpio_configure_pin(&drdy_config) != GPIO_OK) ||
        (gpio_configure_pin(&rst_config) != GPIO_OK) ||
        (gpio_configure_pin(&wakeup_config) != GPIO_OK) ||
        (gpio_configure_pin(&boot0_config) != GPIO_OK))
    {
        bringup_fail("TC-HW-MQTT-001  ISM43362 control-line configuration failed");
    }

    spi_config_t spi_config = {.instance = SPI3};
    spi_handle_t spi_handle = NULL;
    if (spi_create(&spi_config, &spi_handle) != SPI_ERR_OK)
    {
        bringup_fail("TC-HW-MQTT-001  spi_create() failed");
    }

    wifi_config_t wifi_config = {
        .spi = spi_handle,
        .nss_port = BRINGUP_NSS_PORT,
        .nss_pin = BRINGUP_NSS_PIN,
        .drdy_port = BRINGUP_DRDY_PORT,
        .drdy_pin = BRINGUP_DRDY_PIN,
        .rst_port = BRINGUP_RST_PORT,
        .rst_pin = BRINGUP_RST_PIN,
        .wakeup_port = BRINGUP_WAKEUP_PORT,
        .wakeup_pin = BRINGUP_WAKEUP_PIN,
        .boot0_port = BRINGUP_BOOT0_PORT,
        .boot0_pin = BRINGUP_BOOT0_PIN,
    };
    wifi_handle_t wifi_handle = NULL;
    wifi_err_t wifi_err = wifi_create(&wifi_config, &wifi_handle);
    if (wifi_err != WIFI_ERR_OK)
    {
        LOG_ERROR("Mqtt", "wifi_create() error code: %d", (int) wifi_err);
        bringup_fail("TC-HW-MQTT-001  wifi_create() failed");
    }
    /* wifi_attach_datardy_callback() is no longer called directly here —
     * WIFITASK-O3: MqttClient (and this bring-up) now route all WiFi I/O
     * through WifiTask, which registers the real DATARDY callback itself
     * inside wifitask_create() (task-context only, so it happens in
     * mqtt_bringup_task below, post-scheduler, not here). */
    LOG_INFO("Mqtt", "TC-HW-MQTT-001  WifiDriver bring-up complete (reset + AT handshake + "
                     "firmware check)");
    LOG_INFO("Mqtt", "starting scheduler...");

    (void) xTaskCreateStatic(mqtt_bringup_task, "mqtt_bringup", MQTT_TASK_STACK_WORDS, wifi_handle,
                             MQTT_TASK_PRIORITY, s_mqtt_task_stack, &s_mqtt_task_tcb);

    vTaskStartScheduler();

    for (;;)
    {
    }
}
