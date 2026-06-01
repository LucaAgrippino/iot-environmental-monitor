/**
 * @file test_i2c_driver_l4.c
 * @brief Unit tests for I2cDriver — STM32L475 (I2C v2) implementation.
 *
 * Covers TC-I2C-L4-001 through TC-I2C-L4-011 per
 * docs/lld/drivers/i2c-driver.md §7.4.
 *
 * Mock strategy: I2C2 is redirected to g_mock_i2c2 via the macro in
 * stm32l475xx.h. GPIO peripherals are available via g_mock_gpio_l4[].
 * setUp() zeroes all mock peripheral state and resets the driver via
 * i2c_reset_for_test().
 */

#include "unity.h"

#include <stdint.h>
#include <string.h>

#include "stm32l475_cmsis_mock.h"
#include "stm32l475xx.h"
#include "i2c_driver.h"
#include "i2c_driver_l4.h"  /* triggers auto-link of i2c_driver_l4.c */

/* ===================================================================== */
/* Helpers                                                               */
/* ===================================================================== */

/* Pre-set ISR flags that happy-path polls expect to find set.           */
static void mock_txis_set(void)  { g_mock_i2c2.ISR |= I2C_ISR_TXIS;  }
static void mock_stopf_set(void) { g_mock_i2c2.ISR |= I2C_ISR_STOPF; }
static void mock_tc_set(void)    { g_mock_i2c2.ISR |= I2C_ISR_TC;    }
static void mock_rxne_set(void)  { g_mock_i2c2.ISR |= I2C_ISR_RXNE;  }

/* Arrange a successful i2c_init(). */
static void arrange_init_success(void)
{
    TEST_ASSERT_EQUAL(I2C_ERR_OK, i2c_init());
}

/* ===================================================================== */
/* Unity setUp / tearDown                                                */
/* ===================================================================== */

void setUp(void)
{
    stm32l475_cmsis_mock_reset();
    i2c_reset_for_test();
}

void tearDown(void)
{
    /* No teardown — fresh state established in setUp(). */
}

/* ===================================================================== */
/* TC-I2C-L4-001: i2c_init happy path                                   */
/* ===================================================================== */

void test_TC_I2C_L4_001_init_enables_peripheral_and_configures_pins(void)
{
    /* Act */
    i2c_err_t err = i2c_init();

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_OK, err);

    /* GPIOB clock enabled (AHB2ENR). */
    TEST_ASSERT_BITS_HIGH(RCC_AHB2ENR_GPIOBEN, g_mock_rcc_l4.AHB2ENR);

    /* I2C2 clock enabled (APB1ENR1). */
    TEST_ASSERT_BITS_HIGH(RCC_APB1ENR1_I2C2EN, g_mock_rcc_l4.APB1ENR1);

    /* PE set in CR1. */
    TEST_ASSERT_BITS_HIGH(I2C_CR1_PE, g_mock_i2c2.CR1);

    /* TIMINGR set to the placeholder constant. */
    TEST_ASSERT_NOT_EQUAL(0U, g_mock_i2c2.TIMINGR);

    /* PB10 (SCL) configured as AF (MODER = 10b = 2). */
    uint32_t moder_pb10 = (g_mock_gpio_l4[1].MODER >> (10U * 2U)) & 0x3U;
    TEST_ASSERT_EQUAL(2U, moder_pb10);

    /* PB10 open-drain. */
    TEST_ASSERT_BITS_HIGH(1UL << 10U, g_mock_gpio_l4[1].OTYPER);

    /* PB11 (SDA) configured as AF. */
    uint32_t moder_pb11 = (g_mock_gpio_l4[1].MODER >> (11U * 2U)) & 0x3U;
    TEST_ASSERT_EQUAL(2U, moder_pb11);

    /* PB11 open-drain. */
    TEST_ASSERT_BITS_HIGH(1UL << 11U, g_mock_gpio_l4[1].OTYPER);
}

/* ===================================================================== */
/* TC-I2C-L4-002: i2c_init called twice (idempotent)                    */
/* ===================================================================== */

void test_TC_I2C_L4_002_init_idempotent(void)
{
    /* Arrange */
    TEST_ASSERT_EQUAL(I2C_ERR_OK, i2c_init());
    g_mock_i2c2.TIMINGR = 0U; /* clear so 2nd call would be detectable */

    /* Act */
    i2c_err_t err = i2c_init();

    /* Assert: second call returns OK but does not rewrite TIMINGR. */
    TEST_ASSERT_EQUAL(I2C_ERR_OK, err);
    TEST_ASSERT_EQUAL(0U, g_mock_i2c2.TIMINGR);
}

/* ===================================================================== */
/* TC-I2C-L4-003: i2c_write happy path — 2 bytes to 0x44               */
/* ===================================================================== */

void test_TC_I2C_L4_003_write_happy_path(void)
{
    /* Arrange */
    arrange_init_success();
    mock_txis_set();
    mock_stopf_set();

    const uint8_t data[2] = {0xABU, 0xCDU};

    /* Act */
    i2c_err_t err = i2c_write(0x44U, data, 2U);

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_OK, err);

    /* CR2 must have been written with SADD=0x88, NBYTES=2, START=1,
     * AUTOEND=1, RD_WRN=0. Check key fields. */
    TEST_ASSERT_BITS_HIGH(I2C_CR2_START,   g_mock_i2c2.CR2);
    TEST_ASSERT_BITS_HIGH(I2C_CR2_AUTOEND, g_mock_i2c2.CR2);
    TEST_ASSERT_BITS_LOW(I2C_CR2_RD_WRN,   g_mock_i2c2.CR2);

    /* SADD = 0x44 << 1 = 0x88. */
    TEST_ASSERT_EQUAL(0x88U, g_mock_i2c2.CR2 & I2C_CR2_SADD);

    /* STOPF was cleared by writing ICR.STOPCF. */
    TEST_ASSERT_BITS_HIGH(I2C_ICR_STOPCF, g_mock_i2c2.ICR);
}

/* ===================================================================== */
/* TC-I2C-L4-004: i2c_write NACK (ISR.NACKF set during TX)             */
/* ===================================================================== */

void test_TC_I2C_L4_004_write_nack(void)
{
    /* Arrange: TXIS never fires, NACKF is set instead. */
    arrange_init_success();
    g_mock_i2c2.ISR |= I2C_ISR_NACKF;

    const uint8_t data[1] = {0x00U};

    /* Act */
    i2c_err_t err = i2c_write(0x44U, data, 1U);

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_NACK, err);

    /* ICR.NACKCF was written to clear NACKF. */
    TEST_ASSERT_BITS_HIGH(I2C_ICR_NACKCF, g_mock_i2c2.ICR);
}

/* ===================================================================== */
/* TC-I2C-L4-005: i2c_write TXIS timeout                                */
/* ===================================================================== */

void test_TC_I2C_L4_005_write_txis_timeout(void)
{
    /* Arrange: TXIS and NACKF both stay clear. */
    arrange_init_success();

    const uint8_t data[1] = {0x00U};

    /* Act */
    i2c_err_t err = i2c_write(0x44U, data, 1U);

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_TIMEOUT, err);

    /* Recovery: PE cleared then re-enabled. */
    TEST_ASSERT_BITS_HIGH(I2C_CR1_PE, g_mock_i2c2.CR1);
}

/* ===================================================================== */
/* TC-I2C-L4-006: i2c_write bus busy at entry                           */
/* ===================================================================== */

void test_TC_I2C_L4_006_write_bus_busy(void)
{
    /* Arrange: ISR.BUSY set. */
    arrange_init_success();
    g_mock_i2c2.ISR |= I2C_ISR_BUSY;

    const uint8_t data[1] = {0x00U};

    /* Act */
    i2c_err_t err = i2c_write(0x44U, data, 1U);

    /* Assert: immediate BUS_BUSY; CR2.START not set. */
    TEST_ASSERT_EQUAL(I2C_ERR_BUS_BUSY, err);
    TEST_ASSERT_BITS_LOW(I2C_CR2_START, g_mock_i2c2.CR2);
}

/* ===================================================================== */
/* TC-I2C-L4-007: i2c_write_read happy path — 1 byte write, 2 read     */
/* ===================================================================== */

void test_TC_I2C_L4_007_write_read_happy_path(void)
{
    /* Arrange */
    arrange_init_success();
    mock_txis_set();
    mock_tc_set();
    mock_rxne_set();
    mock_stopf_set();
    g_mock_i2c2.RXDR = 0xBEU; /* Both rx bytes return this value. */

    const uint8_t tx[1] = {0x28U};
    uint8_t rx[2]       = {0};

    /* Act */
    i2c_err_t err = i2c_write_read(0x1EU, tx, 1U, rx, 2U);

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_OK, err);

    /* Received data filled. */
    TEST_ASSERT_EQUAL_HEX8(0xBEU, rx[0]);
    TEST_ASSERT_EQUAL_HEX8(0xBEU, rx[1]);

    /* Read CR2: RD_WRN=1, AUTOEND=1, START=1. */
    TEST_ASSERT_BITS_HIGH(I2C_CR2_RD_WRN,  g_mock_i2c2.CR2);
    TEST_ASSERT_BITS_HIGH(I2C_CR2_AUTOEND, g_mock_i2c2.CR2);
    TEST_ASSERT_BITS_HIGH(I2C_CR2_START,   g_mock_i2c2.CR2);

    /* STOPF cleared. */
    TEST_ASSERT_BITS_HIGH(I2C_ICR_STOPCF, g_mock_i2c2.ICR);
}

/* ===================================================================== */
/* TC-I2C-L4-008: i2c_write_read NACK during write phase                */
/* ===================================================================== */

void test_TC_I2C_L4_008_write_read_nack_in_write_phase(void)
{
    /* Arrange: NACKF set immediately (device not present). */
    arrange_init_success();
    g_mock_i2c2.ISR |= I2C_ISR_NACKF;

    const uint8_t tx[1] = {0x00U};
    uint8_t rx[2]       = {0};

    /* Act */
    i2c_err_t err = i2c_write_read(0x44U, tx, 1U, rx, 2U);

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_NACK, err);

    /* NACKCF written. */
    TEST_ASSERT_BITS_HIGH(I2C_ICR_NACKCF, g_mock_i2c2.ICR);

    /* Read-phase CR2 not issued: RD_WRN stays clear. */
    TEST_ASSERT_BITS_LOW(I2C_CR2_RD_WRN, g_mock_i2c2.CR2);
}

/* ===================================================================== */
/* TC-I2C-L4-009: i2c_write_read TC timeout after write phase           */
/* ===================================================================== */

void test_TC_I2C_L4_009_write_read_tc_timeout(void)
{
    /* Arrange: TXIS fires (byte transmitted), but TC never fires. */
    arrange_init_success();
    mock_txis_set();
    /* TC stays clear. */

    const uint8_t tx[1] = {0x00U};
    uint8_t rx[2]       = {0};

    /* Act */
    i2c_err_t err = i2c_write_read(0x44U, tx, 1U, rx, 2U);

    /* Assert: timeout, read phase not started. */
    TEST_ASSERT_EQUAL(I2C_ERR_TIMEOUT, err);
    TEST_ASSERT_BITS_LOW(I2C_CR2_RD_WRN, g_mock_i2c2.CR2);
}

/* ===================================================================== */
/* TC-I2C-L4-010: i2c_read happy path — 6 bytes from 0x1E              */
/* ===================================================================== */

void test_TC_I2C_L4_010_read_happy_path_6_bytes(void)
{
    /* Arrange */
    arrange_init_success();
    mock_rxne_set();
    mock_stopf_set();
    g_mock_i2c2.RXDR = 0xAAU;

    uint8_t buf[6] = {0};

    /* Act */
    i2c_err_t err = i2c_read(0x1EU, buf, 6U);

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_OK, err);

    /* CR2: SADD=0x3C (0x1E<<1), NBYTES=6, RD_WRN=1, AUTOEND=1. */
    TEST_ASSERT_EQUAL(0x3CU, g_mock_i2c2.CR2 & I2C_CR2_SADD);
    TEST_ASSERT_BITS_HIGH(I2C_CR2_RD_WRN,  g_mock_i2c2.CR2);
    TEST_ASSERT_BITS_HIGH(I2C_CR2_AUTOEND, g_mock_i2c2.CR2);

    /* All 6 bytes received. */
    for (uint8_t i = 0U; i < 6U; ++i)
    {
        TEST_ASSERT_EQUAL_HEX8(0xAAU, buf[i]);
    }

    /* STOPF cleared. */
    TEST_ASSERT_BITS_HIGH(I2C_ICR_STOPCF, g_mock_i2c2.ICR);
}

/* ===================================================================== */
/* TC-I2C-L4-011: Bus recovery on timeout                               */
/* ===================================================================== */

void test_TC_I2C_L4_011_bus_recovery_on_timeout(void)
{
    /* Arrange: TXIS never fires → timeout → recovery. */
    arrange_init_success();

    const uint8_t data[1] = {0x00U};

    /* Act */
    i2c_err_t err = i2c_write(0x44U, data, 1U);

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_TIMEOUT, err);

    /* PE was cleared and re-enabled by recovery. */
    TEST_ASSERT_BITS_HIGH(I2C_CR1_PE, g_mock_i2c2.CR1);

    /* PB10 (SCL) was reconfigured as output then restored to AF. */
    uint32_t moder_pb10 = (g_mock_gpio_l4[1].MODER >> (10U * 2U)) & 0x3U;
    TEST_ASSERT_EQUAL(2U, moder_pb10); /* AF restored */
}
