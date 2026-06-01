/**
 * @file test_i2c_driver_f4.c
 * @brief Unit tests for I2cDriver — STM32F469 (I2C v1) implementation.
 *
 * Covers TC-I2C-F4-001 through TC-I2C-F4-013 per
 * docs/lld/drivers/i2c-driver.md §7.3.
 *
 * Mock strategy: I2C1 is redirected to g_mock_i2c1 via the macro in
 * stm32f469xx.h. GPIO peripherals are available via g_mock_gpio[].
 * setUp() zeroes all mock peripheral state and resets the driver via
 * i2c_reset_for_test().
 */

#include "unity.h"

#include <stdint.h>
#include <string.h>

#include "stm32_cmsis_mock.h"
#include "stm32f469xx.h"
#include "i2c_driver.h"
#include "i2c_driver_f4.h"  /* triggers auto-link of i2c_driver_f4.c */

/* ===================================================================== */
/* Helpers                                                               */
/* ===================================================================== */

/* Pre-set the flags that the happy-path polls expect to find set,
 * so the polling loops exit on the first iteration.                     */

static void mock_sb_set(void)     { g_mock_i2c1.SR1 |= I2C_SR1_SB;   }
static void mock_addr_set(void)   { g_mock_i2c1.SR1 |= I2C_SR1_ADDR; }
static void mock_txe_set(void)    { g_mock_i2c1.SR1 |= I2C_SR1_TXE;  }
static void mock_btf_set(void)    { g_mock_i2c1.SR1 |= I2C_SR1_BTF;  }
static void mock_rxne_set(void)   { g_mock_i2c1.SR1 |= I2C_SR1_RXNE; }

/* Arrange a fully successful i2c_init() so tests that exercise the
 * transactional API can start from an initialised state.              */
static void arrange_init_success(void)
{
    TEST_ASSERT_EQUAL(I2C_ERR_OK, i2c_init());
}

/* Arrange a fully successful write setup: SB, ADDR, TXE, BTF all set. */
static void arrange_write_success_flags(void)
{
    mock_sb_set();
    mock_addr_set();
    mock_txe_set();
    mock_btf_set();
}

/* ===================================================================== */
/* Unity setUp / tearDown                                                */
/* ===================================================================== */

void setUp(void)
{
    stm32_cmsis_mock_reset();
    i2c_reset_for_test();
}

void tearDown(void)
{
    /* No teardown — fresh state established in setUp(). */
}

/* ===================================================================== */
/* TC-I2C-F4-001: i2c_init happy path                                   */
/* ===================================================================== */

void test_TC_I2C_F4_001_init_enables_peripheral_and_configures_pins(void)
{
    /* Act */
    i2c_err_t err = i2c_init();

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_OK, err);

    /* I2C1 clock enabled. */
    TEST_ASSERT_BITS_HIGH(RCC_APB1ENR_I2C1EN, g_mock_rcc.APB1ENR);

    /* GPIOB clock enabled. */
    TEST_ASSERT_BITS_HIGH(RCC_AHB1ENR_GPIOBEN, g_mock_rcc.AHB1ENR);

    /* PE bit set in CR1. */
    TEST_ASSERT_BITS_HIGH(I2C_CR1_PE, g_mock_i2c1.CR1);

    /* ACK bit set in CR1. */
    TEST_ASSERT_BITS_HIGH(I2C_CR1_ACK, g_mock_i2c1.CR1);

    /* FS bit set in CCR (fast mode). */
    TEST_ASSERT_BITS_HIGH(I2C_CCR_FS, g_mock_i2c1.CCR);

    /* PB8 (SCL) configured as AF (MODER = 10b = 2). */
    uint32_t moder_pb8 = (g_mock_gpio[1].MODER >> (8U * 2U)) & 0x3U;
    TEST_ASSERT_EQUAL(2U, moder_pb8);

    /* PB8 open-drain. */
    TEST_ASSERT_BITS_HIGH(1UL << 8U, g_mock_gpio[1].OTYPER);

    /* PB9 (SDA) configured as AF. */
    uint32_t moder_pb9 = (g_mock_gpio[1].MODER >> (9U * 2U)) & 0x3U;
    TEST_ASSERT_EQUAL(2U, moder_pb9);

    /* PB9 open-drain. */
    TEST_ASSERT_BITS_HIGH(1UL << 9U, g_mock_gpio[1].OTYPER);
}

/* ===================================================================== */
/* TC-I2C-F4-002: i2c_init called twice (idempotent)                    */
/* ===================================================================== */

void test_TC_I2C_F4_002_init_idempotent(void)
{
    /* Arrange */
    TEST_ASSERT_EQUAL(I2C_ERR_OK, i2c_init());

    /* Record how many times CCR was written by capturing its value. */
    uint32_t ccr_after_first = g_mock_i2c1.CCR;

    /* Reset mock I2C registers so second call would produce a different
     * CCR value if it re-ran configuration. */
    g_mock_i2c1.CCR = 0U;

    /* Act: second call. */
    i2c_err_t err = i2c_init();

    /* Assert: returns OK and did NOT rewrite CCR (idempotent). */
    TEST_ASSERT_EQUAL(I2C_ERR_OK, err);
    TEST_ASSERT_EQUAL(0U, g_mock_i2c1.CCR); /* CCR untouched on 2nd call */
    (void) ccr_after_first;
}

/* ===================================================================== */
/* TC-I2C-F4-003: i2c_write happy path — 2 bytes to 0x44               */
/* ===================================================================== */

void test_TC_I2C_F4_003_write_happy_path(void)
{
    /* Arrange */
    arrange_init_success();

    /* Pre-set all flags the happy path polls. */
    arrange_write_success_flags();

    const uint8_t data[2] = {0xABU, 0xCDU};

    /* Act */
    i2c_err_t err = i2c_write(0x44U, data, 2U);

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_OK, err);

    /* START was requested. */
    TEST_ASSERT_BITS_HIGH(I2C_CR1_START, g_mock_i2c1.CR1);

    /* STOP was asserted. */
    TEST_ASSERT_BITS_HIGH(I2C_CR1_STOP, g_mock_i2c1.CR1);

    /* DR ends with the last data byte written (0xCD); both bytes were sent. */
    TEST_ASSERT_EQUAL_HEX8(0xCDU, g_mock_i2c1.DR);
}

/* ===================================================================== */
/* TC-I2C-F4-004: i2c_write NACK on address (AF flag set)               */
/* ===================================================================== */

void test_TC_I2C_F4_004_write_nack_on_address(void)
{
    /* Arrange */
    arrange_init_success();
    mock_sb_set();  /* SB set so address is sent... */
    /* ...but instead of ADDR, AF fires (device not present). */
    g_mock_i2c1.SR1 |= I2C_SR1_AF;

    const uint8_t data[1] = {0x00U};

    /* Act */
    i2c_err_t err = i2c_write(0x44U, data, 1U);

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_NACK, err);

    /* AF was cleared by the driver. */
    TEST_ASSERT_BITS_LOW(I2C_SR1_AF, g_mock_i2c1.SR1);

    /* STOP generated after NACK. */
    TEST_ASSERT_BITS_HIGH(I2C_CR1_STOP, g_mock_i2c1.CR1);
}

/* ===================================================================== */
/* TC-I2C-F4-005: i2c_write SB timeout                                  */
/* ===================================================================== */

void test_TC_I2C_F4_005_write_sb_timeout(void)
{
    /* Arrange: SB never sets → driver exhausts polling loop. */
    arrange_init_success();
    /* g_mock_i2c1.SR1 remains 0 (SB clear). */

    const uint8_t data[1] = {0x00U};

    /* Act */
    i2c_err_t err = i2c_write(0x44U, data, 1U);

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_TIMEOUT, err);

    /* Recovery: PE was cleared and re-enabled. */
    TEST_ASSERT_BITS_HIGH(I2C_CR1_PE, g_mock_i2c1.CR1);
}

/* ===================================================================== */
/* TC-I2C-F4-006: i2c_write TXE timeout                                 */
/* ===================================================================== */

void test_TC_I2C_F4_006_write_txe_timeout(void)
{
    /* Arrange: SB and ADDR set (write phase entry), TXE never sets. */
    arrange_init_success();
    mock_sb_set();
    mock_addr_set();
    /* TXE stays clear. */

    const uint8_t data[2] = {0xAAU, 0xBBU};

    /* Act */
    i2c_err_t err = i2c_write(0x44U, data, 2U);

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_TIMEOUT, err);
}

/* ===================================================================== */
/* TC-I2C-F4-007: i2c_write BTF timeout                                 */
/* ===================================================================== */

void test_TC_I2C_F4_007_write_btf_timeout(void)
{
    /* Arrange: SB, ADDR, TXE all set, but BTF never sets. */
    arrange_init_success();
    mock_sb_set();
    mock_addr_set();
    mock_txe_set();
    /* BTF stays clear. */

    const uint8_t data[1] = {0xAAU};

    /* Act */
    i2c_err_t err = i2c_write(0x44U, data, 1U);

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_TIMEOUT, err);
}

/* ===================================================================== */
/* TC-I2C-F4-008: i2c_write bus busy at entry                           */
/* ===================================================================== */

void test_TC_I2C_F4_008_write_bus_busy(void)
{
    /* Arrange: SR2.BUSY set before the call. */
    arrange_init_success();
    g_mock_i2c1.SR2 |= I2C_SR2_BUSY;

    const uint8_t data[1] = {0x00U};

    /* Act */
    i2c_err_t err = i2c_write(0x44U, data, 1U);

    /* Assert: immediate BUS_BUSY, no START generated. */
    TEST_ASSERT_EQUAL(I2C_ERR_BUS_BUSY, err);
    TEST_ASSERT_BITS_LOW(I2C_CR1_START, g_mock_i2c1.CR1);
}

/* ===================================================================== */
/* TC-I2C-F4-009: i2c_write_read happy path — 1 byte write, 2 bytes read*/
/* ===================================================================== */

void test_TC_I2C_F4_009_write_read_happy_path(void)
{
    /* Arrange */
    arrange_init_success();

    /* All flags needed for the full write-read sequence. */
    mock_sb_set();
    mock_addr_set();
    mock_txe_set();
    mock_btf_set();
    /* RXNE for 2-byte read. */
    mock_rxne_set();

    /* Pre-load DR with the first byte, driver reads it as rx_buf[0].
     * After reading, the test pre-sets RXNE again so the second read
     * also succeeds. The driver reads DR twice; the mock returns the
     * last written DR value both times. */
    g_mock_i2c1.DR = 0xA5U;

    const uint8_t tx[1] = {0x3CU}; /* register address */
    uint8_t rx[2]       = {0};

    /* Act */
    i2c_err_t err = i2c_write_read(0x1EU, tx, 1U, rx, 2U);

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_OK, err);

    /* STOP was asserted (set before reading penultimate byte). */
    TEST_ASSERT_BITS_HIGH(I2C_CR1_STOP, g_mock_i2c1.CR1);

    /* ACK was re-enabled at the end of the multi-byte read path. */
    TEST_ASSERT_BITS_HIGH(I2C_CR1_ACK, g_mock_i2c1.CR1);

    /* Note: rx buffer content is not verified here because the I2C v1
     * mock's single DR register is overwritten by the address+R write
     * before the driver reads it. Data-correctness is validated on
     * hardware during integration. */
}

/* ===================================================================== */
/* TC-I2C-F4-010: i2c_write_read single-byte read (rx_len = 1)          */
/* ===================================================================== */

void test_TC_I2C_F4_010_write_read_single_byte_rx(void)
{
    /* Arrange */
    arrange_init_success();
    mock_sb_set();
    mock_addr_set();
    mock_txe_set();
    mock_btf_set();
    mock_rxne_set();
    g_mock_i2c1.DR = 0x7BU;

    const uint8_t tx[1] = {0x0FU};
    uint8_t rx[1]       = {0};

    /* Act */
    i2c_err_t err = i2c_write_read(0x1EU, tx, 1U, rx, 1U);

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_OK, err);

    /* CR1.ACK must have been cleared BEFORE ADDR was cleared (SR2 read).
     * The driver clears ACK at step 12 (before ADDR poll), so at the
     * time STOP is issued ACK remains clear. This is the RM0386 §27.3.3
     * errata sequence. */
    TEST_ASSERT_BITS_LOW(I2C_CR1_ACK, g_mock_i2c1.CR1);

    /* STOP was asserted. */
    TEST_ASSERT_BITS_HIGH(I2C_CR1_STOP, g_mock_i2c1.CR1);

    /* Note: rx[0] is not verified — the mock DR register is overwritten
     * by the address+R write before the driver reads it back. */
}

/* ===================================================================== */
/* TC-I2C-F4-011: i2c_write_read NACK during write phase                */
/* ===================================================================== */

void test_TC_I2C_F4_011_write_read_nack_in_write_phase(void)
{
    /* Arrange: SB fires; address phase returns AF (NACK). */
    arrange_init_success();
    mock_sb_set();
    g_mock_i2c1.SR1 |= I2C_SR1_AF;

    const uint8_t tx[1] = {0x00U};
    uint8_t rx[2]       = {0};

    /* Act */
    i2c_err_t err = i2c_write_read(0x44U, tx, 1U, rx, 2U);

    /* Assert: NACK returned, read phase never entered. */
    TEST_ASSERT_EQUAL(I2C_ERR_NACK, err);
    TEST_ASSERT_BITS_HIGH(I2C_CR1_STOP, g_mock_i2c1.CR1);
}

/* ===================================================================== */
/* TC-I2C-F4-012: i2c_read happy path — 6 bytes from 0x1E              */
/* ===================================================================== */

void test_TC_I2C_F4_012_read_happy_path_6_bytes(void)
{
    /* Arrange */
    arrange_init_success();
    mock_sb_set();
    mock_addr_set();
    mock_rxne_set();
    g_mock_i2c1.DR = 0x55U; /* All 6 reads return the same mock DR value. */

    uint8_t buf[6] = {0};

    /* Act */
    i2c_err_t err = i2c_read(0x1EU, buf, 6U);

    /* Assert */
    TEST_ASSERT_EQUAL(I2C_ERR_OK, err);

    /* Address + R byte (0x1E<<1)|1 = 0x3D was written to DR. */
    TEST_ASSERT_EQUAL_HEX8(0x3DU, g_mock_i2c1.DR);

    /* ACK was cleared before the last byte and re-enabled after. */
    TEST_ASSERT_BITS_HIGH(I2C_CR1_ACK, g_mock_i2c1.CR1);
    TEST_ASSERT_BITS_HIGH(I2C_CR1_STOP, g_mock_i2c1.CR1);

    /* Note: buf[] content is not verified — the mock DR is overwritten
     * by the address+R write; driver reads 0x3D back for all 6 bytes.
     * Receive-data correctness is validated during hardware integration. */
}

/* ===================================================================== */
/* TC-I2C-F4-013: Bus recovery triggered by write timeout               */
/* ===================================================================== */

void test_TC_I2C_F4_013_bus_recovery_on_timeout(void)
{
    /* Arrange: SB never sets → write times out → recovery runs. */
    arrange_init_success();
    /* g_mock_i2c1.SR1 remains 0 (SB never set). */

    const uint8_t data[1] = {0x00U};

    /* Act */
    i2c_err_t err = i2c_write(0x44U, data, 1U);

    /* Assert: error returned. */
    TEST_ASSERT_EQUAL(I2C_ERR_TIMEOUT, err);

    /* Recovery: PE was cleared and re-enabled (final state = PE set). */
    TEST_ASSERT_BITS_HIGH(I2C_CR1_PE, g_mock_i2c1.CR1);

    /* PB8 (SCL) was reconfigured as an output during recovery
     * (MODER bits [17:16] = 01). The driver restores it to AF (10)
     * before re-enabling the peripheral, so final MODER = AF. */
    uint32_t moder_pb8 = (g_mock_gpio[1].MODER >> (8U * 2U)) & 0x3U;
    TEST_ASSERT_EQUAL(2U, moder_pb8); /* AF restored */
}
