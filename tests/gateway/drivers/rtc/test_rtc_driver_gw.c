/**
 * @file test_rtc_driver_gw.c
 * @brief Unit tests for RtcDriver — Gateway (STM32L475).
 *
 * Covers TC-RTC-001 through TC-RTC-034 per docs/lld/drivers/rtc-driver.md §7,
 * mirroring tests/field-device/drivers/rtc/test_rtc_driver.c. RtcDriver is a
 * dedicated implementation per board (firmware/gateway/drivers/rtc/rtc.c),
 * matching DebugUartDriver's two-implementation-file split — Field Device
 * and Gateway are separate CubeIDE projects with separate source trees.
 * The register layout is identical across F469 and L475 (rtc-driver.md
 * §4.4), so only the RCC/PWR register *names* differ here (APB1ENR1 not
 * APB1ENR, PWR->CR1 not CR) and the backup-register range is wider
 * (0..31, not 0..19).
 *
 * Suffixed _gw for the same reason as DebugUartDriver's test target: the
 * Field Device already owns Ceedling target :test_rtc_driver:. There is no
 * production basename collision (rtc.c here vs rtc_driver.c on Field
 * Device), so only the test-target name needed disambiguating.
 */

#include "unity.h"

#include <stdint.h>
#include <string.h>
#include "stm32l475_cmsis_mock.h"
#include "stm32l475xx.h"
#include "rtc.h"

/* ===================================================================== */
/* Local fake tick sources                                               */
/* ===================================================================== */

static uint32_t fake_tick_past_deadline(void)
{
    return UINT32_MAX;
}

static uint32_t g_rsf_latch_calls;
static uint32_t fake_tick_set_rsf_after_2(void)
{
    g_rsf_latch_calls++;
    if (g_rsf_latch_calls == 2U)
    {
        g_mock_rtc.ISR |= RTC_ISR_RSF;
    }
    return g_rsf_latch_calls;
}

static uint32_t g_tc034_tick_calls;
static uint32_t tc034_tick(void)
{
    g_tc034_tick_calls++;
    return UINT32_MAX;
}

/* ===================================================================== */
/* Arrangement helpers                                                   */
/* ===================================================================== */

static void mock_clocks_ready(void)
{
    g_mock_rcc_l4.BDCR = RCC_BDCR_LSERDY;
}

static void mock_initf_and_rsf_set(void)
{
    g_mock_rtc.ISR |= RTC_ISR_INITF | RTC_ISR_RSF;
}

static void arrange_init_warm_path_success(void)
{
    mock_clocks_ready();
    g_mock_rtc.ISR |= RTC_ISR_INITS;
    TEST_ASSERT_EQUAL(RTC_OK, rtc_init());
}

static void arrange_init_cold_path_success(void)
{
    mock_clocks_ready();
    mock_initf_and_rsf_set(); /* INITS stays clear -> cold path */
    TEST_ASSERT_EQUAL(RTC_OK, rtc_init());
}

/* ===================================================================== */
/* Unity setUp / tearDown                                                */
/* ===================================================================== */

void setUp(void)
{
    stm32l475_cmsis_mock_reset();
    rtc_reset_for_test();

    g_tc034_tick_calls = 0U;
    g_rsf_latch_calls = 0U;
}

void tearDown(void)
{
}

/* ===================================================================== */
/* rtc_init — TC-RTC-001..007, 033                                       */
/* ===================================================================== */

void test_TC_RTC_001_init_returns_lse_not_ready_when_lserdy_clear(void)
{
    rtc_err_t err = rtc_init();

    TEST_ASSERT_EQUAL(RTC_ERR_LSE_NOT_READY, err);
    TEST_ASSERT_BITS_LOW(RCC_BDCR_RTCEN, g_mock_rcc_l4.BDCR);
}

void test_TC_RTC_002_init_cold_start_configures_peripheral(void)
{
    mock_clocks_ready();
    mock_initf_and_rsf_set(); /* INITS clear -> cold path */

    rtc_err_t err = rtc_init();

    TEST_ASSERT_EQUAL(RTC_OK, err);
    TEST_ASSERT_BITS_HIGH(RCC_APB1ENR1_PWREN, g_mock_rcc_l4.APB1ENR1);
    TEST_ASSERT_BITS_HIGH(PWR_CR1_DBP, g_mock_pwr.CR1);
    TEST_ASSERT_BITS_HIGH(RCC_BDCR_RTCSEL_0 | RCC_BDCR_RTCEN, g_mock_rcc_l4.BDCR);
    TEST_ASSERT_EQUAL_HEX32((127UL << 16) | 255UL, g_mock_rtc.PRER);
    TEST_ASSERT_EQUAL_HEX32(0x00000000UL, g_mock_rtc.TR);
    TEST_ASSERT_EQUAL_HEX32(0x00002101UL, g_mock_rtc.DR);
    TEST_ASSERT_EQUAL_HEX32(0xFFUL, g_mock_rtc.WPR); /* re-locked */
    TEST_ASSERT_FALSE(rtc_is_backup_valid());
}

void test_TC_RTC_003_init_warm_start_leaves_calendar_untouched(void)
{
    mock_clocks_ready();
    g_mock_rtc.ISR |= RTC_ISR_INITS; /* warm path */
    g_mock_rtc.TR = 0xDEADBEEFUL;
    g_mock_rtc.DR = 0xCAFEBABEUL;
    g_mock_rtc.PRER = 0x12345678UL;

    rtc_err_t err = rtc_init();

    TEST_ASSERT_EQUAL(RTC_OK, err);
    TEST_ASSERT_BITS_HIGH(RCC_APB1ENR1_PWREN, g_mock_rcc_l4.APB1ENR1);
    TEST_ASSERT_BITS_HIGH(PWR_CR1_DBP, g_mock_pwr.CR1);
    TEST_ASSERT_BITS_HIGH(RCC_BDCR_RTCSEL_0 | RCC_BDCR_RTCEN, g_mock_rcc_l4.BDCR);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFUL, g_mock_rtc.TR);
    TEST_ASSERT_EQUAL_HEX32(0xCAFEBABEUL, g_mock_rtc.DR);
    TEST_ASSERT_EQUAL_HEX32(0x12345678UL, g_mock_rtc.PRER);
    TEST_ASSERT_TRUE(rtc_is_backup_valid());
}

void test_TC_RTC_004_init_returns_init_timeout_when_initf_never_asserts(void)
{
    mock_clocks_ready();
    g_mock_rtc.ISR = RTC_ISR_RSF; /* INITF and INITS clear */
    rtc_set_tick_source(fake_tick_past_deadline);

    rtc_err_t err = rtc_init();

    TEST_ASSERT_EQUAL(RTC_ERR_INIT_TIMEOUT, err);
    TEST_ASSERT_EQUAL_HEX32(0xFFUL, g_mock_rtc.WPR);
}

void test_TC_RTC_005_init_returns_sync_timeout_when_rsf_never_asserts(void)
{
    mock_clocks_ready();
    g_mock_rtc.ISR = RTC_ISR_INITF; /* RSF and INITS clear */
    rtc_set_tick_source(fake_tick_past_deadline);

    rtc_err_t err = rtc_init();

    TEST_ASSERT_EQUAL(RTC_ERR_SYNC_TIMEOUT, err);
    TEST_ASSERT_EQUAL_HEX32(0xFFUL, g_mock_rtc.WPR);
}

void test_TC_RTC_006_init_is_idempotent_on_second_call(void)
{
    arrange_init_cold_path_success();
    uint32_t pwr_cr1_after_first = g_mock_pwr.CR1;
    uint32_t bdcr_after_first = g_mock_rcc_l4.BDCR;
    uint32_t prer_after_first = g_mock_rtc.PRER;

    rtc_err_t err = rtc_init();

    TEST_ASSERT_EQUAL(RTC_OK, err);
    TEST_ASSERT_EQUAL_HEX32(pwr_cr1_after_first, g_mock_pwr.CR1);
    TEST_ASSERT_EQUAL_HEX32(bdcr_after_first, g_mock_rcc_l4.BDCR);
    TEST_ASSERT_EQUAL_HEX32(prer_after_first, g_mock_rtc.PRER);
}

void test_TC_RTC_007_init_locks_wpr_at_exit(void)
{
    mock_clocks_ready();
    mock_initf_and_rsf_set();

    TEST_ASSERT_EQUAL(RTC_OK, rtc_init());

    TEST_ASSERT_EQUAL_HEX32(0xFFUL, g_mock_rtc.WPR);
}

void test_TC_RTC_033_init_returns_lse_not_ready_when_rtcsel_locked_to_non_lse(void)
{
    g_mock_rcc_l4.BDCR = RCC_BDCR_LSERDY | RCC_BDCR_RTCSEL_1;

    rtc_err_t err = rtc_init();

    TEST_ASSERT_EQUAL(RTC_ERR_LSE_NOT_READY, err);
    TEST_ASSERT_BITS_LOW(RCC_BDCR_RTCEN, g_mock_rcc_l4.BDCR);
    TEST_ASSERT_BITS_LOW(RCC_BDCR_BDRST, g_mock_rcc_l4.BDCR);
}

/* ===================================================================== */
/* rtc_get_time — TC-RTC-008..013                                        */
/* ===================================================================== */

void test_TC_RTC_008_get_time_rejects_null_argument(void)
{
    arrange_init_warm_path_success();

    rtc_err_t err = rtc_get_time(NULL);

    TEST_ASSERT_EQUAL(RTC_ERR_NULL_ARG, err);
}

void test_TC_RTC_009_get_time_decodes_typical_datetime(void)
{
    arrange_init_warm_path_success();
    g_mock_rtc.ISR |= RTC_ISR_RSF;
    g_mock_rtc.TR = 0x00162359UL; /* 16:23:59 */
    g_mock_rtc.DR = 0x00261231UL; /* 2026-12-31 */
    rtc_datetime_t dt = {0};

    rtc_err_t err = rtc_get_time(&dt);

    TEST_ASSERT_EQUAL(RTC_OK, err);
    TEST_ASSERT_EQUAL_UINT16(2026, dt.year);
    TEST_ASSERT_EQUAL_UINT8(12, dt.month);
    TEST_ASSERT_EQUAL_UINT8(31, dt.day);
    TEST_ASSERT_EQUAL_UINT8(16, dt.hour);
    TEST_ASSERT_EQUAL_UINT8(23, dt.minute);
    TEST_ASSERT_EQUAL_UINT8(59, dt.second);
}

void test_TC_RTC_010_get_time_decodes_low_boundary(void)
{
    arrange_init_warm_path_success();
    g_mock_rtc.ISR |= RTC_ISR_RSF;
    g_mock_rtc.TR = 0x00000000UL;
    g_mock_rtc.DR = 0x00002101UL; /* 2000-01-01 */
    rtc_datetime_t dt = {0};

    rtc_err_t err = rtc_get_time(&dt);

    TEST_ASSERT_EQUAL(RTC_OK, err);
    TEST_ASSERT_EQUAL_UINT16(2000, dt.year);
    TEST_ASSERT_EQUAL_UINT8(1, dt.month);
    TEST_ASSERT_EQUAL_UINT8(1, dt.day);
    TEST_ASSERT_EQUAL_UINT8(0, dt.hour);
    TEST_ASSERT_EQUAL_UINT8(0, dt.minute);
    TEST_ASSERT_EQUAL_UINT8(0, dt.second);
}

void test_TC_RTC_011_get_time_decodes_high_boundary(void)
{
    arrange_init_warm_path_success();
    g_mock_rtc.ISR |= RTC_ISR_RSF;
    g_mock_rtc.TR = 0x00235959UL; /* 23:59:59 */
    g_mock_rtc.DR = 0x00991231UL; /* 2099-12-31 */
    rtc_datetime_t dt = {0};

    rtc_err_t err = rtc_get_time(&dt);

    TEST_ASSERT_EQUAL(RTC_OK, err);
    TEST_ASSERT_EQUAL_UINT16(2099, dt.year);
    TEST_ASSERT_EQUAL_UINT8(12, dt.month);
    TEST_ASSERT_EQUAL_UINT8(31, dt.day);
    TEST_ASSERT_EQUAL_UINT8(23, dt.hour);
    TEST_ASSERT_EQUAL_UINT8(59, dt.minute);
    TEST_ASSERT_EQUAL_UINT8(59, dt.second);
}

void test_TC_RTC_012_get_time_polls_until_rsf_latches(void)
{
    arrange_init_warm_path_success();
    g_mock_rtc.TR = 0x00162359UL;
    g_mock_rtc.DR = 0x00261231UL;
    rtc_set_tick_source(fake_tick_set_rsf_after_2);
    rtc_datetime_t dt = {0};

    rtc_err_t err = rtc_get_time(&dt);

    TEST_ASSERT_EQUAL(RTC_OK, err);
    TEST_ASSERT_EQUAL_UINT16(2026, dt.year);
    TEST_ASSERT_EQUAL_UINT8(59, dt.second);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2U, g_rsf_latch_calls);
}

void test_TC_RTC_013_get_time_returns_sync_timeout_when_rsf_never_sets(void)
{
    arrange_init_warm_path_success();
    rtc_set_tick_source(fake_tick_past_deadline);
    rtc_datetime_t dt = {
        .year = 9999, .month = 99, .day = 99, .hour = 99, .minute = 99, .second = 99};

    rtc_err_t err = rtc_get_time(&dt);

    TEST_ASSERT_EQUAL(RTC_ERR_SYNC_TIMEOUT, err);
    TEST_ASSERT_EQUAL_UINT16(0, dt.year);
    TEST_ASSERT_EQUAL_UINT8(0, dt.month);
}

/* ===================================================================== */
/* rtc_set_time — TC-RTC-014..018                                        */
/* ===================================================================== */

void test_TC_RTC_014_set_time_rejects_null_argument(void)
{
    arrange_init_warm_path_success();
    uint32_t wpr_before = g_mock_rtc.WPR;

    rtc_err_t err = rtc_set_time(NULL);

    TEST_ASSERT_EQUAL(RTC_ERR_NULL_ARG, err);
    TEST_ASSERT_EQUAL_HEX32(wpr_before, g_mock_rtc.WPR);
}

void test_TC_RTC_015_set_time_writes_packed_tr_dr_and_relocks_wpr(void)
{
    arrange_init_warm_path_success();
    g_mock_rtc.ISR |= RTC_ISR_INITF | RTC_ISR_RSF;
    rtc_datetime_t dt = {
        .year = 2026, .month = 5, .day = 16, .hour = 10, .minute = 30, .second = 0};

    rtc_err_t err = rtc_set_time(&dt);

    TEST_ASSERT_EQUAL(RTC_OK, err);
    TEST_ASSERT_EQUAL_HEX32(0x00103000UL, g_mock_rtc.TR);
    TEST_ASSERT_EQUAL_HEX32((0x26UL << 16) | (1UL << 13) | (0x05UL << 8) | 0x16UL, g_mock_rtc.DR);
    TEST_ASSERT_EQUAL_HEX32(0xFFUL, g_mock_rtc.WPR);
}

void test_TC_RTC_016_set_time_returns_init_timeout_when_initf_never_asserts(void)
{
    arrange_init_warm_path_success();
    rtc_set_tick_source(fake_tick_past_deadline);
    rtc_datetime_t dt = {
        .year = 2026, .month = 5, .day = 16, .hour = 10, .minute = 30, .second = 0};

    rtc_err_t err = rtc_set_time(&dt);

    TEST_ASSERT_EQUAL(RTC_ERR_INIT_TIMEOUT, err);
    TEST_ASSERT_EQUAL_HEX32(0xFFUL, g_mock_rtc.WPR);
    TEST_ASSERT_EQUAL_HEX32(0x00000000UL, g_mock_rtc.TR);
}

void test_TC_RTC_017_set_time_returns_sync_timeout_when_rsf_never_asserts(void)
{
    arrange_init_warm_path_success();
    g_mock_rtc.ISR |= RTC_ISR_INITF;
    rtc_set_tick_source(fake_tick_past_deadline);
    rtc_datetime_t dt = {
        .year = 2026, .month = 5, .day = 16, .hour = 10, .minute = 30, .second = 0};

    rtc_err_t err = rtc_set_time(&dt);

    TEST_ASSERT_EQUAL(RTC_ERR_SYNC_TIMEOUT, err);
    TEST_ASSERT_EQUAL_HEX32(0xFFUL, g_mock_rtc.WPR);
}

void test_TC_RTC_018_set_time_does_not_modify_backup_valid(void)
{
    arrange_init_cold_path_success();
    TEST_ASSERT_FALSE(rtc_is_backup_valid());

    g_mock_rtc.ISR |= RTC_ISR_INITF | RTC_ISR_RSF;
    rtc_datetime_t dt = {
        .year = 2026, .month = 5, .day = 16, .hour = 10, .minute = 30, .second = 0};

    TEST_ASSERT_EQUAL(RTC_OK, rtc_set_time(&dt));

    TEST_ASSERT_FALSE(rtc_is_backup_valid());
}

/* ===================================================================== */
/* rtc_is_backup_valid — TC-RTC-019, 020                                 */
/* ===================================================================== */

void test_TC_RTC_019_is_backup_valid_returns_false_after_cold_init(void)
{
    arrange_init_cold_path_success();

    TEST_ASSERT_FALSE(rtc_is_backup_valid());
}

void test_TC_RTC_020_is_backup_valid_returns_true_after_warm_init(void)
{
    arrange_init_warm_path_success();

    TEST_ASSERT_TRUE(rtc_is_backup_valid());
}

/* ===================================================================== */
/* Backup registers — TC-RTC-021..028 (L475: 0..31)                      */
/* ===================================================================== */

void test_TC_RTC_021_backup_write_then_read_round_trip(void)
{
    arrange_init_warm_path_success();

    TEST_ASSERT_EQUAL(RTC_OK, rtc_write_backup(0U, 0xA5A55A5AUL));
    TEST_ASSERT_EQUAL_HEX32(0xA5A55A5AUL, g_mock_rtc.BKP0R);

    uint32_t v = 0;
    TEST_ASSERT_EQUAL(RTC_OK, rtc_read_backup(0U, &v));
    TEST_ASSERT_EQUAL_HEX32(0xA5A55A5AUL, v);
}

void test_TC_RTC_022_backup_write_zero_then_read_zero(void)
{
    arrange_init_warm_path_success();

    TEST_ASSERT_EQUAL(RTC_OK, rtc_write_backup(0U, 0UL));
    uint32_t v = 0xFFFFFFFFUL;
    TEST_ASSERT_EQUAL(RTC_OK, rtc_read_backup(0U, &v));
    TEST_ASSERT_EQUAL_HEX32(0UL, v);
}

void test_TC_RTC_023_read_backup_rejects_null_out(void)
{
    arrange_init_warm_path_success();

    rtc_err_t err = rtc_read_backup(0U, NULL);

    TEST_ASSERT_EQUAL(RTC_ERR_NULL_ARG, err);
}

void test_TC_RTC_024_read_backup_at_max_valid_index_l475(void)
{
    arrange_init_warm_path_success();
    g_mock_rtc.BKP31R = 0xDEADBEEFUL;

    uint32_t v = 0;
    rtc_err_t err = rtc_read_backup(RTC_BACKUP_MAX_IDX_L475, &v);

    TEST_ASSERT_EQUAL(RTC_OK, err);
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFUL, v);
}

void test_TC_RTC_025_read_backup_at_first_invalid_index_l475(void)
{
    arrange_init_warm_path_success();

    uint32_t v = 0;
    rtc_err_t err = rtc_read_backup(RTC_BACKUP_MAX_IDX_L475 + 1U, &v);

    TEST_ASSERT_EQUAL(RTC_ERR_BACKUP_BOUNDS, err);
    TEST_ASSERT_EQUAL_HEX32(0UL, v); /* untouched */
}

void test_TC_RTC_026_read_backup_with_obvious_overflow_index(void)
{
    arrange_init_warm_path_success();

    uint32_t v = 0;
    rtc_err_t err = rtc_read_backup(0xFFU, &v);

    TEST_ASSERT_EQUAL(RTC_ERR_BACKUP_BOUNDS, err);
}

void test_TC_RTC_027_write_backup_at_max_valid_index_l475(void)
{
    arrange_init_warm_path_success();

    rtc_err_t err = rtc_write_backup(RTC_BACKUP_MAX_IDX_L475, 0xCAFEF00DUL);

    TEST_ASSERT_EQUAL(RTC_OK, err);
    TEST_ASSERT_EQUAL_HEX32(0xCAFEF00DUL, g_mock_rtc.BKP31R);
}

void test_TC_RTC_028_write_backup_at_first_invalid_index_l475(void)
{
    arrange_init_warm_path_success();

    rtc_err_t err = rtc_write_backup(RTC_BACKUP_MAX_IDX_L475 + 1U, 0xCAFEF00DUL);

    TEST_ASSERT_EQUAL(RTC_ERR_BACKUP_BOUNDS, err);
    TEST_ASSERT_EQUAL_HEX32(0UL, g_mock_rtc.BKP0R);
    TEST_ASSERT_EQUAL_HEX32(0UL, g_mock_rtc.BKP31R);
}

/* ===================================================================== */
/* Post-init state checks — TC-RTC-029..031                              */
/* ===================================================================== */

void test_TC_RTC_029_init_sets_pwr_dbp(void)
{
    arrange_init_warm_path_success();

    TEST_ASSERT_BITS_HIGH(PWR_CR1_DBP, g_mock_pwr.CR1);
}

void test_TC_RTC_030_init_enables_pwr_clock(void)
{
    arrange_init_warm_path_success();

    TEST_ASSERT_BITS_HIGH(RCC_APB1ENR1_PWREN, g_mock_rcc_l4.APB1ENR1);
}

void test_TC_RTC_031_init_selects_lse_and_enables_rtc_clock(void)
{
    arrange_init_warm_path_success();

    TEST_ASSERT_BITS_HIGH(RCC_BDCR_RTCSEL_0, g_mock_rcc_l4.BDCR);
    TEST_ASSERT_BITS_LOW(RCC_BDCR_RTCSEL_1, g_mock_rcc_l4.BDCR);
    TEST_ASSERT_BITS_HIGH(RCC_BDCR_RTCEN, g_mock_rcc_l4.BDCR);
}

/* ===================================================================== */
/* BCD helpers — TC-RTC-032                                              */
/* ===================================================================== */

void test_TC_RTC_032_bcd_round_trip_covers_all_valid_values(void)
{
    for (uint8_t tens = 0U; tens < 10U; tens++)
    {
        for (uint8_t units = 0U; units < 10U; units++)
        {
            uint8_t bin = (uint8_t) (tens * 10U + units);
            uint8_t bcd = rtc_bin_to_bcd(bin);
            uint8_t round_trip = rtc_bcd_to_bin(bcd);
            TEST_ASSERT_EQUAL_UINT8(bin, round_trip);
        }
    }
}

/* ===================================================================== */
/* Tick-source override — TC-RTC-034                                     */
/* ===================================================================== */

void test_TC_RTC_034_set_tick_source_drives_init_timeout(void)
{
    mock_clocks_ready();
    g_mock_rtc.ISR = RTC_ISR_RSF;
    rtc_set_tick_source(tc034_tick);

    rtc_err_t err = rtc_init();

    TEST_ASSERT_EQUAL(RTC_ERR_INIT_TIMEOUT, err);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(2U, g_tc034_tick_calls);
}
