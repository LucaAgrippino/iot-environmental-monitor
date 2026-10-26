/**
 * @file fake_flash_ctrl.c
 * @brief Host model of the STM32L475 flash controller (InternalFlashDriver tests).
 */

#include "fake_flash_ctrl.h"

#include <string.h>

#include "internal_flash_hw.h"

#define PAGE_BYTES (2048UL)
#define PAGES_PER_BANK (256UL)
#define DEFAULT_BUSY_POLLS (2U)
#define EVENT_MAX (4096U)
#define NO_PAGE (0xFFFFFFFFUL)

static uint8_t s_mem[FAKE_FLASH_SIZE];

static uint32_t s_cr;
static uint32_t s_sr;
static uint32_t s_acr;
static uint32_t s_keyr_stage;
static bool s_dual_bank;
static bool s_stuck_lock;
static bool s_stuck_busy;
static uint32_t s_busy_polls_cfg;
static uint32_t s_busy_remaining;
static uint32_t s_protected_page;

static bool s_pg_pending;
static uint32_t s_pg_addr;
static uint32_t s_pg_low;

static bool s_irq_masked;
static uint32_t s_irq_pairs;
static uint32_t s_stores_masked;
static uint32_t s_stores_unmasked;
static uint32_t s_flushes;

static fake_flash_event_t s_events[EVENT_MAX];
static uint32_t s_event_count;

static void record(fake_flash_event_kind_t kind, uint32_t addr, uint32_t value)
{
    if (s_event_count < EVENT_MAX)
    {
        s_events[s_event_count].kind = kind;
        s_events[s_event_count].addr = addr;
        s_events[s_event_count].value = value;
        s_events[s_event_count].irq_masked = s_irq_masked;
        s_event_count++;
    }
}

static bool in_range(uint32_t addr, uint32_t len)
{
    return (addr >= FAKE_FLASH_BASE) && (addr < (FAKE_FLASH_BASE + FAKE_FLASH_SIZE)) &&
           (len <= ((FAKE_FLASH_BASE + FAKE_FLASH_SIZE) - addr));
}

static bool busy_now(void)
{
    return s_stuck_busy || (s_busy_remaining > 0U);
}

void fake_flash_reset(void)
{
    (void) memset(s_mem, 0xFF, sizeof(s_mem));
    s_cr = IFLASH_CR_LOCK;
    s_sr = 0U;
    s_acr = IFLASH_ACR_ICEN | IFLASH_ACR_DCEN;
    s_keyr_stage = 0U;
    s_dual_bank = true;
    s_stuck_lock = false;
    s_stuck_busy = false;
    s_busy_polls_cfg = DEFAULT_BUSY_POLLS;
    s_busy_remaining = 0U;
    s_protected_page = NO_PAGE;
    s_pg_pending = false;
    s_irq_masked = false;
    s_irq_pairs = 0U;
    s_stores_masked = 0U;
    s_stores_unmasked = 0U;
    s_flushes = 0U;
    s_event_count = 0U;
}

/* ---- the controller ---------------------------------------------------- */

void fake_flash_keyr_write(uint32_t value)
{
    record(FAKE_EVT_KEYR, 0U, value);

    if (((s_cr & IFLASH_CR_LOCK) == 0U) || s_stuck_lock)
    {
        return;
    }

    if ((s_keyr_stage == 0U) && (value == IFLASH_KEY1))
    {
        s_keyr_stage = 1U;
    }
    else if ((s_keyr_stage == 1U) && (value == IFLASH_KEY2))
    {
        s_keyr_stage = 0U;
        s_cr &= ~IFLASH_CR_LOCK;
    }
    else
    {
        /* A wrong key locks the controller until the next reset. */
        s_keyr_stage = 0U;
        s_stuck_lock = true;
    }
}

uint32_t fake_flash_sr_read(void)
{
    if (s_stuck_busy)
    {
        return s_sr | IFLASH_SR_BSY;
    }

    if (s_busy_remaining > 0U)
    {
        s_busy_remaining--;
        return s_sr | IFLASH_SR_BSY;
    }

    return s_sr;
}

void fake_flash_sr_write(uint32_t value)
{
    record(FAKE_EVT_SR, 0U, value);
    s_sr &= ~(value & ~IFLASH_SR_BSY);
}

uint32_t fake_flash_cr_read(void)
{
    return s_cr;
}

static void start_erase(void)
{
    const uint32_t page = (s_cr & IFLASH_CR_PNB_MASK) >> IFLASH_CR_PNB_POS;
    const uint32_t index = page + (((s_cr & IFLASH_CR_BKER) != 0U) ? PAGES_PER_BANK : 0U);

    if (index == s_protected_page)
    {
        s_sr |= IFLASH_SR_WRPERR;
        return;
    }

    (void) memset(&s_mem[index * PAGE_BYTES], 0xFF, PAGE_BYTES);
    s_sr |= IFLASH_SR_EOP;
    s_busy_remaining = s_busy_polls_cfg;
}

void fake_flash_cr_write(uint32_t value)
{
    record(FAKE_EVT_CR, 0U, value);

    if ((s_cr & IFLASH_CR_LOCK) != 0U)
    {
        return; /* CR is write-protected while locked */
    }

    if ((value & IFLASH_CR_LOCK) != 0U)
    {
        s_cr = IFLASH_CR_LOCK; /* locking clears PG and PER */
        s_pg_pending = false;
        return;
    }

    const bool start = ((value & IFLASH_CR_START) != 0U) && ((s_cr & IFLASH_CR_START) == 0U);

    s_cr = value & ~IFLASH_CR_START; /* START self-clears */

    if (start && !busy_now() && ((value & IFLASH_CR_PER) != 0U))
    {
        start_erase();
    }
}

void fake_flash_store32(uint32_t addr, uint32_t value)
{
    record(FAKE_EVT_STORE, addr, value);

    if (s_irq_masked)
    {
        s_stores_masked++;
    }
    else
    {
        s_stores_unmasked++;
    }

    if (((s_cr & IFLASH_CR_LOCK) != 0U) || ((s_cr & IFLASH_CR_PG) == 0U) || !in_range(addr, 4U))
    {
        return;
    }

    if (!s_pg_pending)
    {
        if ((addr % 8U) != 0U)
        {
            s_sr |= IFLASH_SR_PGAERR;
            return;
        }

        s_pg_pending = true;
        s_pg_addr = addr;
        s_pg_low = value;
        return;
    }

    s_pg_pending = false;

    if (addr != (s_pg_addr + 4U))
    {
        s_sr |= IFLASH_SR_PGSERR;
        return;
    }

    uint8_t *target = &s_mem[s_pg_addr - FAKE_FLASH_BASE];

    for (uint32_t i = 0U; i < 8U; i++)
    {
        if (target[i] != 0xFFU)
        {
            s_sr |= IFLASH_SR_PROGERR; /* ECC: a double word programs once */
            return;
        }
    }

    (void) memcpy(&target[0], &s_pg_low, sizeof(s_pg_low));
    (void) memcpy(&target[4], &value, sizeof(value));
    s_sr |= IFLASH_SR_EOP;
    s_busy_remaining = s_busy_polls_cfg;
}

void fake_flash_load(uint32_t addr, uint8_t *buf, uint32_t len)
{
    (void) memcpy(buf, &s_mem[addr - FAKE_FLASH_BASE], len);
}

uint32_t fake_flash_optr_read(void)
{
    return s_dual_bank ? IFLASH_OPTR_DUALBANK : 0U;
}

uint32_t fake_flash_acr_read(void)
{
    return s_acr;
}

void fake_flash_acr_write(uint32_t value)
{
    record(FAKE_EVT_ACR, 0U, value);

    if ((value & (IFLASH_ACR_ICRST | IFLASH_ACR_DCRST)) != 0U)
    {
        s_flushes++;
    }

    s_acr = value;
}

uint32_t fake_flash_irq_save(void)
{
    const uint32_t previous = s_irq_masked ? 1U : 0U;

    s_irq_masked = true;

    return previous;
}

void fake_flash_irq_restore(uint32_t state)
{
    s_irq_masked = (state != 0U);
    s_irq_pairs++;
}

/* ---- setup and observation --------------------------------------------- */

void fake_flash_set_dual_bank(bool dual)
{
    s_dual_bank = dual;
}

void fake_flash_set_busy_polls(uint32_t polls)
{
    s_busy_polls_cfg = polls;
}

void fake_flash_set_initial_busy(uint32_t polls)
{
    s_busy_remaining = polls;
}

void fake_flash_set_stuck_busy(bool stuck)
{
    s_stuck_busy = stuck;
}

void fake_flash_set_stuck_lock(bool stuck)
{
    s_stuck_lock = stuck;
}

void fake_flash_set_protected_page(uint32_t page_index)
{
    s_protected_page = page_index;
}

void fake_flash_preset_sr(uint32_t flags)
{
    s_sr |= flags;
}

uint8_t *fake_flash_mem(uint32_t addr)
{
    return &s_mem[addr - FAKE_FLASH_BASE];
}

bool fake_flash_is_locked(void)
{
    return (s_cr & IFLASH_CR_LOCK) != 0U;
}

uint32_t fake_flash_cr_value(void)
{
    return s_cr;
}

uint32_t fake_flash_sr_value(void)
{
    return s_sr;
}

uint32_t fake_flash_event_count(void)
{
    return s_event_count;
}

const fake_flash_event_t *fake_flash_event_at(uint32_t index)
{
    return (index < s_event_count) ? &s_events[index] : NULL;
}

uint32_t fake_flash_cache_flushes(void)
{
    return s_flushes;
}

uint32_t fake_flash_acr_value(void)
{
    return s_acr;
}

bool fake_flash_irq_is_masked(void)
{
    return s_irq_masked;
}

uint32_t fake_flash_irq_pairs(void)
{
    return s_irq_pairs;
}

uint32_t fake_flash_stores_masked(void)
{
    return s_stores_masked;
}

uint32_t fake_flash_stores_unmasked(void)
{
    return s_stores_unmasked;
}
