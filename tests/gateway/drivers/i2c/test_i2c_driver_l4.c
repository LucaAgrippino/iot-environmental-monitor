/**
 * @file test_i2c_driver_l4.c
 * @brief Unit tests for I2cDriver — STM32L475 (I2C v2, I2C2) implementation.
 *
 * Covers TC-I2C-L4-001..011 per docs/lld/drivers/i2c-driver.md §7.4, plus
 * TC-I2C-L4-012..020 for argument validation, NACK on read, the remaining
 * timeout points, early SDA release during bus recovery and the vtable.
 *
 * Mock strategy: I2C2 and GPIOB are the static register banks in
 * tests/mocks/stm32l475_cmsis_mock.c. Flag waits are bounded (3 polls under
 * TEST); happy-path tests pre-set the ISR flags the driver polls for, and
 * timeout tests leave them clear. A register mock cannot see BSRR writes, so
 * SCL pulses are counted through i2c_recovery_pulses_for_test().
 */

#include "unity.h"

#include <stdint.h>

#include "i2c_driver.h"
#include "i2c_driver_l4.h" /* triggers auto-link of i2c_driver_l4.c */
#include "stm32l475_cmsis_mock.h"

#define SCL_PIN (10U)
#define SDA_PIN (11U)
#define TEST_TIMINGR (0x00702991UL)
#define SADD_OF(dev) ((uint32_t) (dev) << 1U)
#define NBYTES_OF(n) ((uint32_t) (n) << I2C_CR2_NBYTES_Pos)
#define PIN_MODE(port, pin) (((port)->MODER >> ((pin) * 2U)) & 3UL)

void setUp(void)
{
    stm32l475_cmsis_mock_reset();
    i2c_reset_for_test();
}

void tearDown(void)
{
}

static void init_ok(void)
{
    TEST_ASSERT_EQUAL(I2C_ERR_OK, i2c_init());
}

/** Both SCL and SDA are back on AF4, open-drain, and the peripheral is enabled. */
static void assert_bus_restored(void)
{
    TEST_ASSERT_EQUAL_UINT32(2U, PIN_MODE(GPIOB, SCL_PIN));
    TEST_ASSERT_EQUAL_UINT32(2U, PIN_MODE(GPIOB, SDA_PIN));
    TEST_ASSERT_BITS_HIGH((1UL << SCL_PIN) | (1UL << SDA_PIN), GPIOB->OTYPER);
    TEST_ASSERT_BITS_HIGH(I2C_CR1_PE, I2C2->CR1);
}

/* TC-I2C-L4-001: init enables clocks, configures pins and timing. */
void test_TC_I2C_L4_001_init_configures_peripheral_and_pins(void)
{
    TEST_ASSERT_EQUAL(I2C_ERR_OK, i2c_init());

    TEST_ASSERT_BITS_HIGH(RCC_AHB2ENR_GPIOBEN, RCC->AHB2ENR);
    TEST_ASSERT_BITS_HIGH(RCC_APB1ENR1_I2C2EN, RCC->APB1ENR1);
    TEST_ASSERT_EQUAL_HEX32(TEST_TIMINGR, I2C2->TIMINGR);
    TEST_ASSERT_BITS_HIGH(I2C_CR1_PE, I2C2->CR1);

    TEST_ASSERT_EQUAL_UINT32(2U, PIN_MODE(GPIOB, SCL_PIN));
    TEST_ASSERT_EQUAL_UINT32(2U, PIN_MODE(GPIOB, SDA_PIN));
    TEST_ASSERT_BITS_HIGH((1UL << SCL_PIN) | (1UL << SDA_PIN), GPIOB->OTYPER);
    TEST_ASSERT_EQUAL_HEX32(4UL, (GPIOB->AFR[1] >> ((SCL_PIN - 8U) * 4U)) & 0xFUL);
    TEST_ASSERT_EQUAL_HEX32(4UL, (GPIOB->AFR[1] >> ((SDA_PIN - 8U) * 4U)) & 0xFUL);
}

/* TC-I2C-L4-002: second init is a no-op. */
void test_TC_I2C_L4_002_init_is_idempotent(void)
{
    init_ok();
    I2C2->TIMINGR = 0xDEADBEEFUL;

    TEST_ASSERT_EQUAL(I2C_ERR_OK, i2c_init());
    TEST_ASSERT_EQUAL_HEX32(0xDEADBEEFUL, I2C2->TIMINGR);
}

/* TC-I2C-L4-003: 2-byte write to 0x44. */
void test_TC_I2C_L4_003_write_happy_path(void)
{
    const uint8_t data[2] = {0x20U, 0x85U};

    init_ok();
    I2C2->ISR = I2C_ISR_TXIS | I2C_ISR_STOPF;

    TEST_ASSERT_EQUAL(I2C_ERR_OK, i2c_write(0x44U, data, 2U));

    TEST_ASSERT_EQUAL_HEX32(SADD_OF(0x44U) | NBYTES_OF(2U) | I2C_CR2_AUTOEND | I2C_CR2_START,
                            I2C2->CR2);
    TEST_ASSERT_BITS_LOW(I2C_CR2_RD_WRN, I2C2->CR2);
    TEST_ASSERT_EQUAL_HEX8(0x85U, (uint8_t) I2C2->TXDR); /* last byte sent */
    TEST_ASSERT_BITS_HIGH(I2C_ICR_STOPCF, I2C2->ICR);
}

/* TC-I2C-L4-004: NACK during the data phase. */
void test_TC_I2C_L4_004_write_nack(void)
{
    const uint8_t data[1] = {0x00U};

    init_ok();
    I2C2->ISR = I2C_ISR_NACKF;

    TEST_ASSERT_EQUAL(I2C_ERR_NACK, i2c_write(0x44U, data, 1U));
    TEST_ASSERT_BITS_HIGH(I2C_ICR_NACKCF, I2C2->ICR);
    TEST_ASSERT_BITS_HIGH(I2C_ICR_STOPCF, I2C2->ICR);
    TEST_ASSERT_EQUAL_UINT32(0U, i2c_recovery_pulses_for_test()); /* NACK is not a stuck bus */
}

/* TC-I2C-L4-005: TXIS never asserts -> timeout, with bus recovery. */
void test_TC_I2C_L4_005_write_txis_timeout(void)
{
    const uint8_t data[1] = {0x00U};

    init_ok();
    I2C2->ISR = 0U;

    TEST_ASSERT_EQUAL(I2C_ERR_TIMEOUT, i2c_write(0x44U, data, 1U));
    TEST_ASSERT_EQUAL_UINT32(9U, i2c_recovery_pulses_for_test());
    assert_bus_restored();
}

/* TC-I2C-L4-006: BUSY at entry -> BUS_BUSY, START not generated. */
void test_TC_I2C_L4_006_write_bus_busy(void)
{
    const uint8_t data[1] = {0x00U};

    init_ok();
    I2C2->ISR = I2C_ISR_BUSY;

    TEST_ASSERT_EQUAL(I2C_ERR_BUS_BUSY, i2c_write(0x44U, data, 1U));
    TEST_ASSERT_BITS_LOW(I2C_CR2_START, I2C2->CR2);
    TEST_ASSERT_EQUAL_UINT32(0U, i2c_recovery_pulses_for_test());
}

/* TC-I2C-L4-007: write_read, 1 byte out, 2 bytes in. */
void test_TC_I2C_L4_007_write_read_happy_path(void)
{
    const uint8_t reg = 0x0FU;
    uint8_t rx[2] = {0U, 0U};

    init_ok();
    I2C2->ISR = I2C_ISR_TXIS | I2C_ISR_TC | I2C_ISR_RXNE | I2C_ISR_STOPF;
    I2C2->RXDR = 0x5AU;

    TEST_ASSERT_EQUAL(I2C_ERR_OK, i2c_write_read(0x44U, &reg, 1U, rx, 2U));

    TEST_ASSERT_EQUAL_HEX8(0x0FU, (uint8_t) I2C2->TXDR);
    TEST_ASSERT_EQUAL_HEX8(0x5AU, rx[0]);
    TEST_ASSERT_EQUAL_HEX8(0x5AU, rx[1]);
    /* Final CR2 is the read phase: repeated START with AUTOEND. */
    TEST_ASSERT_EQUAL_HEX32(SADD_OF(0x44U) | NBYTES_OF(2U) | I2C_CR2_RD_WRN | I2C_CR2_AUTOEND |
                                I2C_CR2_START,
                            I2C2->CR2);
    TEST_ASSERT_BITS_HIGH(I2C_ICR_STOPCF, I2C2->ICR);
}

/* TC-I2C-L4-008: NACK in the write phase never reaches the read phase. */
void test_TC_I2C_L4_008_write_read_nack_in_write_phase(void)
{
    const uint8_t reg = 0x0FU;
    uint8_t rx[1] = {0U};

    init_ok();
    I2C2->ISR = I2C_ISR_NACKF;

    TEST_ASSERT_EQUAL(I2C_ERR_NACK, i2c_write_read(0x44U, &reg, 1U, rx, 1U));
    TEST_ASSERT_BITS_LOW(I2C_CR2_RD_WRN, I2C2->CR2); /* still the write-phase CR2 */
}

/* TC-I2C-L4-009: TC never asserts -> read phase not started; write phase used AUTOEND=0. */
void test_TC_I2C_L4_009_write_read_tc_timeout(void)
{
    const uint8_t reg = 0x0FU;
    uint8_t rx[2] = {0U, 0U};

    init_ok();
    I2C2->ISR = I2C_ISR_TXIS; /* TC clear */

    TEST_ASSERT_EQUAL(I2C_ERR_TIMEOUT, i2c_write_read(0x44U, &reg, 1U, rx, 2U));
    TEST_ASSERT_EQUAL_HEX32(SADD_OF(0x44U) | NBYTES_OF(1U) | I2C_CR2_START, I2C2->CR2);
    TEST_ASSERT_EQUAL_UINT32(9U, i2c_recovery_pulses_for_test());
}

/* TC-I2C-L4-010: 6-byte read from 0x1E. */
void test_TC_I2C_L4_010_read_happy_path(void)
{
    uint8_t buf[6] = {0U};

    init_ok();
    I2C2->ISR = I2C_ISR_RXNE | I2C_ISR_STOPF;
    I2C2->RXDR = 0xA5U;

    TEST_ASSERT_EQUAL(I2C_ERR_OK, i2c_read(0x1EU, buf, 6U));

    for (uint32_t i = 0U; i < 6U; i++)
    {
        TEST_ASSERT_EQUAL_HEX8(0xA5U, buf[i]);
    }
    TEST_ASSERT_EQUAL_HEX32(SADD_OF(0x1EU) | NBYTES_OF(6U) | I2C_CR2_RD_WRN | I2C_CR2_AUTOEND |
                                I2C_CR2_START,
                            I2C2->CR2);
    TEST_ASSERT_BITS_HIGH(I2C_ICR_STOPCF, I2C2->ICR);
}

/* TC-I2C-L4-011: BUSY persisting across the consumer's retry -> recovery + TIMEOUT. */
void test_TC_I2C_L4_011_persistent_busy_triggers_recovery(void)
{
    const uint8_t data[1] = {0x00U};

    init_ok();
    I2C2->ISR = I2C_ISR_BUSY;

    TEST_ASSERT_EQUAL(I2C_ERR_BUS_BUSY, i2c_write(0x44U, data, 1U)); /* first strike */
    TEST_ASSERT_EQUAL_UINT32(0U, i2c_recovery_pulses_for_test());

    TEST_ASSERT_EQUAL(I2C_ERR_TIMEOUT, i2c_write(0x44U, data, 1U)); /* retry still busy */
    TEST_ASSERT_EQUAL_UINT32(9U, i2c_recovery_pulses_for_test());
    assert_bus_restored();
    TEST_ASSERT_BITS_LOW(I2C_CR2_START, I2C2->CR2);
}

/* TC-I2C-L4-012: a free bus between two BUSY results resets the strike count. */
void test_TC_I2C_L4_012_busy_strikes_reset_after_success(void)
{
    const uint8_t data[1] = {0x00U};

    init_ok();
    I2C2->ISR = I2C_ISR_BUSY;
    TEST_ASSERT_EQUAL(I2C_ERR_BUS_BUSY, i2c_write(0x44U, data, 1U));

    I2C2->ISR = I2C_ISR_TXIS | I2C_ISR_STOPF;
    TEST_ASSERT_EQUAL(I2C_ERR_OK, i2c_write(0x44U, data, 1U));

    I2C2->ISR = I2C_ISR_BUSY;
    TEST_ASSERT_EQUAL(I2C_ERR_BUS_BUSY, i2c_write(0x44U, data, 1U)); /* not TIMEOUT */
}

/* TC-I2C-L4-013: recovery stops pulsing once SDA is released and sends a STOP. */
void test_TC_I2C_L4_013_recovery_sda_already_released(void)
{
    const uint8_t data[1] = {0x00U};

    init_ok();
    I2C2->ISR = 0U;
    GPIOB->IDR = (1UL << SDA_PIN);

    TEST_ASSERT_EQUAL(I2C_ERR_TIMEOUT, i2c_write(0x44U, data, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, i2c_recovery_pulses_for_test());
    TEST_ASSERT_EQUAL_HEX32(1UL << SDA_PIN,
                            GPIOB->BSRR); /* last action: SDA released high (STOP) */
    assert_bus_restored();
}

/* TC-I2C-L4-014: invalid arguments are rejected before the bus is touched. */
void test_TC_I2C_L4_014_invalid_arguments(void)
{
    const uint8_t data[1] = {0x00U};
    uint8_t buf[1] = {0U};

    init_ok();

    TEST_ASSERT_EQUAL(I2C_ERR_INVALID_ARG, i2c_write(0x44U, NULL, 1U));
    TEST_ASSERT_EQUAL(I2C_ERR_INVALID_ARG, i2c_write(0x44U, data, 0U));
    TEST_ASSERT_EQUAL(I2C_ERR_INVALID_ARG, i2c_write(0x44U, data, 256U));
    TEST_ASSERT_EQUAL(I2C_ERR_INVALID_ARG, i2c_write(0x80U, data, 1U));
    TEST_ASSERT_EQUAL(I2C_ERR_INVALID_ARG, i2c_read(0x44U, NULL, 1U));
    TEST_ASSERT_EQUAL(I2C_ERR_INVALID_ARG, i2c_read(0x44U, buf, 0U));
    TEST_ASSERT_EQUAL(I2C_ERR_INVALID_ARG, i2c_write_read(0x44U, NULL, 1U, buf, 1U));
    TEST_ASSERT_EQUAL(I2C_ERR_INVALID_ARG, i2c_write_read(0x44U, data, 1U, NULL, 1U));
    TEST_ASSERT_EQUAL(I2C_ERR_INVALID_ARG, i2c_write_read(0x44U, data, 1U, buf, 256U));

    TEST_ASSERT_EQUAL_HEX32(0U, I2C2->CR2);
}

/* TC-I2C-L4-015: the largest legal length (255) is accepted. */
void test_TC_I2C_L4_015_max_length_accepted(void)
{
    uint8_t buf[255];

    init_ok();
    I2C2->ISR = I2C_ISR_RXNE | I2C_ISR_STOPF;

    TEST_ASSERT_EQUAL(I2C_ERR_OK, i2c_read(0x1EU, buf, 255U));
    TEST_ASSERT_EQUAL_HEX32(NBYTES_OF(255U), I2C2->CR2 & I2C_CR2_NBYTES);
}

/* TC-I2C-L4-016: NACK on the address of a read. */
void test_TC_I2C_L4_016_read_nack(void)
{
    uint8_t buf[2] = {0U, 0U};

    init_ok();
    I2C2->ISR = I2C_ISR_NACKF;

    TEST_ASSERT_EQUAL(I2C_ERR_NACK, i2c_read(0x1EU, buf, 2U));
    TEST_ASSERT_BITS_HIGH(I2C_ICR_NACKCF, I2C2->ICR);
}

/* TC-I2C-L4-017: RXNE never asserts during a read. */
void test_TC_I2C_L4_017_read_rxne_timeout(void)
{
    uint8_t buf[2] = {0U, 0U};

    init_ok();
    I2C2->ISR = 0U;

    TEST_ASSERT_EQUAL(I2C_ERR_TIMEOUT, i2c_read(0x1EU, buf, 2U));
    TEST_ASSERT_EQUAL_UINT32(9U, i2c_recovery_pulses_for_test());
}

/* TC-I2C-L4-018: STOPF never asserts after the last byte. */
void test_TC_I2C_L4_018_write_stopf_timeout(void)
{
    const uint8_t data[1] = {0x00U};

    init_ok();
    I2C2->ISR = I2C_ISR_TXIS; /* STOPF clear */

    TEST_ASSERT_EQUAL(I2C_ERR_TIMEOUT, i2c_write(0x44U, data, 1U));
    TEST_ASSERT_BITS_LOW(I2C_ICR_STOPCF, I2C2->ICR);
}

/* TC-I2C-L4-019: RXNE never asserts in the read phase of write_read. */
void test_TC_I2C_L4_019_write_read_rxne_timeout(void)
{
    const uint8_t reg = 0x0FU;
    uint8_t rx[1] = {0U};

    init_ok();
    I2C2->ISR = I2C_ISR_TXIS | I2C_ISR_TC; /* RXNE clear */

    TEST_ASSERT_EQUAL(I2C_ERR_TIMEOUT, i2c_write_read(0x44U, &reg, 1U, rx, 1U));
    TEST_ASSERT_BITS_HIGH(I2C_CR2_RD_WRN, I2C2->CR2); /* read phase was started */
}

/* TC-I2C-L4-020: the singleton vtable dispatches to the public functions. */
void test_TC_I2C_L4_020_vtable_dispatch(void)
{
    const uint8_t data[1] = {0x00U};

    TEST_ASSERT_NOT_NULL(i2c_driver);
    TEST_ASSERT_TRUE(i2c_driver->init == i2c_init);
    TEST_ASSERT_TRUE(i2c_driver->write == i2c_write);
    TEST_ASSERT_TRUE(i2c_driver->read == i2c_read);
    TEST_ASSERT_TRUE(i2c_driver->write_read == i2c_write_read);

    TEST_ASSERT_EQUAL(I2C_ERR_OK, i2c_driver->init());
    I2C2->ISR = I2C_ISR_TXIS | I2C_ISR_STOPF;
    TEST_ASSERT_EQUAL(I2C_ERR_OK, i2c_driver->write(0x44U, data, 1U));
}
