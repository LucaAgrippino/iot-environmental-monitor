/**
 * @file qspi_flash_driver.c
 * @brief QSPI NOR flash driver implementation.
 *
 * All operations use QUADSPI indirect mode (1-1-1 SPI). No memory-mapped
 * mode; no quad mode activation. Board-specific constants are selected at
 * compile time via STM32F469xx / STM32L475xx.
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

#include "gpio/gpio_driver.h"

/* ------------------------------------------------------------------ */
/* Board-specific constants                                            */
/* ------------------------------------------------------------------ */

#if defined(STM32F469xx)
#define QSPI_DEVICE_SIZE_BYTES (16UL * 1024UL * 1024UL) /* 16 MB        */
#define QSPI_DCR_FSIZE (23U)                            /* 2^24 bytes   */
#define QSPI_EXPECTED_RDID (0x20BA18UL)                 /* MT25QL128ABA */
#define QSPI_PIN_MAX (6U)
#elif defined(STM32L475xx)
#define QSPI_DEVICE_SIZE_BYTES (8UL * 1024UL * 1024UL) /* 8 MB         */
#define QSPI_DCR_FSIZE (22U)                           /* 2^23 bytes   */
#define QSPI_EXPECTED_RDID (0xC22817UL)                /* MX25R6435F   */
#else
#error "QspiFlashDriver: define STM32F469xx or STM32L475xx"
#endif

/* ------------------------------------------------------------------ */
/* DR byte-width access macros                                         */
/* The QUADSPI DR register is declared as uint32_t in the CMSIS       */
/* header. Reading or writing it as uint32_t advances the FIFO by 4   */
/* bytes. These macros force a byte-width bus access (LDRB/STRB),     */
/* advancing the FIFO by exactly 1 byte per access.                   */
/* ------------------------------------------------------------------ */
#ifndef QUADSPI_READ_BYTE
#define QUADSPI_DR_BYTE_ADDR (QSPI_R_BASE + 0x20UL)
#define QUADSPI_READ_BYTE (*((volatile uint8_t *) (QUADSPI_DR_BYTE_ADDR)))
#define QUADSPI_WRITE_BYTE(b) (*((volatile uint8_t *) (QUADSPI_DR_BYTE_ADDR)) = (uint8_t) (b))
#endif

/* ------------------------------------------------------------------ */
/* Flash command opcodes                                               */
/* ------------------------------------------------------------------ */

#define QSPI_CMD_WREN (0x06U)  /* Write Enable               */
#define QSPI_CMD_RDSR (0x05U)  /* Read Status Register       */
#define QSPI_CMD_RDID (0x9FU)  /* Read Identification (JEDEC) */
#define QSPI_CMD_READ (0x03U)  /* Read Data                  */
#define QSPI_CMD_PP (0x02U)    /* Page Program               */
#define QSPI_CMD_SE (0x20U)    /* Sector Erase (4 KB)        */
#define QSPI_CMD_RSTEN (0x66U) /* Reset Enable               */
#define QSPI_CMD_RST (0x99U)   /* Reset Memory               */

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
#define QSPI_CR_FTHRES_Pos (8U)
#define QSPI_CR_SSHIFT_Pos (4U)
#define QSPI_PRESCALER_VAL (2U) /* QSPI_CLK = HCLK/3 = 60 MHz   */
#define QSPI_CR_FTHRES_VAL (0U) /* FTF fires when >= 1 byte ready */
#define QSPI_CR_SSHIFT_VAL (1U) /* 1/2 clock sample shift         */
#define QSPI_TCF_POLL_TIMEOUT (10000UL)
#define QSPI_WIP_POLL_TIMEOUT (100000UL)
#define QSPI_TRST_CYCLES (28000U) /* tRST >= 100 us at 180 MHz HCLK */
#define QSPI_FIFO_DEPTH (32U)     /* QUADSPI FIFO depth in bytes    */
#define QSPI_SR_FLEVEL_Pos (8U)   /* SR bits [12:8]: FIFO level     */
#define QSPI_SR_FLEVEL_MASK (0x1FUL)

/* ------------------------------------------------------------------ */
/* Module pins (STM32F469 only)                                        */
/* ------------------------------------------------------------------ */

#if defined(STM32F469xx)
static const gpio_pin_config_t s_pins[QSPI_PIN_MAX] = {
    /* NCS  */ {.port = GPIO_PORT_B,
                .pin = 6U,
                .mode = GPIO_MODE_ALTERNATE,
                .otype = GPIO_OTYPE_PUSH_PULL,
                .speed = GPIO_SPEED_VERY_HIGH,
                .pull = GPIO_PULL_NONE,
                .alternate = 10U},
    /* CLK  */
    {.port = GPIO_PORT_F,
     .pin = 10U,
     .mode = GPIO_MODE_ALTERNATE,
     .otype = GPIO_OTYPE_PUSH_PULL,
     .speed = GPIO_SPEED_VERY_HIGH,
     .pull = GPIO_PULL_NONE,
     .alternate = 9U},
    /* IO1  */
    {.port = GPIO_PORT_F,
     .pin = 9U,
     .mode = GPIO_MODE_ALTERNATE,
     .otype = GPIO_OTYPE_PUSH_PULL,
     .speed = GPIO_SPEED_VERY_HIGH,
     .pull = GPIO_PULL_NONE,
     .alternate = 10U},
    /* IO0  */
    {.port = GPIO_PORT_F,
     .pin = 8U,
     .mode = GPIO_MODE_ALTERNATE,
     .otype = GPIO_OTYPE_PUSH_PULL,
     .speed = GPIO_SPEED_VERY_HIGH,
     .pull = GPIO_PULL_NONE,
     .alternate = 10U},
    /* IO2  */
    {.port = GPIO_PORT_F,
     .pin = 7U,
     .mode = GPIO_MODE_ALTERNATE,
     .otype = GPIO_OTYPE_PUSH_PULL,
     .speed = GPIO_SPEED_VERY_HIGH,
     .pull = GPIO_PULL_NONE,
     .alternate = 9U},
    /* IO3  */
    {.port = GPIO_PORT_F,
     .pin = 6U,
     .mode = GPIO_MODE_ALTERNATE,
     .otype = GPIO_OTYPE_PUSH_PULL,
     .speed = GPIO_SPEED_VERY_HIGH,
     .pull = GPIO_PULL_NONE,
     .alternate = 9U},
};
#endif

/* ------------------------------------------------------------------ */
/* Module-level state (§3.1)                                          */
/* ------------------------------------------------------------------ */

static uint32_t s_device_size = 0U;
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
    return QSPI_FLASH_OK;
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

        /* Wait for TCF before reading DR */
        err = poll_tcf();
        if (err != QSPI_FLASH_OK)
        {
            return err;
        }
        sr_byte = QUADSPI_READ_BYTE;

        if ((sr_byte & QSPI_WIP_BIT) == 0U)
        {
            return QSPI_FLASH_OK;
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

#if defined(STM32F469xx)
static qspi_flash_err_t initialise_gpio(void)
{
    uint8_t i;
    for (i = 0U; i < QSPI_PIN_MAX; i++)
    {
        if (gpio_configure_pin(&s_pins[i]) != GPIO_OK)
        {
            return QSPI_FLASH_ERR_DEVICE;
        }
    }
    return QSPI_FLASH_OK;
}
#endif

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

qspi_flash_err_t qspi_flash_init(void)
{
    uint32_t dr;
    uint8_t b0;
    uint8_t b1;
    uint8_t b2;
    uint32_t rdid;

    /* 1. Enable QUADSPI peripheral clock */
    RCC->AHB3ENR |= RCC_AHB3ENR_QSPIEN;
    (void) RCC->AHB3ENR; /* dummy read — wait for clock to propagate */

/* 2. Configure GPIO pins */
#if defined(STM32F469xx)
    qspi_flash_err_t err = initialise_gpio();
    if (err != QSPI_FLASH_OK)
    {
        return err;
    }
#endif

    /* 3. Sanity check — peripheral must be idle */
    if ((QUADSPI->SR & QUADSPI_SR_BUSY) != 0U)
    {
        return QSPI_FLASH_ERR_BUSY;
    }

    /* 4. Configure QUADSPI peripheral
     *    QSPI_CLK = HCLK / (PRESCALER + 1) = 180 MHz / 3 = 60 MHz
     *    FTHRES = 0: FTF fires when >= 1 byte is available in FIFO
     *    SSHIFT = 1: 1/2 clock sample shift for signal integrity
     */
    QUADSPI->CR = ((uint32_t) QSPI_PRESCALER_VAL << QSPI_CR_PRESCALER_Pos) |
                  ((uint32_t) QSPI_CR_FTHRES_VAL << QSPI_CR_FTHRES_Pos) |
                  ((uint32_t) QSPI_CR_SSHIFT_VAL << QSPI_CR_SSHIFT_Pos);

    /* 5. Configure flash geometry
     *    FSIZE = 23: device capacity = 2^(23+1) = 16 MB
     *    CSHT  = 0:  NCS high for at least 1 QSPI clock between commands
     *    CKMODE = 0: CLK low when NCS is high (SPI Mode 0)
     */
    QUADSPI->DCR = ((uint32_t) QSPI_DCR_FSIZE << QUADSPI_DCR_FSIZE_Pos) |
                   ((uint32_t) QSPI_CSHT_VAL << QUADSPI_DCR_CSHT_Pos);

    /* 6. Enable peripheral */
    QUADSPI->CR |= QUADSPI_CR_EN;

    /* 7. Software reset sequence
     *    RESET ENABLE (0x66) followed by RESET MEMORY (0x99)
     *    Both are instruction-only commands: 1-0-0 mode
     */
    QUADSPI->CCR = ((uint32_t) QSPI_CMD_RSTEN << QUADSPI_CCR_INSTRUCTION_Pos) |
                   (QSPI_CCR_IMODE_SINGLE << QUADSPI_CCR_IMODE_Pos);

    QUADSPI->CCR = ((uint32_t) QSPI_CMD_RST << QUADSPI_CCR_INSTRUCTION_Pos) |
                   (QSPI_CCR_IMODE_SINGLE << QUADSPI_CCR_IMODE_Pos);

    /* 8. Wait tRST >= 100 us before issuing any new command */
    for (uint32_t i = 0U; i < QSPI_TRST_CYCLES; i++)
    {
        __NOP();
    }

    while ((QUADSPI->SR & QUADSPI_SR_BUSY) != 0U)
    {
        ;
    }

    /* 9. Read JEDEC ID (0x9F) — 3 bytes, no address phase, 1-0-1 mode */
    QUADSPI->DLR = QSPI_RDID_BYTE_COUNT - 1U;
    QUADSPI->CCR = ((uint32_t) QSPI_CMD_RDID << QUADSPI_CCR_INSTRUCTION_Pos) |
                   (QSPI_CCR_IMODE_SINGLE << QUADSPI_CCR_IMODE_Pos) |
                   (QSPI_CCR_ADMODE_NONE << QUADSPI_CCR_ADMODE_Pos) |
                   (QSPI_CCR_DMODE_SINGLE << QUADSPI_CCR_DMODE_Pos) |
                   (QSPI_CCR_FMODE_READ << QUADSPI_CCR_FMODE_Pos);

    /* Wait for all 3 bytes to arrive, then read DR as a single 32-bit word.
     * Reading DR byte-by-byte advances the FIFO pointer and discards the
     * remaining bytes — always read as uint32_t and extract manually. */
    while ((QUADSPI->SR & QUADSPI_SR_TCF) == 0U)
    {
        ;
    }
    dr = QUADSPI->DR;
    b0 = (uint8_t) (dr >> 0U);  /* manufacturer ID */
    b1 = (uint8_t) (dr >> 8U);  /* memory type     */
    b2 = (uint8_t) (dr >> 16U); /* capacity        */
    QUADSPI->FCR = QUADSPI_FCR_CTCF;

    /* 10. Verify JEDEC ID */
    rdid = ((uint32_t) b0 << 16U) | ((uint32_t) b1 << 8U) | (uint32_t) b2;
    if (rdid != QSPI_EXPECTED_RDID)
    {
        return QSPI_FLASH_ERR_DEVICE;
    }

    s_device_size = QSPI_DEVICE_SIZE_BYTES;
    s_initialised = true;
    return QSPI_FLASH_OK;
}

qspi_flash_err_t qspi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    uint32_t i;

    if (!s_initialised)
    {
        return QSPI_FLASH_ERR_NOT_INIT;
    }
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
    QUADSPI->AR = addr;

    for (i = 0U; i < len; i++)
    {
        while ((QUADSPI->SR & QUADSPI_SR_FTF) == 0U)
        {
            ;
        }
        buf[i] = QUADSPI_READ_BYTE;
    }

    while ((QUADSPI->SR & QUADSPI_SR_TCF) == 0U)
    {
        ;
    }
    QUADSPI->FCR = QUADSPI_FCR_CTCF;
    return QSPI_FLASH_OK;
}

qspi_flash_err_t qspi_flash_write_page(uint32_t addr, const uint8_t *data, uint16_t len)
{
    qspi_flash_err_t err;
    uint16_t i;

    if (!s_initialised)
    {
        return QSPI_FLASH_ERR_NOT_INIT;
    }
    if ((QUADSPI->SR & QUADSPI_SR_BUSY) != 0U)
    {
        return QSPI_FLASH_ERR_BUSY;
    }
    if (len == 0U)
    {
        return QSPI_FLASH_ERR_LEN;
    }
    if ((uint32_t) len > (uint32_t) QSPI_PAGE_SIZE_BYTES)
    {
        return QSPI_FLASH_ERR_LEN;
    }
    /* Reject writes that cross a 256-byte page boundary.
     * Compares the page-aligned base of the first and last byte addresses.
     * Example: addr=0xF8, len=16 → last byte at 0x107 → different page → reject. */
    if ((addr & QSPI_PAGE_BASE_MASK) != ((addr + (uint32_t) len - 1U) & QSPI_PAGE_BASE_MASK))
    {
        return QSPI_FLASH_ERR_LEN;
    }
    if ((addr + (uint32_t) len) > s_device_size)
    {
        return QSPI_FLASH_ERR_ADDR;
    }

    err = write_enable();
    if (err != QSPI_FLASH_OK)
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
        /* Wait while FIFO is full (31 = max encodable value; 32 wraps to 0) */
        while (((QUADSPI->SR >> QSPI_SR_FLEVEL_Pos) & QSPI_SR_FLEVEL_MASK) == QSPI_SR_FLEVEL_MASK)
        {
            ;
        }
        QUADSPI_WRITE_BYTE(data[i]);
    }

    err = poll_tcf();
    if (err != QSPI_FLASH_OK)
    {
        return err;
    }

    return poll_wip();
}

qspi_flash_err_t qspi_flash_erase_sector(uint32_t addr)
{
    qspi_flash_err_t err;

    if (!s_initialised)
    {
        return QSPI_FLASH_ERR_NOT_INIT;
    }
    if ((QUADSPI->SR & QUADSPI_SR_BUSY) != 0U)
    {
        return QSPI_FLASH_ERR_BUSY;
    }
    if (addr >= s_device_size)
    {
        return QSPI_FLASH_ERR_ADDR;
    }

    err = write_enable();
    if (err != QSPI_FLASH_OK)
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
    if (err != QSPI_FLASH_OK)
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
