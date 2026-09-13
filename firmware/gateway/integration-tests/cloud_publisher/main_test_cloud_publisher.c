/**
 * @file main_test_cloud_publisher.c
 * @brief Hardware bring-up test for CloudPublisher (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default. To activate:
 *   1. Copy this file into Src/ (or add integration-tests/cloud_publisher/
 *      as a source path in CubeIDE).
 *   2. In CubeIDE, right-click Src/main.c -> Properties ->
 *      C/C++ Build -> check "Exclude resource from build".
 *   3. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   4. Wire the ISM43362-M3G-L44 module — same SPI3 + 5 control-line
 *      wiring as integration-tests/mqtt_client/main_test_mqtt_client.c.
 *   5. Create firmware/gateway/certs/bringup_secrets.h with your real
 *      BRINGUP_WIFI_SSID / BRINGUP_WIFI_PASSWORD — see
 *      scripts/bringup_secrets.h.example for the template, identical
 *      setup to MqttClient's own bring-up (see that file's header comment
 *      for the local Mosquitto broker option). bringup_secrets.h is
 *      gitignored and never committed. Separately, BRINGUP_MQTT_* broker
 *      fields come from bringup_certs.h automatically — it's found via the
 *      firmware/gateway/certs/ include path (added to .cproject), no
 *      manual copying needed; run regenerate-bringup-certs.ps1 first if
 *      it doesn't exist yet (see scripts/regenerate-bringup-certs-usage.txt).
 *
 * Scope: nine of CloudPublisher's ten dependencies (ISensorService,
 * IAlarmService, IModbusPoller, IStoreAndForward, IHealthSnapshot/
 * IHealthReport, IConfigProvider/IConfigManager, IUpdateService,
 * ILifecycle) do not have a firmware/gateway implementation yet — see
 * cloud_publisher.h's placeholder types. This bring-up therefore wires
 * CloudPublisher to a REAL, connected MqttClient (the one dependency
 * that is real) plus minimal bringup-local stand-ins for the other
 * nine, just enough for cloud_publisher_create() to link and its task
 * loop to run without crashing. What this proves: the module compiles
 * against real FreeRTOS, creates its queues/timers/task, and actually
 * publishes JSON telemetry/health frames over a live MQTT connection.
 * It does NOT prove correctness of the nine placeholder integrations —
 * that happens when each of those modules is implemented for GW.
 *
 * Hardware outputs:
 *   USART1 TX  PB6   115 200 8N1  Logger output (LOG_INFO/LOG_ERROR lines)
 *   LD2 green  PA5                Heartbeat once CloudPublisherTask is running
 *   SPI3 SCK/MISO/MOSI  PC10/PC11/PC12  AF6, 10 MHz, mode 0, 16-bit frames
 *   NSS/DRDY/RST/WAKEUP/BOOT0  PE0/PE1/PE8/PB13/PB12 (ISM43362 control)
 *
 * Automated test sequence:
 *   TC-HW-CP-001  gpio_init() + spi_create() + wifi_create() all succeed
 *   TC-HW-CP-002  wifitask_create() + wifitask_connect_ap() associates
 *                 with BRINGUP_WIFI_SSID (WIFITASK-O3: routed through
 *                 WifiTask, not WifiDriver directly)
 *   TC-HW-CP-003  mqtt_client_create() succeeds (unconnected)
 *   TC-HW-CP-004  mqtt_client_connect() succeeds — this bring-up task
 *                 connects synchronously, single-task, *before* handing the
 *                 handle to CloudPublisher. Internally this still exercises
 *                 the new mqtt_client_connect_step() state machine (MQTT-D8):
 *                 mqtt_client_connect() is now a thin wrapper looping it to
 *                 completion. NOTE: this means CP-D9/CP-D10's own "CloudPub-
 *                 lisherTask's stats tick drives the connect" behaviour is
 *                 NOT exercised for this *initial* connect in this bring-up
 *                 — only for any later reconnect after a drop, which is
 *                 still solely CloudPublisherTask's job once it's running.
 *                 Connecting here first (rather than handing over an
 *                 unconnected handle, CP-D9's normal flow) is required so
 *                 TC-HW-CP-005 can subscribe from this same task before
 *                 CloudPublisherTask starts touching the handle — MqttClient
 *                 is single-task-caller only, no locking (mqtt-client.md
 *                 §12), so subscribing from a second task concurrently with
 *                 CloudPublisherTask's own process()/publish() calls would
 *                 race on the shared MQTTContext_t/socket.
 *   TC-HW-CP-005  mqtt_client_subscribe() to the config command topic
 *                 succeeds (still single-task, before CloudPublisherTask
 *                 exists); publishing to it externally (e.g. mosquitto_pub)
 *                 is then observed arriving at bringup_msg_cb() (CP-O5:
 *                 logging stand-in only, not routed into CloudPublisher)
 *   TC-HW-CP-006  cloud_publisher_create() returns CP_ERR_OK (queues,
 *                 timers, task all created; alarm_service_subscribe()
 *                 called on the bring-up stand-in) — handle is already
 *                 connected+subscribed, so prv_maybe_reconnect()'s first
 *                 tick is a no-op (CP-D9's is_connected() early-return)
 *   TC-HW-CP-007  telemetry timer fires; a JSON telemetry frame is
 *                 observed publishing on dt/iotmonitor/<serial>/telemetry
 *                 (bringup_stats_mqtt_publish spy logs each call)
 *   TC-HW-CP-008  health timer fires; a JSON health frame is observed
 *                 publishing on dt/iotmonitor/<serial>/health
 *   TC-HW-CP-009  stats timer (1 Hz) drives mqtt_client_get_stats() +
 *                 the bring-up health_report_update_mqtt() stand-in
 *                 without crashing, repeatedly; if the broker is dropped
 *                 and restarted, this same tick's prv_maybe_reconnect()
 *                 is what exercises CP-D10's ticked reconnect for real
 *
 * WIFITASK-O1 Phase 4 (wifi-task.md §10) — the non-blocking recv path
 * under real traffic. These need the laptop side too:
 *   TC-HW-CP-010  data-integrity (TC-HW-WIFITASK-data-integrity): run
 *                 scripts/bringup-data-integrity.py on the broker host;
 *                 it publishes N patterned payloads to the config topic,
 *                 sized to straddle the module's 1460-byte R0 chunk and
 *                 span several chunks. bringup_msg_cb() verifies every
 *                 byte and the sequence, then logs PASS/FAIL totals —
 *                 proves wifitask_try_recv()'s stream cursor never
 *                 duplicates, drops or reorders a byte.
 *   TC-HW-CP-011  alarm-latency (TC-HW-WIFITASK-alarm-latency,
 *                 REQ-NF-113): this task fires a synthetic alarm every
 *                 CP_BRINGUP_ALARM_PERIOD_MS through the captured
 *                 AlarmService subscriber; CloudPublisher's outcome comes
 *                 back through the health_report_push_event() stand-in
 *                 (CP_HEALTH_EVENT_ALARM_PUBLISHED/BUFFERED) and the fire-
 *                 to-outcome latency is logged and checked against 500 ms.
 *                 Restart the broker (docker restart bringup-mosquitto)
 *                 while it runs: alarms must keep being handled (buffered)
 *                 within 500 ms WHILE CloudPublisherTask is ticking the
 *                 reconnect — the exact stall WIFITASK-O1 set out to fix.
 *   TC-HW-CP-012  wallclock-parity (TC-HW-WIFITASK-wallclock-parity):
 *                 wall-clock of the initial connect (TC-HW-CP-004) and of
 *                 each reconnect (first BUFFERED alarm -> reconnect_count
 *                 increment), logged against the pre-Phase-2 ~30 s
 *                 baseline. PASS = no regression; the point is to make a
 *                 regression visible, not to claim an improvement.
 *
 * If cpu_init(), debug_uart_init(), or rtc_init() fail, Logger cannot be
 * trusted as the reporting channel yet — the board halts with a fast LED
 * blink instead (same convention as main_test_mqtt_client.c).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "logger/logger.h"
#include "rtc/rtc.h"
#include "spi/spi.h"
#include "wifi_driver/wifi_driver.h"
#include "wifi_task/wifi_task.h"

#include "mqtt_client/mqtt_client.h"
#include "mqtt_client/mqtt_topic_config.h"

#include "cloud_publisher/cloud_publisher.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Fill these in before flashing — identical to main_test_mqtt_client.c.  */
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

/* bringup_certs.h (gitignored) is generated by
 * scripts/regenerate-bringup-certs.ps1 -BrokerIp <ip> —
 * it supplies BRINGUP_CERTS_BROKER_ENDPOINT and the three DER byte arrays.
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
#define BRINGUP_MQTT_CLIENT_ID "gw-cp-bringup-001"
#define BRINGUP_MQTT_KEEP_ALIVE_S (60U)

/* ---------------------------------------------------------------------- */
/* Board constants — identical wiring to main_test_mqtt_client.c.         */
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

#define CP_BRINGUP_TASK_STACK_WORDS (1024U)
#define CP_BRINGUP_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)

/* TC-HW-CP-011: one alarm in flight at a time — the period must exceed
 * the 500 ms REQ-NF-113 budget so an unhandled alarm shows up as a
 * violation, not as an overlap. */
#define CP_BRINGUP_ALARM_PERIOD_MS (700U)
#define CP_BRINGUP_ALARM_BUDGET_MS (500U) /**< REQ-NF-113. */
#define CP_BRINGUP_ALARM_SUMMARY_EVERY (10U)
/* TC-HW-CP-012: pre-Phase-2 blocking-path baseline (wifi-task.md §10). */
#define CP_BRINGUP_CONNECT_BASELINE_MS (30000UL)

/* ---------------------------------------------------------------------- */
/* Boot-failure halt — identical convention to main_test_mqtt_client.c.   */
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
    LOG_ERROR("CloudPub", "%s", label);
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

/* ---------------------------------------------------------------------- */
/* Bring-up stand-ins for the nine not-yet-implemented GW dependencies.   */
/*                                                                        */
/* Minimal, deliberately simple bodies — just enough for                 */
/* cloud_publisher_create() and its task loop to run. Replace each with a */
/* real call once that module's own GW implementation exists.            */
/* ---------------------------------------------------------------------- */

static uint32_t s_bringup_publish_count;

/* TC-HW-CP-011 — alarm subscriber captured from CloudPublisher, and the
 * fire/outcome bookkeeping shared between this task (fires) and
 * CloudPublisherTask (reports outcome via health_report_push_event()). */
static alarm_event_cb_t s_alarm_cb;
static void *s_alarm_ctx;
static volatile TickType_t s_alarm_fire_tick;
static volatile bool s_alarm_in_flight;
static uint32_t s_alarm_fired;
static uint32_t s_alarm_handled;
static uint32_t s_alarm_published;
static uint32_t s_alarm_buffered;
static uint32_t s_alarm_over_budget;
static uint32_t s_alarm_max_ms;

/* TC-HW-CP-012 — reconnect wall-clock: first BUFFERED outcome after a
 * PUBLISHED one marks the drop; reconnect_count moving marks recovery. */
static volatile TickType_t s_drop_tick;
static bool s_last_outcome_published = true;
static uint32_t s_last_reconnect_count;

/* TC-HW-CP-010 — data-integrity receiver state (see bringup_msg_cb()). */
static uint32_t s_di_expected_seq;
static uint32_t s_di_received;
static uint32_t s_di_corrupt;
static uint32_t s_di_out_of_order;
static uint32_t s_di_bytes;

sensor_reading_t sensor_service_get_latest(sensor_service_handle_t handle)
{
    (void) handle;
    sensor_reading_t reading = {
        .temperature_deci_c = 225,
        .humidity_pct = 45U,
        .pressure_hpa = 1013U,
        .time_synchronised = true,
    };
    return reading;
}

modbus_poller_fd_reading_t modbus_poller_get_latest_fd(modbus_poller_handle_t handle)
{
    (void) handle;
    modbus_poller_fd_reading_t fd = {0};
    fd.valid = false; /* no Field Device wired up for this bring-up */
    return fd;
}

void alarm_service_subscribe(alarm_service_handle_t handle, alarm_event_cb_t cb, void *ctx)
{
    (void) handle;
    s_alarm_cb = cb;
    s_alarm_ctx = ctx;
    LOG_INFO("CloudPub", "TC-HW-CP-006  alarm_service_subscribe() called (captured for TC-HW-CP-011)");
}

saf_err_t store_and_forward_enqueue(store_and_forward_handle_t handle, const char *topic,
                                    const uint8_t *buf, uint32_t len, mqtt_qos_t qos)
{
    (void) handle;
    (void) buf;
    (void) qos;
    LOG_WARN("CloudPub", "SAF stand-in: dropping %lu-byte message for %s (offline path)",
             (unsigned long) len, topic);
    return SAF_ERR_OK;
}

saf_err_t store_and_forward_dequeue(store_and_forward_handle_t handle, saf_entry_t *entry_out)
{
    (void) handle;
    (void) entry_out;
    return SAF_ERR_EMPTY; /* bring-up never buffers anything to drain */
}

saf_err_t store_and_forward_confirm(store_and_forward_handle_t handle)
{
    (void) handle;
    return SAF_ERR_OK;
}

cp_health_snapshot_t health_snapshot_get(health_monitor_handle_t handle)
{
    (void) handle;
    cp_health_snapshot_t snap = {0};
    snap.uptime_s = (uint32_t) (xTaskGetTickCount() / configTICK_RATE_HZ);
    snap.cloud_connected = true;
    return snap;
}

void health_report_update_mqtt(health_monitor_handle_t handle, const mqtt_stats_t *stats,
                               const mqtt_stats_t *last_stats)
{
    (void) handle;
    (void) last_stats;
    LOG_INFO("CloudPub", "TC-HW-CP-009  stats poll: publishes_sent=%lu publish_failures=%lu",
             (unsigned long) stats->publishes_sent, (unsigned long) stats->publish_failures);

    /* TC-HW-CP-012: a reconnect completed since the last poll. */
    if (stats->reconnect_count != s_last_reconnect_count)
    {
        s_last_reconnect_count = stats->reconnect_count;
        const uint32_t ms = (uint32_t) ((xTaskGetTickCount() - s_drop_tick) * portTICK_PERIOD_MS);
        LOG_INFO("CloudPub",
                 "TC-HW-CP-012  reconnect #%lu observed ~%lu ms drop->reconnected "
                 "(informational: dominated by broker-down dwell + reconnect backoff "
                 "WIFITASK-O2, not a clean handshake wall-clock — the initial connect "
                 "above is the parity point)",
                 (unsigned long) stats->reconnect_count, (unsigned long) ms);
    }
}

void health_report_push_event(health_monitor_handle_t handle, cp_health_event_t event)
{
    (void) handle;
    if ((event != CP_HEALTH_EVENT_ALARM_PUBLISHED) && (event != CP_HEALTH_EVENT_ALARM_BUFFERED))
    {
        LOG_WARN("CloudPub", "health event %d", (int) event);
        return;
    }

    /* TC-HW-CP-011: runs in CloudPublisherTask context, right after
     * prv_enqueue_or_publish() returned for the alarm this task fired. */
    const TickType_t now = xTaskGetTickCount();
    const uint32_t ms = (uint32_t) ((now - s_alarm_fire_tick) * portTICK_PERIOD_MS);
    const bool published = (event == CP_HEALTH_EVENT_ALARM_PUBLISHED);
    s_alarm_in_flight = false;
    s_alarm_handled++;
    if (published)
    {
        s_alarm_published++;
    }
    else
    {
        s_alarm_buffered++;
        if (s_last_outcome_published)
        {
            s_drop_tick = now; /* TC-HW-CP-012: connection just went away */
        }
    }
    s_last_outcome_published = published;
    if (ms > s_alarm_max_ms)
    {
        s_alarm_max_ms = ms;
    }
    if (ms > CP_BRINGUP_ALARM_BUDGET_MS)
    {
        s_alarm_over_budget++;
        LOG_ERROR("CloudPub", "TC-HW-CP-011  alarm #%lu %s after %lu ms — OVER %u ms budget",
                  (unsigned long) s_alarm_handled, published ? "published" : "buffered",
                  (unsigned long) ms, (unsigned) CP_BRINGUP_ALARM_BUDGET_MS);
    }
    else
    {
        LOG_INFO("CloudPub", "TC-HW-CP-011  alarm #%lu %s in %lu ms", (unsigned long) s_alarm_handled,
                 published ? "published" : "buffered", (unsigned long) ms);
    }
    if ((s_alarm_handled % CP_BRINGUP_ALARM_SUMMARY_EVERY) == 0u)
    {
        LOG_INFO("CloudPub",
                 "TC-HW-CP-011  summary: fired=%lu handled=%lu published=%lu buffered=%lu "
                 "max=%lu ms over_budget=%lu -> %s",
                 (unsigned long) s_alarm_fired, (unsigned long) s_alarm_handled,
                 (unsigned long) s_alarm_published, (unsigned long) s_alarm_buffered,
                 (unsigned long) s_alarm_max_ms, (unsigned long) s_alarm_over_budget,
                 (s_alarm_over_budget == 0u) ? "PASS" : "FAIL");
    }
}

uint32_t config_provider_get_telemetry_interval_s(config_service_handle_t handle)
{
    (void) handle;
    return 10U; /* shortened from the 60 s default for faster bring-up observation */
}

uint32_t config_provider_get_health_interval_s(config_service_handle_t handle)
{
    (void) handle;
    return 20U; /* shortened from the 600 s default for faster bring-up observation */
}

bool config_manager_apply(config_service_handle_t handle, const uint8_t *payload, uint32_t len)
{
    (void) handle;
    (void) payload;
    (void) len;
    return true;
}

void update_service_handle_command(update_service_handle_t handle, const uint8_t *payload,
                                   uint32_t len)
{
    (void) handle;
    (void) payload;
    (void) len;
}

void lifecycle_handle_remote(lifecycle_handle_t handle, const uint8_t *payload, uint32_t len)
{
    (void) handle;
    (void) payload;
    (void) len;
}

/* ---------------------------------------------------------------------- */
/* MqttClient callbacks — required non-NULL by mqtt_client_create() (see  */
/* MQTT_CLIENT_ERR_NULL_PTR in mqtt_client.h). CP-O5: CloudPublisher does  */
/* not yet own mqtt_client_create() in production, so these are bring-up */
/* stand-ins only — logging, no real routing into CloudPublisher.        */
/* ---------------------------------------------------------------------- */

/* TC-HW-CP-010 payload format (scripts/bringup-data-integrity.py):
 *   "DI:" seq(4 digits) ":" total(4) ":" len(5) ":"   = 19-byte header
 *   followed by exactly `len` bytes where byte i == 'A' + (i % 26).
 * Anything not starting with "DI:" is just logged as before. */
#define DI_HEADER_LEN (19u)

static void bringup_msg_cb(const char *topic, uint16_t topic_len, const uint8_t *payload,
                           uint32_t payload_len)
{
    if ((payload_len < DI_HEADER_LEN) || (memcmp(payload, "DI:", 3u) != 0))
    {
        LOG_INFO("CloudPub", "inbound msg: topic='%.*s' payload_len=%u", (int) topic_len, topic,
                 (unsigned) payload_len);
        return;
    }

    char hdr[DI_HEADER_LEN + 1u];
    (void) memcpy(hdr, payload, DI_HEADER_LEN);
    hdr[DI_HEADER_LEN] = '\0';
    unsigned seq = 0u;
    unsigned total = 0u;
    unsigned len = 0u;
    if (sscanf(hdr, "DI:%4u:%4u:%5u:", &seq, &total, &len) != 3)
    {
        s_di_corrupt++;
        LOG_ERROR("CloudPub", "TC-HW-CP-010  unparseable header '%s'", hdr);
        return;
    }

    bool ok = (payload_len == (DI_HEADER_LEN + len));
    uint32_t first_bad = 0u;
    for (uint32_t i = 0u; ok && (i < len); i++)
    {
        if (payload[DI_HEADER_LEN + i] != (uint8_t) ('A' + (i % 26u)))
        {
            ok = false;
            first_bad = i;
        }
    }
    s_di_received++;
    s_di_bytes += payload_len;
    if (!ok)
    {
        s_di_corrupt++;
    }
    if (seq != s_di_expected_seq)
    {
        s_di_out_of_order++;
    }
    s_di_expected_seq = seq + 1u;

    if (ok)
    {
        LOG_INFO("CloudPub", "TC-HW-CP-010  seq=%u/%u len=%u OK", seq, total, (unsigned) payload_len);
    }
    else
    {
        LOG_ERROR("CloudPub", "TC-HW-CP-010  seq=%u/%u len=%u CORRUPT (got %u bytes, first bad @%lu)",
                  seq, total, (unsigned) (DI_HEADER_LEN + len), (unsigned) payload_len,
                  (unsigned long) first_bad);
    }
    if ((seq + 1u) == total)
    {
        const bool pass = (s_di_received == total) && (s_di_corrupt == 0u) &&
                          (s_di_out_of_order == 0u);
        LOG_INFO("CloudPub",
                 "TC-HW-CP-010  summary: received=%lu/%u bytes=%lu corrupt=%lu out_of_order=%lu -> %s",
                 (unsigned long) s_di_received, total, (unsigned long) s_di_bytes,
                 (unsigned long) s_di_corrupt, (unsigned long) s_di_out_of_order,
                 pass ? "PASS" : "FAIL");
        s_di_expected_seq = 0u;
        s_di_received = 0u;
        s_di_corrupt = 0u;
        s_di_out_of_order = 0u;
        s_di_bytes = 0u;
    }
}

static void bringup_disconnect_cb(void)
{
    LOG_WARN("CloudPub", "MqttClient disconnect_cb fired");
}

/* ---------------------------------------------------------------------- */
/* CloudPublisher bring-up task — Phase 2 (post-scheduler).               */
/* ---------------------------------------------------------------------- */

static StaticTask_t s_cp_bringup_task_tcb;
static StackType_t s_cp_bringup_task_stack[CP_BRINGUP_TASK_STACK_WORDS];

static void cloud_publisher_bringup_task(void *arg)
{
    wifi_handle_t wifi_handle = (wifi_handle_t) arg;

    /* WIFITASK-O3: WifiTask is now the sole caller of WifiDriver (D29).
     * wifitask_create() must run in task context, post-scheduler — it
     * registers the real DATARDY callback itself. */
    wifitask_config_t wifitask_config = {.wifi = wifi_handle};
    wifitask_handle_t wifitask_handle = NULL;
    if (wifitask_create(&wifitask_config, &wifitask_handle) != WIFITASK_ERR_OK)
    {
        bringup_fail("TC-HW-CP-002  wifitask_create() failed");
    }
    LOG_INFO("CloudPub", "wifitask_create() returned WIFITASK_ERR_OK (WifiTask's own task "
                        "is now running)");

    /* TC-HW-CP-002 */
    if (sizeof(BRINGUP_WIFI_SSID) <= 1U)
    {
        bringup_fail("BRINGUP_WIFI_SSID is empty - cannot continue without an AP");
    }
    wifitask_err_t connect_ap_err =
        wifitask_connect_ap(wifitask_handle, BRINGUP_WIFI_SSID, BRINGUP_WIFI_PASSWORD);
    if (connect_ap_err != WIFITASK_ERR_OK)
    {
        LOG_ERROR("CloudPub", "TC-HW-CP-002  wifitask_connect_ap() failed, wifitask_err_t=%d",
                  (int) connect_ap_err);
        bringup_fail("TC-HW-CP-002  wifitask_connect_ap() failed");
    }
    LOG_INFO("CloudPub", "TC-HW-CP-002  associated with %s", BRINGUP_WIFI_SSID);
    vTaskDelay(pdMS_TO_TICKS(200));

    if (sizeof(BRINGUP_MQTT_BROKER_ENDPOINT) <= 1U)
    {
        bringup_fail("BRINGUP_MQTT_BROKER_ENDPOINT is empty - CloudPublisher needs a real MQTT "
                     "connection for this bring-up");
    }

    /* TC-HW-CP-003: mqtt_client_create() (unconnected). */
    mqtt_client_config_t mqtt_config = {
        .wifi = wifitask_handle,
        .msg_cb = bringup_msg_cb,
        .disconnect_cb = bringup_disconnect_cb,
    };
    mqtt_client_handle_t mqtt_handle = NULL;
    if (mqtt_client_create(&mqtt_config, &mqtt_handle) != MQTT_CLIENT_ERR_OK)
    {
        bringup_fail("TC-HW-CP-003  mqtt_client_create() failed");
    }
    LOG_INFO("CloudPub", "TC-HW-CP-003  mqtt_client_create() returned MQTT_CLIENT_ERR_OK");

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

    /* TC-HW-CP-004: connect synchronously, single-task, before
     * CloudPublisherTask exists — see header comment for why (MqttClient
     * is single-task-caller only). Internally still exercises
     * mqtt_client_connect_step() (MQTT-D8): mqtt_client_connect() is just
     * a wrapper looping it to completion now. */
    const TickType_t connect_t0 = xTaskGetTickCount();
    if (mqtt_client_connect(mqtt_handle, &connect_cfg) != MQTT_CLIENT_ERR_OK)
    {
        bringup_fail("TC-HW-CP-004  mqtt_client_connect() failed");
    }
    LOG_INFO("CloudPub", "TC-HW-CP-004  mqtt_client_connect() returned MQTT_CLIENT_ERR_OK");
    {
        const uint32_t ms = (uint32_t) ((xTaskGetTickCount() - connect_t0) * portTICK_PERIOD_MS);
        LOG_INFO("CloudPub",
                 "TC-HW-CP-012  initial TCP+TLS+MQTT connect wall-clock %lu ms (baseline %lu ms) %s",
                 (unsigned long) ms, (unsigned long) CP_BRINGUP_CONNECT_BASELINE_MS,
                 (ms <= CP_BRINGUP_CONNECT_BASELINE_MS) ? "PASS" : "FAIL (regression)");
    }

    /* TC-HW-CP-005: subscribe while still single-task. */
    char config_topic[MQTT_TOPIC_MAX_LEN];
    (void) snprintf(config_topic, sizeof(config_topic), "%s%s%s", MQTT_TOPIC_PREFIX_SUBSCRIBE,
                    BRINGUP_MQTT_CLIENT_ID, MQTT_TOPIC_SUFFIX_CONFIG);
    if (mqtt_client_subscribe(mqtt_handle, config_topic, MQTT_QOS_1) != MQTT_CLIENT_ERR_OK)
    {
        bringup_fail("TC-HW-CP-005  mqtt_client_subscribe() failed (no SUBACK or rejected)");
    }
    LOG_INFO("CloudPub", "TC-HW-CP-005  subscribed to %s, SUBACK received", config_topic);

    /* TC-HW-CP-006 */
    cloud_publisher_config_t cp_config = {
        .mqtt = mqtt_handle,
        .sensors = (sensor_service_handle_t) 1,
        .alarms = (alarm_service_handle_t) 1,
        .poller = (modbus_poller_handle_t) 1,
        .saf = (store_and_forward_handle_t) 1,
        .health_read = (health_monitor_handle_t) 1,
        .health_write = (health_monitor_handle_t) 1,
        .cfg_read = (config_service_handle_t) 1,
        .cfg_write = (config_service_handle_t) 1,
        .update_svc = NULL, /* CP-O1: legitimately NULL until UpdateService LLD */
        .lifecycle = (lifecycle_handle_t) 1,
        .mqtt_connect_cfg = connect_cfg,
    };
    cloud_publisher_handle_t cp_handle = NULL;
    if (cloud_publisher_create(&cp_config, &cp_handle) != CP_ERR_OK)
    {
        bringup_fail("TC-HW-CP-006  cloud_publisher_create() failed");
    }
    LOG_INFO("CloudPub", "TC-HW-CP-006  cloud_publisher_create() returned CP_ERR_OK");

    /* TC-HW-CP-007/008/009: the telemetry (10 s), health (20 s) and stats
     * (1 Hz) timers now drive CloudPublisherTask on their own. The handle
     * is already connected+subscribed (TC-HW-CP-004/005 above), so the
     * first stats tick's prv_maybe_reconnect() is a no-op (CP-D9's
     * is_connected() early-return) — CP-D10's ticked reconnect only comes
     * into play if the broker gets dropped and restarted later. Observe
     * the Logger UART output for the publish/stats log lines above, and
     * publish to the config_topic logged at TC-HW-CP-005 to see
     * bringup_msg_cb() fire (e.g. via mosquitto_pub). */
    LOG_INFO("CloudPub", "Observing telemetry/health/stats ticks — watch the UART log...");
    if (s_alarm_cb == NULL)
    {
        bringup_fail("TC-HW-CP-011  CloudPublisher never subscribed to AlarmService");
    }
    LOG_INFO("CloudPub",
             "TC-HW-CP-011  firing one alarm every %u ms, budget %u ms; restart the broker to "
             "exercise the in-flight-reconnect case",
             (unsigned) CP_BRINGUP_ALARM_PERIOD_MS, (unsigned) CP_BRINGUP_ALARM_BUDGET_MS);
    for (;;)
    {
        (void) gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        vTaskDelay(pdMS_TO_TICKS(CP_BRINGUP_ALARM_PERIOD_MS));
        s_bringup_publish_count++;

        if (s_alarm_in_flight)
        {
            /* The previous alarm has not come back through
             * health_report_push_event() a full period later — that is a
             * budget violation in its own right (CloudPublisherTask is
             * stuck somewhere), counted here since the outcome path will
             * never report it on time. */
            s_alarm_over_budget++;
            LOG_ERROR("CloudPub", "TC-HW-CP-011  alarm #%lu still unhandled after %u ms — OVER budget",
                      (unsigned long) s_alarm_fired, (unsigned) CP_BRINGUP_ALARM_PERIOD_MS);
        }
        alarm_event_t ev = {
            .alarm_type = CP_ALARM_TYPE_HIGH,
            .measured_value_raw = (int32_t) s_alarm_fired,
            .threshold_value_raw = (int32_t) CP_BRINGUP_ALARM_BUDGET_MS,
            .source = CP_ALARM_SOURCE_GATEWAY,
        };
        (void) snprintf(ev.sensor_name, sizeof(ev.sensor_name), "bringup");
        s_alarm_fired++;
        s_alarm_in_flight = true;
        s_alarm_fire_tick = xTaskGetTickCount();
        s_alarm_cb(&ev, s_alarm_ctx); /* production: SensorTask context; here: this task */
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

    LOG_INFO("CloudPub", "===== CloudPublisher Hardware Bring-up =====");
    LOG_INFO("CloudPub", "Board : B-L475E-IOT01A (STM32L475VGTx)");
    LOG_INFO("CloudPub", "SYSCLK=%lu Hz", (unsigned long) cpu_get_sysclk_hz());

    /* TC-HW-CP-001: WifiDriver bring-up — identical wiring to
     * main_test_mqtt_client.c. */
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
            bringup_fail("TC-HW-CP-001  SPI3 pin configuration failed");
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
        bringup_fail("TC-HW-CP-001  ISM43362 control-line configuration failed");
    }

    spi_config_t spi_config = {.instance = SPI3};
    spi_handle_t spi_handle = NULL;
    if (spi_create(&spi_config, &spi_handle) != SPI_ERR_OK)
    {
        bringup_fail("TC-HW-CP-001  spi_create() failed");
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
    if (wifi_create(&wifi_config, &wifi_handle) != WIFI_ERR_OK)
    {
        bringup_fail("TC-HW-CP-001  wifi_create() failed");
    }
    /* wifi_attach_datardy_callback() is no longer called directly here —
     * WIFITASK-O3: routed through WifiTask, which registers the real
     * DATARDY callback itself inside wifitask_create() (task-context
     * only, so it happens in cloud_publisher_bringup_task below). */
    LOG_INFO("CloudPub", "TC-HW-CP-001  WifiDriver bring-up complete");
    LOG_INFO("CloudPub", "starting scheduler...");

    (void) xTaskCreateStatic(cloud_publisher_bringup_task, "cp_bringup",
                             CP_BRINGUP_TASK_STACK_WORDS, wifi_handle, CP_BRINGUP_TASK_PRIORITY,
                             s_cp_bringup_task_stack, &s_cp_bringup_task_tcb);

    vTaskStartScheduler();

    for (;;)
    {
    }
}
