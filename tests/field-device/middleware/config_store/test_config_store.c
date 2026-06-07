/**
 * @file test_config_store.c
 * @brief Unit tests for ConfigStore middleware — TC-CS-001..013.
 *
 * Covers docs/lld/middleware/config-store.md §7.
 *
 * Mocking strategy:
 *   - QspiFlashDriver  → 64 KB RAM buffer (g_config_store_flash_sim) via
 *                        UNIT_TEST macros in config_store.h.  Stub functions
 *                        stub_cs_flash_erase_range / _write / _read defined here.
 *   - IHealthReport    → s_stub_health vtable defined in this TU.
 *   - FreeRTOS         → freertos_mock.h (auto-links freertos_mock.c).
 *   - Logger           → logger_log / debug_uart_send / rtc_get_time are never
 *                        called because logger is never initialised; LOG_* macros
 *                        in config_store.c are no-ops at runtime.
 *
 * Slot selection note (affects TC-CS-003 and TC-CS-004):
 *   First save → slot B (seq=1).  Second save → slot A (seq=2).
 *   Load selects the slot with the highest valid seq_number.
 *
 * Build defines: STM32F469xx, TEST, UNIT_TEST (project.yml :test_config_store:).
 */

#include "unity.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

/* health_monitor_stub.h must come before config_store.h so that
 * struct ihealth_report_s is complete when config_store.h (under TEST)
 * provides only a forward declaration of ihealth_report_t. */
#include "health_monitor_stub.h"

#include "FreeRTOS.h"
#include "semphr.h"
#include "freertos_mock.h"   /* causes Ceedling to auto-link freertos_mock.c */

/* config_store.h with UNIT_TEST active: pulls in flash-sim redirects,
 * declares stub_cs_flash_* stubs, defines g_config_store_flash_sim extern. */
#include "config_store/config_store.h"

/* config_store_crc.h is included here so that Ceedling auto-links
 * config_store_crc.c — it is not included transitively from config_store.h. */
#include "config_store/config_store_crc.h"

/* ======================================================================= */
/* Logger stubs — minimum symbols for config_store.c link                  */
/* ======================================================================= */

/* LOG_LEVEL_MIN=-1 (set in project.yml :test_config_store:) makes all
 * LOG_* macros in config_store.c expand to ((void)0), so logger_log is
 * never referenced.  debug_uart_send and rtc_get_time are provided here
 * in case the logger is linked transitively by another support file. */

typedef struct { uint16_t year; uint8_t month; uint8_t day;
                 uint8_t hour; uint8_t minute; uint8_t second; } rtc_datetime_t;
typedef enum  { RTC_OK = 0 } rtc_err_t;
typedef enum  { DEBUG_UART_OK = 0 } debug_uart_err_t;

rtc_err_t        rtc_get_time(rtc_datetime_t *dt) { (void)dt; return RTC_OK; }
debug_uart_err_t debug_uart_send(const uint8_t *d, size_t n, uint32_t t)
{
    (void)d; (void)n; (void)t; return DEBUG_UART_OK;
}

/* ======================================================================= */
/* Health report mock                                                       */
/* ======================================================================= */

static uint32_t       g_health_push_calls;
static health_event_t g_health_last_event;

static health_monitor_err_t stub_push_event(health_event_t event, uint32_t param)
{
    (void)param;
    g_health_push_calls++;
    g_health_last_event = event;
    return HEALTH_MONITOR_ERR_OK;
}

static const ihealth_report_t s_stub_health = {
    .init       = NULL,
    .push_event = stub_push_event,
};

const ihealth_report_t *const health_report = &s_stub_health;

static void health_mock_reset(void)
{
    g_health_push_calls = 0U;
    g_health_last_event = (health_event_t)0xFFU;
}

/* ======================================================================= */
/* Flash simulation stubs                                                   */
/* ======================================================================= */

static int g_stub_erase_fail_at;    /* fail on Nth erase call (0 = no fault) */
static int g_stub_erase_call_count;
static int g_stub_write_fail_at;    /* fail on Nth write call (0 = no fault) */
static int g_stub_write_call_count;

qspi_flash_err_t stub_cs_flash_erase_range(uint8_t *flash, uint32_t base, uint32_t sectors)
{
    uint32_t offset;

    g_stub_erase_call_count++;
    if ((g_stub_erase_fail_at > 0) && (g_stub_erase_call_count >= g_stub_erase_fail_at))
    {
        return (qspi_flash_err_t)1;
    }

    offset = base - CONFIG_STORE_QSPI_BASE_ADDR;
    memset(flash + offset, 0xFF, (size_t)(sectors * CONFIG_STORE_SECTOR_SIZE));
    return (qspi_flash_err_t)0;
}

qspi_flash_err_t stub_cs_flash_write(uint8_t *flash, uint32_t addr,
                                     const uint8_t *buf, uint32_t len)
{
    uint32_t offset;

    g_stub_write_call_count++;
    if ((g_stub_write_fail_at > 0) && (g_stub_write_call_count >= g_stub_write_fail_at))
    {
        return (qspi_flash_err_t)1;
    }

    offset = addr - CONFIG_STORE_QSPI_BASE_ADDR;
    memcpy(flash + offset, buf, (size_t)len);
    return (qspi_flash_err_t)0;
}

qspi_flash_err_t stub_cs_flash_read(uint8_t *flash, uint32_t addr,
                                    uint8_t *buf, uint32_t len)
{
    uint32_t offset = addr - CONFIG_STORE_QSPI_BASE_ADDR;
    memcpy(buf, flash + offset, (size_t)len);
    return (qspi_flash_err_t)0;
}

static void flash_stubs_reset(void)
{
    g_stub_erase_fail_at    = 0;
    g_stub_erase_call_count = 0;
    g_stub_write_fail_at    = 0;
    g_stub_write_call_count = 0;
}

/* ======================================================================= */
/* Test lifecycle                                                           */
/* ======================================================================= */

void setUp(void)
{
    mock_freertos_reset();
    health_mock_reset();
    flash_stubs_reset();
    config_store_reset_for_test();

    /* Erase-state flash: all 0xFF */
    memset(g_config_store_flash_sim, 0xFF, (size_t)CONFIG_STORE_PARTITION_SIZE);

    /* Semaphore mock always succeeds */
    g_mock_xSemaphoreCreateMutexStatic_return = (SemaphoreHandle_t)1;
    g_mock_xSemaphoreTake_return              = pdTRUE;
    g_mock_xSemaphoreGive_return              = pdTRUE;
}

void tearDown(void) {}

/* ======================================================================= */
/* Helper                                                                   */
/* ======================================================================= */

static void do_init(void)
{
    TEST_ASSERT_EQUAL(CONFIG_STORE_OK,
                      config_store_init((ihealth_report_t *)&s_stub_health));
}

/* ======================================================================= */
/* TC-CS-001: Fresh flash → load returns ERR_NO_VALID_SLOT + health event  */
/* ======================================================================= */

void test_TC_CS_001_fresh_flash_load_no_valid_slot(void)
{
    uint8_t  buf[32U];
    uint32_t len = 0U;

    do_init();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NO_VALID_SLOT,
                      config_store_load(buf, &len, sizeof(buf)));
    TEST_ASSERT_EQUAL(1U, g_health_push_calls);
    TEST_ASSERT_EQUAL(HEALTH_EVENT_CONFIG_NO_VALID_SLOT, g_health_last_event);
}

/* ======================================================================= */
/* TC-CS-002: Save + load round trip → data matches                        */
/* ======================================================================= */

void test_TC_CS_002_save_load_roundtrip(void)
{
    static const uint8_t payload[8U] = {0x01U, 0x02U, 0x03U, 0x04U,
                                         0x05U, 0x06U, 0x07U, 0x08U};
    uint8_t  buf[16U];
    uint32_t len = 0U;

    do_init();

    TEST_ASSERT_EQUAL(CONFIG_STORE_OK,
                      config_store_save(payload, sizeof(payload)));
    TEST_ASSERT_EQUAL(CONFIG_STORE_OK,
                      config_store_load(buf, &len, sizeof(buf)));
    TEST_ASSERT_EQUAL(sizeof(payload), len);
    TEST_ASSERT_EQUAL_MEMORY(payload, buf, sizeof(payload));
}

/* ======================================================================= */
/* TC-CS-003: Save twice → second load returns second blob                 */
/* ======================================================================= */

void test_TC_CS_003_dual_save_slot_selection(void)
{
    static const uint8_t blob1[4U] = {0xAAU, 0xBBU, 0xCCU, 0xDDU};
    static const uint8_t blob2[4U] = {0x11U, 0x22U, 0x33U, 0x44U};
    uint8_t  buf[4U];
    uint32_t len = 0U;

    do_init();

    /* save #1 → slot B (seq=1); save #2 → slot A (seq=2) */
    TEST_ASSERT_EQUAL(CONFIG_STORE_OK, config_store_save(blob1, sizeof(blob1)));
    TEST_ASSERT_EQUAL(CONFIG_STORE_OK, config_store_save(blob2, sizeof(blob2)));
    TEST_ASSERT_EQUAL(CONFIG_STORE_OK,
                      config_store_load(buf, &len, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY(blob2, buf, sizeof(blob2));
}

/* ======================================================================= */
/* TC-CS-004: Corrupt lower-seq slot CRC → load selects intact slot        */
/* ======================================================================= */

void test_TC_CS_004_single_crc_corrupt_selects_valid_slot(void)
{
    static const uint8_t blob1[4U] = {0xAAU, 0xBBU, 0xCCU, 0xDDU};
    static const uint8_t blob2[4U] = {0x11U, 0x22U, 0x33U, 0x44U};
    uint8_t  buf[4U];
    uint32_t len = 0U;

    do_init();

    /* save #1 → slot B (seq=1); save #2 → slot A (seq=2) */
    TEST_ASSERT_EQUAL(CONFIG_STORE_OK, config_store_save(blob1, sizeof(blob1)));
    TEST_ASSERT_EQUAL(CONFIG_STORE_OK, config_store_save(blob2, sizeof(blob2)));

    /* Corrupt slot B CRC (lower seq slot) by flipping bytes */
    g_config_store_flash_sim[CONFIG_STORE_SLOT_SIZE + 0x7FF8U] ^= 0xFFU;

    health_mock_reset();

    /* Load should select slot A (higher seq, valid CRC) and return blob2 */
    TEST_ASSERT_EQUAL(CONFIG_STORE_OK,
                      config_store_load(buf, &len, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY(blob2, buf, sizeof(blob2));
}

/* ======================================================================= */
/* TC-CS-005: Both slots CRC corrupt → ERR_NO_VALID_SLOT + health event   */
/* ======================================================================= */

void test_TC_CS_005_both_crc_corrupt_no_valid_slot(void)
{
    static const uint8_t blob[4U] = {0xDEU, 0xADU, 0xBEU, 0xEFU};
    uint8_t  buf[4U];
    uint32_t len = 0U;

    do_init();

    TEST_ASSERT_EQUAL(CONFIG_STORE_OK, config_store_save(blob, sizeof(blob)));
    TEST_ASSERT_EQUAL(CONFIG_STORE_OK, config_store_save(blob, sizeof(blob)));

    /* Corrupt both slots' CRCs */
    g_config_store_flash_sim[0x7FF8U] ^= 0xFFU;                           /* slot A */
    g_config_store_flash_sim[CONFIG_STORE_SLOT_SIZE + 0x7FF8U] ^= 0xFFU;  /* slot B */

    health_mock_reset();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NO_VALID_SLOT,
                      config_store_load(buf, &len, sizeof(buf)));
    TEST_ASSERT_EQUAL(HEALTH_EVENT_CONFIG_NO_VALID_SLOT, g_health_last_event);
}

/* ======================================================================= */
/* TC-CS-006: Erase failure → ERR_FLASH_ERASE; active slot unchanged       */
/* ======================================================================= */

void test_TC_CS_006_erase_failure_active_slot_intact(void)
{
    static const uint8_t blob1[4U] = {0x01U, 0x02U, 0x03U, 0x04U};
    static const uint8_t blob2[4U] = {0x0AU, 0x0BU, 0x0CU, 0x0DU};
    uint8_t  buf[4U];
    uint32_t len = 0U;

    do_init();

    TEST_ASSERT_EQUAL(CONFIG_STORE_OK, config_store_save(blob1, sizeof(blob1)));

    /* Fail on the next erase call (second save) */
    g_stub_erase_call_count = 0;
    g_stub_erase_fail_at    = 1;

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_FLASH_ERASE,
                      config_store_save(blob2, sizeof(blob2)));
    TEST_ASSERT_EQUAL(HEALTH_EVENT_CONFIG_WRITE_FAIL, g_health_last_event);

    /* Slot B (first save) must still be loadable */
    g_stub_erase_fail_at = 0;
    TEST_ASSERT_EQUAL(CONFIG_STORE_OK,
                      config_store_load(buf, &len, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY(blob1, buf, sizeof(blob1));
}

/* ======================================================================= */
/* TC-CS-007: Write failure at CRC commit → ERR_FLASH_WRITE; slot intact   */
/* ======================================================================= */

void test_TC_CS_007_crc_write_failure_active_slot_intact(void)
{
    static const uint8_t blob1[4U] = {0xAAU, 0xBBU, 0xCCU, 0xDDU};
    static const uint8_t blob2[4U] = {0x55U, 0x66U, 0x77U, 0x88U};
    uint8_t  buf[4U];
    uint32_t len = 0U;

    do_init();

    TEST_ASSERT_EQUAL(CONFIG_STORE_OK, config_store_save(blob1, sizeof(blob1)));

    /* Fail on 3rd write call of next save: (1) header, (2) data, (3) CRC */
    g_stub_write_call_count = 0;
    g_stub_write_fail_at    = 3;

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_FLASH_WRITE,
                      config_store_save(blob2, sizeof(blob2)));
    TEST_ASSERT_EQUAL(HEALTH_EVENT_CONFIG_WRITE_FAIL, g_health_last_event);

    /* Slot B (first save) must still be valid — CRC commit of target slot failed */
    g_stub_write_fail_at = 0;
    TEST_ASSERT_EQUAL(CONFIG_STORE_OK,
                      config_store_load(buf, &len, sizeof(buf)));
    TEST_ASSERT_EQUAL_MEMORY(blob1, buf, sizeof(blob1));
}

/* ======================================================================= */
/* TC-CS-008: erase() → subsequent load returns ERR_NO_VALID_SLOT         */
/* ======================================================================= */

void test_TC_CS_008_factory_erase_clears_both_slots(void)
{
    static const uint8_t blob[8U] = {0x01U, 0x02U, 0x03U, 0x04U,
                                      0x05U, 0x06U, 0x07U, 0x08U};
    uint8_t  buf[8U];
    uint32_t len = 0U;

    do_init();

    TEST_ASSERT_EQUAL(CONFIG_STORE_OK, config_store_save(blob, sizeof(blob)));
    TEST_ASSERT_EQUAL(CONFIG_STORE_OK, config_store_erase());

    health_mock_reset();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NO_VALID_SLOT,
                      config_store_load(buf, &len, sizeof(buf)));
    TEST_ASSERT_EQUAL(HEALTH_EVENT_CONFIG_NO_VALID_SLOT, g_health_last_event);
}

/* ======================================================================= */
/* TC-CS-009: data_len > max_len on load → ERR_TOO_LARGE                  */
/* ======================================================================= */

void test_TC_CS_009_load_too_large_returns_error(void)
{
    static const uint8_t big[100U];  /* zero-initialised */
    uint8_t  small_buf[10U];
    uint32_t len = 0U;

    do_init();

    TEST_ASSERT_EQUAL(CONFIG_STORE_OK, config_store_save(big, sizeof(big)));
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_TOO_LARGE,
                      config_store_load(small_buf, &len, sizeof(small_buf)));
}

/* ======================================================================= */
/* TC-CS-010: Not-init guards on all four public functions                 */
/* ======================================================================= */

void test_TC_CS_010_not_init_guards(void)
{
    uint8_t  buf[8U];
    uint32_t len = 0U;

    /* No config_store_init() called */
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NOT_INIT,
                      config_store_load(buf, &len, sizeof(buf)));
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NOT_INIT,
                      config_store_save(buf, sizeof(buf)));
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NOT_INIT,
                      config_store_check_integrity());
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NOT_INIT,
                      config_store_erase());
}

/* ======================================================================= */
/* TC-CS-011: NULL argument guards (init, load, save)                      */
/* ======================================================================= */

void test_TC_CS_011_null_arg_guards(void)
{
    uint8_t  buf[8U];
    uint32_t len = 0U;

    /* init(NULL) before initialisation */
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NULL_ARG,
                      config_store_init(NULL));

    /* Initialise properly */
    do_init();

    /* load with NULL pointers */
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NULL_ARG,
                      config_store_load(NULL, &len, sizeof(buf)));
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NULL_ARG,
                      config_store_load(buf, NULL, sizeof(buf)));

    /* save with NULL data */
    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NULL_ARG,
                      config_store_save(NULL, 4U));
}

/* ======================================================================= */
/* TC-CS-012: check_integrity on fresh flash → ERR_NO_VALID_SLOT + event  */
/* ======================================================================= */

void test_TC_CS_012_check_integrity_fresh_flash(void)
{
    do_init();

    TEST_ASSERT_EQUAL(CONFIG_STORE_ERR_NO_VALID_SLOT,
                      config_store_check_integrity());
    TEST_ASSERT_EQUAL(HEALTH_EVENT_CONFIG_NO_VALID_SLOT, g_health_last_event);
}

/* ======================================================================= */
/* TC-CS-013: check_integrity after one valid save → OK, no event          */
/* ======================================================================= */

void test_TC_CS_013_check_integrity_after_save(void)
{
    static const uint8_t blob[4U] = {0xCAU, 0xFEU, 0xBAU, 0xBEU};

    do_init();

    TEST_ASSERT_EQUAL(CONFIG_STORE_OK, config_store_save(blob, sizeof(blob)));

    health_mock_reset();

    TEST_ASSERT_EQUAL(CONFIG_STORE_OK, config_store_check_integrity());
    TEST_ASSERT_EQUAL(0U, g_health_push_calls);
}
