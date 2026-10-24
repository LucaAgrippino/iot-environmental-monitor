/**
 * @file main_test_config_service.c
 * @brief Hardware bring-up test for ConfigService (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. Add the current harness (cloud_publisher) to the `excluding=` list of the
 *      `integration-tests` source entry and remove config_service from it.
 *   2. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/config_service.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * DESTRUCTIVE.  Uses the real ConfigStore partition, QSPI device offsets 0x0000_0000 -
 * 0x0000_FFFF.  Anything stored there is erased at the start and again at the end.
 *
 * Real QspiFlashDriver, real ConfigStore, real HealthMonitor.  No scheduler is started:
 * the mutex is taken uncontended, so the whole sequence runs in main().
 *
 * TWO-PHASE TEST (persistence is proven across a real software reset):
 *
 *   Phase A runs when config_store_check_integrity() finds no valid slot.  It starts from
 *   a factory erase, exercises the API against real flash, leaves a known configuration
 *   behind and resets the MCU itself.
 *
 *   Phase B runs when a valid slot exists.  It loads the blob through ConfigStore, applies it
 *   with config_service_apply_loaded() (what LifecycleController will do) and checks every
 *   value Phase A left, then erases the partition so the next run starts at Phase A.
 *
 * Automated test sequence:
 *   Phase A
 *   TC-HW-CFG-001  qspi_flash_init, HealthMonitor, ConfigStore and ConfigService created;
 *                  defaults active and nothing written to flash yet.
 *   TC-HW-CFG-002  A valid set_param is applied and appears in the stored blob.
 *   TC-HW-CFG-003  An invalid set_param is rejected and the stored blob is unchanged.
 *   TC-HW-CFG-004  snapshot, change, restore_snapshot: the old value is back and persisted.
 *   TC-HW-CFG-005  The change callback fires once per successful write and not on a reject.
 *   TC-HW-CFG-006  String parameters (MQTT broker, NTP servers and count) round-trip.
 *   TC-HW-CFG-007  set_param duration including the flash write (printed, informational).
 *   TC-HW-CFG-008  The persistence configuration is flushed and a software reset follows; the
 *                  sentinel filter alpha goes through the deferred path (TC-HW-CFG-011).
 *   TC-HW-CFG-011  set_param_deferred returns without a flash write (timed), the change is
 *                  pending until persist_pending writes it (timed), then nothing is pending.
 *   Phase B (after the reset)
 *   TC-HW-CFG-009  Every value Phase A left is active after load + apply_loaded.
 *   TC-HW-CFG-010  Factory erase clears the partition; ConfigStore then reports NO_VALID_SLOT.
 *
 * Integration checklist:
 *   [ ] set_param and persist_pending durations noted; set_param_deferred is microseconds
 *   [ ] Phase B reports the Phase A values after the reset
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "config_service/config_service.h"
#include "config_store/config_store.h"
#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "health_monitor/health_monitor.h"
#include "led/led_driver.h"
#include "qspi_flash/qspi_flash.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Board constants                                                         */
/* ---------------------------------------------------------------------- */

#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)
#define BRINGUP_TX_TIMEOUT_MS (100U)

#define US_PER_S (1000000UL)
#define US_PER_MS (1000UL)

/* ---------------------------------------------------------------------- */
/* Test data                                                               */
/* ---------------------------------------------------------------------- */

#define SENTINEL_POLL_MS (777U)
#define SENTINEL_ALPHA (321U)
#define SENTINEL_TEMP_HIGH (3300)
#define SENTINEL_BROKER "mqtt.bringup.example"
#define SENTINEL_NTP_0 "162.159.200.1"
#define SENTINEL_NTP_1 "216.239.35.0"
#define SENTINEL_NTP_COUNT (2U)

#define SETTLE_BEFORE_RESET_LOOPS (400000U)

static const led_pin_t k_gw_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = (uint8_t) GPIO_PORT_B, .pin = 14U, .active_high = true, .fitted = true},
    [LED_RED] = {.port = (uint8_t) GPIO_PORT_A, .pin = 5U, .active_high = true, .fitted = false},
};

static uint32_t s_cb_calls;
static config_param_id_t s_cb_last;

/* ---------------------------------------------------------------------- */
/* Reporting helpers — built on DebugUartDriver.                           */
/* ---------------------------------------------------------------------- */

static void bringup_puts(const char *s)
{
    (void) debug_uart_send((const uint8_t *) s, strlen(s), BRINGUP_TX_TIMEOUT_MS);
}

static void bringup_pass(const char *label)
{
    bringup_puts("[PASS] ");
    bringup_puts(label);
    bringup_puts("\r\n");
}

static void bringup_info(const char *label)
{
    bringup_puts("[INFO] ");
    bringup_puts(label);
    bringup_puts("\r\n");
}

static void bringup_fail(const char *label)
{
    bringup_puts("[FAIL] ");
    bringup_puts(label);
    bringup_puts(" - HALTED\r\n");

    for (;;)
    {
    }
}

static void check(bool condition, const char *label)
{
    if (!condition)
    {
        bringup_fail(label);
    }
}

static void expect_cfg(config_service_err_t got, config_service_err_t expected, const char *label)
{
    if (got != expected)
    {
        char msg[96];

        (void) snprintf(msg, sizeof(msg), "%s (err=%d, expected=%d)", label, (int) got,
                        (int) expected);
        bringup_fail(msg);
    }
}

static uint32_t dwt_elapsed_us(uint32_t start)
{
    const uint32_t cycles_per_us = cpu_get_sysclk_hz() / US_PER_S;

    return (DWT->CYCCNT - start) / cycles_per_us;
}

static void on_change(config_param_id_t id)
{
    s_cb_calls++;
    s_cb_last = id;
}

/* Read the blob back from flash through ConfigStore, as the next boot would. */
static config_blob_t stored_blob(config_store_handle_t cs, const char *label)
{
    config_blob_t blob;
    uint32_t len = 0U;

    (void) memset(&blob, 0, sizeof(blob));
    check(config_store_load(cs, &blob, &len, (uint32_t) sizeof(blob)) == CONFIG_STORE_ERR_OK,
          label);
    check(len == (uint32_t) sizeof(blob), label);
    return blob;
}

/* ---------------------------------------------------------------------- */
/* Phase A                                                                 */
/* ---------------------------------------------------------------------- */

static void phase_a(config_store_handle_t cs)
{
    bringup_info("Phase A: factory erase, then exercise ConfigService on real flash");
    check(config_store_erase(cs) == CONFIG_STORE_ERR_OK, "TC-HW-CFG-001 factory erase");

    /* TC-HW-CFG-001 */
    expect_cfg(config_service_init(cs), CONFIG_SERVICE_OK, "TC-HW-CFG-001 config_service_init");
    const config_params_t *p = config_service_get_params();

    check(p != NULL, "TC-HW-CFG-001 get_params");
    check(p->polling_interval_ms == 1000U, "TC-HW-CFG-001 default polling interval");
    check(p->temp_alarm_high == 4000, "TC-HW-CFG-001 default alarm high");
    check(p->mqtt_port == 8883U, "TC-HW-CFG-001 default MQTT port");
    check(config_store_check_integrity(cs) == CONFIG_STORE_ERR_NO_VALID_SLOT,
          "TC-HW-CFG-001 init must not write flash");
    bringup_pass("TC-HW-CFG-001 services created, defaults active, flash untouched");

    expect_cfg(config_service_register_change_cb(on_change), CONFIG_SERVICE_OK,
               "TC-HW-CFG-005 register callback");

    /* TC-HW-CFG-002 */
    uint32_t poll = 500U;
    s_cb_calls = 0U;
    expect_cfg(config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &poll), CONFIG_SERVICE_OK,
               "TC-HW-CFG-002 set_param");
    check(p->polling_interval_ms == 500U, "TC-HW-CFG-002 value applied");
    config_blob_t blob = stored_blob(cs, "TC-HW-CFG-002 load after set");
    check(blob.schema_version == CONFIG_SCHEMA_VERSION, "TC-HW-CFG-002 schema version");
    check(blob.params.polling_interval_ms == 500U, "TC-HW-CFG-002 value persisted");
    bringup_pass("TC-HW-CFG-002 valid change applied and found in the stored blob");

    /* TC-HW-CFG-003 */
    uint32_t bad = 50U;
    expect_cfg(config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &bad),
               CONFIG_SERVICE_ERR_INVALID, "TC-HW-CFG-003 reject");
    check(p->polling_interval_ms == 500U, "TC-HW-CFG-003 value unchanged");
    blob = stored_blob(cs, "TC-HW-CFG-003 load after reject");
    check(blob.params.polling_interval_ms == 500U, "TC-HW-CFG-003 stored value unchanged");
    bringup_pass("TC-HW-CFG-003 invalid change rejected, memory and flash unchanged");

    /* TC-HW-CFG-004 */
    expect_cfg(config_service_snapshot(), CONFIG_SERVICE_OK, "TC-HW-CFG-004 snapshot");
    poll = 900U;
    expect_cfg(config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &poll), CONFIG_SERVICE_OK,
               "TC-HW-CFG-004 change after snapshot");
    expect_cfg(config_service_restore_snapshot(), CONFIG_SERVICE_OK, "TC-HW-CFG-004 restore");
    check(p->polling_interval_ms == 500U, "TC-HW-CFG-004 old value back");
    blob = stored_blob(cs, "TC-HW-CFG-004 load after restore");
    check(blob.params.polling_interval_ms == 500U, "TC-HW-CFG-004 restored value persisted");
    bringup_pass("TC-HW-CFG-004 snapshot / restore rolled the value back and persisted it");

    /* TC-HW-CFG-005: two successful writes (002, 004 change) are already counted; count anew */
    s_cb_calls = 0U;
    poll = 600U;
    expect_cfg(config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &poll), CONFIG_SERVICE_OK,
               "TC-HW-CFG-005 set");
    expect_cfg(config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &bad),
               CONFIG_SERVICE_ERR_INVALID, "TC-HW-CFG-005 reject");
    check(s_cb_calls == 1U, "TC-HW-CFG-005 callback count");
    check(s_cb_last == CONFIG_PARAM_POLL_INTERVAL, "TC-HW-CFG-005 callback id");
    bringup_pass("TC-HW-CFG-005 callback fired once for the write and not for the reject");

    /* TC-HW-CFG-006 */
    expect_cfg(config_service_set_param(CONFIG_PARAM_MQTT_BROKER, SENTINEL_BROKER),
               CONFIG_SERVICE_OK, "TC-HW-CFG-006 broker");
    char servers[CONFIG_MAX_NTP_SERVERS][CONFIG_MAX_NTP_HOST_LEN];
    (void) memset(servers, 0, sizeof(servers));
    (void) strcpy(servers[0], SENTINEL_NTP_0);
    (void) strcpy(servers[1], SENTINEL_NTP_1);
    expect_cfg(config_service_set_param(CONFIG_PARAM_NTP_SERVERS, servers), CONFIG_SERVICE_OK,
               "TC-HW-CFG-006 ntp servers");
    uint8_t count = SENTINEL_NTP_COUNT;
    expect_cfg(config_service_set_param(CONFIG_PARAM_NTP_SERVER_COUNT, &count), CONFIG_SERVICE_OK,
               "TC-HW-CFG-006 ntp count");
    blob = stored_blob(cs, "TC-HW-CFG-006 load strings");
    check(strcmp(blob.params.mqtt_broker, SENTINEL_BROKER) == 0, "TC-HW-CFG-006 broker stored");
    check(strcmp(blob.params.ntp_servers[1], SENTINEL_NTP_1) == 0, "TC-HW-CFG-006 ntp stored");
    check(blob.params.ntp_server_count == SENTINEL_NTP_COUNT, "TC-HW-CFG-006 count stored");
    bringup_pass("TC-HW-CFG-006 string parameters round-trip through flash");

    /* TC-HW-CFG-007 */
    {
        char msg[96];
        poll = 700U;
        const uint32_t start = DWT->CYCCNT;

        expect_cfg(config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &poll), CONFIG_SERVICE_OK,
                   "TC-HW-CFG-007 timed set");
        (void) snprintf(msg, sizeof(msg), "TC-HW-CFG-007 set_param incl. flash write %lu ms",
                        (unsigned long) (dwt_elapsed_us(start) / US_PER_MS));
        bringup_info(msg);
    }
    bringup_pass("TC-HW-CFG-007 set_param duration recorded");

    /* TC-HW-CFG-011 */
    {
        char msg[96];
        poll = 650U;
        uint32_t start = DWT->CYCCNT;

        expect_cfg(config_service_set_param_deferred(CONFIG_PARAM_POLL_INTERVAL, &poll),
                   CONFIG_SERVICE_OK, "TC-HW-CFG-011 deferred set");
        const uint32_t deferred_us = dwt_elapsed_us(start);

        (void) snprintf(msg, sizeof(msg), "TC-HW-CFG-011 set_param_deferred %lu us",
                        (unsigned long) deferred_us);
        bringup_info(msg);
        check(deferred_us < (US_PER_MS * 10UL), "TC-HW-CFG-011 deferred set is not instant");
        check(p->polling_interval_ms == 650U, "TC-HW-CFG-011 value live at once");
        check(config_service_has_pending(), "TC-HW-CFG-011 change should be pending");
        blob = stored_blob(cs, "TC-HW-CFG-011 load before persist");
        check(blob.params.polling_interval_ms == 700U,
              "TC-HW-CFG-011 flash must not have changed yet");

        start = DWT->CYCCNT;
        expect_cfg(config_service_persist_pending(), CONFIG_SERVICE_OK,
                   "TC-HW-CFG-011 persist_pending");
        (void) snprintf(msg, sizeof(msg), "TC-HW-CFG-011 persist_pending %lu ms",
                        (unsigned long) (dwt_elapsed_us(start) / US_PER_MS));
        bringup_info(msg);
        check(!config_service_has_pending(), "TC-HW-CFG-011 nothing should be pending");
        blob = stored_blob(cs, "TC-HW-CFG-011 load after persist");
        check(blob.params.polling_interval_ms == 650U, "TC-HW-CFG-011 value persisted");
    }
    bringup_pass("TC-HW-CFG-011 deferred set is immediate, persist_pending writes it once");

    /* TC-HW-CFG-008: leave the sentinel configuration behind and reset */
    poll = SENTINEL_POLL_MS;
    expect_cfg(config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &poll), CONFIG_SERVICE_OK,
               "TC-HW-CFG-008 poll");
    int16_t high = SENTINEL_TEMP_HIGH;
    expect_cfg(config_service_set_param(CONFIG_PARAM_TEMP_ALARM_HIGH, &high), CONFIG_SERVICE_OK,
               "TC-HW-CFG-008 temperature high");
    uint16_t alpha = SENTINEL_ALPHA;
    expect_cfg(config_service_set_param_deferred(CONFIG_PARAM_FILTER_ALPHA, &alpha),
               CONFIG_SERVICE_OK, "TC-HW-CFG-008 alpha");
    check(config_service_has_pending(), "TC-HW-CFG-008 alpha should be pending");
    expect_cfg(config_service_persist_pending(), CONFIG_SERVICE_OK,
               "TC-HW-CFG-008 persist_pending");
    bringup_pass("TC-HW-CFG-008 sentinel configuration flushed; resetting the MCU");

    for (volatile uint32_t i = 0U; i < SETTLE_BEFORE_RESET_LOOPS; i++)
    {
    }
    NVIC_SystemReset();
}

/* ---------------------------------------------------------------------- */
/* Phase B                                                                 */
/* ---------------------------------------------------------------------- */

static void phase_b(config_store_handle_t cs)
{
    bringup_info("Phase B: load + apply_loaded after the reset");
    expect_cfg(config_service_init(cs), CONFIG_SERVICE_OK, "TC-HW-CFG-009 init");

    config_blob_t blob;
    uint32_t len = 0U;

    (void) memset(&blob, 0, sizeof(blob));
    const config_store_err_t load_rc = config_store_load(cs, &blob, &len, (uint32_t) sizeof(blob));

    /* A blob that is not ours (another size, another schema) is stale data from an earlier
     * run: erase it and reset so the next boot starts at Phase A. A blob that is ours but
     * holds wrong values is a real failure and is caught below. */
    if ((load_rc != CONFIG_STORE_ERR_OK) || (len != (uint32_t) sizeof(blob)) ||
        (blob.schema_version != CONFIG_SCHEMA_VERSION))
    {
        bringup_info("stale data from another run: erasing the partition and resetting");
        check(config_store_erase(cs) == CONFIG_STORE_ERR_OK, "stale erase");
        NVIC_SystemReset();
    }
    expect_cfg(config_service_apply_loaded(&blob, len), CONFIG_SERVICE_OK,
               "TC-HW-CFG-009 apply_loaded");

    /* TC-HW-CFG-009 */
    const config_params_t *p = config_service_get_params();

    check(p->polling_interval_ms == SENTINEL_POLL_MS, "TC-HW-CFG-009 polling interval kept");
    check(p->filter_alpha == SENTINEL_ALPHA, "TC-HW-CFG-009 alpha kept");
    check(p->temp_alarm_high == SENTINEL_TEMP_HIGH, "TC-HW-CFG-009 alarm high kept");
    check(strcmp(p->mqtt_broker, SENTINEL_BROKER) == 0, "TC-HW-CFG-009 broker kept");
    check(strcmp(p->ntp_servers[0], SENTINEL_NTP_0) == 0, "TC-HW-CFG-009 ntp[0] kept");
    check(strcmp(p->ntp_servers[1], SENTINEL_NTP_1) == 0, "TC-HW-CFG-009 ntp[1] kept");
    check(p->ntp_server_count == SENTINEL_NTP_COUNT, "TC-HW-CFG-009 ntp count kept");
    check(p->mqtt_port == 8883U, "TC-HW-CFG-009 untouched default kept");
    bringup_pass("TC-HW-CFG-009 every value Phase A left is active after the reset");

    /* TC-HW-CFG-010 */
    check(config_store_erase(cs) == CONFIG_STORE_ERR_OK, "TC-HW-CFG-010 erase");
    check(config_store_check_integrity(cs) == CONFIG_STORE_ERR_NO_VALID_SLOT,
          "TC-HW-CFG-010 partition must be blank");
    bringup_pass(
        "TC-HW-CFG-010 factory erase left the partition blank (next run starts at Phase A)");

    bringup_puts("\r\n===== ALL TESTS PASSED =====\r\n");
}

/* ---------------------------------------------------------------------- */
/* main                                                                    */
/* ---------------------------------------------------------------------- */

int main(void)
{
    /* cpu_init() first - clock to 80 MHz, DWT cycle counter, fault handlers. */
    if (cpu_init() != STATUS_OK)
    {
        for (;;)
        {
            /* Nothing can be reported before the UART exists. */
        }
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

    if ((gpio_init() != GPIO_OK) || (gpio_configure_pin(&uart_tx_config) != GPIO_OK) ||
        (gpio_configure_pin(&uart_rx_config) != GPIO_OK) || (debug_uart_init() != DEBUG_UART_OK))
    {
        for (;;)
        {
            /* No sink to report to. */
        }
    }

    bringup_puts("\r\n===== ConfigService Hardware Bring-up =====\r\n");
    bringup_puts("Board  : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("WARNING: erases the ConfigStore partition (0x00000000, 64 KB)\r\n");
    bringup_puts("===========================================\r\n\r\n");

    if (qspi_flash_init() != QSPI_FLASH_OK)
    {
        bringup_fail("TC-HW-CFG-001 qspi_flash_init failed - check MX25R6435F wiring");
    }
    if (led_init(k_gw_led_pins, (uint8_t) LED_COUNT) != LED_ERR_OK)
    {
        bringup_fail("TC-HW-CFG-001 led_init failed");
    }

    health_monitor_config_t hm_cfg = {.boot_reset_cause = LIFECYCLE_RESET_SOFT};
    health_monitor_handle_t hm = NULL;
    config_store_config_t cs_cfg = {.flash = qspi_flash_driver, .health = NULL};
    config_store_handle_t cs = NULL;

    check(health_monitor_create(&hm_cfg, &hm) == HEALTH_MONITOR_ERR_OK,
          "TC-HW-CFG-001 health_monitor_create");
    cs_cfg.health = hm;
    check(config_store_create(&cs_cfg, &cs) == CONFIG_STORE_ERR_OK, "TC-HW-CFG-001 create");

    /* The boot integrity check selects the phase (LifecycleController does this during
     * Init.CheckingIntegrity). */
    const config_store_err_t integrity = config_store_check_integrity(cs);

    if (integrity == CONFIG_STORE_ERR_NO_VALID_SLOT)
    {
        phase_a(cs);
    }
    else if (integrity == CONFIG_STORE_ERR_OK)
    {
        phase_b(cs);
    }
    else
    {
        bringup_fail("boot integrity check returned a flash error");
    }

    for (;;)
    {
        /* Nothing more to do; the result is on the UART. */
    }
}
