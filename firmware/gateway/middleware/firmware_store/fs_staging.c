/**
 * @file fs_staging.c
 * @brief FirmwareStore private: QSPI staging area, resume journal and lazy sector erase.
 *
 * @see docs/lld/middleware/firmware-store.md §6.4, §10
 */

#include "fs_staging.h"

#include <string.h>

#include "config_store/config_store_crc.h"
#include "firmware_image_format.h"
#include "logger/logger.h"

#define FS_MODULE_TAG "FS"

/** Largest single QSPI read issued: one install page. */
#define FS_READ_CHUNK (2048U)

static uint32_t journal_addr(void)
{
    return FIRMWARE_STORE_STAGING_BASE;
}

static uint32_t image_addr(uint32_t pos)
{
    return FIRMWARE_STORE_STAGING_BASE + FIRMWARE_STAGING_IMAGE_OFFSET + pos;
}

/* ====================================================================== */
/* QSPI access with logging                                               */
/* ====================================================================== */

static firmware_store_err_t q_read(const iqspi_flash_t *q, uint32_t addr, uint8_t *buf,
                                   uint32_t len)
{
    if (q->read(addr, buf, len) != QSPI_FLASH_OK)
    {
        LOG_ERROR(FS_MODULE_TAG, "QSPI read failed off=0x%08lX", (unsigned long) addr);
        return FIRMWARE_STORE_ERR_FLASH_READ;
    }

    return FIRMWARE_STORE_ERR_OK;
}

static firmware_store_err_t q_erase(const iqspi_flash_t *q, uint32_t addr)
{
    if (q->erase_sector(addr) != QSPI_FLASH_OK)
    {
        LOG_ERROR(FS_MODULE_TAG, "QSPI erase failed off=0x%08lX", (unsigned long) addr);
        return FIRMWARE_STORE_ERR_FLASH_ERASE;
    }

    return FIRMWARE_STORE_ERR_OK;
}

static firmware_store_err_t q_program(const iqspi_flash_t *q, uint32_t addr, const uint8_t *data,
                                      uint32_t len)
{
    if (q->write_page(addr, data, (uint16_t) len) != QSPI_FLASH_OK)
    {
        LOG_ERROR(FS_MODULE_TAG, "QSPI program failed off=0x%08lX", (unsigned long) addr);
        return FIRMWARE_STORE_ERR_FLASH_WRITE;
    }

    return FIRMWARE_STORE_ERR_OK;
}

firmware_store_err_t fs_stage_read(const iqspi_flash_t *q, uint32_t pos, uint8_t *buf, uint32_t len)
{
    uint32_t done = 0U;

    while (done < len)
    {
        const uint32_t n = ((len - done) < FS_READ_CHUNK) ? (len - done) : FS_READ_CHUNK;
        const firmware_store_err_t err = q_read(q, image_addr(pos + done), &buf[done], n);

        if (err != FIRMWARE_STORE_ERR_OK)
        {
            return err;
        }

        done += n;
    }

    return FIRMWARE_STORE_ERR_OK;
}

/* ====================================================================== */
/* Journal                                                                */
/* ====================================================================== */

/** @brief Append {value, ~value} to the first free entry. */
static firmware_store_err_t journal_append(const iqspi_flash_t *q, uint32_t *next, uint32_t value)
{
    const firmware_journal_entry_t entry = {.value = value, .inverse = ~value};

    if (*next >= FIRMWARE_JOURNAL_SIZE)
    {
        LOG_ERROR(FS_MODULE_TAG, "journal full");
        return FIRMWARE_STORE_ERR_FLASH_WRITE;
    }

    const firmware_store_err_t err =
        q_program(q, journal_addr() + *next, (const uint8_t *) &entry, sizeof(entry));

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        *next += (uint32_t) sizeof(entry);
    }

    return err;
}

firmware_store_err_t fs_journal_begin(const iqspi_flash_t *q, uint32_t image_id, uint32_t file_len,
                                      uint8_t *scratch, uint32_t *next)
{
    firmware_journal_session_t session = {
        .magic = FIRMWARE_JOURNAL_MAGIC,
        .image_id = image_id,
        .file_len = file_len,
        .crc32 = 0U,
    };
    firmware_store_err_t err = q_erase(q, journal_addr());

    session.crc32 =
        config_store_crc32((const uint8_t *) &session, FIRMWARE_JOURNAL_SESSION_CRC_SPAN);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        err = q_program(q, journal_addr(), (const uint8_t *) &session, sizeof(session));
    }

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        err = q_read(q, journal_addr(), scratch, sizeof(session));
    }

    if ((err == FIRMWARE_STORE_ERR_OK) && (memcmp(scratch, &session, sizeof(session)) != 0))
    {
        LOG_ERROR(FS_MODULE_TAG, "journal session read-back mismatch");
        err = FIRMWARE_STORE_ERR_FLASH_WRITE;
    }

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        *next = FIRMWARE_JOURNAL_ENTRIES_OFFSET;
    }

    return err;
}

/** @brief True if the entry is a plausible checkpoint for a file of file_len bytes. */
static bool checkpoint_plausible(uint32_t value, uint32_t file_len)
{
    return (value <= file_len) &&
           (((value % FIRMWARE_CHECKPOINT_STEP) == 0U) || (value == file_len));
}

firmware_store_err_t fs_journal_scan(const iqspi_flash_t *q, uint32_t image_id, uint32_t file_len,
                                     uint8_t *scratch, bool *session_ok, uint32_t *checkpoint,
                                     uint32_t *next)
{
    firmware_journal_session_t session;
    firmware_store_err_t err = q_read(q, journal_addr(), scratch, sizeof(session));

    *session_ok = false;
    *checkpoint = 0U;
    *next = FIRMWARE_JOURNAL_ENTRIES_OFFSET;

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        return err;
    }

    (void) memcpy(&session, scratch, sizeof(session));

    if ((session.magic != FIRMWARE_JOURNAL_MAGIC) ||
        (session.crc32 != config_store_crc32(scratch, FIRMWARE_JOURNAL_SESSION_CRC_SPAN)) ||
        (session.image_id != image_id) || (session.file_len != file_len))
    {
        return FIRMWARE_STORE_ERR_OK;
    }

    *session_ok = true;

    /* Walk the entries until the first fully erased one, in scratch-sized windows. */
    const uint32_t per_window =
        FS_STAGING_SCRATCH_LEN / (uint32_t) sizeof(firmware_journal_entry_t);

    for (uint32_t first = 0U; first < FIRMWARE_JOURNAL_ENTRY_COUNT; first += per_window)
    {
        const uint32_t count = ((FIRMWARE_JOURNAL_ENTRY_COUNT - first) < per_window)
                                   ? (FIRMWARE_JOURNAL_ENTRY_COUNT - first)
                                   : per_window;
        const uint32_t base =
            FIRMWARE_JOURNAL_ENTRIES_OFFSET + (first * (uint32_t) sizeof(firmware_journal_entry_t));

        err = q_read(q, journal_addr() + base, scratch,
                     count * (uint32_t) sizeof(firmware_journal_entry_t));

        if (err != FIRMWARE_STORE_ERR_OK)
        {
            return err;
        }

        for (uint32_t i = 0U; i < count; i++)
        {
            firmware_journal_entry_t e;

            (void) memcpy(&e, &scratch[i * sizeof(e)], sizeof(e));
            *next = base + (i * (uint32_t) sizeof(e));

            if ((e.value == FIRMWARE_ERASED_WORD) && (e.inverse == FIRMWARE_ERASED_WORD))
            {
                return FIRMWARE_STORE_ERR_OK;
            }

            /* A torn append fails the pair check and is skipped. */
            if ((e.value == (uint32_t) ~e.inverse) && checkpoint_plausible(e.value, file_len))
            {
                *checkpoint = e.value;
            }
        }
    }

    /* Journal full: no free entry remains. */
    *next = FIRMWARE_JOURNAL_SIZE;

    return FIRMWARE_STORE_ERR_OK;
}

firmware_store_err_t fs_journal_erase(const iqspi_flash_t *q)
{
    return q_erase(q, journal_addr());
}

/* ====================================================================== */
/* Image staging                                                          */
/* ====================================================================== */

firmware_store_err_t fs_stage_write(const iqspi_flash_t *q, uint32_t pos, const uint8_t *data,
                                    uint32_t len, uint32_t file_len, uint32_t *next)
{
    uint32_t done = 0U;

    while (done < len)
    {
        const uint32_t at = pos + done;
        firmware_store_err_t err = FIRMWARE_STORE_ERR_OK;

        /* Lazy erase: the first byte of a 4 KB sector erases it. */
        if ((at % QSPI_FLASH_SECTOR_SIZE_BYTES) == 0U)
        {
            err = q_erase(q, image_addr(at));
        }

        const uint32_t room = QSPI_FLASH_PAGE_SIZE_BYTES - (at % QSPI_FLASH_PAGE_SIZE_BYTES);
        const uint32_t n = ((len - done) < room) ? (len - done) : room;

        if (err == FIRMWARE_STORE_ERR_OK)
        {
            err = q_program(q, image_addr(at), &data[done], n);
        }

        if (err != FIRMWARE_STORE_ERR_OK)
        {
            return err;
        }

        done += n;

        /* Checkpoint at each 4 KB boundary and at the end of the file. */
        if ((((at + n) % FIRMWARE_CHECKPOINT_STEP) == 0U) || ((at + n) == file_len))
        {
            err = journal_append(q, next, at + n);

            if (err != FIRMWARE_STORE_ERR_OK)
            {
                return err;
            }
        }
    }

    return FIRMWARE_STORE_ERR_OK;
}
