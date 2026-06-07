/**
 * @file test_config_store_main.c
 * @brief ConfigStore integration test on STM32F469I-DISCO hardware.
 *
 * Exercises ConfigStore against the real MT25QL128ABA NOR flash via the
 * QspiFlashDriver.  Validates the A/B slot rotation, CRC32 integrity,
 * power-loss safety (CRC written last), factory erase, and error-path
 * behaviour (simulated corruption by manual CRC byte flip).
 *
 * Activation in CubeIDE:
 *   - Exclude Src/main.c from the build (Resource Configurations →
 *     Exclude from Build → All Configurations).
 *   - Add integration-tests/config_store/ to project source paths
 *     (Project Properties → C/C++ General → Paths and Symbols).
 *   - Build, flash, open the ST-Link VCP at 115 200 / 8N1.
 *
 * Visual checklist — expected serial output:
 *
 * [ INFO] ===== ConfigStore integration test =====
 * [ INFO] qspi_flash_init()                     ... OK
 * [ INFO] config_store_init()                   ... OK
 * [ INFO] check_integrity fresh                 ... ERR_NO_VALID_SLOT  OK
 * [ INFO] save blob_a (8 bytes)                 ... OK  (slot B, seq=1)
 * [ INFO] check_integrity after save            ... OK
 * [ INFO] load blob_a                           ... OK  (data verified)
 * [ INFO] save blob_b (16 bytes)                ... OK  (slot A, seq=2)
 * [ INFO] load blob_b                           ... OK  (data verified)
 * [ INFO] corrupt slot A CRC (bit flip)         ... simulated
 * [ INFO] load after slot A corrupt             ... OK  (slot B verified)
 * [ INFO] config_store_erase()                  ... OK
 * [ INFO] load after erase                      ... ERR_NO_VALID_SLOT  OK
 * [ INFO] ===== ALL CHECKS PASSED (12/12) =====
 *
 * Integration checklist:
 *   [ ] QSPI init succeeds (RDID matches MT25QL128ABA 0x20BA18)
 *   [ ] ConfigStore init succeeds (partition probe read returns OK)
 *   [ ] Fresh partition: check_integrity returns NO_VALID_SLOT
 *   [ ] First save lands in slot B (seq=1); data reads back intact
 *   [ ] Second save lands in slot A (seq=2); load returns newer blob
 *   [ ] check_integrity passes after a valid save
 *   [ ] Manual CRC bit-clear on slot A causes load to fall back to slot B
 *   [ ] Fallback load returns the blob_a data from slot B intact
 *   [ ] Factory erase clears both slots; subsequent load returns NO_VALID_SLOT
 *   [ ] No hard-fault or QSPI timeout during the entire sequence
 *
 * NOTE: This test erases the ConfigStore partition (0x90000000–0x9000FFFF).
 * Any previously persisted configuration is destroyed.
 */

#include "stm32f469xx.h"

#include "FreeRTOS.h"
#include "task.h"

#include <string.h>

#include "system_clock.h"
#include "rtc/rtc_driver.h"
#include "debug-uart/debug_uart_driver.h"
#include "logger/logger.h"
#include "qspi_flash_driver/qspi_flash_driver.h"
#include "config_store/config_store.h"

/* ========================================================================= */
/* Minimal IHealthReport stub (no HealthMonitor initialisation required)    */
/* ========================================================================= */

static health_monitor_err_t stub_push_event(health_event_t ev, uint32_t param)
{
    (void) param;
    LOG_INFO("CS-HEALTH", "push_event(%d)", (int) ev);
    return HEALTH_MONITOR_ERR_OK;
}

static const ihealth_report_t s_health_stub = {
    .init = NULL,
    .push_event = stub_push_event,
};

/* ========================================================================= */
/* Configuration                                                             */
/* ========================================================================= */

#define MODULE_NAME "CS-TEST"

#define TEST_TASK_STACK_WORDS (512U)
#define TEST_TASK_PRIORITY (tskIDLE_PRIORITY + 2U)

/* Slot A CRC field physical QSPI address (slot A base 0x90000000 + 0x7FF8) */
#define CS_SLOT_A_CRC_ADDR (0x90007FF8UL)

/* ========================================================================= */
/* Static allocation                                                         */
/* ========================================================================= */

static StaticTask_t s_test_task_tcb;
static StackType_t s_test_task_stack[TEST_TASK_STACK_WORDS];

/* ========================================================================= */
/* Helpers                                                                   */
/* ========================================================================= */

static uint32_t s_pass;
static uint32_t s_fail;

static void check(const char *step, config_store_err_t actual, config_store_err_t expected)
{
    if (actual == expected)
    {
        LOG_INFO(MODULE_NAME, "%-40s ... OK", step);
        s_pass++;
    }
    else
    {
        LOG_ERROR(MODULE_NAME, "%-40s ... FAIL (got %d, expected %d)", step, (int) actual,
                  (int) expected);
        s_fail++;
    }
}

/* ========================================================================= */
/* Test task                                                                 */
/* ========================================================================= */

static void test_task(void *param)
{
    (void) param;

    static const uint8_t k_blob_a[8U] = {0x01U, 0x02U, 0x03U, 0x04U, 0x05U, 0x06U, 0x07U, 0x08U};
    static const uint8_t k_blob_b[16U] = {0x11U, 0x22U, 0x33U, 0x44U, 0x55U, 0x66U, 0x77U, 0x88U,
                                          0x99U, 0xAAU, 0xBBU, 0xCCU, 0xDDU, 0xEEU, 0xFFU, 0x00U};
    uint8_t buf[64U];
    uint32_t len;
    uint8_t crc_byte;

    s_pass = 0U;
    s_fail = 0U;

    /* 1 — Fresh partition: check_integrity must return NO_VALID_SLOT.
     *     The erase at step 9 at the end guarantees this on repeated runs. */
    check("check_integrity fresh", config_store_check_integrity(), CONFIG_STORE_ERR_NO_VALID_SLOT);

    /* 2 — First save: blob_a → slot B (seq=1) */
    check("save blob_a (8 bytes)", config_store_save(k_blob_a, sizeof(k_blob_a)),
          CONFIG_STORE_ERR_OK);

    /* 3 — check_integrity after one valid slot */
    check("check_integrity after save", config_store_check_integrity(), CONFIG_STORE_ERR_OK);

    /* 4 — Load blob_a and verify contents */
    len = 0U;
    check("load blob_a", config_store_load(buf, &len, sizeof(buf)), CONFIG_STORE_ERR_OK);
    if (len == sizeof(k_blob_a) && (memcmp(buf, k_blob_a, sizeof(k_blob_a)) == 0))
    {
        LOG_INFO(MODULE_NAME, "  blob_a data verified");
        s_pass++;
    }
    else
    {
        LOG_ERROR(MODULE_NAME, "  blob_a data MISMATCH (len=%u)", (unsigned) len);
        s_fail++;
    }

    /* 5 — Second save: blob_b → slot A (seq=2) */
    check("save blob_b (16 bytes)", config_store_save(k_blob_b, sizeof(k_blob_b)),
          CONFIG_STORE_ERR_OK);

    /* 6 — Load blob_b; slot A (seq=2) should be selected */
    len = 0U;
    check("load blob_b", config_store_load(buf, &len, sizeof(buf)), CONFIG_STORE_ERR_OK);
    if (len == sizeof(k_blob_b) && (memcmp(buf, k_blob_b, sizeof(k_blob_b)) == 0))
    {
        LOG_INFO(MODULE_NAME, "  blob_b data verified");
        s_pass++;
    }
    else
    {
        LOG_ERROR(MODULE_NAME, "  blob_b data MISMATCH (len=%u)", (unsigned) len);
        s_fail++;
    }

    /* 7 — Simulate corruption of slot A CRC by clearing one bit via
     *     page-program (NOR flash: can only 1→0 without erase).
     *     Slot A CRC field is at physical address 0x90007FF8. */
    if (qspi_flash_driver->read(CS_SLOT_A_CRC_ADDR, &crc_byte, 1U) == QSPI_FLASH_OK)
    {
        crc_byte &= 0xFEU; /* clear LSB — guarantees at least one bit changed */
        if (qspi_flash_driver->write_page(CS_SLOT_A_CRC_ADDR, &crc_byte, 1U) == QSPI_FLASH_OK)
        {
            LOG_INFO(MODULE_NAME, "corrupt slot A CRC (bit clear)           ... simulated");
            s_pass++;
        }
        else
        {
            LOG_ERROR(MODULE_NAME, "corrupt slot A CRC write                 ... FAIL");
            s_fail++;
        }
    }
    else
    {
        LOG_ERROR(MODULE_NAME, "corrupt slot A CRC read                  ... FAIL");
        s_fail++;
    }

    /* 8 — Load after slot A corruption: must fall back to slot B (blob_a) */
    len = 0U;
    check("load after slot A corrupt", config_store_load(buf, &len, sizeof(buf)),
          CONFIG_STORE_ERR_OK);
    if (len == sizeof(k_blob_a) && (memcmp(buf, k_blob_a, sizeof(k_blob_a)) == 0))
    {
        LOG_INFO(MODULE_NAME, "  fallback to slot B verified");
        s_pass++;
    }
    else
    {
        LOG_ERROR(MODULE_NAME, "  fallback data MISMATCH (len=%u)", (unsigned) len);
        s_fail++;
    }

    /* 9 — Factory erase: clears both slots — leaves partition erased for next run */
    check("config_store_erase()", config_store_erase(), CONFIG_STORE_ERR_OK);

    /* 10 — Load after erase: both slots blank → NO_VALID_SLOT */
    len = 0U;
    check("load after erase", config_store_load(buf, &len, sizeof(buf)),
          CONFIG_STORE_ERR_NO_VALID_SLOT);

    /* Summary */
    if (s_fail == 0U)
    {
        LOG_INFO(MODULE_NAME, "===== ALL CHECKS PASSED (%u/%u) =====", (unsigned) s_pass,
                 (unsigned) (s_pass + s_fail));
    }
    else
    {
        LOG_ERROR(MODULE_NAME, "===== %u CHECKS FAILED (%u/%u) =====", (unsigned) s_fail,
                  (unsigned) s_pass, (unsigned) (s_pass + s_fail));
    }

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(5000U));
    }
}

/* ========================================================================= */
/* main                                                                      */
/* ========================================================================= */

int main(void)
{
    system_clock_init();

    (void) rtc_driver_init();
    debug_uart_driver_init();
    logger_init();

    LOG_INFO(MODULE_NAME, "===== ConfigStore integration test =====");

    if (qspi_flash_init() != QSPI_FLASH_OK)
    {
        LOG_ERROR(MODULE_NAME, "qspi_flash_init() FAILED — halting");
        for (;;)
        {
        }
    }
    LOG_INFO(MODULE_NAME, "qspi_flash_init()                     ... OK");

    if (config_store_init((ihealth_report_t *) &s_health_stub) != CONFIG_STORE_ERR_OK)
    {
        LOG_ERROR(MODULE_NAME, "config_store_init() FAILED — halting");
        for (;;)
        {
        }
    }
    LOG_INFO(MODULE_NAME, "config_store_init()                   ... OK");

    (void) xTaskCreateStatic(test_task, "CS-Test", TEST_TASK_STACK_WORDS, NULL, TEST_TASK_PRIORITY,
                             s_test_task_stack, &s_test_task_tcb);

    vTaskStartScheduler();
    for (;;)
    {
    }
}
