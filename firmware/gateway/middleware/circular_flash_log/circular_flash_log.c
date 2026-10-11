/**
 * @file circular_flash_log.c
 * @brief CircularFlashLog implementation — ring mechanics, CRC, boot scan.
 *
 * Companion: docs/lld/middleware/circular-flash-log.md
 *
 * Layout of the 1 MB partition (companion §5): the first two 4 KB sectors are
 * the metadata zone holding the persistent head pointer in A/B rotation; the
 * remaining 254 sectors are the data ring. Ring sector indices used inside
 * this file are 0-based over the data ring, so the absolute partition sector
 * is CFL_DATA_SECTOR_BASE + ring index.
 */

#include "circular_flash_log.h"

#include <string.h>

/* ====================================================================== */
/* Geometry and format constants                                          */
/* ====================================================================== */

/** @brief Instances served from the static pool (companion CFL-D1). */
#define CFL_MAX_INSTANCES (1U)

/** Partition base as a device byte offset. The QspiFlashDriver addresses the
 *  device from zero; 0x0002_0000 is the offset of the memory-mapped base
 *  0x9002_0000 quoted in flash-partition-layout.md §5.2. */
#define CFL_PARTITION_BASE_ADDR (0x00020000UL)

/** @brief Erase granularity, and therefore the ring's unit of reuse. */
#define CFL_SECTOR_SIZE_BYTES (QSPI_FLASH_SECTOR_SIZE_BYTES)

/** @brief First data sector, expressed as a partition sector index. */
#define CFL_DATA_SECTOR_BASE (2U)

/** @brief Data ring length in sectors. */
#define CFL_DATA_SECTOR_COUNT (254U)

/** @brief Metadata slots taking part in the A/B rotation. */
#define CFL_META_SLOT_COUNT (2U)

/** @brief Metadata slot bytes covered by the CRC: magic + seq_num + head. */
#define CFL_META_SLOT_BODY_SZ (12U)

/** @brief Metadata slot size including the trailing CRC-32. */
#define CFL_META_SLOT_SZ (16U)

/** @brief Marks a written metadata slot; distinct from CFL_MAGIC. */
#define CFL_META_MAGIC (0xBEEF5A5BUL)

/** @brief Record start alignment, in bytes. */
#define CFL_ALIGNMENT_BYTES (4U)

/** @brief Smallest record the ring can hold: header + one payload byte + CRC,
 *         rounded up to CFL_ALIGNMENT_BYTES. */
#define CFL_MIN_RECORD_BYTES (16U)

/** @brief Scratch window used to fold a payload CRC without a sector-sized
 *         stack buffer during the boot scan. */
#define CFL_SCAN_CHUNK_BYTES (64U)

/* CRC-16/IBM (a.k.a. CRC-16/ARC): reflected, poly 0x8005, init 0x0000. */
#define CFL_CRC16_POLY_REFLECTED (0xA001U)
#define CFL_CRC16_INIT (0x0000U)

/* CRC-32 (IEEE 802.3): reflected, poly 0x04C11DB7, init and final XOR all-ones. */
#define CFL_CRC32_POLY_REFLECTED (0xEDB88320UL)
#define CFL_CRC32_INIT (0xFFFFFFFFUL)
#define CFL_CRC32_FINAL_XOR (0xFFFFFFFFUL)

#define CFL_BITS_PER_BYTE (8U)
#define CFL_BYTE_MASK (0xFFU)

/** @brief Value of an erased NOR flash byte. */
#define CFL_ERASED_BYTE (0xFFU)

/* ====================================================================== */
/* Private state                                                          */
/* ====================================================================== */

/** @brief Parsed record header (companion §3). */
typedef struct
{
    uint32_t magic;
    uint32_t seq_num;
    uint32_t data_len;
} cfl_record_hdr_t;

/** @brief Internal instance state — hidden from consumers. */
struct cfl_inst
{
    /* Injected dependencies */
    const iqspi_flash_t *qspi;
    health_monitor_handle_t health;

    /* Ring state */
    uint8_t head_sector;  /**< Ring index (0..253) of the oldest record.   */
    uint32_t head_offset; /**< Byte offset of the oldest record.           */
    uint8_t tail_sector;  /**< Ring index of the next write location.      */
    uint32_t tail_offset; /**< Byte offset of the next write location.     */
    uint32_t next_seq_num;
    uint32_t entry_count; /**< Unconsumed records.                         */
    uint32_t byte_count;  /**< Total payload bytes held.                   */

    /* Metadata A/B rotation state */
    uint8_t meta_slot;     /**< Slot holding the current head pointer.    */
    uint32_t meta_seq_num; /**< Sequence number of that slot.             */
    bool meta_slot_valid;  /**< False until a slot has been written.      */

    bool in_use;
};

static struct cfl_inst g_pool[CFL_MAX_INSTANCES];
static uint8_t g_count;

/* ====================================================================== */
/* IHealthReport default (CFL-O5)                                         */
/* ====================================================================== */

/**
 * @brief Weak no-op default for the IHealthReport push.
 *
 * The Gateway has no HealthMonitor yet, so without a definition the image
 * would not link once this module is compiled into it. The weak binding lets
 * a real implementation — or a unit test's spy, or a bring-up harness's stub —
 * override it with an ordinary strong definition, exactly as
 * panic_symbols.c does for its generated tables. Remove this default when the
 * GW HealthMonitor lands (CFL-O5).
 */
__attribute__((weak)) void health_report_push_buffer_event(health_monitor_handle_t handle,
                                                           cfl_health_event_t event)
{
    (void) handle;
    (void) event;
}

/* ====================================================================== */
/* Little-endian serialisation                                            */
/* ====================================================================== */

static void cfl_put_u32(uint8_t *dest, uint32_t value)
{
    dest[0] = (uint8_t) (value & CFL_BYTE_MASK);
    dest[1] = (uint8_t) ((value >> 8U) & CFL_BYTE_MASK);
    dest[2] = (uint8_t) ((value >> 16U) & CFL_BYTE_MASK);
    dest[3] = (uint8_t) ((value >> 24U) & CFL_BYTE_MASK);
}

static uint32_t cfl_get_u32(const uint8_t *src)
{
    return (uint32_t) src[0] | ((uint32_t) src[1] << 8U) | ((uint32_t) src[2] << 16U) |
           ((uint32_t) src[3] << 24U);
}

static void cfl_put_u16(uint8_t *dest, uint16_t value)
{
    dest[0] = (uint8_t) (value & CFL_BYTE_MASK);
    dest[1] = (uint8_t) ((value >> 8U) & CFL_BYTE_MASK);
}

static uint16_t cfl_get_u16(const uint8_t *src)
{
    return (uint16_t) ((uint16_t) src[0] | (uint16_t) ((uint16_t) src[1] << 8U));
}

/* ====================================================================== */
/* CRC helpers (CFL-D4)                                                   */
/* ====================================================================== */

/**
 * @brief Fold bytes into a running CRC-16/IBM.
 *
 * Bitwise rather than table-driven: the ring writes at most a few kilobytes
 * per record and a 512-byte table would cost more flash than the loop costs
 * cycles.
 */
static uint16_t cfl_crc16_update(uint16_t crc, const uint8_t *data, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++)
    {
        crc ^= (uint16_t) data[i];
        for (uint8_t bit = 0U; bit < CFL_BITS_PER_BYTE; bit++)
        {
            if ((crc & 1U) != 0U)
            {
                crc = (uint16_t) ((crc >> 1U) ^ CFL_CRC16_POLY_REFLECTED);
            }
            else
            {
                crc = (uint16_t) (crc >> 1U);
            }
        }
    }

    return crc;
}

/** @brief CRC-32 over a buffer, used for the metadata slots. */
static uint32_t cfl_crc32(const uint8_t *data, uint32_t len)
{
    uint32_t crc = CFL_CRC32_INIT;

    for (uint32_t i = 0U; i < len; i++)
    {
        crc ^= (uint32_t) data[i];
        for (uint8_t bit = 0U; bit < CFL_BITS_PER_BYTE; bit++)
        {
            if ((crc & 1UL) != 0UL)
            {
                crc = (crc >> 1U) ^ CFL_CRC32_POLY_REFLECTED;
            }
            else
            {
                crc = crc >> 1U;
            }
        }
    }

    return crc ^ CFL_CRC32_FINAL_XOR;
}

/* ====================================================================== */
/* Flash access helpers                                                   */
/* ====================================================================== */

/** @brief Device address of a data-ring sector, by ring index. */
static uint32_t cfl_data_sector_addr(uint8_t ring_index)
{
    return CFL_PARTITION_BASE_ADDR +
           (((uint32_t) CFL_DATA_SECTOR_BASE + (uint32_t) ring_index) * CFL_SECTOR_SIZE_BYTES);
}

/** @brief Device address of a metadata slot sector. */
static uint32_t cfl_meta_slot_addr(uint8_t slot)
{
    return CFL_PARTITION_BASE_ADDR + ((uint32_t) slot * CFL_SECTOR_SIZE_BYTES);
}

static cfl_err_t cfl_flash_read(const struct cfl_inst *inst, uint32_t addr, uint8_t *buf,
                                uint32_t len)
{
    if (inst->qspi->read(addr, buf, len) != QSPI_FLASH_OK)
    {
        return CFL_ERR_FLASH_READ;
    }

    return CFL_ERR_OK;
}

/**
 * @brief Program an arbitrary byte range, splitting at page boundaries.
 *
 * qspi_flash_write_page() rejects a transfer longer than 256 bytes or one
 * that crosses a page boundary (QSPI_FLASH_ERR_LEN), so every multi-page
 * write has to be chunked here — the same arrangement FD ConfigStore uses.
 */
static cfl_err_t cfl_flash_write(const struct cfl_inst *inst, uint32_t addr, const uint8_t *data,
                                 uint32_t len)
{
    uint32_t written = 0U;

    while (written < len)
    {
        uint32_t page_left =
            QSPI_FLASH_PAGE_SIZE_BYTES - ((addr + written) % QSPI_FLASH_PAGE_SIZE_BYTES);
        uint32_t chunk = len - written;

        if (chunk > page_left)
        {
            chunk = page_left;
        }

        if (inst->qspi->write_page(addr + written, &data[written], (uint16_t) chunk) !=
            QSPI_FLASH_OK)
        {
            return CFL_ERR_FLASH_WRITE;
        }

        written += chunk;
    }

    return CFL_ERR_OK;
}

static cfl_err_t cfl_flash_erase(const struct cfl_inst *inst, uint32_t addr)
{
    if (inst->qspi->erase_sector(addr) != QSPI_FLASH_OK)
    {
        return CFL_ERR_FLASH_ERASE;
    }

    return CFL_ERR_OK;
}

/* ====================================================================== */
/* Record helpers                                                         */
/* ====================================================================== */

/** @brief Total flash footprint of a record carrying @p len payload bytes. */
static uint32_t cfl_record_size(uint32_t len)
{
    uint32_t raw = CFL_RECORD_HEADER_SZ + len + CFL_RECORD_CRC_SZ;

    return (raw + (CFL_ALIGNMENT_BYTES - 1U)) & ~(uint32_t) (CFL_ALIGNMENT_BYTES - 1U);
}

/** @brief Read and parse a record header at a ring position. */
static cfl_err_t cfl_read_header(const struct cfl_inst *inst, uint8_t sector, uint32_t offset,
                                 cfl_record_hdr_t *hdr_out)
{
    uint8_t raw[CFL_RECORD_HEADER_SZ];
    cfl_err_t err = cfl_flash_read(inst, cfl_data_sector_addr(sector) + offset, raw, sizeof(raw));

    if (err != CFL_ERR_OK)
    {
        return err;
    }

    hdr_out->magic = cfl_get_u32(&raw[0]);
    hdr_out->seq_num = cfl_get_u32(&raw[4]);
    hdr_out->data_len = cfl_get_u32(&raw[8]);

    return CFL_ERR_OK;
}

/**
 * @brief True when a header describes a structurally plausible record that
 *        fits inside its sector.
 *
 * Structure is checked before the CRC so that an erased region (all 0xFF,
 * giving a data_len of 0xFFFFFFFF) is rejected without reading 4 GB of
 * payload to compute a CRC over it.
 */
static bool cfl_header_plausible(const cfl_record_hdr_t *hdr, uint32_t offset)
{
    if (hdr->magic != CFL_MAGIC)
    {
        return false;
    }

    if ((hdr->data_len == 0U) || (hdr->data_len > CFL_MAX_RECORD_BYTES))
    {
        return false;
    }

    return (offset + cfl_record_size(hdr->data_len)) <= CFL_SECTOR_SIZE_BYTES;
}

/** @brief True when another record could begin at @p offset in a sector. */
static bool cfl_room_for_record(uint32_t offset)
{
    return (offset + CFL_MIN_RECORD_BYTES) <= CFL_SECTOR_SIZE_BYTES;
}

/**
 * @brief Verify a record's stored CRC-16 without a sector-sized buffer.
 *
 * The payload is folded through a CFL_SCAN_CHUNK_BYTES window, so the boot
 * scan costs 64 bytes of stack regardless of record size.
 */
static cfl_err_t cfl_verify_record_crc(const struct cfl_inst *inst, uint8_t sector, uint32_t offset,
                                       const cfl_record_hdr_t *hdr, bool *ok_out)
{
    uint8_t raw_hdr[CFL_RECORD_HEADER_SZ];
    uint8_t chunk[CFL_SCAN_CHUNK_BYTES];
    uint8_t stored[CFL_RECORD_CRC_SZ];
    uint32_t base = cfl_data_sector_addr(sector) + offset;
    uint32_t remaining = hdr->data_len;
    uint32_t read_at = base + CFL_RECORD_HEADER_SZ;
    uint16_t crc;
    cfl_err_t err;

    cfl_put_u32(&raw_hdr[0], hdr->magic);
    cfl_put_u32(&raw_hdr[4], hdr->seq_num);
    cfl_put_u32(&raw_hdr[8], hdr->data_len);
    crc = cfl_crc16_update(CFL_CRC16_INIT, raw_hdr, sizeof(raw_hdr));

    while (remaining > 0U)
    {
        uint32_t take = (remaining > CFL_SCAN_CHUNK_BYTES) ? CFL_SCAN_CHUNK_BYTES : remaining;

        err = cfl_flash_read(inst, read_at, chunk, take);
        if (err != CFL_ERR_OK)
        {
            return err;
        }

        crc = cfl_crc16_update(crc, chunk, take);
        read_at += take;
        remaining -= take;
    }

    err = cfl_flash_read(inst, base + CFL_RECORD_HEADER_SZ + hdr->data_len, stored, sizeof(stored));
    if (err != CFL_ERR_OK)
    {
        return err;
    }

    *ok_out = (cfl_get_u16(stored) == crc);

    return CFL_ERR_OK;
}

/* ====================================================================== */
/* Metadata zone — persistent head pointer (D40)                          */
/* ====================================================================== */

/**
 * @brief Recover the head sector from the A/B metadata slots.
 *
 * On fresh flash neither slot is valid; the head starts at ring index 0 and
 * meta_slot is primed so the first persist writes slot 0.
 */
static cfl_err_t cfl_meta_load(struct cfl_inst *inst)
{
    inst->head_sector = 0U;
    inst->head_offset = 0U;
    inst->meta_seq_num = 0U;
    inst->meta_slot = CFL_META_SLOT_COUNT - 1U;
    inst->meta_slot_valid = false;

    for (uint8_t slot = 0U; slot < CFL_META_SLOT_COUNT; slot++)
    {
        uint8_t raw[CFL_META_SLOT_SZ];
        cfl_err_t err = cfl_flash_read(inst, cfl_meta_slot_addr(slot), raw, sizeof(raw));

        if (err != CFL_ERR_OK)
        {
            return err;
        }

        if (cfl_get_u32(&raw[0]) != CFL_META_MAGIC)
        {
            continue;
        }

        if (cfl_get_u32(&raw[CFL_META_SLOT_BODY_SZ]) != cfl_crc32(raw, CFL_META_SLOT_BODY_SZ))
        {
            continue;
        }

        uint32_t seq = cfl_get_u32(&raw[4]);
        uint32_t head = cfl_get_u32(&raw[8]);

        if (head >= CFL_DATA_SECTOR_COUNT)
        {
            continue;
        }

        if (!inst->meta_slot_valid || (seq > inst->meta_seq_num))
        {
            inst->meta_slot_valid = true;
            inst->meta_slot = slot;
            inst->meta_seq_num = seq;
            inst->head_sector = (uint8_t) head;
        }
    }

    return CFL_ERR_OK;
}

/**
 * @brief Write the current head sector to the inactive metadata slot.
 *
 * The previous slot stays readable throughout the erase-then-write window,
 * which is the point of the rotation: a power loss mid-write leaves the older
 * head pointer intact and recoverable.
 */
static cfl_err_t cfl_meta_store(struct cfl_inst *inst)
{
    uint8_t slot = (uint8_t) ((inst->meta_slot + 1U) % CFL_META_SLOT_COUNT);
    uint32_t seq = inst->meta_seq_num + 1U;
    uint8_t raw[CFL_META_SLOT_SZ];
    cfl_err_t err;

    cfl_put_u32(&raw[0], CFL_META_MAGIC);
    cfl_put_u32(&raw[4], seq);
    cfl_put_u32(&raw[8], (uint32_t) inst->head_sector);
    cfl_put_u32(&raw[CFL_META_SLOT_BODY_SZ], cfl_crc32(raw, CFL_META_SLOT_BODY_SZ));

    err = cfl_flash_erase(inst, cfl_meta_slot_addr(slot));
    if (err != CFL_ERR_OK)
    {
        return err;
    }

    err = cfl_flash_write(inst, cfl_meta_slot_addr(slot), raw, sizeof(raw));
    if (err != CFL_ERR_OK)
    {
        return err;
    }

    inst->meta_slot = slot;
    inst->meta_seq_num = seq;
    inst->meta_slot_valid = true;

    return CFL_ERR_OK;
}

/* ====================================================================== */
/* Boot scan (companion §6.4)                                             */
/* ====================================================================== */

/**
 * @brief Walk one sector's records, counting the valid prefix.
 *
 * Scanning stops at the first record that is implausible or fails its CRC —
 * a record interrupted by power loss therefore terminates the sector without
 * discarding anything written before it (CFL-T12).
 */
static cfl_err_t cfl_scan_sector(const struct cfl_inst *inst, uint8_t sector, uint32_t *count_out,
                                 uint32_t *bytes_out, uint32_t *end_offset_out,
                                 uint32_t *max_seq_out)
{
    uint32_t offset = 0U;

    *count_out = 0U;
    *bytes_out = 0U;
    *end_offset_out = 0U;

    while (cfl_room_for_record(offset))
    {
        cfl_record_hdr_t hdr;
        bool crc_ok = false;
        cfl_err_t err = cfl_read_header(inst, sector, offset, &hdr);

        if (err != CFL_ERR_OK)
        {
            return err;
        }

        if (!cfl_header_plausible(&hdr, offset))
        {
            break;
        }

        err = cfl_verify_record_crc(inst, sector, offset, &hdr, &crc_ok);
        if (err != CFL_ERR_OK)
        {
            return err;
        }

        if (!crc_ok)
        {
            break;
        }

        (*count_out)++;
        *bytes_out += hdr.data_len;
        offset += cfl_record_size(hdr.data_len);
        *end_offset_out = offset;

        if (hdr.seq_num >= *max_seq_out)
        {
            *max_seq_out = hdr.seq_num;
        }
    }

    return CFL_ERR_OK;
}

/**
 * @brief True when every byte from @p offset to the end of @p sector is still
 *        erased (0xFF).
 *
 * NOR flash programming only clears bits, so a record interrupted by power
 * loss leaves partially programmed bytes behind. Writing a fresh record over
 * them would store the bitwise AND of old and new data — a record that fails
 * its CRC on every subsequent read and halts the boot scan at that offset for
 * good. Detecting the dirty region lets the tail retire the rest of the
 * sector instead (CFL-T12).
 */
static cfl_err_t cfl_region_erased(const struct cfl_inst *inst, uint8_t sector, uint32_t offset,
                                   bool *clean_out)
{
    uint32_t addr = cfl_data_sector_addr(sector) + offset;
    uint32_t remaining = CFL_SECTOR_SIZE_BYTES - offset;

    *clean_out = true;

    while (remaining > 0U)
    {
        uint8_t chunk[CFL_SCAN_CHUNK_BYTES];
        uint32_t take = (remaining > CFL_SCAN_CHUNK_BYTES) ? CFL_SCAN_CHUNK_BYTES : remaining;
        cfl_err_t err = cfl_flash_read(inst, addr, chunk, take);

        if (err != CFL_ERR_OK)
        {
            return err;
        }

        for (uint32_t i = 0U; i < take; i++)
        {
            if (chunk[i] != CFL_ERASED_BYTE)
            {
                *clean_out = false;
                return CFL_ERR_OK;
            }
        }

        addr += take;
        remaining -= take;
    }

    return CFL_ERR_OK;
}

/**
 * @brief Locate the tail and rebuild the counters, starting from the head.
 *
 * A sector holding no valid record at offset 0 is the sector past the tail,
 * which terminates the walk. Slack left at the end of a full sector does not
 * terminate it, because the next sector still begins with a valid record.
 */
static cfl_err_t cfl_boot_scan(struct cfl_inst *inst)
{
    uint32_t max_seq = 0U;
    bool any_record = false;

    inst->entry_count = 0U;
    inst->byte_count = 0U;
    inst->tail_sector = inst->head_sector;
    inst->tail_offset = 0U;

    for (uint32_t step = 0U; step < CFL_DATA_SECTOR_COUNT; step++)
    {
        uint8_t sector = (uint8_t) (((uint32_t) inst->head_sector + step) % CFL_DATA_SECTOR_COUNT);
        uint32_t count = 0U;
        uint32_t bytes = 0U;
        uint32_t end_offset = 0U;
        cfl_err_t err = cfl_scan_sector(inst, sector, &count, &bytes, &end_offset, &max_seq);

        if (err != CFL_ERR_OK)
        {
            return err;
        }

        if (count == 0U)
        {
            if (!any_record)
            {
                inst->tail_sector = sector;
                inst->tail_offset = 0U;
            }
            break;
        }

        any_record = true;
        inst->entry_count += count;
        inst->byte_count += bytes;
        inst->tail_sector = sector;
        inst->tail_offset = end_offset;
    }

    inst->next_seq_num = any_record ? (max_seq + 1U) : 0U;

    /* Retire the remainder of the tail sector if it is not pristine, so an
     * interrupted record is never overwritten in place. */
    if (cfl_room_for_record(inst->tail_offset))
    {
        bool clean = true;
        cfl_err_t err = cfl_region_erased(inst, inst->tail_sector, inst->tail_offset, &clean);

        if (err != CFL_ERR_OK)
        {
            return err;
        }

        if (!clean)
        {
            inst->tail_offset = CFL_SECTOR_SIZE_BYTES;
        }
    }

    return CFL_ERR_OK;
}

/* ====================================================================== */
/* Shared guards                                                          */
/* ====================================================================== */

static cfl_err_t cfl_check_handle(cfl_handle_t handle)
{
    if (handle == NULL)
    {
        return CFL_ERR_NULL_PTR;
    }

    if (!handle->in_use)
    {
        return CFL_ERR_NOT_INIT;
    }

    return CFL_ERR_OK;
}

/* ====================================================================== */
/* Public API                                                             */
/* ====================================================================== */

cfl_err_t cfl_create(const cfl_config_t *config, cfl_handle_t *handle)
{
    if ((config == NULL) || (handle == NULL) || (config->qspi == NULL))
    {
        return CFL_ERR_NULL_PTR;
    }

    if (g_count >= CFL_MAX_INSTANCES)
    {
        return CFL_ERR_NO_RESOURCE;
    }

    struct cfl_inst *inst = &g_pool[g_count];

    (void) memset(inst, 0, sizeof(*inst));
    inst->qspi = config->qspi;
    inst->health = config->health;

    cfl_err_t err = cfl_meta_load(inst);
    if (err != CFL_ERR_OK)
    {
        return err;
    }

    err = cfl_boot_scan(inst);
    if (err != CFL_ERR_OK)
    {
        return err;
    }

    inst->in_use = true;
    g_count++;
    *handle = inst;

    return CFL_ERR_OK;
}

cfl_err_t cfl_append(cfl_handle_t handle, const uint8_t *data, uint32_t len)
{
    cfl_err_t err = cfl_check_handle(handle);

    if (err != CFL_ERR_OK)
    {
        return err;
    }

    if (data == NULL)
    {
        return CFL_ERR_NULL_PTR;
    }

    if ((len == 0U) || (len > CFL_MAX_RECORD_BYTES))
    {
        return CFL_ERR_TOO_LARGE;
    }

    uint32_t size = cfl_record_size(len);
    bool was_empty = (handle->entry_count == 0U);

    /* Advance to the next sector when this one cannot hold the record. */
    if ((handle->tail_offset + size) > CFL_SECTOR_SIZE_BYTES)
    {
        uint8_t next = (uint8_t) (((uint32_t) handle->tail_sector + 1U) % CFL_DATA_SECTOR_COUNT);

        if ((next == handle->head_sector) && !was_empty)
        {
            return CFL_ERR_FULL;
        }

        err = cfl_flash_erase(handle, cfl_data_sector_addr(next));
        if (err != CFL_ERR_OK)
        {
            health_report_push_buffer_event(handle->health, CFL_HEALTH_EVENT_BUFFER_FLASH_ERR);
            return err;
        }

        handle->tail_sector = next;
        handle->tail_offset = 0U;

        /* An empty ring parks the head on the tail. It has to travel with the
         * tail, or the first record written after the move would be looked for
         * at a stale head position holding nothing. */
        if (was_empty)
        {
            uint8_t previous_head = handle->head_sector;

            handle->head_sector = handle->tail_sector;
            handle->head_offset = 0U;

            if (handle->head_sector != previous_head)
            {
                err = cfl_meta_store(handle);
                if (err != CFL_ERR_OK)
                {
                    health_report_push_buffer_event(handle->health,
                                                    CFL_HEALTH_EVENT_BUFFER_FLASH_ERR);
                    return err;
                }
            }
        }
    }

    uint8_t raw_hdr[CFL_RECORD_HEADER_SZ];
    uint8_t raw_crc[CFL_RECORD_CRC_SZ];
    uint32_t base = cfl_data_sector_addr(handle->tail_sector) + handle->tail_offset;
    uint16_t crc;

    cfl_put_u32(&raw_hdr[0], CFL_MAGIC);
    cfl_put_u32(&raw_hdr[4], handle->next_seq_num);
    cfl_put_u32(&raw_hdr[8], len);

    crc = cfl_crc16_update(CFL_CRC16_INIT, raw_hdr, sizeof(raw_hdr));
    crc = cfl_crc16_update(crc, data, len);
    cfl_put_u16(raw_crc, crc);

    /* Payload first, then the header: the magic is what makes a record
     * discoverable, so writing it last means a power loss part-way through
     * leaves a region the boot scan skips rather than a header promising a
     * payload that was never written. */
    err = cfl_flash_write(handle, base + CFL_RECORD_HEADER_SZ, data, len);
    if (err == CFL_ERR_OK)
    {
        err = cfl_flash_write(handle, base + CFL_RECORD_HEADER_SZ + len, raw_crc, sizeof(raw_crc));
    }

    if (err == CFL_ERR_OK)
    {
        err = cfl_flash_write(handle, base, raw_hdr, sizeof(raw_hdr));
    }

    if (err != CFL_ERR_OK)
    {
        /* Part of the record may already be programmed. Retire the rest of the
         * sector rather than write the next record over dirty bytes — see
         * cfl_region_erased() for why that would be unrecoverable. */
        handle->tail_offset = CFL_SECTOR_SIZE_BYTES;
        health_report_push_buffer_event(handle->health, CFL_HEALTH_EVENT_BUFFER_FLASH_ERR);
        return err;
    }

    handle->tail_offset += size;
    handle->next_seq_num++;
    handle->entry_count++;
    handle->byte_count += len;

    return CFL_ERR_OK;
}

cfl_err_t cfl_peek_oldest(cfl_handle_t handle, uint8_t *data_out, uint32_t *len_out,
                          uint32_t max_len)
{
    cfl_err_t err = cfl_check_handle(handle);

    if (err != CFL_ERR_OK)
    {
        return err;
    }

    if ((data_out == NULL) || (len_out == NULL))
    {
        return CFL_ERR_NULL_PTR;
    }

    if (handle->entry_count == 0U)
    {
        return CFL_ERR_EMPTY;
    }

    cfl_record_hdr_t hdr;

    err = cfl_read_header(handle, handle->head_sector, handle->head_offset, &hdr);
    if (err != CFL_ERR_OK)
    {
        return err;
    }

    if (!cfl_header_plausible(&hdr, handle->head_offset))
    {
        return CFL_ERR_CORRUPT;
    }

    if (hdr.data_len > max_len)
    {
        *len_out = hdr.data_len;
        return CFL_ERR_TOO_LARGE;
    }

    uint32_t base = cfl_data_sector_addr(handle->head_sector) + handle->head_offset;

    err = cfl_flash_read(handle, base + CFL_RECORD_HEADER_SZ, data_out, hdr.data_len);
    if (err != CFL_ERR_OK)
    {
        return err;
    }

    uint8_t raw_hdr[CFL_RECORD_HEADER_SZ];
    uint8_t stored[CFL_RECORD_CRC_SZ];
    uint16_t crc;

    cfl_put_u32(&raw_hdr[0], hdr.magic);
    cfl_put_u32(&raw_hdr[4], hdr.seq_num);
    cfl_put_u32(&raw_hdr[8], hdr.data_len);
    crc = cfl_crc16_update(CFL_CRC16_INIT, raw_hdr, sizeof(raw_hdr));
    crc = cfl_crc16_update(crc, data_out, hdr.data_len);

    err =
        cfl_flash_read(handle, base + CFL_RECORD_HEADER_SZ + hdr.data_len, stored, sizeof(stored));
    if (err != CFL_ERR_OK)
    {
        return err;
    }

    if (cfl_get_u16(stored) != crc)
    {
        return CFL_ERR_CORRUPT;
    }

    *len_out = hdr.data_len;

    return CFL_ERR_OK;
}

/**
 * @brief Advance the head past a record of @p size bytes and persist the head
 *        sector if it changed.
 */
static cfl_err_t cfl_advance_head(struct cfl_inst *inst, uint32_t size, uint32_t payload_len)
{
    uint8_t previous_sector = inst->head_sector;

    inst->head_offset += size;
    inst->entry_count--;
    inst->byte_count -= payload_len;

    if (inst->entry_count == 0U)
    {
        /* Drained: park the head on the tail so the ring reads as empty and
         * the next append does not mistake a stale head for a full ring. */
        inst->head_sector = inst->tail_sector;
        inst->head_offset = inst->tail_offset;
    }
    else
    {
        bool more_here = false;

        if (cfl_room_for_record(inst->head_offset) &&
            !((inst->head_sector == inst->tail_sector) && (inst->head_offset >= inst->tail_offset)))
        {
            cfl_record_hdr_t hdr;
            cfl_err_t err = cfl_read_header(inst, inst->head_sector, inst->head_offset, &hdr);

            if (err != CFL_ERR_OK)
            {
                return err;
            }

            more_here = cfl_header_plausible(&hdr, inst->head_offset);
        }

        if (!more_here)
        {
            inst->head_sector =
                (uint8_t) (((uint32_t) inst->head_sector + 1U) % CFL_DATA_SECTOR_COUNT);
            inst->head_offset = 0U;
        }
    }

    if (inst->head_sector != previous_sector)
    {
        return cfl_meta_store(inst);
    }

    return CFL_ERR_OK;
}

cfl_err_t cfl_consume_oldest(cfl_handle_t handle)
{
    cfl_err_t err = cfl_check_handle(handle);

    if (err != CFL_ERR_OK)
    {
        return err;
    }

    if (handle->entry_count == 0U)
    {
        return CFL_ERR_EMPTY;
    }

    cfl_record_hdr_t hdr;

    err = cfl_read_header(handle, handle->head_sector, handle->head_offset, &hdr);
    if (err != CFL_ERR_OK)
    {
        return err;
    }

    /* A corrupt record must still be consumable — StoreAndForward advances
     * past it after a CFL_ERR_CORRUPT peek (companion §10). Fall back to the
     * smallest record footprint when the header itself is unusable. */
    uint32_t size = CFL_MIN_RECORD_BYTES;
    uint32_t payload = 0U;

    if (cfl_header_plausible(&hdr, handle->head_offset))
    {
        size = cfl_record_size(hdr.data_len);
        payload = (hdr.data_len <= handle->byte_count) ? hdr.data_len : handle->byte_count;
    }

    return cfl_advance_head(handle, size, payload);
}

cfl_err_t cfl_drop_oldest(cfl_handle_t handle)
{
    cfl_err_t err = cfl_consume_oldest(handle);

    if (err == CFL_ERR_OK)
    {
        health_report_push_buffer_event(handle->health, CFL_HEALTH_EVENT_BUFFER_OVERFLOW);
    }

    return err;
}

cfl_err_t cfl_is_empty(cfl_handle_t handle, bool *empty_out)
{
    cfl_err_t err = cfl_check_handle(handle);

    if (err != CFL_ERR_OK)
    {
        return err;
    }

    if (empty_out == NULL)
    {
        return CFL_ERR_NULL_PTR;
    }

    *empty_out = (handle->entry_count == 0U);

    return CFL_ERR_OK;
}

cfl_err_t cfl_get_occupancy(cfl_handle_t handle, uint32_t *entry_count, uint32_t *byte_count)
{
    cfl_err_t err = cfl_check_handle(handle);

    if (err != CFL_ERR_OK)
    {
        return err;
    }

    if ((entry_count == NULL) || (byte_count == NULL))
    {
        return CFL_ERR_NULL_PTR;
    }

    *entry_count = handle->entry_count;
    *byte_count = handle->byte_count;

    return CFL_ERR_OK;
}

#ifdef TEST
void cfl_reset_for_test(void)
{
    (void) memset(g_pool, 0, sizeof(g_pool));
    g_count = 0U;
}
#endif
