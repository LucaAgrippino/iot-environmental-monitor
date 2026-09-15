/**
 * @file qspi_flash.c
 * @brief QSPI NOR flash driver implementation (Gateway, MX25R6435F).
 *
 * All operations use QUADSPI indirect mode (1-1-1 SPI). No memory-mapped
 * mode; no quad mode activation. Every hardware wait is a bounded software
 * counter so that no call can hang the caller (P8) and no RTOS primitive
 * is needed (companion §3.7).
 *
 * Concurrency: none — callers must serialise access (QSPID-O1).
 */

#include "qspi_flash/qspi_flash.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "stm32l475xx.h"

#include "qspi_flash/qspi_flash_hw.h"

/* ------------------------------------------------------------------ */
/* Board-specific constants (companion §3.6, §4)                       */
/* ------------------------------------------------------------------ */

#define QSPI_DCR_FSIZE_VAL (22U)        /* 2^(22+1) = 8 MB              */
#define QSPI_EXPECTED_RDID (0xC22817UL) /* Macronix 0xC2, MX25R 0x28, 64 Mbit 0x17 */
#define QSPI_RDID_BYTE_SHIFT (8U)       /* MSB-first: mfr, type, capacity */

/* QSPI_CLK = HCLK / (PRESCALER + 1) = 80 MHz / 3 = 26.67 MHz.
 * The plain READ opcode (0x03) used by this driver is rated to 33 MHz on
 * the MX25R6435F (the 80 MHz figure applies to FAST_READ only), so
 * PRESCALER = 1 (40 MHz) would be out of spec. Resolves QSPID-O2. */
#define QSPI_PRESCALER_VAL (2U)
#define QSPI_CR_FTHRES_VAL (0U) /* FTF fires when >= 1 byte is in the FIFO */
#define QSPI_CR_SSHIFT_VAL (1U) /* 1/2 clock sample shift                  */
#define QSPI_CSHT_VAL (0U)      /* NCS high >= 1 QSPI clock between commands */

/* QUADSPI pins on the B-L475E-IOT01A (UM2153 §6.x, resolves QSPID-O5):
 * PE10 CLK, PE11 NCS, PE12 IO0, PE13 IO1, PE14 IO2, PE15 IO3 — all AF10. */
#define QSPI_GPIO_PORT GPIOE
#define QSPI_PIN_FIRST (10U)
#define QSPI_PIN_COUNT (6U)
#define QSPI_PIN_AF (10U)
#define QSPI_GPIO_MODE_AF (2UL)         /* MODER[2n+1:2n] = 10b            */
#define QSPI_GPIO_SPEED_VERY_HIGH (3UL) /* OSPEEDR[2n+1:2n] = 11b          */
#define QSPI_GPIO_FIELD_MASK (3UL)
#define QSPI_GPIO_BITS_PER_PIN (2U)
#define QSPI_GPIO_AFR_BITS_PER_PIN (4U)
#define QSPI_GPIO_AFR_PINS_PER_REG (8U)
#define QSPI_GPIO_AFR_MASK (0xFUL)
#define QSPI_GPIO_AFR_HIGH_IDX (1U) /* AFR[1] covers pins 8..15 */

/* ------------------------------------------------------------------ */
/* Flash command opcodes                                               */
/* ------------------------------------------------------------------ */

#define QSPI_CMD_WREN (0x06U)  /* Write Enable                */
#define QSPI_CMD_RDSR (0x05U)  /* Read Status Register        */
#define QSPI_CMD_RDID (0x9FU)  /* Read Identification (JEDEC) */
#define QSPI_CMD_READ (0x03U)  /* Read Data                   */
#define QSPI_CMD_PP (0x02U)    /* Page Program                */
#define QSPI_CMD_SE (0x20U)    /* Sector Erase (4 KB)         */
#define QSPI_CMD_RSTEN (0x66U) /* Reset Enable                */
#define QSPI_CMD_RST (0x99U)   /* Reset Memory                */

/* ------------------------------------------------------------------ */
/* CCR field encoding constants                                        */
/* ------------------------------------------------------------------ */

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

#define QSPI_SECTOR_BASE_MASK (0xFFFFF000UL) /* addr aligned to 4 KB sector */
#define QSPI_PAGE_BASE_MASK (0xFFFFFF00UL)   /* addr aligned to 256 B page  */
#define QSPI_RDID_BYTE_COUNT (3U)
#define QSPI_WIP_BIT (0x01U)          /* Status register bit 0: Write-In-Progress */
#define QSPI_FIFO_FULL_LEVEL (0x1FUL) /* FLEVEL saturates at 31 when full  */

/* Bounded poll windows. TCF/FTF/FLEVEL waits cover a single 256-byte
 * transfer (~80 us at 26.67 MHz) with wide margin. The WIP window must
 * outlast the worst-case sector erase (240 ms); one RDSR round trip is
 * ~1.2 us, so 400 000 polls is ~500 ms (companion §2.5, §3.7). */
#define QSPI_FLAG_POLL_TIMEOUT (10000UL)
#define QSPI_WIP_POLL_TIMEOUT (400000UL)

/* tRST after Reset Memory is well under 100 us; ~10 000 iterations of a
 * NOP loop at 80 MHz is > 300 us. */
#define QSPI_TRST_LOOPS (10000U)

/* ------------------------------------------------------------------ */
/* Module-level state (companion §3.1)                                */
/* ------------------------------------------------------------------ */

static uint32_t s_device_size = 0U;
static bool s_initialised = false;

/* ------------------------------------------------------------------ */
/* Private helpers                                                     */
/* ------------------------------------------------------------------ */

/** Build a CCR value from its individual fields. */
static uint32_t make_ccr(uint8_t instruction, uint32_t admode, uint32_t dmode, uint32_t fmode)
{
    return ((uint32_t) instruction << QUADSPI_CCR_INSTRUCTION_Pos) |
           (QSPI_CCR_IMODE_SINGLE << QUADSPI_CCR_IMODE_Pos) | (admode << QUADSPI_CCR_ADMODE_Pos) |
           (QSPI_CCR_ADSIZE_24BIT << QUADSPI_CCR_ADSIZE_Pos) | (dmode << QUADSPI_CCR_DMODE_Pos) |
           (fmode << QUADSPI_CCR_FMODE_Pos);
}

/** Wait, bounded, until every bit in flag_mask is set (or clear). */
static qspi_flash_err_t wait_sr(uint32_t flag_mask, bool want_set)
{
    uint32_t count = QSPI_FLAG_POLL_TIMEOUT;

    while (count > 0U)
    {
        bool is_set = (QUADSPI->SR & flag_mask) != 0U;
        if (is_set == want_set)
        {
            return QSPI_FLASH_OK;
        }
        count--;
    }
    return QSPI_FLASH_ERR_TIMEOUT;
}

/** Wait for transfer complete, then acknowledge it. */
static qspi_flash_err_t poll_tcf(void)
{
    qspi_flash_err_t err = wait_sr(QUADSPI_SR_TCF, true);
    if (err == QSPI_FLASH_OK)
    {
        QUADSPI->FCR = QUADSPI_FCR_CTCF;
    }
    return err;
}

/** Wait, bounded, while the TX FIFO is full. */
static qspi_flash_err_t wait_fifo_not_full(void)
{
    uint32_t count = QSPI_FLAG_POLL_TIMEOUT;

    while (count > 0U)
    {
        uint32_t level = (QUADSPI->SR & QUADSPI_SR_FLEVEL_Msk) >> QUADSPI_SR_FLEVEL_Pos;
        if (level != QSPI_FIFO_FULL_LEVEL)
        {
            return QSPI_FLASH_OK;
        }
        count--;
    }
    return QSPI_FLASH_ERR_TIMEOUT;
}

/** Issue an instruction-only command (no address, no data) and wait for it. */
static qspi_flash_err_t send_command(uint8_t instruction)
{
    QSPI_HW_WRITE_CCR(
        make_ccr(instruction, QSPI_CCR_ADMODE_NONE, QSPI_CCR_DMODE_NONE, QSPI_CCR_FMODE_WRITE));
    return poll_tcf();
}

/** Read Status Register (0x05) repeatedly until WIP clears (QSPID-D5). */
static qspi_flash_err_t poll_wip(void)
{
    uint32_t count = QSPI_WIP_POLL_TIMEOUT;

    while (count > 0U)
    {
        qspi_flash_err_t err;
        uint8_t status;

        QUADSPI->DLR = 0U; /* one data byte */
        QSPI_HW_WRITE_CCR(make_ccr(QSPI_CMD_RDSR, QSPI_CCR_ADMODE_NONE, QSPI_CCR_DMODE_SINGLE,
                                   QSPI_CCR_FMODE_READ));
        err = poll_tcf();
        if (err != QSPI_FLASH_OK)
        {
            return err;
        }
        status = QSPI_HW_READ_DR_BYTE();

        if ((status & QSPI_WIP_BIT) == 0U)
        {
            return QSPI_FLASH_OK;
        }
        count--;
    }
    return QSPI_FLASH_ERR_TIMEOUT;
}

/** Configure PE10..PE15 as very-high-speed push-pull AF10 (CMSIS only). */
static void configure_pins(void)
{
    uint32_t pin;

    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOEEN;
    (void) RCC->AHB2ENR; /* dummy read — wait for clock to propagate */

    for (pin = QSPI_PIN_FIRST; pin < (QSPI_PIN_FIRST + QSPI_PIN_COUNT); pin++)
    {
        uint32_t field_pos = pin * QSPI_GPIO_BITS_PER_PIN;
        uint32_t afr_pos = (pin - QSPI_GPIO_AFR_PINS_PER_REG) * QSPI_GPIO_AFR_BITS_PER_PIN;

        QSPI_GPIO_PORT->AFR[QSPI_GPIO_AFR_HIGH_IDX] &= ~(QSPI_GPIO_AFR_MASK << afr_pos);
        QSPI_GPIO_PORT->AFR[QSPI_GPIO_AFR_HIGH_IDX] |= ((uint32_t) QSPI_PIN_AF << afr_pos);

        QSPI_GPIO_PORT->OSPEEDR &= ~(QSPI_GPIO_FIELD_MASK << field_pos);
        QSPI_GPIO_PORT->OSPEEDR |= (QSPI_GPIO_SPEED_VERY_HIGH << field_pos);

        QSPI_GPIO_PORT->OTYPER &= ~(1UL << pin);                       /* push-pull */
        QSPI_GPIO_PORT->PUPDR &= ~(QSPI_GPIO_FIELD_MASK << field_pos); /* no pull */

        QSPI_GPIO_PORT->MODER &= ~(QSPI_GPIO_FIELD_MASK << field_pos);
        QSPI_GPIO_PORT->MODER |= (QSPI_GPIO_MODE_AF << field_pos);
    }
}

/** Read the 3-byte JEDEC ID into *rdid (manufacturer << 16 | type << 8 | capacity). */
static qspi_flash_err_t read_jedec_id(uint32_t *rdid)
{
    qspi_flash_err_t err;
    uint32_t id = 0U;
    uint32_t i;

    QUADSPI->DLR = QSPI_RDID_BYTE_COUNT - 1U;
    QSPI_HW_WRITE_CCR(
        make_ccr(QSPI_CMD_RDID, QSPI_CCR_ADMODE_NONE, QSPI_CCR_DMODE_SINGLE, QSPI_CCR_FMODE_READ));

    err = poll_tcf();
    if (err != QSPI_FLASH_OK)
    {
        return err;
    }
    for (i = 0U; i < QSPI_RDID_BYTE_COUNT; i++)
    {
        id = (id << QSPI_RDID_BYTE_SHIFT) | (uint32_t) QSPI_HW_READ_DR_BYTE();
    }
    *rdid = id;
    return QSPI_FLASH_OK;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

qspi_flash_err_t qspi_flash_init(void)
{
    qspi_flash_err_t err;
    uint32_t rdid;
    uint32_t i;

    if (s_initialised)
    {
        return QSPI_FLASH_OK;
    }

    /* 1. Clocks and pins */
    RCC->AHB3ENR |= RCC_AHB3ENR_QSPIEN;
    (void) RCC->AHB3ENR; /* dummy read — wait for clock to propagate */
    configure_pins();

    /* 2. Peripheral must be idle before reconfiguration */
    err = wait_sr(QUADSPI_SR_BUSY, false);
    if (err != QSPI_FLASH_OK)
    {
        return err;
    }

    /* 3. Controller configuration, then enable */
    QUADSPI->CR = ((uint32_t) QSPI_PRESCALER_VAL << QUADSPI_CR_PRESCALER_Pos) |
                  ((uint32_t) QSPI_CR_FTHRES_VAL << QUADSPI_CR_FTHRES_Pos) |
                  ((uint32_t) QSPI_CR_SSHIFT_VAL << QUADSPI_CR_SSHIFT_Pos);
    QUADSPI->DCR = ((uint32_t) QSPI_DCR_FSIZE_VAL << QUADSPI_DCR_FSIZE_Pos) |
                   ((uint32_t) QSPI_CSHT_VAL << QUADSPI_DCR_CSHT_Pos);
    QUADSPI->CR |= QUADSPI_CR_EN;

    /* 4. Software reset of the flash device: RSTEN then RST */
    err = send_command(QSPI_CMD_RSTEN);
    if (err != QSPI_FLASH_OK)
    {
        return err;
    }
    err = send_command(QSPI_CMD_RST);
    if (err != QSPI_FLASH_OK)
    {
        return err;
    }
    for (i = 0U; i < QSPI_TRST_LOOPS; i++)
    {
        __NOP();
    }
    err = wait_sr(QUADSPI_SR_BUSY, false);
    if (err != QSPI_FLASH_OK)
    {
        return err;
    }

    /* 5. Verify the device identity (QSPID-D7) */
    err = read_jedec_id(&rdid);
    if (err != QSPI_FLASH_OK)
    {
        return err;
    }
    if (rdid != QSPI_EXPECTED_RDID)
    {
        return QSPI_FLASH_ERR_DEVICE;
    }

    s_device_size = QSPI_FLASH_DEVICE_SIZE_BYTES;
    s_initialised = true;
    return QSPI_FLASH_OK;
}

qspi_flash_err_t qspi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    qspi_flash_err_t err;
    uint32_t i;

    if (!s_initialised)
    {
        return QSPI_FLASH_ERR_NOT_INITIALISED;
    }
    if (buf == NULL)
    {
        return QSPI_FLASH_ERR_NULL_POINTER;
    }
    if ((QUADSPI->SR & QUADSPI_SR_BUSY) != 0U)
    {
        return QSPI_FLASH_ERR_BUSY;
    }
    if (len == 0U)
    {
        return QSPI_FLASH_ERR_LEN;
    }
    if ((addr >= s_device_size) || (len > (s_device_size - addr)))
    {
        return QSPI_FLASH_ERR_ADDR;
    }

    QUADSPI->DLR = len - 1U;
    QSPI_HW_WRITE_CCR(make_ccr(QSPI_CMD_READ, QSPI_CCR_ADMODE_SINGLE, QSPI_CCR_DMODE_SINGLE,
                               QSPI_CCR_FMODE_READ));
    QUADSPI->AR = addr;

    for (i = 0U; i < len; i++)
    {
        err = wait_sr(QUADSPI_SR_FTF, true);
        if (err != QSPI_FLASH_OK)
        {
            return err;
        }
        buf[i] = QSPI_HW_READ_DR_BYTE();
    }

    return poll_tcf();
}

qspi_flash_err_t qspi_flash_write_page(uint32_t addr, const uint8_t *data, uint16_t len)
{
    qspi_flash_err_t err;
    uint32_t last;
    uint16_t i;

    if (!s_initialised)
    {
        return QSPI_FLASH_ERR_NOT_INITIALISED;
    }
    if (data == NULL)
    {
        return QSPI_FLASH_ERR_NULL_POINTER;
    }
    if ((QUADSPI->SR & QUADSPI_SR_BUSY) != 0U)
    {
        return QSPI_FLASH_ERR_BUSY;
    }
    if ((len == 0U) || (len > QSPI_FLASH_PAGE_SIZE_BYTES))
    {
        return QSPI_FLASH_ERR_LEN;
    }
    if ((addr >= s_device_size) || ((uint32_t) len > (s_device_size - addr)))
    {
        return QSPI_FLASH_ERR_ADDR;
    }
    /* Reject writes that cross a 256-byte page boundary (QSPID-D3):
     * the page-aligned base of the first and last byte must match. */
    last = addr + (uint32_t) len - 1U;
    if ((addr & QSPI_PAGE_BASE_MASK) != (last & QSPI_PAGE_BASE_MASK))
    {
        return QSPI_FLASH_ERR_LEN;
    }

    err = send_command(QSPI_CMD_WREN);
    if (err != QSPI_FLASH_OK)
    {
        return err;
    }

    QUADSPI->DLR = (uint32_t) len - 1U;
    QSPI_HW_WRITE_CCR(
        make_ccr(QSPI_CMD_PP, QSPI_CCR_ADMODE_SINGLE, QSPI_CCR_DMODE_SINGLE, QSPI_CCR_FMODE_WRITE));
    QUADSPI->AR = addr;

    for (i = 0U; i < len; i++)
    {
        err = wait_fifo_not_full();
        if (err != QSPI_FLASH_OK)
        {
            return err;
        }
        QSPI_HW_WRITE_DR_BYTE(data[i]);
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
        return QSPI_FLASH_ERR_NOT_INITIALISED;
    }
    if ((QUADSPI->SR & QUADSPI_SR_BUSY) != 0U)
    {
        return QSPI_FLASH_ERR_BUSY;
    }
    if (addr >= s_device_size)
    {
        return QSPI_FLASH_ERR_ADDR;
    }

    err = send_command(QSPI_CMD_WREN);
    if (err != QSPI_FLASH_OK)
    {
        return err;
    }

    QSPI_HW_WRITE_CCR(
        make_ccr(QSPI_CMD_SE, QSPI_CCR_ADMODE_SINGLE, QSPI_CCR_DMODE_NONE, QSPI_CCR_FMODE_WRITE));
    QUADSPI->AR = addr & QSPI_SECTOR_BASE_MASK; /* auto-align to 4 KB (QSPID-D4) */

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
