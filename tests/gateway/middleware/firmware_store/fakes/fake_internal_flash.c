/**
 * @file fake_internal_flash.c
 * @brief RAM on-chip flash with ECC semantics (FirmwareStore tests).
 */

#include "fake_internal_flash.h"

#include <string.h>

#include "power_cut.h"

#define MAX_GUARDS (8U)

static uint8_t s_mem[FAKE_IFLASH_SIZE];

static uint32_t s_reads;
static uint32_t s_programs;
static uint32_t s_erases;
static uint32_t s_violations;
static uint32_t s_guard_hits;

static uint32_t s_guard_lo[MAX_GUARDS];
static uint32_t s_guard_hi[MAX_GUARDS];
static uint32_t s_guard_count;

static fake_iflash_log_entry_t s_log[FAKE_IFLASH_LOG_MAX];
static uint32_t s_log_count;

static uint32_t s_fail_erase_addr = FAKE_IFLASH_NO_ADDR;
static uint32_t s_fail_program_addr = FAKE_IFLASH_NO_ADDR;
static uint32_t s_fail_program_once_addr = FAKE_IFLASH_NO_ADDR;
static uint32_t s_fail_read_addr = FAKE_IFLASH_NO_ADDR;
static uint32_t s_corrupt_addr = FAKE_IFLASH_NO_ADDR;
static bool s_fail_all_reads;

static bool in_range(uint32_t addr, uint32_t len)
{
    return (addr >= FAKE_IFLASH_BASE) && (addr < (FAKE_IFLASH_BASE + FAKE_IFLASH_SIZE)) &&
           (len <= ((FAKE_IFLASH_BASE + FAKE_IFLASH_SIZE) - addr));
}

static void note_guard(uint32_t addr, uint32_t len)
{
    for (uint32_t i = 0U; i < s_guard_count; i++)
    {
        if ((addr < s_guard_hi[i]) && ((addr + len) > s_guard_lo[i]))
        {
            s_guard_hits++;
        }
    }
}

static void log_op(fake_iflash_op_t op, uint32_t addr, uint32_t len)
{
    if (s_log_count < FAKE_IFLASH_LOG_MAX)
    {
        s_log[s_log_count].op = op;
        s_log[s_log_count].addr = addr;
        s_log[s_log_count].len = len;
        s_log_count++;
    }
}

static internal_flash_err_t fake_erase_page(uint32_t addr)
{
    s_erases++;

    if (((addr % INTERNAL_FLASH_PAGE_SIZE_BYTES) != 0U) ||
        !in_range(addr, INTERNAL_FLASH_PAGE_SIZE_BYTES))
    {
        s_violations++;
        return INTERNAL_FLASH_ERR_ADDR;
    }

    note_guard(addr, INTERNAL_FLASH_PAGE_SIZE_BYTES);
    log_op(FAKE_IFLASH_OP_ERASE, addr, INTERNAL_FLASH_PAGE_SIZE_BYTES);

    if ((addr / INTERNAL_FLASH_PAGE_SIZE_BYTES) ==
        (s_fail_erase_addr / INTERNAL_FLASH_PAGE_SIZE_BYTES))
    {
        return INTERNAL_FLASH_ERR_ERASE;
    }

    const power_cut_verdict_t verdict = power_cut_tick();

    if (verdict == POWER_CUT_DEAD)
    {
        return INTERNAL_FLASH_ERR_ERASE;
    }

    /* A torn erase clears only the first half of the page. */
    const uint32_t n = (verdict == POWER_CUT_TORN) ? (INTERNAL_FLASH_PAGE_SIZE_BYTES / 2U)
                                                   : INTERNAL_FLASH_PAGE_SIZE_BYTES;

    (void) memset(&s_mem[addr - FAKE_IFLASH_BASE], 0xFF, n);

    return (verdict == POWER_CUT_TORN) ? INTERNAL_FLASH_ERR_ERASE : INTERNAL_FLASH_OK;
}

static internal_flash_err_t fake_program(uint32_t addr, const uint8_t *data, uint32_t len)
{
    s_programs++;

    if (data == NULL)
    {
        s_violations++;
        return INTERNAL_FLASH_ERR_NULL_POINTER;
    }

    if (((addr % INTERNAL_FLASH_DWORD_SIZE_BYTES) != 0U) ||
        ((len % INTERNAL_FLASH_DWORD_SIZE_BYTES) != 0U) || (len == 0U))
    {
        s_violations++;
        return INTERNAL_FLASH_ERR_ALIGN;
    }

    if (!in_range(addr, len))
    {
        s_violations++;
        return INTERNAL_FLASH_ERR_ADDR;
    }

    note_guard(addr, len);
    log_op(FAKE_IFLASH_OP_PROGRAM, addr, len);

    /* ECC: every target double word must still be erased. */
    for (uint32_t off = 0U; off < len; off += INTERNAL_FLASH_DWORD_SIZE_BYTES)
    {
        for (uint32_t b = 0U; b < INTERNAL_FLASH_DWORD_SIZE_BYTES; b++)
        {
            if (s_mem[addr - FAKE_IFLASH_BASE + off + b] != 0xFFU)
            {
                s_violations++;
                return INTERNAL_FLASH_ERR_PROGRAM;
            }
        }
    }

    if (addr == s_fail_program_addr)
    {
        return INTERNAL_FLASH_ERR_PROGRAM;
    }

    if (addr == s_fail_program_once_addr)
    {
        s_fail_program_once_addr = FAKE_IFLASH_NO_ADDR;
        return INTERNAL_FLASH_ERR_PROGRAM;
    }

    const power_cut_verdict_t verdict = power_cut_tick();

    if (verdict == POWER_CUT_DEAD)
    {
        return INTERNAL_FLASH_ERR_PROGRAM;
    }

    /* A torn program lands only its first whole double words up to the half-way point. */
    const uint32_t n =
        (verdict == POWER_CUT_TORN)
            ? (((len / 2U) / INTERNAL_FLASH_DWORD_SIZE_BYTES) * INTERNAL_FLASH_DWORD_SIZE_BYTES)
            : len;

    (void) memcpy(&s_mem[addr - FAKE_IFLASH_BASE], data, n);

    if ((s_corrupt_addr >= addr) && (s_corrupt_addr < (addr + n)))
    {
        s_mem[s_corrupt_addr - FAKE_IFLASH_BASE] ^= 0x01U;
    }

    return (verdict == POWER_CUT_TORN) ? INTERNAL_FLASH_ERR_PROGRAM : INTERNAL_FLASH_OK;
}

static internal_flash_err_t fake_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    s_reads++;

    if (s_fail_all_reads || (addr == s_fail_read_addr))
    {
        return INTERNAL_FLASH_ERR_ADDR;
    }

    if ((buf == NULL) || (len == 0U) || !in_range(addr, len))
    {
        s_violations++;
        return INTERNAL_FLASH_ERR_ADDR;
    }

    (void) memcpy(buf, &s_mem[addr - FAKE_IFLASH_BASE], len);

    return INTERNAL_FLASH_OK;
}

const iinternal_flash_t fake_iflash_if = {
    .erase_page = fake_erase_page,
    .program = fake_program,
    .read = fake_read,
};

void fake_iflash_reset(void)
{
    (void) memset(s_mem, 0xFF, sizeof(s_mem));
    s_reads = 0U;
    s_programs = 0U;
    s_erases = 0U;
    s_violations = 0U;
    s_guard_hits = 0U;
    s_guard_count = 0U;
    s_log_count = 0U;
    s_fail_erase_addr = FAKE_IFLASH_NO_ADDR;
    s_fail_program_addr = FAKE_IFLASH_NO_ADDR;
    s_fail_program_once_addr = FAKE_IFLASH_NO_ADDR;
    s_fail_read_addr = FAKE_IFLASH_NO_ADDR;
    s_corrupt_addr = FAKE_IFLASH_NO_ADDR;
    s_fail_all_reads = false;
}

uint8_t *fake_iflash_ptr(uint32_t addr)
{
    return &s_mem[addr - FAKE_IFLASH_BASE];
}

uint32_t fake_iflash_erase_calls(void)
{
    return s_erases;
}

uint32_t fake_iflash_program_calls(void)
{
    return s_programs;
}

uint32_t fake_iflash_read_calls(void)
{
    return s_reads;
}

uint32_t fake_iflash_rule_violations(void)
{
    return s_violations;
}

void fake_iflash_guard_add(uint32_t lo, uint32_t hi)
{
    if (s_guard_count < MAX_GUARDS)
    {
        s_guard_lo[s_guard_count] = lo;
        s_guard_hi[s_guard_count] = hi;
        s_guard_count++;
    }
}

uint32_t fake_iflash_guard_violations(void)
{
    return s_guard_hits;
}

uint32_t fake_iflash_log_count(void)
{
    return s_log_count;
}

const fake_iflash_log_entry_t *fake_iflash_log_at(uint32_t index)
{
    return (index < s_log_count) ? &s_log[index] : NULL;
}

void fake_iflash_fail_erase_at(uint32_t addr)
{
    s_fail_erase_addr = addr;
}

void fake_iflash_fail_program_at(uint32_t addr)
{
    s_fail_program_addr = addr;
}

void fake_iflash_fail_program_once_at(uint32_t addr)
{
    s_fail_program_once_addr = addr;
}

void fake_iflash_fail_all_reads(bool fail)
{
    s_fail_all_reads = fail;
}

void fake_iflash_fail_read_at(uint32_t addr)
{
    s_fail_read_addr = addr;
}

void fake_iflash_corrupt_after_program(uint32_t addr)
{
    s_corrupt_addr = addr;
}
