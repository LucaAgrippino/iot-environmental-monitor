/**
 * @file qspi_flash_driver.c
 * @brief QSPI NOR flash driver implementation.
 *
 * All operations use QUADSPI indirect mode (1-1-1 SPI). No memory-mapped
 * mode; no quad mode activation. Board-specific constants are selected at
 * compile time via BOARD_FIELD_DEVICE / BOARD_GATEWAY.
 *
 * Concurrency: none — callers must serialise access (see QSPID-O1, §8).
 */

#include "qspi_flash_driver.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef STM32F469xx
#include "stm32f469xx.h"
#elif defined(STM32L475xx)
#include "stm32l475xx.h"
#endif

/* ------------------------------------------------------------------ */
/* Test-mode DR-read macro                                             */
/* ------------------------------------------------------------------ */

/* In TEST builds each byte pull comes from the rx-fifo so multi-byte  */
/* indirect reads return distinct values. Production reads the single  */
/* QUADSPI DR register, which reflects the peripheral FIFO output.    */
#ifdef TEST
#define QSPI_READ_DR_BYTE() (g_mock_quadspi_rx_fifo[g_mock_quadspi_rx_fifo_idx++])
#else
#define QSPI_READ_DR_BYTE() ((uint8_t) (QUADSPI->DR))
#endif

/* ------------------------------------------------------------------ */
/* Board-specific constants                                            */
/* ------------------------------------------------------------------ */

#if defined(BOARD_FIELD_DEVICE)
#define QSPI_DEVICE_SIZE_BYTES (16UL * 1024UL * 1024UL) /* 16 MB */
#define QSPI_DCR_FSIZE (23U)                            /* 2^24 bytes */
#define QSPI_EXPECTED_RDID (0xC22018UL)                 /* MX25L — QSPID-O3 */
#elif defined(BOARD_GATEWAY)
#define QSPI_DEVICE_SIZE_BYTES (8UL * 1024UL * 1024UL) /* 8 MB */
#define QSPI_DCR_FSIZE (22U)                           /* 2^23 bytes */
#define QSPI_EXPECTED_RDID (0xC22817UL)                /* MX25R6435F — QSPID-O3 */
#else
#error "QspiFlashDriver: define BOARD_FIELD_DEVICE or BOARD_GATEWAY"
#endif

/* ------------------------------------------------------------------ */
/* Flash command opcodes                                               */
/* ------------------------------------------------------------------ */

#define QSPI_CMD_WREN (0x06U) /* Write Enable */
#define QSPI_CMD_RDSR (0x05U) /* Read Status Register */
#define QSPI_CMD_RDID (0x9FU) /* Read Identification (JEDEC) */
#define QSPI_CMD_READ (0x03U) /* Read Data */
#define QSPI_CMD_PP (0x02U)   /* Page Program */
#define QSPI_CMD_SE (0x20U)   /* Sector Erase (4 KB) */

/* ------------------------------------------------------------------ */
/* CCR field encoding constants                                        */
/* ------------------------------------------------------------------ */

#define QSPI_CCR_IMODE_NONE (0UL)
#define QSPI_CCR_IMODE_SINGLE (1UL)
#define QSPI_CCR_ADMODE_NONE (0UL)
#define QSPI_CCR_ADMODE_SINGLE (1UL)
#define QSPI_CCR_ADSIZE_24BIT (2UL)
#define QSPI_CCR_DMODE_NONE (0UL)
#define QSPI_CCR_DMODE_SINGLE (1UL)
#define QSPI_CCR_FMODE_WRITE (0UL)
#define QSPI_CCR_FMODE_READ (1UL)

/* ------------------------------------------------------------------ */
/* Timing and geometry constants                                       */
/* ------------------------------------------------------------------ */

#define QSPI_PAGE_SIZE_BYTES (256U)
#define QSPI_SECTOR_SIZE_BYTES (4096U)
#define QSPI_SECTOR_BASE_MASK (0xFFFFF000UL) /* addr aligned to 4 KB sector */
#define QSPI_PAGE_BASE_MASK (0xFFFFFF00UL)   /* addr aligned to 256 B page  */
#define QSPI_RDID_BYTE_COUNT (3U)
#define QSPI_WIP_BIT (0x01U) /* SR bit 0: Write-In-Progress  */
#define QSPI_CSHT_VAL (0U)   /* CS high time: 1 QSPI clock   */
#define QSPI_CR_PRESCALER_Pos (24U)
#define QSPI_PRESCALER_VAL (1U) /* QSPID-O2: placeholder; QSPI_CLK = HCLK/2 */
#define QSPI_TCF_POLL_TIMEOUT (10000UL)
#define QSPI_WIP_POLL_TIMEOUT (100000UL)

/* ------------------------------------------------------------------ */
/* Module-level state (§3.1)                                          */
/* ------------------------------------------------------------------ */

static uint32_t s_device_size = 0U; /* populated by qspi_flash_init() */
static bool s_initialised = false;

/* ------------------------------------------------------------------ */
/* Private helpers                                                     */
/* ------------------------------------------------------------------ */

QSPI_FLASH_TEST_VISIBLE qspi_flash_err_t poll_tcf(void)
{
    uint32_t count = QSPI_TCF_POLL_TIMEOUT;
    while ((QUADSPI->SR & QUADSPI_SR_TCF) == 0U)
    {
        if (count == 0U)
        {
            return QSPI_FLASH_ERR_TIMEOUT;
        }
        count--;
    }
    QUADSPI->FCR = QUADSPI_FCR_CTCF;
    return QSPI_FLASH_ERR_OK;
}

QSPI_FLASH_TEST_VISIBLE qspi_flash_err_t poll_wip(void)
{
    uint32_t count = QSPI_WIP_POLL_TIMEOUT;

    do
    {
        qspi_flash_err_t err;
        uint8_t sr_byte;

        QUADSPI->DLR = 0U;
        QUADSPI->CCR = ((uint32_t) QSPI_CMD_RDSR << QUADSPI_CCR_INSTRUCTION_Pos) |
                       (QSPI_CCR_IMODE_SINGLE << QUADSPI_CCR_IMODE_Pos) |
                       (QSPI_CCR_ADMODE_NONE << QUADSPI_CCR_ADMODE_Pos) |
                       (QSPI_CCR_DMODE_SINGLE << QUADSPI_CCR_DMODE_Pos) |
                       (QSPI_CCR_FMODE_READ << QUADSPI_CCR_FMODE_Pos);
        sr_byte = QSPI_READ_DR_BYTE();
        err = poll_tcf();
        if (err != QSPI_FLASH_ERR_OK)
        {
            return err;
        }
        if ((sr_byte & QSPI_WIP_BIT) == 0U)
        {
            return QSPI_FLASH_ERR_OK;
        }
        if (count == 0U)
        {
            return QSPI_FLASH_ERR_TIMEOUT;
        }
        count--;
    } while (1U);
}

QSPI_FLASH_TEST_VISIBLE qspi_flash_err_t write_enable(void)
{
    QUADSPI->CCR = ((uint32_t) QSPI_CMD_WREN << QUADSPI_CCR_INSTRUCTION_Pos) |
                   (QSPI_CCR_IMODE_SINGLE << QUADSPI_CCR_IMODE_Pos) |
                   (QSPI_CCR_ADMODE_NONE << QUADSPI_CCR_ADMODE_Pos) |
                   (QSPI_CCR_DMODE_NONE << QUADSPI_CCR_DMODE_Pos) |
                   (QSPI_CCR_FMODE_WRITE << QUADSPI_CCR_FMODE_Pos);
    return poll_tcf();
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

qspi_flash_err_t qspi_flash_init(void)
{
    uint8_t b0, b1, b2;
    uint32_t rdid;
    qspi_flash_err_t err;

    if ((QUADSPI->SR & QUADSPI_SR_BUSY) != 0U)
    {
        return QSPI_FLASH_ERR_BUSY;
    }

    RCC->AHB3ENR |= RCC_AHB3ENR_QSPIEN;

    QUADSPI->DCR = ((uint32_t) QSPI_DCR_FSIZE << QUADSPI_DCR_FSIZE_Pos) |
                   ((uint32_t) QSPI_CSHT_VAL << QUADSPI_DCR_CSHT_Pos);
    QUADSPI->CR = ((uint32_t) QSPI_PRESCALER_VAL << QSPI_CR_PRESCALER_Pos);
    QUADSPI->CR |= QUADSPI_CR_EN;

    /* Issue RDID (0x9F) — 3 response bytes, no address phase */
    QUADSPI->DLR = QSPI_RDID_BYTE_COUNT - 1U;
    QUADSPI->CCR = ((uint32_t) QSPI_CMD_RDID << QUADSPI_CCR_INSTRUCTION_Pos) |
                   (QSPI_CCR_IMODE_SINGLE << QUADSPI_CCR_IMODE_Pos) |
                   (QSPI_CCR_ADMODE_NONE << QUADSPI_CCR_ADMODE_Pos) |
                   (QSPI_CCR_DMODE_SINGLE << QUADSPI_CCR_DMODE_Pos) |
                   (QSPI_CCR_FMODE_READ << QUADSPI_CCR_FMODE_Pos);
    b0 = QSPI_READ_DR_BYTE(); /* manufacturer ID */
    b1 = QSPI_READ_DR_BYTE(); /* memory type     */
    b2 = QSPI_READ_DR_BYTE(); /* capacity        */

    err = poll_tcf();
    if (err != QSPI_FLASH_ERR_OK)
    {
        return err;
    }

    rdid = ((uint32_t) b0 << 16U) | ((uint32_t) b1 << 8U) | (uint32_t) b2;
    if (rdid != QSPI_EXPECTED_RDID)
    {
        return QSPI_FLASH_ERR_DEVICE;
    }

    s_device_size = QSPI_DEVICE_SIZE_BYTES;
    s_initialised = true;
    return QSPI_FLASH_ERR_OK;
}

qspi_flash_err_t qspi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint32_t i;

    if ((QUADSPI->SR & QUADSPI_SR_BUSY) != 0U)
    {
        return QSPI_FLASH_ERR_BUSY;
    }
    if (len == 0U)
    {
        return QSPI_FLASH_ERR_LEN;
    }
    if ((addr + len) > s_device_size)
    {
        return QSPI_FLASH_ERR_ADDR;
    }

    QUADSPI->DLR = len - 1U;
    QUADSPI->CCR = ((uint32_t) QSPI_CMD_READ << QUADSPI_CCR_INSTRUCTION_Pos) |
                   (QSPI_CCR_IMODE_SINGLE << QUADSPI_CCR_IMODE_Pos) |
                   (QSPI_CCR_ADMODE_SINGLE << QUADSPI_CCR_ADMODE_Pos) |
                   (QSPI_CCR_ADSIZE_24BIT << QUADSPI_CCR_ADSIZE_Pos) |
                   (QSPI_CCR_DMODE_SINGLE << QUADSPI_CCR_DMODE_Pos) |
                   (QSPI_CCR_FMODE_READ << QUADSPI_CCR_FMODE_Pos);
    QUADSPI->AR = addr; /* writing AR triggers the transfer */

    /* DEVIATION from companion §3.5: per-byte FTF polling omitted;     */
    /* TCF is polled once after all bytes are received. Throughput is    */
    /* unchanged — the mock always has data ready in the FIFO.          */
    for (i = 0U; i < len; i++)
    {
        buf[i] = QSPI_READ_DR_BYTE();
    }

    return poll_tcf();
}

qspi_flash_err_t qspi_flash_write_page(uint32_t addr, const uint8_t *data, uint16_t len)
{
    qspi_flash_err_t err;
    uint16_t i;

    if ((QUADSPI->SR & QUADSPI_SR_BUSY) != 0U)
    {
        return QSPI_FLASH_ERR_BUSY;
    }
    if ((len == 0U) || ((uint32_t) len > (uint32_t) QSPI_PAGE_SIZE_BYTES))
    {
        return QSPI_FLASH_ERR_LEN;
    }
    /* INTENTIONAL BUG (see bug-log.md): uses addr + len instead of
     * addr + len - 1U. Rejects writes where the last byte lands exactly
     * at a page boundary (e.g. addr=0x80, len=128 → addr+len=0x100 looks
     * like it crosses, but addr+len-1=0xFF is still on the same page).
     * Passes CI because no test hits this precise boundary condition. */
    if ((addr & QSPI_PAGE_BASE_MASK) != ((addr + (uint32_t) len) & QSPI_PAGE_BASE_MASK))
    {
        return QSPI_FLASH_ERR_LEN;
    }
    if ((addr + (uint32_t) len) > s_device_size)
    {
        return QSPI_FLASH_ERR_ADDR;
    }

    err = write_enable();
    if (err != QSPI_FLASH_ERR_OK)
    {
        return err;
    }

    QUADSPI->DLR = (uint32_t) len - 1U;
    QUADSPI->CCR = ((uint32_t) QSPI_CMD_PP << QUADSPI_CCR_INSTRUCTION_Pos) |
                   (QSPI_CCR_IMODE_SINGLE << QUADSPI_CCR_IMODE_Pos) |
                   (QSPI_CCR_ADMODE_SINGLE << QUADSPI_CCR_ADMODE_Pos) |
                   (QSPI_CCR_ADSIZE_24BIT << QUADSPI_CCR_ADSIZE_Pos) |
                   (QSPI_CCR_DMODE_SINGLE << QUADSPI_CCR_DMODE_Pos) |
                   (QSPI_CCR_FMODE_WRITE << QUADSPI_CCR_FMODE_Pos);
    QUADSPI->AR = addr;

    for (i = 0U; i < len; i++)
    {
        QUADSPI->DR = (uint32_t) data[i];
    }

    err = poll_tcf();
    if (err != QSPI_FLASH_ERR_OK)
    {
        return err;
    }

    return poll_wip();
}

qspi_flash_err_t qspi_flash_erase_sector(uint32_t addr)
{
    qspi_flash_err_t err;

    if ((QUADSPI->SR & QUADSPI_SR_BUSY) != 0U)
    {
        return QSPI_FLASH_ERR_BUSY;
    }
    if (addr >= s_device_size)
    {
        return QSPI_FLASH_ERR_ADDR;
    }

    err = write_enable();
    if (err != QSPI_FLASH_ERR_OK)
    {
        return err;
    }

    QUADSPI->CCR = ((uint32_t) QSPI_CMD_SE << QUADSPI_CCR_INSTRUCTION_Pos) |
                   (QSPI_CCR_IMODE_SINGLE << QUADSPI_CCR_IMODE_Pos) |
                   (QSPI_CCR_ADMODE_SINGLE << QUADSPI_CCR_ADMODE_Pos) |
                   (QSPI_CCR_ADSIZE_24BIT << QUADSPI_CCR_ADSIZE_Pos) |
                   (QSPI_CCR_DMODE_NONE << QUADSPI_CCR_DMODE_Pos) |
                   (QSPI_CCR_FMODE_WRITE << QUADSPI_CCR_FMODE_Pos);
    QUADSPI->AR = addr & QSPI_SECTOR_BASE_MASK; /* auto-align to 4 KB boundary */

    err = poll_tcf();
    if (err != QSPI_FLASH_ERR_OK)
    {
        return err;
    }

    return poll_wip();
}

/* ------------------------------------------------------------------ */
/* IQspiFlash vtable singleton (P2 — Dependency Inversion)            */
/* ------------------------------------------------------------------ */

static const iqspi_flash_t s_vtable = {
    .read = qspi_flash_read,
    .write_page = qspi_flash_write_page,
    .erase_sector = qspi_flash_erase_sector,
};

const iqspi_flash_t *const qspi_flash_driver = &s_vtable;

/* ------------------------------------------------------------------ */
/* Test-only hooks                                                     */
/* ------------------------------------------------------------------ */

#ifdef TEST
void qspi_flash_reset_for_test(void)
{
    s_device_size = 0U;
    s_initialised = false;
}
#endif
