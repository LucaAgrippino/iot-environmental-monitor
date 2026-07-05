/**
 * @file test_spi_driver.c
 * @brief Unit tests for SpiDriver — SPI3 on STM32L475 (Gateway).
 *
 * Covers T-SPI-01 through T-SPI-14 per docs/lld/drivers/spi-driver.md §9.
 *
 * Mock limitation: the host mock models SPI3->DR as a single plain
 * register (no separate TX shift register / RX FIFO), so a read
 * immediately reflects whatever was most recently written — the same
 * documented limitation as the F469 I2C v1 mock (see dev-tools bug-log
 * for I2cDriver). Multi-word tests therefore verify that DR's final
 * state reflects the last word processed and that rx_buf mirrors each
 * word's DR value at the time it was captured, rather than an
 * independently-supplied "received" sequence.
 */

#include "unity.h"

#include "stm32l475_cmsis_mock.h"
#include "stm32l475xx.h"
#include "spi.h"

extern void spi_reset_for_test(void);

void setUp(void)
{
    stm32l475_cmsis_mock_reset();
    spi_reset_for_test();
}

void tearDown(void)
{
}

/* Pre-set SR so TXE and RXNE are always ready and BSY is always clear —
 * the "happy path" steady state for a mock with no automatic flag
 * toggling. */
static void mock_spi_ready(void)
{
    g_mock_spi3.SR = SPI_SR_TXE | SPI_SR_RXNE;
}

static spi_handle_t create_ready_handle(void)
{
    mock_spi_ready();
    spi_config_t config = {.instance = SPI3};
    spi_handle_t handle = NULL;
    TEST_ASSERT_EQUAL_INT(SPI_ERR_OK, spi_create(&config, &handle));
    return handle;
}

/* --------------------------------------------------------------------- */
/* spi_create                                                             */
/* --------------------------------------------------------------------- */

void test_spi_create_happy_path(void)
{
    spi_handle_t handle = create_ready_handle();

    TEST_ASSERT_NOT_NULL(handle);

    TEST_ASSERT_BITS(SPI_CR1_CPOL, 0u, SPI3->CR1);
    TEST_ASSERT_BITS(SPI_CR1_CPHA, 0u, SPI3->CR1);
    TEST_ASSERT_BITS(SPI_CR1_MSTR, SPI_CR1_MSTR, SPI3->CR1);
    TEST_ASSERT_BITS(SPI_CR1_BR, (0x2UL << SPI_CR1_BR_Pos), SPI3->CR1);
    TEST_ASSERT_BITS(SPI_CR1_SSM, SPI_CR1_SSM, SPI3->CR1);
    TEST_ASSERT_BITS(SPI_CR1_SSI, SPI_CR1_SSI, SPI3->CR1);
    TEST_ASSERT_BITS(SPI_CR1_SPE, SPI_CR1_SPE, SPI3->CR1);

    TEST_ASSERT_BITS(SPI_CR2_DS, SPI_CR2_DS, SPI3->CR2);
    TEST_ASSERT_BITS(SPI_CR2_FRXTH, 0u, SPI3->CR2);
}

void test_spi_create_rejects_null_config(void)
{
    spi_handle_t handle = NULL;
    TEST_ASSERT_EQUAL_INT(SPI_ERR_NULL_PTR, spi_create(NULL, &handle));
    TEST_ASSERT_EQUAL_HEX32(0u, SPI3->CR1);
}

void test_spi_create_rejects_null_handle(void)
{
    spi_config_t config = {.instance = SPI3};
    TEST_ASSERT_EQUAL_INT(SPI_ERR_NULL_PTR, spi_create(&config, NULL));
    TEST_ASSERT_EQUAL_HEX32(0u, SPI3->CR1);
}

void test_spi_create_pool_exhaustion_on_second_call(void)
{
    spi_config_t config = {.instance = SPI3};
    spi_handle_t handle1 = NULL;
    spi_handle_t handle2 = NULL;

    TEST_ASSERT_EQUAL_INT(SPI_ERR_OK, spi_create(&config, &handle1));
    TEST_ASSERT_EQUAL_INT(SPI_ERR_NO_RESOURCE, spi_create(&config, &handle2));
}

void test_spi_create_idempotent_after_reset_for_test(void)
{
    spi_config_t config = {.instance = SPI3};
    spi_handle_t handle1 = NULL;
    spi_handle_t handle2 = NULL;

    TEST_ASSERT_EQUAL_INT(SPI_ERR_OK, spi_create(&config, &handle1));

    spi_reset_for_test();

    TEST_ASSERT_EQUAL_INT(SPI_ERR_OK, spi_create(&config, &handle2));
    TEST_ASSERT_NOT_NULL(handle2);
}

/* --------------------------------------------------------------------- */
/* spi_transceive                                                        */
/* --------------------------------------------------------------------- */

void test_spi_transceive_full_duplex_four_words(void)
{
    spi_handle_t handle = create_ready_handle();

    uint16_t tx[4] = {0xAAAAu, 0xBBBBu, 0xCCCCu, 0xDDDDu};
    uint16_t rx[4] = {0};

    TEST_ASSERT_EQUAL_INT(SPI_ERR_OK, spi_transceive(handle, tx, rx, 4));

    /* Mock DR is a single register — rx mirrors what was last written to
     * it at capture time, i.e. each tx word in turn (see file header). */
    TEST_ASSERT_EQUAL_HEX16(tx[0], rx[0]);
    TEST_ASSERT_EQUAL_HEX16(tx[1], rx[1]);
    TEST_ASSERT_EQUAL_HEX16(tx[2], rx[2]);
    TEST_ASSERT_EQUAL_HEX16(tx[3], rx[3]);
    TEST_ASSERT_EQUAL_HEX16(tx[3], (uint16_t) SPI3->DR);
}

void test_spi_transceive_tx_null_sends_dummy_words(void)
{
    spi_handle_t handle = create_ready_handle();

    uint16_t rx[2] = {0xFFFFu, 0xFFFFu};

    TEST_ASSERT_EQUAL_INT(SPI_ERR_OK, spi_transceive(handle, NULL, rx, 2));

    TEST_ASSERT_EQUAL_HEX16(0x0000u, rx[0]);
    TEST_ASSERT_EQUAL_HEX16(0x0000u, rx[1]);
}

void test_spi_transceive_rx_null_discards_received_words(void)
{
    spi_handle_t handle = create_ready_handle();

    uint16_t tx[2] = {0x1234u, 0x5678u};

    TEST_ASSERT_EQUAL_INT(SPI_ERR_OK, spi_transceive(handle, tx, NULL, 2));

    /* No rx_buf to check for corruption; verify the last tx word reached DR. */
    TEST_ASSERT_EQUAL_HEX16(tx[1], (uint16_t) SPI3->DR);
}

void test_spi_transceive_rejects_both_buffers_null(void)
{
    spi_handle_t handle = create_ready_handle();

    TEST_ASSERT_EQUAL_INT(SPI_ERR_NULL_PTR, spi_transceive(handle, NULL, NULL, 2));
}

void test_spi_transceive_rejects_null_handle(void)
{
    uint16_t tx[1] = {0x0001u};
    TEST_ASSERT_EQUAL_INT(SPI_ERR_NULL_PTR, spi_transceive(NULL, tx, NULL, 1));
}

void test_spi_transceive_single_word(void)
{
    spi_handle_t handle = create_ready_handle();

    uint16_t tx[1] = {0x55AAu};
    uint16_t rx[1] = {0};

    TEST_ASSERT_EQUAL_INT(SPI_ERR_OK, spi_transceive(handle, tx, rx, 1));

    TEST_ASSERT_EQUAL_HEX16(tx[0], rx[0]);
}

void test_spi_transceive_txe_timeout_sends_nothing(void)
{
    spi_handle_t handle = create_ready_handle();

    /* TXE never asserts. */
    g_mock_spi3.SR = 0u;
    g_mock_spi3.DR = 0xFFFFu; /* sentinel — must remain untouched */

    uint16_t tx[1] = {0x0001u};

    TEST_ASSERT_EQUAL_INT(SPI_ERR_TIMEOUT, spi_transceive(handle, tx, NULL, 1));
    TEST_ASSERT_EQUAL_HEX16(0xFFFFu, (uint16_t) SPI3->DR);
}

void test_spi_transceive_rxne_timeout(void)
{
    spi_handle_t handle = create_ready_handle();

    /* TXE ready, RXNE never asserts. */
    g_mock_spi3.SR = SPI_SR_TXE;

    uint16_t tx[1] = {0xBEEFu};

    TEST_ASSERT_EQUAL_INT(SPI_ERR_TIMEOUT, spi_transceive(handle, tx, NULL, 1));
    /* The TXE branch did run before the RXNE wait timed out. */
    TEST_ASSERT_EQUAL_HEX16(tx[0], (uint16_t) SPI3->DR);
}

void test_spi_transceive_bsy_timeout_after_full_transfer(void)
{
    spi_handle_t handle = create_ready_handle();

    /* TXE/RXNE ready throughout, but BSY is stuck set. */
    g_mock_spi3.SR = SPI_SR_TXE | SPI_SR_RXNE | SPI_SR_BSY;

    uint16_t tx[2] = {0x1111u, 0x2222u};
    uint16_t rx[2] = {0};

    TEST_ASSERT_EQUAL_INT(SPI_ERR_TIMEOUT, spi_transceive(handle, tx, rx, 2));

    /* Both words were fully transferred before the trailing BSY wait failed. */
    TEST_ASSERT_EQUAL_HEX16(tx[0], rx[0]);
    TEST_ASSERT_EQUAL_HEX16(tx[1], rx[1]);
}
