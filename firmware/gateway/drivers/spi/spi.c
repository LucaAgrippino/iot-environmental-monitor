/**
 * @file spi_driver.c
 * @brief SPI driver implementation — SPI3 on STM32L475 (Gateway).
 *
 * ADT pattern: opaque handle backed by a static pool. Pool size is 1 —
 * only SPI3 is used on this board — but the pattern is applied for API
 * consistency across the Gateway driver layer (companion §2.2, SPID-D6).
 *
 * @note See docs/lld/drivers/spi-driver.md for the full design specification.
 */

#include <spi/spi.h>
#include <stddef.h>

/** Static instance pool size. Only SPI3 is used on this board. */
#define SPI_MAX_INSTANCES (1u)

/** Bounded poll iteration count for TXE / RXNE / BSY waits (no CpuDriver
 *  dependency — SpiDriver's USES is CMSIS only, per components.md). */
#define SPI_POLL_TIMEOUT (10000UL)

/** SPI3 clock: BR[2:0] = 010 -> PCLK1 / 8 = 80 MHz / 8 = 10 MHz (companion §4.2). */
#define SPI_CR1_BR_DIV8 (0x2UL << SPI_CR1_BR_Pos)

/** SPI3 alternate function 6 (SCK/MISO/MOSI on PC10/PC11/PC12, companion §4.3). */
#define SPI_AF6 (6u)

struct spi_inst
{
    SPI_TypeDef *periph; /**< Pointer to SPI register block. */
    bool in_use;         /**< Slot allocated by spi_create(). */
};

static struct spi_inst g_pool[SPI_MAX_INSTANCES];
static uint8_t g_count;

/**
 * @brief Poll an SPI status flag until it reaches the requested state.
 *
 * @param[in] periph    SPI peripheral to poll.
 * @param[in] flag_mask Bit mask within SR to test.
 * @param[in] set       true to wait for the flag to become set, false to
 *                       wait for it to clear.
 * @return SPI_ERR_OK if the flag reached the requested state within
 *         SPI_POLL_TIMEOUT iterations; SPI_ERR_TIMEOUT otherwise.
 */
static spi_err_t spi_wait_flag(const SPI_TypeDef *periph, uint32_t flag_mask, bool set)
{
    uint32_t count = SPI_POLL_TIMEOUT;

    while (count > 0u)
    {
        bool flag_is_set = (periph->SR & flag_mask) != 0u;
        if (flag_is_set == set)
        {
            return SPI_ERR_OK;
        }
        count--;
    }

    return SPI_ERR_TIMEOUT;
}

spi_err_t spi_create(const spi_config_t *config, spi_handle_t *handle)
{
    if ((NULL == config) || (NULL == handle))
    {
        return SPI_ERR_NULL_PTR;
    }

    if (g_count >= SPI_MAX_INSTANCES)
    {
        return SPI_ERR_NO_RESOURCE;
    }

    struct spi_inst *inst = &g_pool[g_count];
    g_count++;

    inst->periph = config->instance;

    RCC->APB1ENR1 |= RCC_APB1ENR1_SPI3EN;

    /* GPIO pin configuration (SCK/MISO/MOSI, AF6) is the caller's
     * responsibility via GpioDriver — this driver only configures the
     * SPI peripheral registers (companion §2.1 dependency-conformance). */

    inst->periph->CR2 |= SPI_CR2_DS;     /* DS[3:0]=1111, 16-bit frame */
    inst->periph->CR2 &= ~SPI_CR2_FRXTH; /* FRXTH=0 (companion §3.3) */

    inst->periph->CR1 = SPI_CR1_MSTR | SPI_CR1_BR_DIV8 | SPI_CR1_SSM | SPI_CR1_SSI;
    inst->periph->CR1 &= ~(SPI_CR1_CPOL | SPI_CR1_CPHA); /* Mode 0 */
    inst->periph->CR1 |= SPI_CR1_SPE;

    inst->in_use = true;
    *handle = inst;

    return SPI_ERR_OK;
}

spi_err_t spi_transceive(spi_handle_t handle, const uint16_t *tx_buf, uint16_t *rx_buf,
                         uint16_t len)
{
    if (NULL == handle)
    {
        return SPI_ERR_NULL_PTR;
    }

    if ((NULL == tx_buf) && (NULL == rx_buf))
    {
        return SPI_ERR_NULL_PTR;
    }

    SPI_TypeDef *periph = handle->periph;

    for (uint16_t i = 0u; i < len; i++)
    {
        spi_err_t st = spi_wait_flag(periph, SPI_SR_TXE, true);
        if (SPI_ERR_OK != st)
        {
            return st;
        }

        periph->DR = (NULL != tx_buf) ? tx_buf[i] : 0x0000u;

        st = spi_wait_flag(periph, SPI_SR_RXNE, true);
        if (SPI_ERR_OK != st)
        {
            return st;
        }

        uint16_t rx_word = (uint16_t) periph->DR;
        if (NULL != rx_buf)
        {
            rx_buf[i] = rx_word;
        }
    }

    return spi_wait_flag(periph, SPI_SR_BSY, false);
}

#ifdef TEST
void spi_reset_for_test(void)
{
    for (uint8_t i = 0u; i < SPI_MAX_INSTANCES; i++)
    {
        g_pool[i].periph = NULL;
        g_pool[i].in_use = false;
    }
    g_count = 0u;
}
#endif
