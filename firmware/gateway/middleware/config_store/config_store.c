/**
 * @file config_store.c
 * @brief ConfigStore implementation (Gateway middleware layer).
 *
 * See docs/lld/middleware/config-store-lld.md. Two 32 KB slots A and B hold the
 * blob; the CRC32 is written last, so a slot interrupted mid-write is never
 * chosen over the older valid one.
 */

#include "config_store.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include "config_store_config.h"
#include "config_store_crc.h"

#ifdef TEST
#define LOG_ERROR(m, f, ...) ((void) 0)
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_INFO(m, f, ...) ((void) 0)
#define LOG_DEBUG(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

#define CS_LOG_MODULE "CS"

#define CS_MAX_INSTANCES (1U)
#define CS_CHUNK_SIZE (256U)
#define CS_ERASED_BYTE (0xFFU)
#define CS_NO_SLOT (0xFFU)
#define CS_BYTE_SHIFT (8U)
#define CS_BYTE_MASK (0xFFU)

_Static_assert(CONFIG_STORE_MAX_DATA_BYTES <= CONFIG_STORE_DATA_AREA_SIZE,
               "the largest blob must fit the slot's data area");

/** An erased flash word. */
#define CS_ERASED_WORD (0xFFFFFFFFU)

/** @brief Result of examining one slot. */
typedef enum
{
    CS_SLOT_VALID = 0,      /**< Magic and CRC both check out.        */
    CS_SLOT_INVALID = 1,    /**< Erased, torn or corrupt.             */
    CS_SLOT_READ_ERROR = 2, /**< The flash read itself failed.      */
    CS_SLOT_ERASED = 3      /**< Header is all 0xFF: never written (LC-O14). */
} cs_slot_status_t;

/** @brief What was learned about one slot. */
typedef struct
{
    cs_slot_status_t status;
    uint32_t seq;
    uint32_t data_len;
} cs_slot_info_t;

/** @brief Internal instance state — hidden from consumers. */
struct config_store_inst
{
    const iqspi_flash_t *flash;
    health_monitor_handle_t health;
    SemaphoreHandle_t mutex;
    StaticSemaphore_t mutex_buf;
    uint8_t active_slot_idx; /**< 0 or 1; cached after load or save.  */
    uint32_t active_seq;     /**< Cached seq_number.                  */
    bool loaded;             /**< True after a successful load.       */
    bool in_use;
};

static struct config_store_inst g_pool[CS_MAX_INSTANCES];
static uint8_t g_count;

/* ====================================================================== */
/* Helpers                                                                */
/* ====================================================================== */

static uint32_t cs_slot_base(uint8_t slot)
{
    return CONFIG_STORE_PARTITION_BASE + ((uint32_t) slot * CONFIG_STORE_SLOT_SIZE);
}

static void cs_put_u32(uint8_t *dest, uint32_t value)
{
    dest[0] = (uint8_t) (value & CS_BYTE_MASK);
    dest[1] = (uint8_t) ((value >> CS_BYTE_SHIFT) & CS_BYTE_MASK);
    dest[2] = (uint8_t) ((value >> (2U * CS_BYTE_SHIFT)) & CS_BYTE_MASK);
    dest[3] = (uint8_t) ((value >> (3U * CS_BYTE_SHIFT)) & CS_BYTE_MASK);
}

static uint32_t cs_get_u32(const uint8_t *src)
{
    return (uint32_t) src[0] | ((uint32_t) src[1] << CS_BYTE_SHIFT) |
           ((uint32_t) src[2] << (2U * CS_BYTE_SHIFT)) |
           ((uint32_t) src[3] << (3U * CS_BYTE_SHIFT));
}

static config_store_err_t cs_check_handle(config_store_handle_t self)
{
    if (self == NULL)
    {
        return CONFIG_STORE_ERR_NULL_PTR;
    }

    if (!self->in_use)
    {
        return CONFIG_STORE_ERR_NOT_INIT;
    }

    return CONFIG_STORE_ERR_OK;
}

static void cs_lock(const struct config_store_inst *inst)
{
    (void) xSemaphoreTake(inst->mutex, portMAX_DELAY);
}

static void cs_unlock(const struct config_store_inst *inst)
{
    (void) xSemaphoreGive(inst->mutex);
}

/** @brief Push a health event if a HealthMonitor was injected. */
static void cs_push(const struct config_store_inst *inst, health_event_t event, uint32_t param)
{
    if (inst->health != NULL)
    {
        (void) health_monitor_push_event(inst->health, event, param);
    }
}

static bool cs_flash_read(const struct config_store_inst *inst, uint32_t addr, uint8_t *buf,
                          uint32_t len)
{
    return inst->flash->read(addr, buf, len) == QSPI_FLASH_OK;
}

/**
 * @brief Program a buffer, split at 256-byte page boundaries.
 *
 * The blob starts 16 bytes into the slot, so it almost never begins on a page
 * boundary, and the driver rejects a write that crosses one.
 */
static bool cs_flash_write(const struct config_store_inst *inst, uint32_t addr, const uint8_t *data,
                           uint32_t len)
{
    uint32_t done = 0U;

    while (done < len)
    {
        uint32_t page_room =
            QSPI_FLASH_PAGE_SIZE_BYTES - ((addr + done) % QSPI_FLASH_PAGE_SIZE_BYTES);
        uint32_t take = ((len - done) < page_room) ? (len - done) : page_room;

        if (inst->flash->write_page(addr + done, &data[done], (uint16_t) take) != QSPI_FLASH_OK)
        {
            return false;
        }

        done += take;
    }

    return true;
}

/** @brief CRC32 of a slot's bytes 0..CRC_OFFSET-1, read in small chunks (CS-D7). */
static bool cs_slot_crc(const struct config_store_inst *inst, uint8_t slot, uint32_t *crc_out)
{
    uint8_t chunk[CS_CHUNK_SIZE];
    uint32_t crc = config_store_crc32_begin();
    uint32_t offset = 0U;

    while (offset < CONFIG_STORE_CRC_OFFSET)
    {
        uint32_t take = CONFIG_STORE_CRC_OFFSET - offset;

        if (take > CS_CHUNK_SIZE)
        {
            take = CS_CHUNK_SIZE;
        }

        if (!cs_flash_read(inst, cs_slot_base(slot) + offset, chunk, take))
        {
            return false;
        }

        crc = config_store_crc32_feed(crc, chunk, take);
        offset += take;
    }

    *crc_out = config_store_crc32_end(crc);

    return true;
}

/**
 * @brief Examine one slot: header first, then the full CRC only if the header
 *        looks written, so an erased slot costs a single 16-byte read.
 */
static cs_slot_info_t cs_eval_slot(const struct config_store_inst *inst, uint8_t slot)
{
    cs_slot_info_t info = {.status = CS_SLOT_INVALID, .seq = 0U, .data_len = 0U};
    uint8_t hdr[CONFIG_STORE_HEADER_SIZE];
    uint8_t stored[CONFIG_STORE_CRC_SIZE];
    uint32_t computed = 0U;

    if (!cs_flash_read(inst, cs_slot_base(slot), hdr, sizeof(hdr)))
    {
        info.status = CS_SLOT_READ_ERROR;
        return info;
    }

    if (cs_get_u32(&hdr[0]) != CONFIG_STORE_MAGIC)
    {
        if (cs_get_u32(&hdr[0]) == CS_ERASED_WORD)
        {
            info.status = CS_SLOT_ERASED;
        }
        return info;
    }

    info.seq = cs_get_u32(&hdr[4]);
    info.data_len = cs_get_u32(&hdr[8]);

    if (info.data_len > CONFIG_STORE_MAX_DATA_BYTES)
    {
        LOG_WARN(CS_LOG_MODULE, "slot %u: implausible length %lu", (unsigned) slot,
                 (unsigned long) info.data_len);
        return info;
    }

    if (!cs_slot_crc(inst, slot, &computed) ||
        !cs_flash_read(inst, cs_slot_base(slot) + CONFIG_STORE_CRC_OFFSET, stored, sizeof(stored)))
    {
        info.status = CS_SLOT_READ_ERROR;
        return info;
    }

    if (computed != cs_get_u32(stored))
    {
        /* Expected after an interrupted write; the other slot is used. */
        LOG_WARN(CS_LOG_MODULE, "slot %u: CRC mismatch", (unsigned) slot);
        return info;
    }

    info.status = CS_SLOT_VALID;

    return info;
}

/**
 * @brief Pick the active slot from two evaluations.
 *
 * @return 0 or 1, or CS_NO_SLOT when neither slot is valid. Equal sequence
 *         numbers (which a correct write never produces) resolve to slot A.
 */
static uint8_t cs_pick_active(const cs_slot_info_t info[CONFIG_STORE_SLOT_COUNT])
{
    bool a = (info[0].status == CS_SLOT_VALID);
    bool b = (info[1].status == CS_SLOT_VALID);

    if (a && b)
    {
        return (info[1].seq > info[0].seq) ? 1U : 0U;
    }

    if (a)
    {
        return 0U;
    }

    return b ? 1U : CS_NO_SLOT;
}

static bool cs_any_read_error(const cs_slot_info_t info[CONFIG_STORE_SLOT_COUNT])
{
    return (info[0].status == CS_SLOT_READ_ERROR) || (info[1].status == CS_SLOT_READ_ERROR);
}

/**
 * @brief Report that no slot is usable and return the matching error.
 *
 * A flash read error is reported as such, because an unreadable slot says
 * nothing about whether a configuration exists; two readable slots with no
 * valid blob is "no valid slot".
 */
static config_store_err_t cs_report_none_valid(const struct config_store_inst *inst,
                                               const cs_slot_info_t info[CONFIG_STORE_SLOT_COUNT])
{
    if (cs_any_read_error(info))
    {
        cs_push(inst, HEALTH_EVENT_FAULT, FAULT_CONFIG_READ_FAIL);
        LOG_ERROR(CS_LOG_MODULE, "no slot could be read");
        return CONFIG_STORE_ERR_FLASH_READ;
    }

    if ((info[0].status == CS_SLOT_ERASED) && (info[1].status == CS_SLOT_ERASED))
    {
        /* A fresh device: not a fault, no health event (LC-O14). */
        LOG_INFO(CS_LOG_MODULE, "no configuration stored yet");
        return CONFIG_STORE_ERR_NEVER_WRITTEN;
    }

    cs_push(inst, HEALTH_EVENT_FAULT, FAULT_CONFIG_NO_VALID_SLOT);
    LOG_ERROR(CS_LOG_MODULE, "no valid configuration slot");

    return CONFIG_STORE_ERR_NO_VALID_SLOT;
}

/** @brief Erase every sector of one slot. */
static bool cs_erase_slot(const struct config_store_inst *inst, uint8_t slot)
{
    for (uint32_t s = 0U; s < CONFIG_STORE_SECTORS_PER_SLOT; s++)
    {
        if (inst->flash->erase_sector(cs_slot_base(slot) + (s * CONFIG_STORE_SECTOR_SIZE)) !=
            QSPI_FLASH_OK)
        {
            return false;
        }
    }

    return true;
}

/**
 * @brief Fold the 0xFF padding between the blob and the CRC field into a running CRC.
 *
 * Erased flash reads as 0xFF and the CRC covers it, so the writer can compute
 * the final value from RAM without reading the slot back.
 */
static uint32_t cs_crc_feed_padding(uint32_t crc, uint32_t pad_len)
{
    uint8_t pad[CS_CHUNK_SIZE];
    uint32_t remaining = pad_len;

    (void) memset(pad, CS_ERASED_BYTE, sizeof(pad));

    while (remaining > 0U)
    {
        uint32_t take = (remaining > CS_CHUNK_SIZE) ? CS_CHUNK_SIZE : remaining;

        crc = config_store_crc32_feed(crc, pad, take);
        remaining -= take;
    }

    return crc;
}

/* ====================================================================== */
/* Public API                                                             */
/* ====================================================================== */

config_store_err_t config_store_create(const config_store_config_t *config,
                                       config_store_handle_t *handle_out)
{
    if ((config == NULL) || (handle_out == NULL) || (config->flash == NULL))
    {
        return CONFIG_STORE_ERR_NULL_PTR;
    }

    if (g_count >= CS_MAX_INSTANCES)
    {
        return CONFIG_STORE_ERR_NO_RESOURCE;
    }

    struct config_store_inst *inst = &g_pool[g_count];

    (void) memset(inst, 0, sizeof(*inst));

    inst->mutex = xSemaphoreCreateMutexStatic(&inst->mutex_buf);
    if (inst->mutex == NULL)
    {
        return CONFIG_STORE_ERR_NO_RESOURCE;
    }

    inst->flash = config->flash;
    inst->health = config->health;
    inst->in_use = true;
    g_count++;
    *handle_out = inst;

    return CONFIG_STORE_ERR_OK;
}

config_store_err_t config_store_load(config_store_handle_t self, void *data_out, uint32_t *len_out,
                                     uint32_t max_len)
{
    cs_slot_info_t info[CONFIG_STORE_SLOT_COUNT];
    config_store_err_t err = cs_check_handle(self);
    uint8_t active;

    if (err != CONFIG_STORE_ERR_OK)
    {
        /* A NULL handle is reported before the data pointers are looked at. */
        return err;
    }

    if ((data_out == NULL) || (len_out == NULL))
    {
        return CONFIG_STORE_ERR_NULL_PTR;
    }

    cs_lock(self);

    info[0] = cs_eval_slot(self, 0U);
    info[1] = cs_eval_slot(self, 1U);
    active = cs_pick_active(info);

    if (active == CS_NO_SLOT)
    {
        err = cs_report_none_valid(self, info);
        cs_unlock(self);
        return err;
    }

    if (info[active].data_len > max_len)
    {
        cs_unlock(self);
        return CONFIG_STORE_ERR_TOO_LARGE;
    }

    /* The driver rejects a zero-length read, so an empty blob is not read at all. */
    if ((info[active].data_len > 0U) &&
        !cs_flash_read(self, cs_slot_base(active) + CONFIG_STORE_HEADER_SIZE, (uint8_t *) data_out,
                       info[active].data_len))
    {
        cs_push(self, HEALTH_EVENT_FAULT, FAULT_CONFIG_READ_FAIL);
        cs_unlock(self);
        return CONFIG_STORE_ERR_FLASH_READ;
    }

    *len_out = info[active].data_len;
    self->active_slot_idx = active;
    self->active_seq = info[active].seq;
    self->loaded = true;

    cs_unlock(self);

    return CONFIG_STORE_ERR_OK;
}

config_store_err_t config_store_check_integrity(config_store_handle_t self)
{
    cs_slot_info_t info[CONFIG_STORE_SLOT_COUNT];
    config_store_err_t err = cs_check_handle(self);

    if (err != CONFIG_STORE_ERR_OK)
    {
        return err;
    }

    cs_lock(self);

    info[0] = cs_eval_slot(self, 0U);
    info[1] = cs_eval_slot(self, 1U);

    if (cs_pick_active(info) == CS_NO_SLOT)
    {
        err = cs_report_none_valid(self, info);
    }

    cs_unlock(self);

    return err;
}

config_store_err_t config_store_save(config_store_handle_t self, const void *data, uint32_t len)
{
    cs_slot_info_t info[CONFIG_STORE_SLOT_COUNT];
    uint8_t hdr[CONFIG_STORE_HEADER_SIZE];
    uint8_t crc_bytes[CONFIG_STORE_CRC_SIZE];
    config_store_err_t err = cs_check_handle(self);
    uint8_t active;
    uint8_t target;
    uint32_t next_seq;
    uint32_t crc;

    if (err != CONFIG_STORE_ERR_OK)
    {
        return err;
    }

    if (data == NULL)
    {
        return CONFIG_STORE_ERR_NULL_PTR;
    }

    if (len > CONFIG_STORE_MAX_DATA_BYTES)
    {
        return CONFIG_STORE_ERR_TOO_LARGE;
    }

    cs_lock(self);

    /* Write into the slot that is not active; with no valid slot, slot A. */
    info[0] = cs_eval_slot(self, 0U);
    info[1] = cs_eval_slot(self, 1U);
    active = cs_pick_active(info);
    target = (active == 0U) ? 1U : 0U;
    next_seq = (active == CS_NO_SLOT) ? 1U : (info[active].seq + 1U);

    if (!cs_erase_slot(self, target))
    {
        cs_push(self, HEALTH_EVENT_CONFIG_WRITE_FAIL, 0U);
        LOG_ERROR(CS_LOG_MODULE, "erase of slot %u failed", (unsigned) target);
        cs_unlock(self);
        return CONFIG_STORE_ERR_FLASH_ERASE;
    }

    cs_put_u32(&hdr[0], CONFIG_STORE_MAGIC);
    cs_put_u32(&hdr[4], next_seq);
    cs_put_u32(&hdr[8], len);
    cs_put_u32(&hdr[12], 0U);

    if (!cs_flash_write(self, cs_slot_base(target), hdr, sizeof(hdr)) ||
        !cs_flash_write(self, cs_slot_base(target) + CONFIG_STORE_HEADER_SIZE,
                        (const uint8_t *) data, len))
    {
        cs_push(self, HEALTH_EVENT_CONFIG_WRITE_FAIL, 0U);
        LOG_ERROR(CS_LOG_MODULE, "program of slot %u failed", (unsigned) target);
        cs_unlock(self);
        return CONFIG_STORE_ERR_FLASH_WRITE;
    }

    /* The CRC is computed from RAM: header, blob, then 0xFF up to the CRC field. */
    crc = config_store_crc32_begin();
    crc = config_store_crc32_feed(crc, hdr, sizeof(hdr));
    crc = config_store_crc32_feed(crc, (const uint8_t *) data, len);
    crc = cs_crc_feed_padding(crc, CONFIG_STORE_DATA_AREA_SIZE - len);
    crc = config_store_crc32_end(crc);
    cs_put_u32(crc_bytes, crc);

    /* Commit point: until this lands the slot is never chosen over the old one. */
    if (!cs_flash_write(self, cs_slot_base(target) + CONFIG_STORE_CRC_OFFSET, crc_bytes,
                        sizeof(crc_bytes)))
    {
        cs_push(self, HEALTH_EVENT_CONFIG_WRITE_FAIL, 0U);
        LOG_ERROR(CS_LOG_MODULE, "CRC commit of slot %u failed", (unsigned) target);
        cs_unlock(self);
        return CONFIG_STORE_ERR_FLASH_WRITE;
    }

    self->active_slot_idx = target;
    self->active_seq = next_seq;

    cs_unlock(self);
    LOG_INFO(CS_LOG_MODULE, "saved %lu bytes to slot %u, seq %lu", (unsigned long) len,
             (unsigned) target, (unsigned long) next_seq);

    return CONFIG_STORE_ERR_OK;
}

config_store_err_t config_store_erase(config_store_handle_t self)
{
    config_store_err_t err = cs_check_handle(self);

    if (err != CONFIG_STORE_ERR_OK)
    {
        return err;
    }

    cs_lock(self);

    for (uint8_t slot = 0U; slot < CONFIG_STORE_SLOT_COUNT; slot++)
    {
        if (!cs_erase_slot(self, slot))
        {
            cs_push(self, HEALTH_EVENT_CONFIG_WRITE_FAIL, 0U);
            cs_unlock(self);
            return CONFIG_STORE_ERR_FLASH_ERASE;
        }
    }

    self->active_slot_idx = 0U;
    self->active_seq = 0U;
    self->loaded = false;

    cs_unlock(self);
    LOG_INFO(CS_LOG_MODULE, "both slots erased");

    return CONFIG_STORE_ERR_OK;
}

#ifdef TEST
void config_store_reset_for_test(void)
{
    (void) memset(g_pool, 0, sizeof(g_pool));
    g_count = 0U;
}
#endif
