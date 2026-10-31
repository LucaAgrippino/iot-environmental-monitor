/**
 * @file internal_flash.c
 * @brief InternalFlashDriver — erase, program and read the on-chip flash (Gateway).
 *
 * @see docs/lld/drivers/internal-flash-driver.md
 */

#include "internal_flash.h"

#include <stdbool.h>
#include <string.h>

#include "internal_flash_hw.h"

/** Bytes of a double word, the program unit. */
#define IFLASH_DWORD_BYTES (INTERNAL_FLASH_DWORD_SIZE_BYTES)

/** Bytes of one word, the bus access size. */
#define IFLASH_WORD_BYTES (4U)

/** Pages in one bank. */
#define IFLASH_PAGES_PER_BANK (256U)

/** Bound on a BSY wait: >= 200 ms at 80 MHz against a 24.5 ms worst-case erase (companion §3.5). */
#define IFLASH_BSY_POLL_LIMIT (4000000UL)

/** Every status flag the driver clears after an operation (write 1 to clear). */
#define IFLASH_SR_CLEARABLE (IFLASH_SR_EOP | IFLASH_SR_ERRORS)

static bool s_initialised;

/* ====================================================================== */
/* Helpers                                                                */
/* ====================================================================== */

/** @brief True if [addr, addr + len) lies inside the 1 MB main flash. */
static bool in_flash(uint32_t addr, uint32_t len)
{
    const uint32_t end = INTERNAL_FLASH_BASE_ADDR + INTERNAL_FLASH_SIZE_BYTES;

    return (addr >= INTERNAL_FLASH_BASE_ADDR) && (addr < end) && (len <= (end - addr));
}

/** @brief True if [addr, addr + len) lies in main flash and above the bootloader. */
static bool in_writable_flash(uint32_t addr, uint32_t len)
{
    return in_flash(addr, len) && (addr >= INTERNAL_FLASH_WRITABLE_FROM_ADDR);
}

/** @brief Wait, bounded, for the controller to go idle. */
static bool wait_idle(void)
{
    uint32_t polls = IFLASH_BSY_POLL_LIMIT;

    while ((IFLASH_HW_SR_READ() & IFLASH_SR_BSY) != 0U)
    {
        if (polls == 0U)
        {
            return false;
        }

        polls--;
    }

    return true;
}

/** @brief Clear EOP and every error flag. */
static void clear_flags(void)
{
    IFLASH_HW_SR_WRITE(IFLASH_SR_CLEARABLE);
}

/** @brief Run the unlock sequence; true if the controller is then unlocked. */
static bool unlock(void)
{
    if ((IFLASH_HW_CR_READ() & IFLASH_CR_LOCK) != 0U)
    {
        IFLASH_HW_KEYR_WRITE(IFLASH_KEY1);
        IFLASH_HW_KEYR_WRITE(IFLASH_KEY2);
    }

    return (IFLASH_HW_CR_READ() & IFLASH_CR_LOCK) == 0U;
}

/** @brief Drop PG/PER, lock the controller and make the caches see the new contents. */
static void lock_and_flush(void)
{
    const uint32_t acr = IFLASH_HW_ACR_READ();
    const uint32_t enables = IFLASH_ACR_ICEN | IFLASH_ACR_DCEN;
    const uint32_t resets = IFLASH_ACR_ICRST | IFLASH_ACR_DCRST;

    IFLASH_HW_CR_WRITE(IFLASH_CR_LOCK);

    /* Caches off, reset pulse, caches back to what they were. */
    IFLASH_HW_ACR_WRITE(acr & ~enables);
    IFLASH_HW_ACR_WRITE((acr & ~enables) | resets);
    IFLASH_HW_ACR_WRITE(acr & ~enables & ~resets);
    IFLASH_HW_ACR_WRITE((acr & ~resets) | (acr & enables));
    IFLASH_HW_BARRIER();
}

/* ====================================================================== */
/* Public API                                                             */
/* ====================================================================== */

internal_flash_err_t internal_flash_init(void)
{
    if ((IFLASH_HW_OPTR_READ() & IFLASH_OPTR_DUALBANK) == 0U)
    {
        s_initialised = false;
        return INTERNAL_FLASH_ERR_BANK_MODE;
    }

    /* Make sure the controller starts locked. */
    if ((IFLASH_HW_CR_READ() & IFLASH_CR_LOCK) == 0U)
    {
        IFLASH_HW_CR_WRITE(IFLASH_CR_LOCK);
    }

    s_initialised = true;

    return INTERNAL_FLASH_OK;
}

internal_flash_err_t internal_flash_erase_page(uint32_t addr)
{
    if (!s_initialised)
    {
        return INTERNAL_FLASH_ERR_NOT_INITIALISED;
    }

    if ((addr % INTERNAL_FLASH_PAGE_SIZE_BYTES) != 0U)
    {
        return INTERNAL_FLASH_ERR_ALIGN;
    }

    if (!in_writable_flash(addr, INTERNAL_FLASH_PAGE_SIZE_BYTES))
    {
        return INTERNAL_FLASH_ERR_ADDR;
    }

    const uint32_t index = (addr - INTERNAL_FLASH_BASE_ADDR) / INTERNAL_FLASH_PAGE_SIZE_BYTES;
    const bool bank2 = (index >= IFLASH_PAGES_PER_BANK);
    const uint32_t page = bank2 ? (index - IFLASH_PAGES_PER_BANK) : index;

    if (!wait_idle())
    {
        return INTERNAL_FLASH_ERR_ERASE;
    }

    clear_flags();

    if (!unlock())
    {
        return INTERNAL_FLASH_ERR_ERASE;
    }

    const uint32_t select =
        IFLASH_CR_PER | (page << IFLASH_CR_PNB_POS) | (bank2 ? IFLASH_CR_BKER : 0U);

    IFLASH_HW_CR_WRITE(select);
    IFLASH_HW_CR_WRITE(select | IFLASH_CR_START);

    const bool idle = wait_idle();
    const uint32_t status = IFLASH_HW_SR_READ();

    clear_flags();
    lock_and_flush();

    return (idle && ((status & IFLASH_SR_ERRORS) == 0U)) ? INTERNAL_FLASH_OK
                                                         : INTERNAL_FLASH_ERR_ERASE;
}

internal_flash_err_t internal_flash_program(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (!s_initialised)
    {
        return INTERNAL_FLASH_ERR_NOT_INITIALISED;
    }

    if (data == NULL)
    {
        return INTERNAL_FLASH_ERR_NULL_POINTER;
    }

    if (((addr % IFLASH_DWORD_BYTES) != 0U) || ((len % IFLASH_DWORD_BYTES) != 0U) || (len == 0U))
    {
        return INTERNAL_FLASH_ERR_ALIGN;
    }

    if (!in_writable_flash(addr, len))
    {
        return INTERNAL_FLASH_ERR_ADDR;
    }

    if (!wait_idle())
    {
        return INTERNAL_FLASH_ERR_PROGRAM;
    }

    clear_flags();

    if (!unlock())
    {
        return INTERNAL_FLASH_ERR_PROGRAM;
    }

    IFLASH_HW_CR_WRITE(IFLASH_CR_PG);

    internal_flash_err_t result = INTERNAL_FLASH_OK;

    for (uint32_t off = 0U; (off < len) && (result == INTERNAL_FLASH_OK); off += IFLASH_DWORD_BYTES)
    {
        uint32_t low;
        uint32_t high;

        (void) memcpy(&low, &data[off], IFLASH_WORD_BYTES);
        (void) memcpy(&high, &data[off + IFLASH_WORD_BYTES], IFLASH_WORD_BYTES);

        /* The two stores of one double word must be consecutive (companion §4). */
        const uint32_t mask = IFLASH_HW_IRQ_SAVE();

        IFLASH_HW_STORE32(addr + off, low);
        IFLASH_HW_STORE32(addr + off + IFLASH_WORD_BYTES, high);
        IFLASH_HW_IRQ_RESTORE(mask);
        IFLASH_HW_BARRIER();

        if (!wait_idle() || ((IFLASH_HW_SR_READ() & IFLASH_SR_ERRORS) != 0U))
        {
            result = INTERNAL_FLASH_ERR_PROGRAM;
        }
        else
        {
            IFLASH_HW_SR_WRITE(IFLASH_SR_EOP);
        }
    }

    clear_flags();
    lock_and_flush();

    return result;
}

internal_flash_err_t internal_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (!s_initialised)
    {
        return INTERNAL_FLASH_ERR_NOT_INITIALISED;
    }

    if (buf == NULL)
    {
        return INTERNAL_FLASH_ERR_NULL_POINTER;
    }

    if ((len == 0U) || !in_flash(addr, len))
    {
        return INTERNAL_FLASH_ERR_ADDR;
    }

    IFLASH_HW_LOAD(addr, buf, len);

    return INTERNAL_FLASH_OK;
}

static const iinternal_flash_t s_vtable = {
    .erase_page = internal_flash_erase_page,
    .program = internal_flash_program,
    .read = internal_flash_read,
};

const iinternal_flash_t *const internal_flash_driver = &s_vtable;

#ifdef TEST
void internal_flash_reset_for_test(void)
{
    s_initialised = false;
}
#endif
