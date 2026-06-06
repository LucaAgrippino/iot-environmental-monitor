/**
 * @file test_qspi_flash_driver_main.c
 * @brief QspiFlashDriver integration test on STM32F469I-DISCO hardware.
 *
 * Exercises the driver against the real MT25QL128ABA (Micron, 16 MB) NOR
 * flash device over the QUADSPI peripheral. Validates the full read /
 * page-program / sector-erase sequence on a live device.
 *
 * Activation in CubeIDE:
 *   - Exclude Src/main.c from the build (Resource Config → Exclude).
 *   - Add integration-tests/qspi_flash_driver/ to project source paths.
 *   - Build and flash; open the ST-Link VCP at 115 200 / 8N1.
 *
 * Visual checklist — expected serial output:
 *
 * [ INFO] ===== QspiFlashDriver integration test =====
 * [ INFO] qspi_flash_init() ... OK  (RDID = 0x20BA18, device = 16 MB)
 * [ INFO] Sector erase at 0x00000000 ... OK
 * [ INFO] Page write  64 bytes at 0x00000000 ... OK
 * [ INFO] Read back   64 bytes at 0x00000000 ... OK  (data verified)
 * [ INFO] Page write 128 bytes at 0x00000040 ... OK
 * [ INFO] Read back  128 bytes at 0x00000040 ... OK  (data verified)
 * [ INFO] addr=0xFFFFFFFF erase_sector bounds check ... QSPI_FLASH_ERR_ADDR  OK
 * [ INFO] len=0 read bounds check                  ... QSPI_FLASH_ERR_LEN   OK
 * [ INFO] page-boundary write_page check           ... QSPI_FLASH_ERR_LEN   OK
 * [ INFO] ===== ALL CHECKS PASSED =====
 *
 * Integration checklist:
 *   [ ] RDID matches 0x20BA18 (Micron MT25QL128ABA — verify against actual device)
 *   [ ] Sector erase completes without TIMEOUT (WIP clears within ~800 ms worst case)
 *   [ ] Written data reads back identically
 *   [ ] Bounds and page-boundary checks return expected error codes
 *   [ ] No QSPI_FLASH_ERR_DEVICE at init (correct device fitted)
 *   [ ] No QSPI_FLASH_ERR_TIMEOUT during write or erase
 *
 */

#include "stm32f469xx.h"

#include "FreeRTOS.h"
#include "task.h"

#include "system_clock.h"
#include "rtc/rtc_driver.h"
#include "debug-uart/debug_uart_driver.h"
#include "logger/logger.h"
#include "qspi_flash_driver/qspi_flash_driver.h"
#include "gpio/gpio_driver.h"

/* ====================================================================== */
/* Configuration                                                          */
/* ====================================================================== */

#define TEST_SECTOR_ADDR   (0x00000000UL) /* First sector of flash */
#define TEST_PAGE_A_ADDR   (0x00000000UL) /* First page */
#define TEST_PAGE_A_LEN    (64U)
#define TEST_PAGE_B_ADDR   (0x00000040UL) /* Second 64-byte block, same sector */
#define TEST_PAGE_B_LEN    (128U)

#define TEST_TASK_STACK_WORDS (512U)
#define TEST_TASK_PRIORITY    (tskIDLE_PRIORITY + 2U)

static StaticTask_t  s_test_task_tcb;
static StackType_t   s_test_task_stack[TEST_TASK_STACK_WORDS];

/* ====================================================================== */
/* Helpers                                                                */
/* ====================================================================== */

static void log_result(const char *step, qspi_flash_err_t err,
                       qspi_flash_err_t expected)
{
    if (err == expected)
    {
        LOG_INFO("QSPI-TEST", "%s ... OK", step);
    }
    else
    {
        LOG_ERROR("QSPI-TEST", "%s ... FAIL (err=%d, expected=%d)",
                  step, (int)err, (int)expected);
    }
}

static bool verify_buffer(const uint8_t *expected, const uint8_t *actual,
                           uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++)
    {
        if (expected[i] != actual[i])
        {
        	LOG_ERROR("QSPI", "mismatch@%lu exp=0x%02X got=0x%02X",
        	          (unsigned long)i, expected[i], actual[i]);
            return false;
        }
    }
    return true;
}

/* ====================================================================== */
/* Test task                                                              */
/* ====================================================================== */

static void qspi_test_task(void *arg)
{
    (void)arg;

    bool     passed   = true;
    uint8_t  write_a[TEST_PAGE_A_LEN];
    uint8_t  read_a[TEST_PAGE_A_LEN];
    uint8_t  write_b[TEST_PAGE_B_LEN];
    uint8_t  read_b[TEST_PAGE_B_LEN];

    LOG_INFO("QSPI-TEST", "===== QspiFlashDriver integration test =====");

    /* -- Init ---------------------------------------------------------- */
    {
        qspi_flash_err_t err = qspi_flash_init();
        log_result("qspi_flash_init()", err, QSPI_FLASH_OK);
        if (err != QSPI_FLASH_OK)
        {
            LOG_ERROR("QSPI-TEST", "init failed — aborting");
            passed = false;
        }
    }

    if (passed)
    {
        /* -- Sector erase ---------------------------------------------- */
        {
            qspi_flash_err_t err = qspi_flash_erase_sector(TEST_SECTOR_ADDR);
            log_result("Sector erase at 0x00000000", err, QSPI_FLASH_OK);
            if (err != QSPI_FLASH_OK)
            {
                passed = false;
            }
        }

        /* -- Page write A (64 bytes) ----------------------------------- */
        for (uint8_t i = 0U; i < TEST_PAGE_A_LEN; i++)
        {
            write_a[i] = (uint8_t)(0xA0U + i);
        }
        {
            qspi_flash_err_t err =
                qspi_flash_write_page(TEST_PAGE_A_ADDR, write_a, TEST_PAGE_A_LEN);
            log_result("Page write 64 bytes at 0x00000000", err, QSPI_FLASH_OK);
            if (err != QSPI_FLASH_OK)
            {
                passed = false;
            }
        }

        /* -- Read back A ----------------------------------------------- */
        {
            qspi_flash_err_t err =
                qspi_flash_read(TEST_PAGE_A_ADDR, read_a, TEST_PAGE_A_LEN);
            log_result("Read back 64 bytes at 0x00000000", err, QSPI_FLASH_OK);
            if (err == QSPI_FLASH_OK)
            {
                if (!verify_buffer(write_a, read_a, TEST_PAGE_A_LEN))
                {
                    passed = false;
                    LOG_ERROR("QSPI-TEST", "Read-back verify FAILED");
                }
                else
                {
                    LOG_INFO("QSPI-TEST", "  data verified OK");
                }
            }
            else
            {
                passed = false;
            }
        }

        /* -- Page write B (128 bytes) ---------------------------------- */
        for (uint8_t i = 0U; i < TEST_PAGE_B_LEN; i++)
        {
            write_b[i] = (uint8_t)(0xB0U + i);
        }
        {
            qspi_flash_err_t err =
                qspi_flash_write_page(TEST_PAGE_B_ADDR, write_b, TEST_PAGE_B_LEN);
            log_result("Page write 128 bytes at 0x00000040", err, QSPI_FLASH_OK);
            if (err != QSPI_FLASH_OK)
            {
                passed = false;
            }
        }

        /* -- Read back B ----------------------------------------------- */
        {
            qspi_flash_err_t err =
                qspi_flash_read(TEST_PAGE_B_ADDR, read_b, TEST_PAGE_B_LEN);
            log_result("Read back 128 bytes at 0x00000040", err, QSPI_FLASH_OK);
            if (err == QSPI_FLASH_OK)
            {
                if (!verify_buffer(write_b, read_b, TEST_PAGE_B_LEN))
                {
                    passed = false;
                    LOG_ERROR("QSPI-TEST", "Read-back verify FAILED");
                }
                else
                {
                    LOG_INFO("QSPI-TEST", "  data verified OK");
                }
            }
            else
            {
                passed = false;
            }
        }

        /* -- Bounds checks (error-path validation) --------------------- */
        {
            qspi_flash_err_t err = qspi_flash_erase_sector(0xFFFFFFFFUL);
            if (err == QSPI_FLASH_ERR_ADDR)
            {
                LOG_INFO("QSPI-TEST",
                         "addr=0xFFFFFFFF erase bounds check ... QSPI_FLASH_ERR_ADDR  OK");
            }
            else
            {
                LOG_ERROR("QSPI-TEST", "erase bounds check FAILED (err=%d)", (int)err);
                passed = false;
            }
        }
        {
            uint8_t          tmp[1];
            qspi_flash_err_t err = qspi_flash_read(0U, tmp, 0U);
            if (err == QSPI_FLASH_ERR_LEN)
            {
                LOG_INFO("QSPI-TEST",
                         "len=0 read bounds check ... QSPI_FLASH_ERR_LEN  OK");
            }
            else
            {
                LOG_ERROR("QSPI-TEST", "read len=0 check FAILED (err=%d)", (int)err);
                passed = false;
            }
        }
        {
            /* addr=0xF8, len=16: last byte at 0x107 — crosses 0x100 boundary */
            uint8_t          tmp[16];
            qspi_flash_err_t err = qspi_flash_write_page(0x00F8U, tmp, 16U);
            if (err == QSPI_FLASH_ERR_LEN)
            {
                LOG_INFO("QSPI-TEST",
                         "page-boundary write check ... QSPI_FLASH_ERR_LEN  OK");
            }
            else
            {
                LOG_ERROR("QSPI-TEST",
                          "page-boundary check FAILED (err=%d)", (int)err);
                passed = false;
            }
        }
    }

    if (passed)
    {
        LOG_INFO("QSPI-TEST", "===== ALL CHECKS PASSED =====");
    }
    else
    {
        LOG_ERROR("QSPI-TEST", "===== ONE OR MORE CHECKS FAILED =====");
    }

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(5000U));
    }
}

/* ====================================================================== */
/* main                                                                   */
/* ====================================================================== */

int main(void)
{
    system_clock_init();


    gpio_init();
    rtc_init();
    debug_uart_init();
    logger_init(LOG_LEVEL_DEBUG);

    LOG_INFO("QSPI-TEST", "system up — starting scheduler");

    (void)xTaskCreateStatic(qspi_test_task, "QspiTest",
                            TEST_TASK_STACK_WORDS, NULL,
                            TEST_TASK_PRIORITY,
                            s_test_task_stack, &s_test_task_tcb);

    vTaskStartScheduler();
    for (;;) {}
}
