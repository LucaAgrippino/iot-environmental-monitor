/**
 * @file boot_core.c
 * @brief Bootloader decision logic (see boot_core.h).
 *
 * @see docs/lld/drivers/bootloader.md §3
 */

#include "boot_core.h"

#include <stdbool.h>
#include <string.h>

#include "config_store/config_store_crc.h"

#define BOOT_CHUNK_BYTES (256U)
#define BOOT_WRITE_ATTEMPTS (2U)

/** Bytes of a boot record that are programmed before the CRC double word. */
#define BOOT_META_HEAD_BYTES (16U)

/** SRAM1 + SRAM2 as seen at 0x2000_0000 (end 0x2002_0000), and SRAM2 at its own address. */
#define BOOT_SRAM_ALIAS_BASE (0x20000000UL)
#define BOOT_SRAM_ALIAS_END (0x20020000UL)
#define BOOT_SRAM2_BASE (0x10000000UL)
#define BOOT_SRAM2_END (0x10008000UL)

#define BOOT_THUMB_BIT (1UL)
#define BOOT_SP_ALIGN_MASK (7UL)

/** @brief The record in force: the valid copy with the higher generation. */
typedef struct
{
    bool valid;
    uint8_t copy; /**< 0 = copy A, 1 = copy B. */
    firmware_boot_meta_t rec;
} boot_record_t;

static bool bank_is_legal(uint8_t bank)
{
    return (bank == FIRMWARE_BANK_A) || (bank == FIRMWARE_BANK_B);
}

static uint8_t other_bank(uint8_t bank)
{
    return (bank == FIRMWARE_BANK_A) ? (uint8_t) FIRMWARE_BANK_B : (uint8_t) FIRMWARE_BANK_A;
}

static uint32_t slot_base(uint8_t bank)
{
    return (bank == FIRMWARE_BANK_B) ? FIRMWARE_SLOT_B_BASE : FIRMWARE_SLOT_A_BASE;
}

static uint32_t desc_addr(uint8_t bank)
{
    return (bank == FIRMWARE_BANK_B) ? FIRMWARE_DESC_B_ADDR : FIRMWARE_DESC_A_ADDR;
}

static uint32_t meta_addr(uint8_t copy)
{
    return (copy == 0U) ? FIRMWARE_META_A_ADDR : FIRMWARE_META_B_ADDR;
}

static bool record_is_valid(const firmware_boot_meta_t *r)
{
    return (r->magic == FIRMWARE_META_MAGIC) &&
           (r->crc32 == config_store_crc32((const uint8_t *) r, FIRMWARE_META_CRC_SPAN)) &&
           bank_is_legal(r->active_bank) && bank_is_legal(r->previous_bank) &&
           (r->boot_state >= FIRMWARE_META_STATE_CONFIRMED) &&
           (r->boot_state <= FIRMWARE_META_STATE_ROLLED_BACK);
}

/** @brief True if every byte of the record is erased: a copy that was never written is no fault. */
static bool record_is_blank(const firmware_boot_meta_t *r)
{
    const uint8_t *const p = (const uint8_t *) r;

    for (uint32_t i = 0U; i < sizeof(*r); i++)
    {
        if (p[i] != 0xFFU)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief Read both copies and pick the valid one with the higher generation (copy A on a tie).
 *
 * BOOT_EVT_COPY_INVALID is raised for a copy that holds something but fails validation (a torn
 * write); a blank copy, the normal state of the one not yet used, raises nothing.
 *
 * @return false if a copy could not be read at all, which is a flash fault and not "no record".
 */
static bool record_select(const iinternal_flash_t *fl, boot_record_t *cur, uint32_t *events)
{
    cur->valid = false;
    cur->copy = 0U;

    for (uint8_t c = 0U; c < 2U; c++)
    {
        firmware_boot_meta_t r;

        if (fl->read(meta_addr(c), (uint8_t *) &r, sizeof(r)) != INTERNAL_FLASH_OK)
        {
            return false;
        }

        if (!record_is_valid(&r))
        {
            if (!record_is_blank(&r))
            {
                *events |= BOOT_EVT_COPY_INVALID;
            }

            continue;
        }

        if (!cur->valid || (r.generation > cur->rec.generation))
        {
            cur->valid = true;
            cur->copy = c;
            cur->rec = r;
        }
    }

    return true;
}

/** @brief One attempt of the write protocol: erase, program (CRC last), read back, compare. */
static bool record_write_once(const iinternal_flash_t *fl, uint32_t addr,
                              const firmware_boot_meta_t *r)
{
    const uint8_t *const bytes = (const uint8_t *) r;
    uint8_t back[sizeof(firmware_boot_meta_t)];

    if (fl->erase_page(addr) != INTERNAL_FLASH_OK)
    {
        return false;
    }

    if ((fl->program(addr, bytes, BOOT_META_HEAD_BYTES) != INTERNAL_FLASH_OK) ||
        (fl->program(addr + BOOT_META_HEAD_BYTES, bytes + BOOT_META_HEAD_BYTES,
                     (uint32_t) sizeof(*r) - BOOT_META_HEAD_BYTES) != INTERNAL_FLASH_OK))
    {
        return false;
    }

    return (fl->read(addr, back, (uint32_t) sizeof(back)) == INTERNAL_FLASH_OK) &&
           (memcmp(back, bytes, sizeof(back)) == 0);
}

/**
 * @brief Write a new record to the copy that is not selected (copy A if none is), then make it
 * the one in force. A failure leaves @p cur, and the selected copy on flash, untouched.
 */
static bool record_write(const iinternal_flash_t *fl, boot_record_t *cur, uint8_t active,
                         uint8_t state, uint8_t previous, uint32_t count)
{
    const uint8_t target = (cur->valid && (cur->copy == 0U)) ? 1U : 0U;
    firmware_boot_meta_t r = {
        .magic = FIRMWARE_META_MAGIC,
        .active_bank = active,
        .boot_state = state,
        .previous_bank = previous,
        .reserved = 0xFFU,
        .rollback_count = count,
        .generation = cur->valid ? (cur->rec.generation + 1U) : 1U,
        .crc32 = 0U,
        .pad = FIRMWARE_ERASED_WORD,
    };

    r.crc32 = config_store_crc32((const uint8_t *) &r, FIRMWARE_META_CRC_SPAN);

    for (uint8_t attempt = 0U; attempt < BOOT_WRITE_ATTEMPTS; attempt++)
    {
        if (record_write_once(fl, meta_addr(target), &r))
        {
            cur->valid = true;
            cur->copy = target;
            cur->rec = r;
            return true;
        }
    }

    return false;
}

/** @brief True if the vector pair could be the start of an image of @p len bytes at @p base. */
static bool vectors_are_plausible(uint32_t sp, uint32_t pc, uint32_t base, uint32_t len)
{
    const bool sp_ok = ((sp & BOOT_SP_ALIGN_MASK) == 0U) &&
                       (((sp > BOOT_SRAM_ALIAS_BASE) && (sp <= BOOT_SRAM_ALIAS_END)) ||
                        ((sp > BOOT_SRAM2_BASE) && (sp <= BOOT_SRAM2_END)));
    const uint32_t entry = pc & ~BOOT_THUMB_BIT;
    const bool pc_ok = ((pc & BOOT_THUMB_BIT) != 0U) && (entry >= base) && (entry < (base + len));

    return sp_ok && pc_ok;
}

/** @brief Descriptor, body CRC and vector checks of one slot (docs/lld/drivers/bootloader.md §3.3).
 */
static bool slot_is_bootable(const iinternal_flash_t *fl, uint8_t bank, uint32_t *len_out,
                             uint32_t *version_out)
{
    const uint32_t base = slot_base(bank);
    firmware_slot_descriptor_t d;
    uint32_t vectors[2];
    uint8_t chunk[BOOT_CHUNK_BYTES];

    if (fl->read(desc_addr(bank), (uint8_t *) &d, (uint32_t) sizeof(d)) != INTERNAL_FLASH_OK)
    {
        return false;
    }

    if ((d.magic != FIRMWARE_DESC_MAGIC) ||
        (d.desc_crc32 != config_store_crc32((const uint8_t *) &d, FIRMWARE_DESC_CRC_SPAN)) ||
        (d.load_addr != base) || (d.image_len == 0U) || (d.image_len > FIRMWARE_SLOT_SIZE))
    {
        return false;
    }

    uint32_t crc = config_store_crc32_begin();

    for (uint32_t off = 0U; off < d.image_len; off += BOOT_CHUNK_BYTES)
    {
        const uint32_t left = d.image_len - off;
        const uint32_t n = (left < BOOT_CHUNK_BYTES) ? left : BOOT_CHUNK_BYTES;

        if (fl->read(base + off, chunk, n) != INTERNAL_FLASH_OK)
        {
            return false;
        }

        crc = config_store_crc32_feed(crc, chunk, n);
    }

    if (config_store_crc32_end(crc) != d.body_crc32)
    {
        return false;
    }

    if ((fl->read(base, (uint8_t *) vectors, (uint32_t) sizeof(vectors)) != INTERNAL_FLASH_OK) ||
        !vectors_are_plausible(vectors[0], vectors[1], base, d.image_len))
    {
        return false;
    }

    *len_out = d.image_len;
    *version_out = d.version;

    return true;
}

static boot_outcome_t finish(boot_report_t *rep, const boot_record_t *cur, boot_outcome_t outcome)
{
    rep->outcome = outcome;

    if (cur->valid)
    {
        rep->state_out = cur->rec.boot_state;
        rep->active_bank = cur->rec.active_bank;
        rep->generation = cur->rec.generation;
        rep->rollback_count = cur->rec.rollback_count;
    }

    return outcome;
}

boot_outcome_t boot_core_run(const iinternal_flash_t *fl, boot_report_t *report)
{
    boot_record_t cur;

    if ((fl == NULL) || (report == NULL))
    {
        return BOOT_OUTCOME_HALT_FLASH_FAULT;
    }

    (void) memset(report, 0, sizeof(*report));
    (void) memset(&cur, 0, sizeof(cur));

    if (!record_select(fl, &cur, &report->events))
    {
        return finish(report, &cur, BOOT_OUTCOME_HALT_FLASH_FAULT);
    }

    report->state_in = cur.valid ? cur.rec.boot_state : 0U;

    /* Step 2 of the layout: nothing valid means the factory image is in Slot A. */
    if (!cur.valid)
    {
        report->events |= BOOT_EVT_RECORD_INITIALISED;

        if (!record_write(fl, &cur, FIRMWARE_BANK_A, FIRMWARE_META_STATE_CONFIRMED, FIRMWARE_BANK_B,
                          0U))
        {
            report->events |= BOOT_EVT_META_WRITE_FAILED;
            return finish(report, &cur, BOOT_OUTCOME_HALT_FLASH_FAULT);
        }
    }
    else if (cur.rec.boot_state == FIRMWARE_META_STATE_TRIAL_PENDING)
    {
        /* A trial that cannot be supervised must not start: halt rather than boot it. */
        if (!record_write(fl, &cur, cur.rec.active_bank, FIRMWARE_META_STATE_TRIAL_RUNNING,
                          cur.rec.previous_bank, cur.rec.rollback_count))
        {
            report->events |= BOOT_EVT_META_WRITE_FAILED;
            return finish(report, &cur, BOOT_OUTCOME_HALT_FLASH_FAULT);
        }

        report->events |= BOOT_EVT_TRIAL_STARTED;
    }
    else if (cur.rec.boot_state == FIRMWARE_META_STATE_TRIAL_RUNNING)
    {
        /* The application never confirmed: crash, watchdog, power loss or a failed self-check. */
        if (cur.rec.rollback_count >= BOOT_MAX_ROLLBACKS)
        {
            return finish(report, &cur, BOOT_OUTCOME_HALT_ROLLBACK_LIMIT);
        }

        /* If this write fails the record stays as it was and the next reset repeats the rollback.
         */
        if (!record_write(fl, &cur, cur.rec.previous_bank, FIRMWARE_META_STATE_ROLLED_BACK,
                          cur.rec.active_bank, cur.rec.rollback_count + 1U))
        {
            report->events |= BOOT_EVT_META_WRITE_FAILED;
        }
        else
        {
            report->events |= BOOT_EVT_TRIAL_ABANDONED;
        }
    }
    else
    {
        /* CONFIRMED or ROLLED_BACK: boot the active slot as it stands. */
    }

    /* Which slot to run: the one the record in force names, unless the abandoned trial's rollback
     * could not be recorded, in which case the previous slot is run on the strength of its own
     * verification and the record still names the failed one. */
    uint8_t run_bank = cur.rec.active_bank;

    if (((report->events & BOOT_EVT_META_WRITE_FAILED) != 0U) &&
        (cur.rec.boot_state == FIRMWARE_META_STATE_TRIAL_RUNNING))
    {
        run_bank = cur.rec.previous_bank;
    }

    uint32_t len = 0U;
    uint32_t version = 0U;

    if (slot_is_bootable(fl, run_bank, &len, &version))
    {
        report->slot_base = slot_base(run_bank);
        report->image_len = len;
        report->version = version;
        return finish(report, &cur, BOOT_OUTCOME_RUN);
    }

    /* Step 3: the slot named failed verification. Try the other one. */
    report->events |= BOOT_EVT_ACTIVE_INVALID;

    const uint8_t other = other_bank(run_bank);

    if (cur.rec.rollback_count >= BOOT_MAX_ROLLBACKS)
    {
        return finish(report, &cur, BOOT_OUTCOME_HALT_ROLLBACK_LIMIT);
    }

    if (!slot_is_bootable(fl, other, &len, &version))
    {
        return finish(report, &cur, BOOT_OUTCOME_HALT_NO_IMAGE);
    }

    if (!record_write(fl, &cur, other, FIRMWARE_META_STATE_ROLLED_BACK, run_bank,
                      cur.rec.rollback_count + 1U))
    {
        report->events |= BOOT_EVT_META_WRITE_FAILED;
    }

    report->slot_base = slot_base(other);
    report->image_len = len;
    report->version = version;

    return finish(report, &cur, BOOT_OUTCOME_RUN);
}
