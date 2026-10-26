/**
 * @file fake_qspi_flash.c
 * @brief RAM QSPI NOR flash with the real driver's rules (FirmwareStore tests).
 */

#include "fake_qspi_flash.h"

#include <string.h>

#include "power_cut.h"

static uint8_t s_mem[FAKE_QSPI_SIZE];
static uint32_t s_sector_erases[FAKE_QSPI_SECTORS];

static uint32_t s_reads;
static uint32_t s_programs;
static uint32_t s_erases;
static uint32_t s_violations;

static bool s_fail_all_reads;
static uint32_t s_fail_read_addr = FAKE_QSPI_NO_ADDR;
static uint32_t s_fail_program_addr = FAKE_QSPI_NO_ADDR;
static uint32_t s_fail_erase_addr = FAKE_QSPI_NO_ADDR;

static qspi_flash_err_t fake_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    s_reads++;

    if (s_fail_all_reads || (addr == s_fail_read_addr))
    {
        return QSPI_FLASH_ERR_TIMEOUT;
    }

    if ((buf == NULL) || (len == 0U) || (addr >= FAKE_QSPI_SIZE) || (len > (FAKE_QSPI_SIZE - addr)))
    {
        s_violations++;
        return QSPI_FLASH_ERR_ADDR;
    }

    (void) memcpy(buf, &s_mem[addr], len);

    return QSPI_FLASH_OK;
}

static qspi_flash_err_t fake_write_page(uint32_t addr, const uint8_t *data, uint16_t len)
{
    s_programs++;

    /* The real driver: 1..256 bytes, all within one 256-byte page, inside the device. */
    if ((data == NULL) || (len == 0U) || (len > QSPI_FLASH_PAGE_SIZE_BYTES) ||
        (addr >= FAKE_QSPI_SIZE) || (len > (FAKE_QSPI_SIZE - addr)) ||
        ((addr / QSPI_FLASH_PAGE_SIZE_BYTES) != ((addr + len - 1U) / QSPI_FLASH_PAGE_SIZE_BYTES)))
    {
        s_violations++;
        return QSPI_FLASH_ERR_LEN;
    }

    if (addr == s_fail_program_addr)
    {
        return QSPI_FLASH_ERR_TIMEOUT;
    }

    const power_cut_verdict_t verdict = power_cut_tick();

    if (verdict == POWER_CUT_DEAD)
    {
        return QSPI_FLASH_ERR_TIMEOUT;
    }

    /* A torn program lands only its first half. */
    const uint32_t n = (verdict == POWER_CUT_TORN) ? ((uint32_t) len / 2U) : (uint32_t) len;

    for (uint32_t i = 0U; i < n; i++)
    {
        s_mem[addr + i] &= data[i]; /* NOR: programming only clears bits */
    }

    return (verdict == POWER_CUT_TORN) ? QSPI_FLASH_ERR_TIMEOUT : QSPI_FLASH_OK;
}

static qspi_flash_err_t fake_erase_sector(uint32_t addr)
{
    s_erases++;

    if (addr >= FAKE_QSPI_SIZE)
    {
        s_violations++;
        return QSPI_FLASH_ERR_ADDR;
    }

    const uint32_t sector = addr / QSPI_FLASH_SECTOR_SIZE_BYTES;

    if (sector == (s_fail_erase_addr / QSPI_FLASH_SECTOR_SIZE_BYTES))
    {
        return QSPI_FLASH_ERR_TIMEOUT;
    }

    const power_cut_verdict_t verdict = power_cut_tick();

    if (verdict == POWER_CUT_DEAD)
    {
        return QSPI_FLASH_ERR_TIMEOUT;
    }

    /* A torn erase clears only the first half of the sector. */
    const uint32_t n = (verdict == POWER_CUT_TORN) ? (QSPI_FLASH_SECTOR_SIZE_BYTES / 2U)
                                                   : QSPI_FLASH_SECTOR_SIZE_BYTES;

    (void) memset(&s_mem[sector * QSPI_FLASH_SECTOR_SIZE_BYTES], 0xFF, n);
    s_sector_erases[sector]++;

    return (verdict == POWER_CUT_TORN) ? QSPI_FLASH_ERR_TIMEOUT : QSPI_FLASH_OK;
}

const iqspi_flash_t fake_qspi_flash_if = {
    .read = fake_read,
    .write_page = fake_write_page,
    .erase_sector = fake_erase_sector,
};

void fake_qspi_reset(void)
{
    (void) memset(s_mem, 0xFF, sizeof(s_mem));
    (void) memset(s_sector_erases, 0, sizeof(s_sector_erases));
    s_reads = 0U;
    s_programs = 0U;
    s_erases = 0U;
    s_violations = 0U;
    s_fail_all_reads = false;
    s_fail_read_addr = FAKE_QSPI_NO_ADDR;
    s_fail_program_addr = FAKE_QSPI_NO_ADDR;
    s_fail_erase_addr = FAKE_QSPI_NO_ADDR;
}

uint8_t *fake_qspi_mem(void)
{
    return s_mem;
}

uint32_t fake_qspi_read_calls(void)
{
    return s_reads;
}

uint32_t fake_qspi_program_calls(void)
{
    return s_programs;
}

uint32_t fake_qspi_erase_calls(void)
{
    return s_erases;
}

uint32_t fake_qspi_rule_violations(void)
{
    return s_violations;
}

uint32_t fake_qspi_sector_erases(uint32_t sector)
{
    return (sector < FAKE_QSPI_SECTORS) ? s_sector_erases[sector] : 0U;
}

void fake_qspi_fail_all_reads(bool fail)
{
    s_fail_all_reads = fail;
}

void fake_qspi_fail_read_at(uint32_t addr)
{
    s_fail_read_addr = addr;
}

void fake_qspi_fail_program_at(uint32_t addr)
{
    s_fail_program_addr = addr;
}

void fake_qspi_fail_erase_at(uint32_t addr)
{
    s_fail_erase_addr = addr;
}
