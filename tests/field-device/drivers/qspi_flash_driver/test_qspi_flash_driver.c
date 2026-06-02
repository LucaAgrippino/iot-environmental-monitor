/**
 * @file test_qspi_flash_driver.c
 * @brief Unity unit tests for QspiFlashDriver — T-QSPI-01 through T-QSPI-13.
 *
 * Tests use the QUADSPI register mock from stm32_cmsis_mock.h. TCF is
 * pre-set in SR for all happy-path tests so poll_tcf() returns immediately.
 * Timeout tests leave SR.TCF clear so poll_tcf() exhausts its loop.
 *
 * RDID byte order: fifo[0]=manufacturer, fifo[1]=memory-type, fifo[2]=capacity.
 * For BOARD_FIELD_DEVICE: expected RDID = 0xC22018 (0xC2, 0x20, 0x18).
 *
 * Build: STM32F469xx, BOARD_FIELD_DEVICE, and TEST must be defined
 *        (project.yml :test_qspi_flash_driver:).
 */

#include "unity.h"
#include "stm32_cmsis_mock.h"
#include "qspi_flash_driver.h"
#include <string.h>
#include <stdint.h>

/* ====================================================================== */
/* Helpers                                                                */
/* ====================================================================== */

/** Pre-load RDID bytes and TCF, call init(), then reset mock peripheral
 *  state. s_device_size and s_initialised are preserved in the SUT. */
static void helper_init_driver(void)
{
    g_mock_quadspi.SR         = QUADSPI_SR_TCF;
    g_mock_quadspi_rx_fifo[0] = 0xC2U;
    g_mock_quadspi_rx_fifo[1] = 0x20U;
    g_mock_quadspi_rx_fifo[2] = 0x18U;
    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_OK, qspi_flash_init());
    stm32_cmsis_mock_reset(); /* clear fifo_idx; SUT static state untouched */
}

/* ====================================================================== */
/* setUp / tearDown                                                       */
/* ====================================================================== */

void setUp(void)
{
    stm32_cmsis_mock_reset();
    qspi_flash_reset_for_test();
}

void tearDown(void) {}

/* ====================================================================== */
/* T-QSPI-01: qspi_flash_init happy path                                 */
/* ====================================================================== */

void test_T_QSPI_01_init_happy_path(void)
{
    g_mock_quadspi.SR         = QUADSPI_SR_TCF;
    g_mock_quadspi_rx_fifo[0] = 0xC2U;
    g_mock_quadspi_rx_fifo[1] = 0x20U;
    g_mock_quadspi_rx_fifo[2] = 0x18U;

    qspi_flash_err_t err = qspi_flash_init();

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_OK, err);

    /* AHB3ENR QSPIEN set */
    TEST_ASSERT_TRUE((RCC->AHB3ENR & RCC_AHB3ENR_QSPIEN) != 0U);

    /* DCR FSIZE = 23 for BOARD_FIELD_DEVICE (2^24 = 16 MB) */
    uint32_t fsize = (QUADSPI->DCR >> QUADSPI_DCR_FSIZE_Pos) & 0x1FU;
    TEST_ASSERT_EQUAL_UINT32(23U, fsize);

    /* Peripheral enabled */
    TEST_ASSERT_TRUE((QUADSPI->CR & QUADSPI_CR_EN) != 0U);
}

/* ====================================================================== */
/* T-QSPI-02: qspi_flash_init — wrong RDID                               */
/* ====================================================================== */

void test_T_QSPI_02_init_wrong_rdid(void)
{
    g_mock_quadspi.SR         = QUADSPI_SR_TCF;
    g_mock_quadspi_rx_fifo[0] = 0xEFU; /* wrong manufacturer */
    g_mock_quadspi_rx_fifo[1] = 0x40U;
    g_mock_quadspi_rx_fifo[2] = 0x18U;

    qspi_flash_err_t err = qspi_flash_init();

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_DEVICE, err);
}

/* ====================================================================== */
/* T-QSPI-03: qspi_flash_read — 256 bytes at addr 0                      */
/* ====================================================================== */

void test_T_QSPI_03_read_256_bytes_at_addr_0(void)
{
    uint8_t buf[256];
    memset(buf, 0xAAU, sizeof(buf));

    helper_init_driver();

    /* Pre-load fifo with a known pattern */
    for (uint32_t i = 0U; i < 256U; i++)
    {
        g_mock_quadspi_rx_fifo[i] = (uint8_t)i;
    }
    g_mock_quadspi.SR = QUADSPI_SR_TCF;

    qspi_flash_err_t err = qspi_flash_read(0U, buf, 256U);

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_OK, err);

    /* CCR carries READ opcode (0x03) */
    uint32_t instr = (QUADSPI->CCR >> QUADSPI_CCR_INSTRUCTION_Pos) & 0xFFU;
    TEST_ASSERT_EQUAL_UINT32(0x03U, instr);

    /* Address register set to 0 */
    TEST_ASSERT_EQUAL_UINT32(0U, QUADSPI->AR);

    /* DLR = len - 1 = 255 */
    TEST_ASSERT_EQUAL_UINT32(255U, QUADSPI->DLR);

    /* First byte correctly read from fifo */
    TEST_ASSERT_EQUAL_UINT8(0x00U, buf[0]);
    /* Last byte */
    TEST_ASSERT_EQUAL_UINT8(0xFFU, buf[255]);
}

/* ====================================================================== */
/* T-QSPI-04: qspi_flash_read — addr + len exceeds device size           */
/* ====================================================================== */

void test_T_QSPI_04_read_addr_exceeds_device(void)
{
    uint8_t buf[4];
    helper_init_driver();

    /* 0x01000000 = 16 MB boundary; +4 clearly exceeds */
    qspi_flash_err_t err = qspi_flash_read(0x01000000U, buf, 4U);

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_ADDR, err);
    TEST_ASSERT_EQUAL_UINT32(0U, QUADSPI->AR); /* no command issued */
}

/* ====================================================================== */
/* T-QSPI-05: qspi_flash_read — len = 0                                  */
/* ====================================================================== */

void test_T_QSPI_05_read_len_zero(void)
{
    uint8_t buf[1];
    helper_init_driver();

    qspi_flash_err_t err = qspi_flash_read(0U, buf, 0U);

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_LEN, err);
}

/* ====================================================================== */
/* T-QSPI-06: qspi_flash_write_page — 128 bytes, page-aligned            */
/* ====================================================================== */

void test_T_QSPI_06_write_page_128_bytes_aligned(void)
{
    uint8_t data[128];
    for (uint16_t i = 0U; i < 128U; i++)
    {
        data[i] = (uint8_t)(i + 1U);
    }

    helper_init_driver();

    g_mock_quadspi.SR         = QUADSPI_SR_TCF;
    g_mock_quadspi_rx_fifo[0] = 0x00U; /* RDSR: WIP = 0 */

    qspi_flash_err_t err = qspi_flash_write_page(0x0000U, data, 128U);

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_OK, err);

    /* AR holds the Page Program address (not overwritten by RDSR) */
    TEST_ASSERT_EQUAL_UINT32(0x0000U, QUADSPI->AR);

    /* DR holds the last byte written (data[127] = 128) */
    TEST_ASSERT_EQUAL_UINT32(128U, QUADSPI->DR);
}

/* ====================================================================== */
/* T-QSPI-07: qspi_flash_write_page — crosses page boundary              */
/* ====================================================================== */

void test_T_QSPI_07_write_page_crosses_boundary(void)
{
    uint8_t data[16];
    memset(data, 0xFFU, sizeof(data));

    helper_init_driver();

    /* addr=0xF8, len=16: last byte at 0x107 — crosses 0x100 boundary */
    qspi_flash_err_t err = qspi_flash_write_page(0x00F8U, data, 16U);

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_LEN, err);

    /* No WREN issued — CCR still zero */
    TEST_ASSERT_EQUAL_UINT32(0U, QUADSPI->CCR);
}

/* ====================================================================== */
/* T-QSPI-08: qspi_flash_write_page — WIP timeout                        */
/* ====================================================================== */

void test_T_QSPI_08_write_page_wip_timeout(void)
{
    uint8_t data[4] = {0x01U, 0x02U, 0x03U, 0x04U};

    helper_init_driver();

    /* SR.TCF clear → poll_tcf() in write_enable() exhausts its loop */
    g_mock_quadspi.SR = 0U;

    qspi_flash_err_t err = qspi_flash_write_page(0x0000U, data, 4U);

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_TIMEOUT, err);
}

/* ====================================================================== */
/* T-QSPI-09: qspi_flash_erase_sector — addr within sector               */
/* ====================================================================== */

void test_T_QSPI_09_erase_sector_aligned_addr(void)
{
    helper_init_driver();

    g_mock_quadspi.SR         = QUADSPI_SR_TCF;
    g_mock_quadspi_rx_fifo[0] = 0x00U; /* RDSR: WIP = 0 */

    qspi_flash_err_t err = qspi_flash_erase_sector(0x1000U);

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_OK, err);

    /* AR holds the sector-aligned address (preserved through RDSR) */
    TEST_ASSERT_EQUAL_UINT32(0x1000U, QUADSPI->AR);
}

/* ====================================================================== */
/* T-QSPI-10: qspi_flash_erase_sector — non-aligned addr auto-aligns     */
/* ====================================================================== */

void test_T_QSPI_10_erase_sector_auto_aligns(void)
{
    helper_init_driver();

    g_mock_quadspi.SR         = QUADSPI_SR_TCF;
    g_mock_quadspi_rx_fifo[0] = 0x00U;

    /* addr=0x1234 → sector base = 0x1000 */
    qspi_flash_err_t err = qspi_flash_erase_sector(0x1234U);

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_OK, err);
    TEST_ASSERT_EQUAL_UINT32(0x1000U, QUADSPI->AR);
}

/* ====================================================================== */
/* T-QSPI-11: qspi_flash_erase_sector — WIP timeout                      */
/* ====================================================================== */

void test_T_QSPI_11_erase_sector_wip_timeout(void)
{
    helper_init_driver();

    /* SR.TCF clear → poll_tcf() in write_enable() exhausts */
    g_mock_quadspi.SR = 0U;

    qspi_flash_err_t err = qspi_flash_erase_sector(0x1000U);

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_TIMEOUT, err);
}

/* ====================================================================== */
/* T-QSPI-12: qspi_flash_erase_sector — addr exceeds device size         */
/* ====================================================================== */

void test_T_QSPI_12_erase_sector_addr_exceeds_device(void)
{
    helper_init_driver();

    /* 0x01000000 = exactly at 16 MB boundary → addr >= s_device_size */
    qspi_flash_err_t err = qspi_flash_erase_sector(0x01000000U);

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_ADDR, err);
    TEST_ASSERT_EQUAL_UINT32(0U, QUADSPI->CCR); /* no WREN issued */
}

/* ====================================================================== */
/* T-QSPI-13: QUADSPI BUSY — all operations return BUSY immediately      */
/* ====================================================================== */

void test_T_QSPI_13_busy_blocks_all_operations(void)
{
    uint8_t buf[4] = {0};

    helper_init_driver();

    g_mock_quadspi.SR = QUADSPI_SR_BUSY;

    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_BUSY, qspi_flash_init());
    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_BUSY, qspi_flash_read(0U, buf, 4U));
    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_BUSY, qspi_flash_write_page(0U, buf, 4U));
    TEST_ASSERT_EQUAL(QSPI_FLASH_ERR_BUSY, qspi_flash_erase_sector(0U));

    /* No commands issued — CCR unchanged since mock_reset in helper */
    TEST_ASSERT_EQUAL_UINT32(0U, QUADSPI->CCR);
}
