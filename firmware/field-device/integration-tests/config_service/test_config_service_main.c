/**
 * @file test_config_service_main.c
 * @brief ConfigService integration test — STM32F469I-DISCO.
 *
 * Exercises the full validate → apply → persist → restore pipeline against the
 * real QSPI NOR flash (MX25L51245G) via ConfigStore.
 *
 * Activation in CubeIDE:
 *   - Exclude firmware/main.c from build (Resource Configurations).
 *   - Add integration-tests/config_service/ to project source paths.
 *   - Build, flash, open PuTTY at 115200/8N1 on the ST-Link VCP.
 *
 * Fixed-point scales:
 *   Temperature : int16_t ×100 centi-°C
 *   Humidity    : uint16_t ×100 centi-%RH
 *   Pressure    : uint16_t ×10  deci-hPa
 *   filter_alpha: uint16_t ×1000 per-mille (100 = 0.100)
 *
 * ==========================================================================
 * Expected output checklist — tick each item as it appears on the terminal:
 * ==========================================================================
 *
 * | # | What to observe                                                 | Verifies                               |
 * |---|-----------------------------------------------------------------|----------------------------------------|
 * | 1 | "===== ConfigService integration test =====" (INFO)             | Test task started                      |
 * | 2 | "ConfigService initialised (defaults applied)" (INFO)           | config_service_init() succeeded        |
 * | 3 | "poll_interval=1000 filter_alpha=100" (INFO)                    | Defaults applied correctly             |
 * | 4 | "set poll_interval=3000: OK" (INFO)                             | set_param() valid path works           |
 * | 5 | "set poll_interval=50: ERR_INVALID" (INFO)                      | Validation rejects out-of-range value  |
 * | 6 | "poll_interval still=3000" (INFO)                               | Param unchanged after invalid set      |
 * | 7 | "flush: OK" (INFO)                                              | Explicit persist via ConfigStore works |
 * | 8 | "snapshot: OK" (INFO)                                           | Snapshot saved                         |
 * | 9 | "set poll_interval=5000: OK" (INFO)                             | Second valid set_param works           |
 * | 10| "restore_snapshot: OK" (INFO)                                   | Snapshot restored                      |
 * | 11| "poll_interval after restore=3000" (INFO)                       | Restore reverted to snapshot value     |
 * | 12| "=== ALL CHECKS PASSED ===" (INFO)                              | All steps verified                     |
 * | 13| Green LED (LD1, PG13) lit continuously                          | No assertion failure                   |
 * | 14| Red LED (LD4, PD4) remains off                                  | No error condition triggered           |
 */

#include "stm32f469xx.h"

#include "FreeRTOS.h"
#include "task.h"

#include "system_clock.h"
#include "gpio/gpio_driver.h"
#include "qspi_flash_driver/qspi_flash_driver.h"
#include "debug-uart/debug_uart_driver.h"
#include "rtc/rtc_driver.h"
#include "health_monitor/health_monitor.h"
#include "logger/logger.h"
#include "config_store/config_store.h"
#include "config_service/config_service.h"

/* ========================================================================= */
/* Board-specific LED macros (F469-DISCO)                                   */
/* ========================================================================= */

#define LED_GREEN_PIN   (1UL << 13U)   /* PG13 */
#define LED_RED_PIN     (1UL << 4U)    /* PD4  */

static void led_green_on(void)  { GPIOG->BSRR = LED_GREEN_PIN; }
static void led_red_on(void)    { GPIOD->BSRR = LED_RED_PIN;   }
static void led_green_off(void) { GPIOG->BSRR = (LED_GREEN_PIN << 16U); }

/* ========================================================================= */
/* Test task configuration                                                   */
/* ========================================================================= */

#define TEST_STACK_WORDS  1024U
#define TEST_TASK_TAG     "IT"

static StackType_t  s_test_stack[TEST_STACK_WORDS] __attribute__((aligned(8)));
static StaticTask_t s_test_tcb;

/* ========================================================================= */
/* Fail helper                                                               */
/* ========================================================================= */

static void fail(const char *msg)
{
    LOG_ERROR(TEST_TASK_TAG, "FAIL: %s", msg);
    led_green_off();
    led_red_on();
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/* ========================================================================= */
/* Test task                                                                 */
/* ========================================================================= */

static void test_task(void *arg)
{
    (void) arg;

    LOG_INFO(TEST_TASK_TAG, "===== ConfigService integration test =====");

    /* ------------------------------------------------------------------ */
    /* Step 1: Verify defaults                                             */
    /* ------------------------------------------------------------------ */
    const config_params_t *p = config_service_get_params();
    if (p == NULL)
    {
        fail("get_params returned NULL after init");
    }

    /* filter_alpha displayed as per-mille integer (100 = 0.100). */
    LOG_INFO(TEST_TASK_TAG, "poll_interval=%lu filter_alpha=%u",
             (unsigned long) p->polling_interval_ms, (unsigned) p->filter_alpha);

    if (p->polling_interval_ms < 100U || p->polling_interval_ms > 60000U)
    {
        fail("default polling_interval_ms out of valid range");
    }
    /* filter_alpha valid range: (0, 1000) exclusive in per-mille units. */
    if (p->filter_alpha == 0U || p->filter_alpha >= 1000U)
    {
        fail("default filter_alpha out of valid range");
    }

    /* ------------------------------------------------------------------ */
    /* Step 2: Valid set_param                                             */
    /* ------------------------------------------------------------------ */
    uint32_t new_interval = 3000U;
    config_service_err_t err = config_service_set_param(CONFIG_PARAM_POLL_INTERVAL,
                                                         &new_interval);
    if (err != CONFIG_SERVICE_OK)
    {
        fail("set_param(3000) failed");
    }
    LOG_INFO(TEST_TASK_TAG, "set poll_interval=3000: OK");

    /* ------------------------------------------------------------------ */
    /* Step 3: Invalid set_param — must be rejected                       */
    /* ------------------------------------------------------------------ */
    uint32_t bad_interval = 50U;
    err = config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &bad_interval);
    if (err != CONFIG_SERVICE_ERR_INVALID)
    {
        fail("set_param(50) should return ERR_INVALID");
    }
    LOG_INFO(TEST_TASK_TAG, "set poll_interval=50: ERR_INVALID");

    p = config_service_get_params();
    if (p->polling_interval_ms != 3000U)
    {
        fail("param must not change after validation failure");
    }
    LOG_INFO(TEST_TASK_TAG, "poll_interval still=%lu", (unsigned long) p->polling_interval_ms);

    /* ------------------------------------------------------------------ */
    /* Step 4: Flush                                                       */
    /* ------------------------------------------------------------------ */
    err = config_service_flush();
    if (err != CONFIG_SERVICE_OK)
    {
        fail("flush failed");
    }
    LOG_INFO(TEST_TASK_TAG, "flush: OK");

    /* ------------------------------------------------------------------ */
    /* Step 5: Snapshot + change + restore                                 */
    /* ------------------------------------------------------------------ */
    err = config_service_snapshot();
    if (err != CONFIG_SERVICE_OK)
    {
        fail("snapshot failed");
    }
    LOG_INFO(TEST_TASK_TAG, "snapshot: OK");

    uint32_t changed_interval = 5000U;
    err = config_service_set_param(CONFIG_PARAM_POLL_INTERVAL, &changed_interval);
    if (err != CONFIG_SERVICE_OK)
    {
        fail("set_param(5000) failed");
    }
    LOG_INFO(TEST_TASK_TAG, "set poll_interval=5000: OK");

    err = config_service_restore_snapshot();
    if (err != CONFIG_SERVICE_OK)
    {
        fail("restore_snapshot failed");
    }
    LOG_INFO(TEST_TASK_TAG, "restore_snapshot: OK");

    p = config_service_get_params();
    if (p->polling_interval_ms != 3000U)
    {
        fail("restore_snapshot did not revert poll_interval to 3000");
    }
    LOG_INFO(TEST_TASK_TAG, "poll_interval after restore=%lu",
             (unsigned long) p->polling_interval_ms);

    /* ------------------------------------------------------------------ */
    /* All checks passed                                                   */
    /* ------------------------------------------------------------------ */
    LOG_INFO(TEST_TASK_TAG, "=== ALL CHECKS PASSED ===");
    led_green_on();

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/* ========================================================================= */
/* main                                                                      */
/* ========================================================================= */

int main(void)
{
    system_clock_init();

    (void) gpio_init();
    (void) debug_uart_init();
    (void) rtc_init();
    (void) qspi_flash_init();
    (void) health_monitor_init();
    (void) logger_init(LOG_LEVEL_DEBUG);
    (void) config_store_init(health_report);

    /* Init ConfigService with defaults; apply any stored config. */
    (void) config_service_init(config_store);

    /* Attempt to load stored config (may return ERR_NO_VALID_SLOT on blank flash). */
    {
        config_blob_t blob;
        uint32_t      len   = 0U;
        config_store_err_t cs_err = config_store->load(&blob, &len, sizeof(blob));
        if (cs_err == CONFIG_STORE_OK)
        {
            (void) config_service_apply_loaded(&blob, len);
        }
    }

    (void) xTaskCreateStatic(test_task, "IT", TEST_STACK_WORDS,
                             NULL, tskIDLE_PRIORITY + 1U,
                             s_test_stack, &s_test_tcb);

    vTaskStartScheduler();

    for (;;) {}
    return 0;
}
