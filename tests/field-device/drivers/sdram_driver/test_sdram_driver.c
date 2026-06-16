/**
 * @file test_sdram_driver.c
 * @brief Unity unit tests for SdramDriver — TC-SDRAM-001..004.
 *
 * FMC peripheral state is controlled via the mock stm32f469xx.h.
 * SDSR.BUSY is left at 0 for happy-path tests (wait_busy returns
 * immediately); set to FMC_SDSR_BUSY for timeout tests.
 *
 * Build defines required: STM32F469xx, TEST (see project.yml).
 */

#include "unity.h"
#include "stm32_cmsis_mock.h"
#include "sdram_driver.h"

/* ====================================================================== */
/* Constants mirrored from sdram_driver.c for assertion checks           */
/* ====================================================================== */

/* SDCR1 expected value (see companion §3.3) */
#define EXP_SDCR1 \
    ((0x00UL <<  0U) | /* NC  = 8 cols  */  \
     (0x01UL <<  2U) | /* NR  = 12 rows */  \
     (0x02UL <<  4U) | /* MWID = 32-bit */  \
     (0x01UL <<  6U) | /* NB  = 4 banks */  \
     (0x03UL <<  7U) | /* CAS = 3       */  \
     (0x00UL <<  9U) | /* WP  = off     */  \
     (0x02UL << 10U) | /* SDCLK = /2    */  \
     (0x01UL << 12U) | /* RBURST = on   */  \
     (0x00UL << 13U))  /* RPIPE = 0     */

/* SDTR1 expected value */
#define EXP_SDTR1 \
    ((1UL <<  0U) | /* TMRD */  \
     (6UL <<  4U) | /* TXSR */  \
     (3UL <<  8U) | /* TRAS */  \
     (5UL << 12U) | /* TRC  */  \
     (1UL << 16U) | /* TWR  */  \
     (1UL << 20U) | /* TRP  */  \
     (1UL << 24U))  /* TRCD */

/* SDRTR COUNT field: 1386 << 1 (COUNT occupies bits [13:1]) */
#define EXP_SDRTR_BITS (1386UL << 1U)

/* FMC SDSR BUSY bit (bit 5) */
#define FMC_SDSR_BUSY_BIT (0x20UL)

/* ====================================================================== */
/* setUp / tearDown                                                       */
/* ====================================================================== */

void setUp(void)
{
    stm32_cmsis_mock_reset();
    sdram_driver_reset();
}

void tearDown(void) {}

/* ====================================================================== */
/* TC-SDRAM-001: Happy path — BUSY clears immediately                    */
/* ====================================================================== */

void test_TC_SDRAM_001_init_happy_path(void)
{
    /* SDSR.BUSY = 0 → wait_busy returns OK on first read. */
    g_mock_fmc_bank5_6.SDSR = 0U;

    sdram_err_t err = sdram_init();

    TEST_ASSERT_EQUAL(SDRAM_ERR_OK, err);
    TEST_ASSERT_TRUE(s_initialised);

    /* FMC clock enabled. */
    TEST_ASSERT_BITS_HIGH(RCC_AHB3ENR_FMCEN, g_mock_rcc.AHB3ENR);

    /* Control and timing registers written correctly. */
    TEST_ASSERT_EQUAL_HEX32(EXP_SDCR1, g_mock_fmc_bank5_6.SDCR[0]);
    TEST_ASSERT_EQUAL_HEX32(EXP_SDTR1, g_mock_fmc_bank5_6.SDTR[0]);

    /* Refresh timer programmed — COUNT field in bits [13:1]. */
    TEST_ASSERT_BITS_HIGH(EXP_SDRTR_BITS, g_mock_fmc_bank5_6.SDRTR);
}

/* ====================================================================== */
/* TC-SDRAM-002: Timeout — BUSY never clears                             */
/* ====================================================================== */

void test_TC_SDRAM_002_init_timeout(void)
{
    /* SDSR.BUSY stuck high → wait_busy exhausts its loop. */
    g_mock_fmc_bank5_6.SDSR = FMC_SDSR_BUSY_BIT;

    sdram_err_t err = sdram_init();

    TEST_ASSERT_EQUAL(SDRAM_ERR_TIMEOUT, err);
    TEST_ASSERT_FALSE(s_initialised);
}

/* ====================================================================== */
/* TC-SDRAM-003: sdram_get_base_addr returns 0xC0000000                  */
/* ====================================================================== */

void test_TC_SDRAM_003_get_base_addr(void)
{
    TEST_ASSERT_EQUAL_HEX32(0xC0000000UL, sdram_get_base_addr());
}

/* ====================================================================== */
/* TC-SDRAM-004: Re-init — second call re-runs sequence, returns OK      */
/* ====================================================================== */

void test_TC_SDRAM_004_reinit_idempotent(void)
{
    /* First init. */
    g_mock_fmc_bank5_6.SDSR = 0U;
    TEST_ASSERT_EQUAL(SDRAM_ERR_OK, sdram_init());
    TEST_ASSERT_TRUE(s_initialised);

    /* Reset peripheral mock state but leave driver state untouched. */
    stm32_cmsis_mock_reset();
    /* Confirm s_initialised is still true after mock reset. */
    TEST_ASSERT_TRUE(s_initialised);

    /* Second init — driver has no re-init guard; re-runs the sequence. */
    g_mock_fmc_bank5_6.SDSR = 0U;
    sdram_err_t err = sdram_init();
    TEST_ASSERT_EQUAL(SDRAM_ERR_OK, err);
    TEST_ASSERT_TRUE(s_initialised);
    TEST_ASSERT_EQUAL_HEX32(EXP_SDCR1, g_mock_fmc_bank5_6.SDCR[0]);
}
