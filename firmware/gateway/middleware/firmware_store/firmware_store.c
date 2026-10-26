/**
 * @file firmware_store.c
 * @brief FirmwareStore — OTA image staging, authentication, installation and boot control
 * (Gateway).
 *
 * Public API, operation-state machine, verification and step-wise installation.
 * The on-chip metadata and descriptors live in fs_metadata.c, the QSPI staging
 * journal in fs_staging.c and the crypto in ota_crypto.c.
 *
 * One static priority-inheritance mutex is held for the whole of every public
 * call except create. Lock order is FirmwareStore mutex -> QspiFlashDriver.
 *
 * @see docs/lld/middleware/firmware-store.md
 */

#include "firmware_store.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "config_store/config_store_crc.h"
#include "firmware_image_format.h"
#include "fs_metadata.h"
#include "fs_staging.h"
#include "logger/logger.h"
#include "ota_crypto.h"
#include "semphr.h"

#define FS_MODULE_TAG "FS"

/** Largest byte count of one install or hash page. */
#define FS_PAGE_BYTES (FIRMWARE_PAGE_SIZE)

/** Window used when reading the slot back during installation. */
#define FS_COMPARE_WINDOW (FS_STAGING_SCRATCH_LEN)

/** Bytes of the reset vector and the stack pointer that precede it. */
#define FS_VECTOR_PROBE_LEN (8U)
#define FS_RESET_VECTOR_OFFSET (4U)

/** Thumb bit of an exception-vector entry. */
#define FS_THUMB_BIT (1U)

/** Install pads the last page to a whole double word with this value. */
#define FS_PAD_BYTE (0xFFU)

/** @brief Operation state of the staging and installation sequence (RAM only). */
typedef enum
{
    FS_OP_IDLE = 0,
    FS_OP_RECEIVING,
    FS_OP_RECEIVED,
    FS_OP_VERIFIED,
    FS_OP_INSTALLING,
    FS_OP_INSTALLED,
    FS_OP_ACTIVATED
} fs_op_state_t;

struct firmware_store
{
    bool created;
    firmware_store_config_t cfg;
    firmware_boot_info_t boot; /**< Cached; updated only after a verified write. */
    fs_meta_t meta;
    fs_op_state_t op;
    uint32_t image_id;
    uint32_t file_len;
    uint32_t write_offset;
    uint32_t journal_next;               /**< Byte offset of the next free journal entry. */
    uint8_t body_digest[OTA_SHA256_LEN]; /**< From the authenticated header. */
    uint32_t image_len;
    uint32_t version;
    firmware_slot_t target_slot;
    uint32_t install_page;
    uint32_t install_crc;
    ota_sha256_ctx_t install_sha;
    StaticSemaphore_t mutex_buf;
    SemaphoreHandle_t mutex;
};

static struct firmware_store s_pool[1];
static uint8_t s_work[FS_PAGE_BYTES];    /**< QSPI -> flash page buffer and hash read buffer. */
static uint8_t s_cmp[FS_COMPARE_WINDOW]; /**< Read-back compare window and journal scratch. */

/* ====================================================================== */
/* Helpers                                                                */
/* ====================================================================== */

static firmware_store_err_t fs_check(const firmware_store_t *self)
{
    if (self == NULL)
    {
        return FIRMWARE_STORE_ERR_NULL_POINTER;
    }

    if ((self != &s_pool[0]) || !s_pool[0].created)
    {
        return FIRMWARE_STORE_ERR_NOT_INITIALISED;
    }

    return FIRMWARE_STORE_ERR_OK;
}

static void fs_lock(const struct firmware_store *fs)
{
    (void) xSemaphoreTake(fs->mutex, portMAX_DELAY);
}

static void fs_unlock(const struct firmware_store *fs)
{
    (void) xSemaphoreGive(fs->mutex);
}

/**
 * @brief Log a failure that no lower layer has logged yet.
 *
 * Flash failures are logged at the failing call with the address; here are the
 * rest, at the level the error table assigns them.
 */
static void fs_log_result(const char *op, firmware_store_err_t err)
{
    switch (err)
    {
    case FIRMWARE_STORE_ERR_BAD_PARAM:
    case FIRMWARE_STORE_ERR_BAD_STATE:
    case FIRMWARE_STORE_ERR_INCOMPLETE:
        LOG_WARN(FS_MODULE_TAG, "%s rejected: code %u", op, (unsigned) err);
        break;
    case FIRMWARE_STORE_ERR_TOO_LARGE:
    case FIRMWARE_STORE_ERR_BAD_HEADER:
    case FIRMWARE_STORE_ERR_WRONG_SLOT:
    case FIRMWARE_STORE_ERR_SIG_INVALID:
    case FIRMWARE_STORE_ERR_HASH_MISMATCH:
    case FIRMWARE_STORE_ERR_BAD_IMAGE:
    case FIRMWARE_STORE_ERR_CRYPTO:
    case FIRMWARE_STORE_ERR_INSTALL_VERIFY:
        LOG_ERROR(FS_MODULE_TAG, "%s failed: code %u", op, (unsigned) err);
        break;
    case FIRMWARE_STORE_ERR_NO_SESSION:
        LOG_DEBUG(FS_MODULE_TAG, "%s: no matching session", op);
        break;
    default:
        break;
    }
}

static firmware_slot_t fs_other_slot(firmware_slot_t slot)
{
    return (slot == FIRMWARE_SLOT_A) ? FIRMWARE_SLOT_B : FIRMWARE_SLOT_A;
}

static uint32_t read_u32(const uint8_t *p)
{
    uint32_t v;

    (void) memcpy(&v, p, sizeof(v));

    return v;
}

/** @brief SHA-256 of a RAM buffer. */
static firmware_store_err_t sha256_buffer(const uint8_t *data, uint32_t len,
                                          uint8_t digest[OTA_SHA256_LEN])
{
    ota_sha256_ctx_t ctx;

    if ((ota_sha256_begin(&ctx) != OTA_CRYPTO_OK) ||
        (ota_sha256_update(&ctx, data, len) != OTA_CRYPTO_OK) ||
        (ota_sha256_finish(&ctx, digest) != OTA_CRYPTO_OK))
    {
        return FIRMWARE_STORE_ERR_CRYPTO;
    }

    return FIRMWARE_STORE_ERR_OK;
}

/* ====================================================================== */
/* Boot-state resolution (read-only)                                      */
/* ====================================================================== */

static firmware_slot_t slot_from_image_base(uint32_t base)
{
    if (base == FIRMWARE_SLOT_A_BASE)
    {
        return FIRMWARE_SLOT_A;
    }

    return (base == FIRMWARE_SLOT_B_BASE) ? FIRMWARE_SLOT_B : FIRMWARE_SLOT_NONE;
}

static firmware_store_err_t resolve_boot(struct firmware_store *fs)
{
    const iinternal_flash_t *fl = fs->cfg.iflash;
    const firmware_slot_t running = slot_from_image_base(fs->cfg.running_image_base);
    firmware_store_err_t err = fs_meta_load(fl, &fs->meta);

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        return err;
    }

    const bool have_meta = (fs->meta.selected != FS_META_NONE);

    fs->boot.running_slot = running;
    fs->boot.state =
        have_meta ? (firmware_boot_state_t) fs->meta.boot_state : FIRMWARE_BOOT_CONFIRMED;
    fs->boot.rollback_count = have_meta ? fs->meta.rollback_count : 0U;
    fs->boot.running_version = 0U;

    if (running != FIRMWARE_SLOT_NONE)
    {
        bool valid = false;
        firmware_slot_descriptor_t desc;

        err = fs_desc_read(fl, running, &valid, &desc);

        if (err != FIRMWARE_STORE_ERR_OK)
        {
            return err;
        }

        fs->boot.running_version = valid ? desc.version : 0U;
    }

    fs->boot.ota_capable = (running != FIRMWARE_SLOT_NONE) && have_meta &&
                           (fs_slot_from_bank(fs->meta.active_bank) == running) &&
                           ((fs->meta.boot_state == FIRMWARE_META_STATE_CONFIRMED) ||
                            (fs->meta.boot_state == FIRMWARE_META_STATE_TRIAL_RUNNING) ||
                            (fs->meta.boot_state == FIRMWARE_META_STATE_ROLLED_BACK));

    if (running == FIRMWARE_SLOT_NONE)
    {
        LOG_WARN(FS_MODULE_TAG, "not running from a slot: OTA disabled");
    }
    else if (!fs->boot.ota_capable)
    {
        LOG_ERROR(FS_MODULE_TAG, "boot metadata absent or inconsistent: OTA disabled");
    }
    else
    {
        /* Capable: nothing to report. */
    }

    return FIRMWARE_STORE_ERR_OK;
}

firmware_store_err_t firmware_store_create(const firmware_store_config_t *cfg,
                                           firmware_store_t **out)
{
    if ((cfg == NULL) || (out == NULL) || (cfg->qspi == NULL) || (cfg->iflash == NULL))
    {
        return FIRMWARE_STORE_ERR_NULL_POINTER;
    }

    if (s_pool[0].created)
    {
        LOG_ERROR(FS_MODULE_TAG, "create called twice");
        return FIRMWARE_STORE_ERR_NO_INSTANCE;
    }

    struct firmware_store *fs = &s_pool[0];

    (void) memset(fs, 0, sizeof(*fs));
    fs->cfg = *cfg;

    firmware_store_err_t err = resolve_boot(fs);

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        return err;
    }

    fs->mutex = xSemaphoreCreateMutexStatic(&fs->mutex_buf);

    if (fs->mutex == NULL)
    {
        LOG_ERROR(FS_MODULE_TAG, "mutex creation failed");
        return FIRMWARE_STORE_ERR_NO_INSTANCE;
    }

    fs->op = FS_OP_IDLE;
    fs->created = true;
    *out = fs;

    return FIRMWARE_STORE_ERR_OK;
}

/* ====================================================================== */
/* IFirmwareImageStore — download and resume                              */
/* ====================================================================== */

/** @brief Pre-conditions shared by begin and resume. */
static firmware_store_err_t session_precondition(const struct firmware_store *fs)
{
    if (!fs->boot.ota_capable)
    {
        return FIRMWARE_STORE_ERR_NOT_OTA_CAPABLE;
    }

    if ((fs->boot.state != FIRMWARE_BOOT_CONFIRMED) || (fs->op == FS_OP_ACTIVATED))
    {
        return FIRMWARE_STORE_ERR_BAD_STATE;
    }

    return FIRMWARE_STORE_ERR_OK;
}

static firmware_store_err_t session_params(uint32_t image_id, uint32_t file_len)
{
    if ((image_id == FIRMWARE_IMAGE_ID_ERASED) || (file_len <= FIRMWARE_HEADER_SIZE))
    {
        return FIRMWARE_STORE_ERR_BAD_PARAM;
    }

    if ((file_len - FIRMWARE_HEADER_SIZE) > FIRMWARE_MAX_IMAGE_LEN)
    {
        return FIRMWARE_STORE_ERR_TOO_LARGE;
    }

    return FIRMWARE_STORE_ERR_OK;
}

static firmware_store_err_t do_begin(struct firmware_store *fs, uint32_t image_id,
                                     uint32_t file_len)
{
    firmware_store_err_t err = session_precondition(fs);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        err = session_params(image_id, file_len);
    }

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        return err;
    }

    /* Whatever the outcome, the old session is gone: a verification never survives. */
    fs->op = FS_OP_IDLE;

    err = fs_journal_begin(fs->cfg.qspi, image_id, file_len, s_cmp, &fs->journal_next);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        fs->image_id = image_id;
        fs->file_len = file_len;
        fs->write_offset = 0U;
        fs->op = FS_OP_RECEIVING;
    }

    return err;
}

static firmware_store_err_t do_resume(struct firmware_store *fs, uint32_t image_id,
                                      uint32_t file_len, uint32_t *offset_out)
{
    bool session_ok = false;
    uint32_t checkpoint = 0U;
    uint32_t next = 0U;
    firmware_store_err_t err = session_precondition(fs);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        err = session_params(image_id, file_len);
    }

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        return err;
    }

    err = fs_journal_scan(fs->cfg.qspi, image_id, file_len, s_cmp, &session_ok, &checkpoint, &next);

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        return err;
    }

    if (!session_ok)
    {
        return FIRMWARE_STORE_ERR_NO_SESSION;
    }

    fs->image_id = image_id;
    fs->file_len = file_len;
    fs->write_offset = checkpoint;
    fs->journal_next = next;
    fs->op = (checkpoint == file_len) ? FS_OP_RECEIVED : FS_OP_RECEIVING;
    *offset_out = checkpoint;

    return FIRMWARE_STORE_ERR_OK;
}

static firmware_store_err_t do_write_chunk(struct firmware_store *fs, uint32_t offset,
                                           const uint8_t *data, uint32_t len)
{
    if (fs->op != FS_OP_RECEIVING)
    {
        return FIRMWARE_STORE_ERR_BAD_STATE;
    }

    if ((len == 0U) || (offset != fs->write_offset) || (len > (fs->file_len - offset)))
    {
        return FIRMWARE_STORE_ERR_BAD_PARAM;
    }

    const firmware_store_err_t err =
        fs_stage_write(fs->cfg.qspi, offset, data, len, fs->file_len, &fs->journal_next);

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        /* The journal still holds the last good checkpoint; the caller resumes. */
        fs->op = FS_OP_IDLE;
        return err;
    }

    fs->write_offset += len;

    if (fs->write_offset == fs->file_len)
    {
        fs->op = FS_OP_RECEIVED;
    }

    return FIRMWARE_STORE_ERR_OK;
}

static firmware_store_err_t do_discard(struct firmware_store *fs)
{
    if (fs->op == FS_OP_ACTIVATED)
    {
        return FIRMWARE_STORE_ERR_BAD_STATE;
    }

    fs->op = FS_OP_IDLE;
    fs->write_offset = 0U;

    return fs_journal_erase(fs->cfg.qspi);
}

/* ====================================================================== */
/* IFirmwareImageStore — verification                                     */
/* ====================================================================== */

/** @brief Check the header fields that need no cryptography. */
static firmware_store_err_t verify_structure(const struct firmware_store *fs,
                                             const firmware_image_header_t *h)
{
    if ((h->magic != FIRMWARE_IMAGE_MAGIC) ||
        (h->header_version != FIRMWARE_IMAGE_HEADER_VERSION) ||
        (h->header_size != FIRMWARE_HEADER_SIZE) ||
        (h->image_len != (fs->file_len - FIRMWARE_HEADER_SIZE)) ||
        (h->image_len < FS_VECTOR_PROBE_LEN))
    {
        return FIRMWARE_STORE_ERR_BAD_HEADER;
    }

    if (h->image_len > FIRMWARE_MAX_IMAGE_LEN)
    {
        return FIRMWARE_STORE_ERR_TOO_LARGE;
    }

    if (h->load_addr != fs_slot_base(fs_other_slot(fs->boot.running_slot)))
    {
        return FIRMWARE_STORE_ERR_WRONG_SLOT;
    }

    return FIRMWARE_STORE_ERR_OK;
}

static firmware_store_err_t verify_signature(const firmware_image_header_t *h)
{
    uint8_t digest[OTA_SHA256_LEN];

    if (sha256_buffer((const uint8_t *) h, FIRMWARE_SIGNED_HEADER_LEN, digest) !=
        FIRMWARE_STORE_ERR_OK)
    {
        return FIRMWARE_STORE_ERR_CRYPTO;
    }

    switch (ota_ecdsa_p256_verify(digest, h->signature))
    {
    case OTA_CRYPTO_OK:
        return FIRMWARE_STORE_ERR_OK;
    case OTA_CRYPTO_ERR_SIG_INVALID:
        return FIRMWARE_STORE_ERR_SIG_INVALID;
    default:
        return FIRMWARE_STORE_ERR_CRYPTO;
    }
}

/** @brief Hash the staged body and compare it with the signed digest. */
static firmware_store_err_t verify_body_hash(const struct firmware_store *fs,
                                             const firmware_image_header_t *h)
{
    ota_sha256_ctx_t ctx;
    uint8_t digest[OTA_SHA256_LEN];

    if (ota_sha256_begin(&ctx) != OTA_CRYPTO_OK)
    {
        return FIRMWARE_STORE_ERR_CRYPTO;
    }

    for (uint32_t done = 0U; done < h->image_len; done += FS_PAGE_BYTES)
    {
        const uint32_t n =
            ((h->image_len - done) < FS_PAGE_BYTES) ? (h->image_len - done) : FS_PAGE_BYTES;
        const firmware_store_err_t err =
            fs_stage_read(fs->cfg.qspi, FIRMWARE_HEADER_SIZE + done, s_work, n);

        if (err != FIRMWARE_STORE_ERR_OK)
        {
            return err;
        }

        if (ota_sha256_update(&ctx, s_work, n) != OTA_CRYPTO_OK)
        {
            return FIRMWARE_STORE_ERR_CRYPTO;
        }
    }

    if (ota_sha256_finish(&ctx, digest) != OTA_CRYPTO_OK)
    {
        return FIRMWARE_STORE_ERR_CRYPTO;
    }

    return (memcmp(digest, h->body_sha256, OTA_SHA256_LEN) == 0) ? FIRMWARE_STORE_ERR_OK
                                                                 : FIRMWARE_STORE_ERR_HASH_MISMATCH;
}

/** @brief The reset vector must carry the Thumb bit and point inside the image. */
static firmware_store_err_t verify_reset_vector(const struct firmware_store *fs,
                                                const firmware_image_header_t *h)
{
    const firmware_store_err_t err =
        fs_stage_read(fs->cfg.qspi, FIRMWARE_HEADER_SIZE, s_cmp, FS_VECTOR_PROBE_LEN);

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        return err;
    }

    const uint32_t vector = read_u32(&s_cmp[FS_RESET_VECTOR_OFFSET]);
    const uint32_t target = vector & ~FS_THUMB_BIT;

    if (((vector & FS_THUMB_BIT) == 0U) || (target < h->load_addr) ||
        (target >= (h->load_addr + h->image_len)))
    {
        return FIRMWARE_STORE_ERR_BAD_IMAGE;
    }

    return FIRMWARE_STORE_ERR_OK;
}

static firmware_store_err_t do_verify(struct firmware_store *fs, firmware_image_info_t *info_out)
{
    firmware_image_header_t h;

    if (fs->op == FS_OP_RECEIVING)
    {
        return FIRMWARE_STORE_ERR_INCOMPLETE;
    }

    if (fs->op != FS_OP_RECEIVED)
    {
        return FIRMWARE_STORE_ERR_BAD_STATE;
    }

    firmware_store_err_t err = fs_stage_read(fs->cfg.qspi, 0U, s_work, FIRMWARE_HEADER_SIZE);

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        return err;
    }

    (void) memcpy(&h, s_work, sizeof(h));

    err = verify_structure(fs, &h);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        err = verify_signature(&h);
    }

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        err = verify_body_hash(fs, &h);
    }

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        err = verify_reset_vector(fs, &h);
    }

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        return err;
    }

    (void) memcpy(fs->body_digest, h.body_sha256, sizeof(fs->body_digest));
    fs->image_len = h.image_len;
    fs->version = h.version;
    fs->target_slot = fs_other_slot(fs->boot.running_slot);

    info_out->version = h.version;
    info_out->image_len = h.image_len;
    info_out->target_slot = fs->target_slot;
    fs->op = FS_OP_VERIFIED;

    return FIRMWARE_STORE_ERR_OK;
}

/* ====================================================================== */
/* IFirmwareImageStore — installation                                     */
/* ====================================================================== */

static firmware_store_err_t do_install_begin(struct firmware_store *fs)
{
    if (fs->op != FS_OP_VERIFIED)
    {
        return FIRMWARE_STORE_ERR_BAD_STATE;
    }

    /* The target descriptor goes first, so a half-written slot is never bootable. */
    firmware_store_err_t err = fs_desc_erase(fs->cfg.iflash, fs->target_slot);

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        return err;
    }

    if (ota_sha256_begin(&fs->install_sha) != OTA_CRYPTO_OK)
    {
        return FIRMWARE_STORE_ERR_CRYPTO;
    }

    fs->install_crc = config_store_crc32_begin();
    fs->install_page = 0U;
    fs->op = FS_OP_INSTALLING;

    return FIRMWARE_STORE_ERR_OK;
}

/** @brief Read the freshly programmed page back and compare it with s_work. */
static firmware_store_err_t install_readback(const struct firmware_store *fs, uint32_t addr,
                                             uint32_t padded)
{
    for (uint32_t done = 0U; done < padded; done += FS_COMPARE_WINDOW)
    {
        const uint32_t n =
            ((padded - done) < FS_COMPARE_WINDOW) ? (padded - done) : FS_COMPARE_WINDOW;
        const firmware_store_err_t err = fs_if_read(fs->cfg.iflash, addr + done, s_cmp, n);

        if (err != FIRMWARE_STORE_ERR_OK)
        {
            return err;
        }

        if (memcmp(s_cmp, &s_work[done], n) != 0)
        {
            LOG_ERROR(FS_MODULE_TAG, "install read-back mismatch addr=0x%08lX",
                      (unsigned long) (addr + done));
            return FIRMWARE_STORE_ERR_INSTALL_VERIFY;
        }
    }

    return FIRMWARE_STORE_ERR_OK;
}

/** @brief Copy and check one page: stage -> slot -> read-back -> hash and CRC. */
static firmware_store_err_t install_page_copy(struct firmware_store *fs, uint32_t offset,
                                              uint32_t n)
{
    const uint32_t padded =
        (n + (INTERNAL_FLASH_DWORD_SIZE_BYTES - 1U)) & ~(INTERNAL_FLASH_DWORD_SIZE_BYTES - 1U);
    const uint32_t addr = fs_slot_base(fs->target_slot) + offset;
    firmware_store_err_t err =
        fs_stage_read(fs->cfg.qspi, FIRMWARE_HEADER_SIZE + offset, s_work, n);

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        return err;
    }

    (void) memset(&s_work[n], FS_PAD_BYTE, padded - n);

    err = fs_if_erase_page(fs->cfg.iflash, addr);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        err = fs_if_program(fs->cfg.iflash, addr, s_work, padded);
    }

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        err = install_readback(fs, addr, padded);
    }

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        return err;
    }

    if (ota_sha256_update(&fs->install_sha, s_work, n) != OTA_CRYPTO_OK)
    {
        return FIRMWARE_STORE_ERR_CRYPTO;
    }

    fs->install_crc = config_store_crc32_feed(fs->install_crc, s_work, n);

    return FIRMWARE_STORE_ERR_OK;
}

/** @brief After the last page: re-check the digest, then seal the slot with its descriptor. */
static firmware_store_err_t install_finish(struct firmware_store *fs)
{
    uint8_t digest[OTA_SHA256_LEN];

    if (ota_sha256_finish(&fs->install_sha, digest) != OTA_CRYPTO_OK)
    {
        return FIRMWARE_STORE_ERR_CRYPTO;
    }

    /* Closes the gap between verify and the copy (a QSPI bit flip, or staging changed). */
    if (memcmp(digest, fs->body_digest, OTA_SHA256_LEN) != 0)
    {
        LOG_ERROR(FS_MODULE_TAG, "installed image digest differs from the verified one");
        return FIRMWARE_STORE_ERR_INSTALL_VERIFY;
    }

    return fs_desc_write(fs->cfg.iflash, fs->target_slot, fs->image_len, fs->version,
                         config_store_crc32_end(fs->install_crc));
}

static firmware_store_err_t do_install_step(struct firmware_store *fs, bool *done_out)
{
    *done_out = false;

    if (fs->op != FS_OP_INSTALLING)
    {
        return FIRMWARE_STORE_ERR_BAD_STATE;
    }

    const uint32_t offset = fs->install_page * FS_PAGE_BYTES;
    const uint32_t left = fs->image_len - offset;
    const uint32_t n = (left < FS_PAGE_BYTES) ? left : FS_PAGE_BYTES;
    const bool last = (n == left);
    firmware_store_err_t err = install_page_copy(fs, offset, n);

    if ((err == FIRMWARE_STORE_ERR_OK) && last)
    {
        err = install_finish(fs);

        if (err != FIRMWARE_STORE_ERR_OK)
        {
            /* A torn descriptor must not look installed: best-effort re-erase. */
            (void) fs_desc_erase(fs->cfg.iflash, fs->target_slot);
        }
    }

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        fs->op = FS_OP_VERIFIED;
        return err;
    }

    if (last)
    {
        fs->op = FS_OP_INSTALLED;
        *done_out = true;
    }
    else
    {
        fs->install_page++;
    }

    return FIRMWARE_STORE_ERR_OK;
}

/* ====================================================================== */
/* Boot control                                                           */
/* ====================================================================== */

static firmware_store_err_t do_activate(struct firmware_store *fs)
{
    if (!fs->boot.ota_capable)
    {
        return FIRMWARE_STORE_ERR_NOT_OTA_CAPABLE;
    }

    if ((fs->op != FS_OP_INSTALLED) || (fs->boot.state != FIRMWARE_BOOT_CONFIRMED))
    {
        return FIRMWARE_STORE_ERR_BAD_STATE;
    }

    const firmware_store_err_t err =
        fs_meta_write(fs->cfg.iflash, &fs->meta, fs_bank_from_slot(fs->target_slot),
                      FIRMWARE_META_STATE_TRIAL_PENDING, fs_bank_from_slot(fs->boot.running_slot),
                      fs->meta.rollback_count);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        fs->boot.state = FIRMWARE_BOOT_TRIAL_PENDING;
        fs->op = FS_OP_ACTIVATED;
    }

    return err;
}

/**
 * @brief Mark the record CONFIRMED, then erase the descriptor of the slot left behind.
 *
 * The metadata comes first: a power cut between the two leaves CONFIRMED with a
 * stale descriptor, which is harmless (§14).
 */
static firmware_store_err_t settle_confirmed(struct firmware_store *fs)
{
    firmware_store_err_t err = fs_meta_write(fs->cfg.iflash, &fs->meta, fs->meta.active_bank,
                                             FIRMWARE_META_STATE_CONFIRMED, fs->meta.previous_bank,
                                             fs->meta.rollback_count);

    if (err != FIRMWARE_STORE_ERR_OK)
    {
        return err;
    }

    fs->boot.state = FIRMWARE_BOOT_CONFIRMED;

    return fs_desc_erase(fs->cfg.iflash, fs_slot_from_bank(fs->meta.previous_bank));
}

static firmware_store_err_t do_confirm(struct firmware_store *fs)
{
    if (!fs->boot.ota_capable)
    {
        return FIRMWARE_STORE_ERR_NOT_OTA_CAPABLE;
    }

    if (fs->boot.state == FIRMWARE_BOOT_CONFIRMED)
    {
        return FIRMWARE_STORE_ERR_OK;
    }

    return (fs->boot.state == FIRMWARE_BOOT_TRIAL_RUNNING) ? settle_confirmed(fs)
                                                           : FIRMWARE_STORE_ERR_BAD_STATE;
}

static firmware_store_err_t do_rollback(struct firmware_store *fs)
{
    if (!fs->boot.ota_capable)
    {
        return FIRMWARE_STORE_ERR_NOT_OTA_CAPABLE;
    }

    if (fs->boot.state != FIRMWARE_BOOT_TRIAL_RUNNING)
    {
        return FIRMWARE_STORE_ERR_BAD_STATE;
    }

    const uint32_t count = fs->meta.rollback_count + 1U;
    const firmware_store_err_t err =
        fs_meta_write(fs->cfg.iflash, &fs->meta, fs->meta.previous_bank,
                      FIRMWARE_META_STATE_ROLLED_BACK, fs->meta.active_bank, count);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        fs->boot.state = FIRMWARE_BOOT_ROLLED_BACK;
        fs->boot.rollback_count = count;
    }

    return err;
}

static firmware_store_err_t do_acknowledge_rollback(struct firmware_store *fs)
{
    if (!fs->boot.ota_capable)
    {
        return FIRMWARE_STORE_ERR_NOT_OTA_CAPABLE;
    }

    if (fs->boot.state == FIRMWARE_BOOT_CONFIRMED)
    {
        return FIRMWARE_STORE_ERR_OK;
    }

    return (fs->boot.state == FIRMWARE_BOOT_ROLLED_BACK) ? settle_confirmed(fs)
                                                         : FIRMWARE_STORE_ERR_BAD_STATE;
}

/* ====================================================================== */
/* Public entry points: validate -> lock -> do_* -> unlock -> log          */
/* ====================================================================== */

static firmware_store_err_t op_begin(firmware_store_t *self, uint32_t image_id, uint32_t file_len)
{
    firmware_store_err_t err = fs_check(self);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        fs_lock(self);
        err = do_begin(self, image_id, file_len);
        fs_unlock(self);
        fs_log_result("begin", err);
    }

    return err;
}

static firmware_store_err_t op_resume(firmware_store_t *self, uint32_t image_id, uint32_t file_len,
                                      uint32_t *offset_out)
{
    firmware_store_err_t err = fs_check(self);

    if ((err == FIRMWARE_STORE_ERR_OK) && (offset_out == NULL))
    {
        err = FIRMWARE_STORE_ERR_NULL_POINTER;
    }

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        fs_lock(self);
        err = do_resume(self, image_id, file_len, offset_out);
        fs_unlock(self);
        fs_log_result("resume", err);
    }

    return err;
}

static firmware_store_err_t op_write_chunk(firmware_store_t *self, uint32_t offset,
                                           const uint8_t *data, uint32_t len)
{
    firmware_store_err_t err = fs_check(self);

    if ((err == FIRMWARE_STORE_ERR_OK) && (data == NULL))
    {
        err = FIRMWARE_STORE_ERR_NULL_POINTER;
    }

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        fs_lock(self);
        err = do_write_chunk(self, offset, data, len);
        fs_unlock(self);
        fs_log_result("write_chunk", err);
    }

    return err;
}

static firmware_store_err_t op_verify(firmware_store_t *self, firmware_image_info_t *info_out)
{
    firmware_store_err_t err = fs_check(self);

    if ((err == FIRMWARE_STORE_ERR_OK) && (info_out == NULL))
    {
        err = FIRMWARE_STORE_ERR_NULL_POINTER;
    }

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        fs_lock(self);
        err = do_verify(self, info_out);
        fs_unlock(self);
        fs_log_result("verify", err);
    }

    return err;
}

static firmware_store_err_t op_install_begin(firmware_store_t *self)
{
    firmware_store_err_t err = fs_check(self);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        fs_lock(self);
        err = do_install_begin(self);
        fs_unlock(self);
        fs_log_result("install_begin", err);
    }

    return err;
}

static firmware_store_err_t op_install_step(firmware_store_t *self, bool *done_out)
{
    firmware_store_err_t err = fs_check(self);

    if ((err == FIRMWARE_STORE_ERR_OK) && (done_out == NULL))
    {
        err = FIRMWARE_STORE_ERR_NULL_POINTER;
    }

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        fs_lock(self);
        err = do_install_step(self, done_out);
        fs_unlock(self);
        fs_log_result("install_step", err);
    }

    return err;
}

static firmware_store_err_t op_activate(firmware_store_t *self)
{
    firmware_store_err_t err = fs_check(self);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        fs_lock(self);
        err = do_activate(self);
        fs_unlock(self);
        fs_log_result("activate", err);
    }

    return err;
}

static firmware_store_err_t op_discard(firmware_store_t *self)
{
    firmware_store_err_t err = fs_check(self);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        fs_lock(self);
        err = do_discard(self);
        fs_unlock(self);
        fs_log_result("discard", err);
    }

    return err;
}

static firmware_store_err_t op_get_boot_info(const firmware_store_t *self,
                                             firmware_boot_info_t *info_out)
{
    firmware_store_err_t err = fs_check(self);

    if ((err == FIRMWARE_STORE_ERR_OK) && (info_out == NULL))
    {
        err = FIRMWARE_STORE_ERR_NULL_POINTER;
    }

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        fs_lock(self);
        *info_out = self->boot;
        fs_unlock(self);
    }

    return err;
}

static firmware_store_err_t op_confirm(firmware_store_t *self)
{
    firmware_store_err_t err = fs_check(self);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        fs_lock(self);
        err = do_confirm(self);
        fs_unlock(self);
        fs_log_result("confirm", err);
    }

    return err;
}

static firmware_store_err_t op_rollback(firmware_store_t *self)
{
    firmware_store_err_t err = fs_check(self);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        fs_lock(self);
        err = do_rollback(self);
        fs_unlock(self);
        fs_log_result("rollback", err);
    }

    return err;
}

static firmware_store_err_t op_acknowledge_rollback(firmware_store_t *self)
{
    firmware_store_err_t err = fs_check(self);

    if (err == FIRMWARE_STORE_ERR_OK)
    {
        fs_lock(self);
        err = do_acknowledge_rollback(self);
        fs_unlock(self);
        fs_log_result("acknowledge_rollback", err);
    }

    return err;
}

const ifirmware_image_store_t firmware_store_image_ops = {
    .begin = op_begin,
    .resume = op_resume,
    .write_chunk = op_write_chunk,
    .verify = op_verify,
    .install_begin = op_install_begin,
    .install_step = op_install_step,
    .activate = op_activate,
    .discard = op_discard,
};

const ifirmware_boot_control_t firmware_store_boot_ops = {
    .get_boot_info = op_get_boot_info,
    .confirm = op_confirm,
    .rollback = op_rollback,
    .acknowledge_rollback = op_acknowledge_rollback,
};

#ifdef TEST
void firmware_store_reset_for_test(void)
{
    (void) memset(s_pool, 0, sizeof(s_pool));
    (void) memset(s_work, 0, sizeof(s_work));
    (void) memset(s_cmp, 0, sizeof(s_cmp));
}
#endif
