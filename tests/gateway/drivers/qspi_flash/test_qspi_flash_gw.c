/**
 * @file test_qspi_flash_gw.c
 * @brief Unit tests for QspiFlashDriver (Gateway, STM32L475 / MX25R6435F).
 *
 * Test IDs follow docs/lld/drivers/qspi-flash-driver.md §7.3 (TC-QSPI-xxx).
 * The QUADSPI peripheral is the L475 CMSIS mock; command sequences are
 * observed through the qspi_flash_hw.h instrumentation (CCR log, RX FIFO,
 * TX capture) described in companion §7.2.
 */

#include "unity.h"

#include "stm32l475_cmsis_mock.h"
#include "stm32l475xx.h"

#include "qspi_flash.h"

#include <stdint.h>
#include <string.h>

/* ====================================================================== */
/* Constants mirrored from the driver / datasheet                         */
/* ====================================================================== */

#define CMD_PP (0x02U)
#define CMD_READ (0x03U)
#define CMD_RDSR (0x05U)
#define CMD_WREN (0x06U)
#define CMD_SE (0x20U)
#define CMD_RSTEN (0x66U)
#define CMD_RST (0x99U)
#define CMD_RDID (0x9FU)

#define RDID_MFR (0xC2U)
#define RDID_TYPE (0x28U)
#define RDID_CAP (0x17U)

#define EXPECTED_FSIZE (22U)
#define EXPECTED_PRESCALER (2U)

#define ADMODE_SINGLE (1U)
#define ADSIZE_24BIT (2U)

#define SR_IDLE_READY (QUADSPI_SR_TCF | QUADSPI_SR_FTF)

/* ====================================================================== */
/* Helpers                                                                */
/* ====================================================================== */

static uint8_t ccr_instruction(uint32_t idx)
{
    return (uint8_t) (g_mock_quadspi_ccr_log[idx] & QUADSPI_CCR_INSTRUCTION_Msk);
}

static uint32_t ccr_field(uint32_t idx, uint32_t msk, uint32_t pos)
{
    return (g_mock_quadspi_ccr_log[idx] & msk) >> pos;
}

/** Return true if instruction `op` appears anywhere in the CCR log. */
static bool ccr_log_contains(uint8_t op)
{
    uint32_t n = g_mock_quadspi_ccr_count;
    if (n > QUADSPI_MOCK_CCR_LOG_DEPTH)
    {
        n = QUADSPI_MOCK_CCR_LOG_DEPTH;
    }
    for (uint32_t i = 0U; i < n; i++)
    {
        if (ccr_instruction(i) == op)
        {
            return true;
        }
    }
    return false;
}

static void helper_push_rdid(void)
{
    mock_quadspi_push_dr(RDID_MFR);
    mock_quadspi_push_dr(RDID_TYPE);
    mock_quadspi_push_dr(RDID_CAP);
}

/** Bring the driver to "initialised", then clear mock state so each test
 *  sees only its own traffic. SUT static state is preserved. */
static void helper_init_driver(void)
{
    helper_push_rdid();
    TEST_ASSERT_EQUAL(QSPI_FLASH_OK, qspi_flash_init());
    stm32l475_cmsis_mock_reset();
    g_mock_quadspi.SR = SR_IDLE_READY;
}

/* ====================================================================== */
/* setUp / tearDown                                                       */
/* ====================================================================== */

void setUp(void)
{
    stm32l475_cmsis_mock_reset();
    qspi_flash_reset_for_test();
    /* Companion §7.2 default: BUSY = 0, TCF = 1, FTF = 1 */
    g_mock_quadspi.SR = SR_IDLE_READY;
}

void tearDown(void) {}

/* ====================================================================== */
/* qspi_flash_init                                                        */
/* ====================================================================== */

/* TC-QSPI-001 */
void test_qspi_flash_init_happy_path(void)
{
    uint8_t buf[4];

    helper_push_rdid();

    TEST_ASSERT_EQUAL(QSPI_FLASH_OK, qspi_flash_init());

    /* RDID issued (after RSTEN / RST) */
    TEST_ASSERT_TRUE(ccr_log_contains(CMD_RDID));
    TEST_ASSERT_EQUAL_HEX8(CMD_RSTEN, ccr_instruction(0U));
    TEST_ASSERT_EQUAL_HEX8(CMD_RST, ccr_instruction(1U));
    TEST_ASSERT_EQUAL_HEX8(CMD_RDID, ccr_instruction(2U));

    /* DCR.FSIZE = 22 for the 8 MB MX25R6435F */
    TEST_ASSERT_EQUAL_UINT32(EXPECTED_FSIZE,
                             (QUADSPI->DCR & QUADSPI_DCR_FSIZE_Msk) >> QUADSPI_DCR_FSIZE_Pos);

    /* Peripheral enabled */
    TEST_ASSERT_TRUE((QUADSPI->CR & QUADSPI_CR_EN) != 0U);

    /* Subsequent operations are no longer refused as not-initialised */
    mock_quadspi_push_dr(0x00U);
    TEST_ASSERT_NOT_EQUAL(QSPI_FLASH_ERR_NOT_INITIALISED, qspi_flash_read(0U, buf, 1U));
}

/* TC-QSPI-002 */
void test_qspi_flash_init_wrong_rdid(void)
{
    uint8_t buf[4];

    mock_quadspi_push_dr(0xFFU);
    mock_quadspi_push_dr(0xFFU);
    mock_quadspi_push_dr(0xFFU);

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_DEVICE, qspi_flash_init());

    /* Driver remains not-initialised */
    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_NOT_INITIALISED, qspi_flash_read(0U, buf, 1U));
}

/* TC-QSPI-003 */
void test_qspi_flash_init_timeout(void)
{
    uint8_t buf[4];

    g_mock_quadspi.SR = 0U; /* TCF never asserts */
    helper_push_rdid();

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_TIMEOUT, qspi_flash_init());
    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_NOT_INITIALISED, qspi_flash_read(0U, buf, 1U));
}

/* TC-QSPI-004 */
void test_qspi_flash_init_idempotent(void)
{
    helper_init_driver();

    TEST_ASSERT_EQUAL(QSPI_FLASH_OK, qspi_flash_init());

    /* No new command of any kind was issued */
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_quadspi_ccr_count);
    TEST_ASSERT_FALSE(ccr_log_contains(CMD_RDID));
}

/* TC-QSPI-005 */
void test_qspi_flash_init_enables_clocks_and_configures_pins(void)
{
    helper_push_rdid();

    TEST_ASSERT_EQUAL(QSPI_FLASH_OK, qspi_flash_init());

    /* QUADSPI clock on AHB3, GPIOE clock on AHB2 (L4: GPIO is AHB2) */
    TEST_ASSERT_TRUE((RCC->AHB3ENR & RCC_AHB3ENR_QSPIEN) != 0U);
    TEST_ASSERT_TRUE((RCC->AHB2ENR & RCC_AHB2ENR_GPIOEEN) != 0U);

    /* PE10..PE15: MODER = 10b (AF), AFR[1] nibble = 10 (AF10), OSPEEDR = 11b */
    for (uint32_t pin = 10U; pin <= 15U; pin++)
    {
        uint32_t moder = (GPIOE->MODER >> (pin * 2U)) & 0x3U;
        uint32_t speed = (GPIOE->OSPEEDR >> (pin * 2U)) & 0x3U;
        uint32_t af = (GPIOE->AFR[1] >> ((pin - 8U) * 4U)) & 0xFU;
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(2U, moder, "MODER not AF");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(3U, speed, "OSPEEDR not very-high");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(10U, af, "AF not 10");
        TEST_ASSERT_EQUAL_UINT32_MESSAGE(0U, (GPIOE->OTYPER >> pin) & 0x1U, "not push-pull");
    }
    /* Pins outside PE10..PE15 untouched */
    TEST_ASSERT_EQUAL_UINT32(0U, (GPIOE->MODER >> (9U * 2U)) & 0x3U);

    /* CR prescaler set (26.67 MHz from 80 MHz HCLK — QSPID-O2) */
    TEST_ASSERT_EQUAL_UINT32(EXPECTED_PRESCALER,
                             (QUADSPI->CR & QUADSPI_CR_PRESCALER_Msk) >> QUADSPI_CR_PRESCALER_Pos);
}

/* ====================================================================== */
/* qspi_flash_read                                                        */
/* ====================================================================== */

/* TC-QSPI-010 */
void test_qspi_flash_read_happy_path(void)
{
    uint8_t expected[256];
    uint8_t buf[256];

    helper_init_driver();
    for (uint32_t i = 0U; i < 256U; i++)
    {
        expected[i] = (uint8_t) (i ^ 0x5AU);
        mock_quadspi_push_dr(expected[i]);
    }
    memset(buf, 0xAAU, sizeof(buf));

    TEST_ASSERT_EQUAL(QSPI_FLASH_OK, qspi_flash_read(0x0000U, buf, 256U));

    TEST_ASSERT_EQUAL_HEX8_ARRAY(expected, buf, 256U);
    TEST_ASSERT_EQUAL_HEX8(CMD_READ, ccr_instruction(0U));
    TEST_ASSERT_EQUAL_UINT32(ADMODE_SINGLE,
                             ccr_field(0U, QUADSPI_CCR_ADMODE_Msk, QUADSPI_CCR_ADMODE_Pos));
    TEST_ASSERT_EQUAL_UINT32(ADSIZE_24BIT,
                             ccr_field(0U, QUADSPI_CCR_ADSIZE_Msk, QUADSPI_CCR_ADSIZE_Pos));
    TEST_ASSERT_EQUAL_UINT32(0U, QUADSPI->AR);
    TEST_ASSERT_EQUAL_UINT32(255U, QUADSPI->DLR);
    TEST_ASSERT_EQUAL_UINT8(0U, g_mock_quadspi_rx_underflow);
}

/* TC-QSPI-011 */
void test_qspi_flash_read_addr_plus_len_exceeds_device(void)
{
    uint8_t buf[32];

    helper_init_driver();

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_ADDR,
                      qspi_flash_read(QSPI_FLASH_DEVICE_SIZE_BYTES - 10U, buf, 20U));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_quadspi_ccr_count);
}

/* TC-QSPI-012 */
void test_qspi_flash_read_len_zero(void)
{
    uint8_t buf[32];

    helper_init_driver();

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_LEN, qspi_flash_read(0U, buf, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_quadspi_ccr_count);
}

/* TC-QSPI-013 */
void test_qspi_flash_read_null_buf(void)
{
    helper_init_driver();

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_NULL_POINTER, qspi_flash_read(0U, NULL, 16U));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_quadspi_ccr_count);
}

/* TC-QSPI-014 */
void test_qspi_flash_read_not_initialised(void)
{
    uint8_t buf[16];

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_NOT_INITIALISED, qspi_flash_read(0U, buf, 16U));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_quadspi_ccr_count);
}

/* TC-QSPI-015 */
void test_qspi_flash_read_peripheral_busy(void)
{
    uint8_t buf[16];

    helper_init_driver();
    g_mock_quadspi.SR = SR_IDLE_READY | QUADSPI_SR_BUSY;

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_BUSY, qspi_flash_read(0U, buf, 16U));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_quadspi_ccr_count);
}

/* ====================================================================== */
/* qspi_flash_write_page                                                  */
/* ====================================================================== */

/* TC-QSPI-020 */
void test_qspi_flash_write_page_happy_path(void)
{
    uint8_t data[128];

    helper_init_driver();
    for (uint32_t i = 0U; i < sizeof(data); i++)
    {
        data[i] = (uint8_t) (0xA5U ^ i);
    }
    mock_quadspi_push_dr(0x00U); /* RDSR: WIP clear on first poll */

    TEST_ASSERT_EQUAL(QSPI_FLASH_OK, qspi_flash_write_page(0x0000U, data, 128U));

    /* WREN, then Page Program, then at least one RDSR */
    TEST_ASSERT_EQUAL_HEX8(CMD_WREN, ccr_instruction(0U));
    TEST_ASSERT_EQUAL_HEX8(CMD_PP, ccr_instruction(1U));
    TEST_ASSERT_EQUAL_HEX8(CMD_RDSR, ccr_instruction(2U));
    TEST_ASSERT_EQUAL_UINT32(3U, g_mock_quadspi_ccr_count);

    TEST_ASSERT_EQUAL_UINT32(0x0000U, QUADSPI->AR);
    /* DLR as latched when the Page Program command was issued (the later
     * RDSR polls legitimately reprogram DLR = 0) */
    TEST_ASSERT_EQUAL_UINT32(127U, g_mock_quadspi_dlr_log[1U]);
    TEST_ASSERT_EQUAL_UINT32(128U, g_mock_quadspi_written_count);
    TEST_ASSERT_EQUAL_HEX8_ARRAY(data, g_mock_quadspi_written_data, 128U);
}

/* TC-QSPI-021 */
void test_qspi_flash_write_page_max_page_size(void)
{
    uint8_t data[256];

    helper_init_driver();
    memset(data, 0x3CU, sizeof(data));
    mock_quadspi_push_dr(0x00U);

    TEST_ASSERT_EQUAL(QSPI_FLASH_OK, qspi_flash_write_page(0x0100U, data, 256U));

    TEST_ASSERT_EQUAL_UINT32(255U, g_mock_quadspi_dlr_log[1U]);
    TEST_ASSERT_EQUAL_UINT32(0x0100U, QUADSPI->AR);
    TEST_ASSERT_EQUAL_UINT32(256U, g_mock_quadspi_written_count);
}

/* TC-QSPI-022 */
void test_qspi_flash_write_page_crosses_page_boundary(void)
{
    uint8_t data[32];

    helper_init_driver();
    memset(data, 0x11U, sizeof(data));

    /* 0xF0 + 32 = 0x110 — spills into the next page */
    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_LEN, qspi_flash_write_page(0x00F0U, data, 32U));

    TEST_ASSERT_FALSE(ccr_log_contains(CMD_WREN));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_quadspi_written_count);
}

/* TC-QSPI-023 */
void test_qspi_flash_write_page_wip_timeout(void)
{
    uint8_t data[1] = {0x42U};

    helper_init_driver();
    /* RX FIFO left empty: every RDSR returns 0xFF (WIP = 1) forever */

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_TIMEOUT, qspi_flash_write_page(0x0000U, data, 1U));

    /* The write itself was committed before the poll gave up */
    TEST_ASSERT_EQUAL_HEX8(CMD_WREN, ccr_instruction(0U));
    TEST_ASSERT_EQUAL_HEX8(CMD_PP, ccr_instruction(1U));
    TEST_ASSERT_EQUAL_HEX8(CMD_RDSR, ccr_instruction(2U));
    TEST_ASSERT_TRUE(g_mock_quadspi_ccr_count > 3U);
    TEST_ASSERT_EQUAL_UINT8(1U, g_mock_quadspi_rx_underflow);
}

/* TC-QSPI-024 */
void test_qspi_flash_write_page_len_zero(void)
{
    uint8_t data[4] = {0U};

    helper_init_driver();

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_LEN, qspi_flash_write_page(0x0000U, data, 0U));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_quadspi_ccr_count);
}

/* TC-QSPI-025 */
void test_qspi_flash_write_page_addr_exceeds_device(void)
{
    uint8_t data[4] = {0U};

    helper_init_driver();

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_ADDR,
                      qspi_flash_write_page(QSPI_FLASH_DEVICE_SIZE_BYTES, data, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_quadspi_ccr_count);
}

/* TC-QSPI-026 */
void test_qspi_flash_write_page_null_data(void)
{
    helper_init_driver();

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_NULL_POINTER, qspi_flash_write_page(0x0000U, NULL, 16U));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_quadspi_ccr_count);
}

/* TC-QSPI-027 */
void test_qspi_flash_write_page_not_initialised(void)
{
    uint8_t data[1] = {0U};

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_NOT_INITIALISED, qspi_flash_write_page(0x0000U, data, 1U));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_quadspi_ccr_count);
}

/* TC-QSPI-028 */
void test_qspi_flash_write_page_peripheral_busy(void)
{
    uint8_t data[1] = {0U};

    helper_init_driver();
    g_mock_quadspi.SR = SR_IDLE_READY | QUADSPI_SR_BUSY;

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_BUSY, qspi_flash_write_page(0x0000U, data, 1U));
    TEST_ASSERT_FALSE(ccr_log_contains(CMD_WREN));
}

/* ====================================================================== */
/* qspi_flash_erase_sector                                                */
/* ====================================================================== */

/* TC-QSPI-030 */
void test_qspi_flash_erase_sector_happy_path(void)
{
    helper_init_driver();
    mock_quadspi_push_dr(0x00U);

    TEST_ASSERT_EQUAL(QSPI_FLASH_OK, qspi_flash_erase_sector(0x1000U));

    TEST_ASSERT_EQUAL_HEX8(CMD_WREN, ccr_instruction(0U));
    TEST_ASSERT_EQUAL_HEX8(CMD_SE, ccr_instruction(1U));
    TEST_ASSERT_EQUAL_HEX8(CMD_RDSR, ccr_instruction(2U));
    TEST_ASSERT_EQUAL_UINT32(0x1000U, QUADSPI->AR);
}

/* TC-QSPI-031 */
void test_qspi_flash_erase_sector_auto_aligns(void)
{
    helper_init_driver();
    mock_quadspi_push_dr(0x00U);

    TEST_ASSERT_EQUAL(QSPI_FLASH_OK, qspi_flash_erase_sector(0x1234U));

    TEST_ASSERT_EQUAL_UINT32(0x1000U, QUADSPI->AR);
}

/* TC-QSPI-032 */
void test_qspi_flash_erase_sector_wip_timeout(void)
{
    helper_init_driver();
    /* RX FIFO empty: WIP reads as set forever */

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_TIMEOUT, qspi_flash_erase_sector(0x0000U));

    TEST_ASSERT_EQUAL_HEX8(CMD_WREN, ccr_instruction(0U));
    TEST_ASSERT_EQUAL_HEX8(CMD_SE, ccr_instruction(1U));
}

/* TC-QSPI-033 */
void test_qspi_flash_erase_sector_addr_exceeds_device(void)
{
    helper_init_driver();

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_ADDR, qspi_flash_erase_sector(QSPI_FLASH_DEVICE_SIZE_BYTES));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_quadspi_ccr_count);
}

/* TC-QSPI-034 */
void test_qspi_flash_erase_sector_not_initialised(void)
{
    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_NOT_INITIALISED, qspi_flash_erase_sector(0x0000U));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_quadspi_ccr_count);
}

/* TC-QSPI-035 */
void test_qspi_flash_erase_sector_peripheral_busy(void)
{
    helper_init_driver();
    g_mock_quadspi.SR = SR_IDLE_READY | QUADSPI_SR_BUSY;

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_BUSY, qspi_flash_erase_sector(0x0000U));
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_quadspi_ccr_count);
}

/* ====================================================================== */
/* IQspiFlash vtable                                                      */
/* ====================================================================== */

void test_qspi_flash_vtable_binds_public_functions(void)
{
    TEST_ASSERT_NOT_NULL(qspi_flash_driver);
    TEST_ASSERT_EQUAL_PTR(qspi_flash_read, qspi_flash_driver->read);
    TEST_ASSERT_EQUAL_PTR(qspi_flash_write_page, qspi_flash_driver->write_page);
    TEST_ASSERT_EQUAL_PTR(qspi_flash_erase_sector, qspi_flash_driver->erase_sector);
}
