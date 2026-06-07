/**
 * @file config_store.c
 * @brief ConfigStore — power-loss-safe A/B slot NOR flash configuration persistence.
 *
 * Implements IConfigStore per docs/lld/middleware/config-store.md.
 * A/B slot rotation with CRC32/ISO-HDLC integrity per flash-partition-layout.md §6.1 (D39).
 *
 * In UNIT_TEST builds the QSPI calls are replaced by macros that redirect
 * to RAM-backed stubs defined in the test TU (see config_store.h).
 */

#include "config_store/config_store.h"
#include "config_store/config_store_crc.h"

#include <string.h>

#include "FreeRTOS.h"
#include "semphr.h"

#include "logger/logger.h"

#ifdef TEST
/* health_monitor_stub.h provides health_event_t and HEALTH_EVENT_* constants.
 * In production builds these come from health_monitor.h via config_store.h. */
#  include "health_monitor_stub.h"
#endif

#ifndef UNIT_TEST
#  include "qspi_flash_driver/qspi_flash_driver.h"
#endif

/* ========================================================================= */
/* UNIT_TEST: flash simulation buffer                                        */
/* ========================================================================= */

#ifdef UNIT_TEST
/* Backing store for the flash simulation.  Initialised to 0xFF in setUp().
 * Extern declaration is in config_store.h. */
uint8_t g_config_store_flash_sim[CONFIG_STORE_PARTITION_SIZE];
/* cs_flash_erase_range, cs_flash_write_bytes, cs_flash_read_bytes are
 * macros defined in config_store.h — no static function definitions needed. */
#endif /* UNIT_TEST */

/* ========================================================================= */
/* Internal constants                                                        */
/* ========================================================================= */

#define CS_HEADER_SIZE       (16U)
#define CS_CRC_OFFSET        (0x7FF8U)  /* byte offset of CRC within a slot */
#define CS_CRC_COVERAGE      (0x7FF8U)  /* bytes 0x0000..0x7FF7 are covered */
#define CS_DATA_AREA_SIZE    (CS_CRC_OFFSET - CS_HEADER_SIZE)     /* 32744 */
#define CS_SECTORS_PER_SLOT  (CONFIG_STORE_SLOT_SIZE / CONFIG_STORE_SECTOR_SIZE) /* 8 */
#define CS_CHUNK_SIZE        (256U)
#define CS_FF_PAD_SIZE       (64U)
#define CS_SLOT_COUNT        (2U)
#define CS_MODULE_NAME       "ConfigStore"

/* ========================================================================= */
/* Internal types                                                            */
/* ========================================================================= */

typedef struct
{
    uint32_t magic;
    uint32_t seq_number;
    uint32_t data_len;
    uint32_t reserved;
} cs_slot_header_t;

typedef struct
{
    bool              initialised;
    ihealth_report_t *health;
    StaticSemaphore_t mutex_buf;
    SemaphoreHandle_t mutex;
    uint8_t           active_slot_index;
    uint32_t          active_seq_number;
} cs_state_t;

/* ========================================================================= */
/* Module state                                                              */
/* ========================================================================= */

static cs_state_t s_cs;

/* 0xFF padding buffer used when computing the CRC over the padded data area. */
static const uint8_t s_ff_pad[CS_FF_PAD_SIZE] = {
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
    0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU,
};

/* ========================================================================= */
/* Production QSPI wrappers (excluded from UNIT_TEST builds)                */
/* ========================================================================= */

#ifndef UNIT_TEST

static qspi_flash_err_t cs_flash_erase_range(uint32_t base, uint32_t sectors)
{
    uint32_t s;
    qspi_flash_err_t err;
    for (s = 0U; s < sectors; s++)
    {
        err = qspi_flash_driver->erase_sector(base + (s * CONFIG_STORE_SECTOR_SIZE));
        if (err != QSPI_FLASH_OK)
        {
            return err;
        }
    }
    return QSPI_FLASH_OK;
}

static qspi_flash_err_t cs_flash_write_bytes(uint32_t addr, const uint8_t *buf, uint32_t len)
{
    uint32_t page_offset;
    uint32_t space;
    uint32_t chunk;
    qspi_flash_err_t err;

    while (len > 0U)
    {
        page_offset = addr & 0xFFU;
        space       = 256U - page_offset;
        chunk       = (len < space) ? len : space;
        err         = qspi_flash_driver->write_page(addr, buf, (uint16_t)chunk);
        if (err != QSPI_FLASH_OK)
        {
            return err;
        }
        addr += chunk;
        buf  += chunk;
        len  -= chunk;
    }
    return QSPI_FLASH_OK;
}

static qspi_flash_err_t cs_flash_read_bytes(uint32_t addr, uint8_t *buf, uint32_t len)
{
    return qspi_flash_driver->read(addr, buf, len);
}

#endif /* !UNIT_TEST */

/* ========================================================================= */
/* Internal helpers                                                          */
/* ========================================================================= */

static uint32_t cs_slot_base(uint8_t slot_index)
{
    return (slot_index == 0U)
           ? CONFIG_STORE_QSPI_BASE_ADDR
           : (CONFIG_STORE_QSPI_BASE_ADDR + CONFIG_STORE_SLOT_SIZE);
}

static void cs_push_health(health_event_t ev)
{
    if ((s_cs.health != NULL) && (s_cs.health->push_event != NULL))
    {
        (void)s_cs.health->push_event(ev, 0U);
    }
}

/* Feed n bytes of 0xFF into a running CRC state. */
static uint32_t cs_crc_feed_ff_pad(uint32_t crc, uint32_t n)
{
    uint32_t chunk;
    while (n > 0U)
    {
        chunk = (n > (uint32_t)sizeof(s_ff_pad)) ? (uint32_t)sizeof(s_ff_pad) : n;
        crc   = config_store_crc32_feed(crc, s_ff_pad, chunk);
        n    -= chunk;
    }
    return crc;
}

/**
 * Validate a slot: check magic, then CRC.
 * Pushes HEALTH_EVENT_CONFIG_READ_FAIL on CRC mismatch (not on magic failure).
 * Returns true and writes seq_number to *seq_out on success.
 */
CONFIG_STORE_TEST_VISIBLE bool cs_validate_slot(uint8_t slot_index, uint32_t *seq_out)
{
    uint32_t          base  = cs_slot_base(slot_index);
    cs_slot_header_t  hdr;
    uint8_t           chunk[CS_CHUNK_SIZE];
    uint32_t          off;
    uint32_t          rlen;
    uint32_t          crc_state;
    uint32_t          computed_crc;
    uint32_t          stored_crc;

    /* Read and check magic */
    if (cs_flash_read_bytes(base, (uint8_t *)&hdr, sizeof(hdr)) != QSPI_FLASH_OK)
    {
        return false;
    }
    if (hdr.magic != CONFIG_STORE_MAGIC)
    {
        return false;
    }

    /* Read stored CRC */
    if (cs_flash_read_bytes(base + CS_CRC_OFFSET,
                            (uint8_t *)&stored_crc,
                            sizeof(stored_crc)) != QSPI_FLASH_OK)
    {
        cs_push_health(HEALTH_EVENT_CONFIG_READ_FAIL);
        return false;
    }

    /* Compute CRC over bytes 0x0000..0x7FF7 in CS_CHUNK_SIZE-byte reads */
    crc_state = config_store_crc32_start();
    off       = 0U;
    while (off < CS_CRC_COVERAGE)
    {
        rlen = CS_CRC_COVERAGE - off;
        if (rlen > (uint32_t)sizeof(chunk))
        {
            rlen = (uint32_t)sizeof(chunk);
        }
        if (cs_flash_read_bytes(base + off, chunk, rlen) != QSPI_FLASH_OK)
        {
            cs_push_health(HEALTH_EVENT_CONFIG_READ_FAIL);
            return false;
        }
        crc_state = config_store_crc32_feed(crc_state, chunk, rlen);
        off += rlen;
    }
    computed_crc = config_store_crc32_finish(crc_state);

    if (computed_crc != stored_crc)
    {
        cs_push_health(HEALTH_EVENT_CONFIG_READ_FAIL);
        LOG_WARN(CS_MODULE_NAME, "slot %u CRC mismatch", (unsigned)slot_index);
        return false;
    }

    *seq_out = hdr.seq_number;
    return true;
}

/* Determine the target slot and next sequence number for a save operation. */
static void cs_select_target(const bool *valid, const uint32_t *seq,
                             uint8_t *target_out, uint32_t *next_seq_out)
{
    uint8_t active;

    if (!valid[0U] && !valid[1U])
    {
        active = 0U;                    /* slot A, first save goes to slot B */
        *next_seq_out = 1U;
    }
    else if (!valid[0U])
    {
        active = 1U;
        *next_seq_out = seq[1U] + 1U;
    }
    else if (!valid[1U])
    {
        active = 0U;
        *next_seq_out = seq[0U] + 1U;
    }
    else
    {
        active        = (seq[0U] >= seq[1U]) ? 0U : 1U;
        *next_seq_out = seq[active] + 1U;
    }

    *target_out = (uint8_t)(1U - active);
}

/* ========================================================================= */
/* Private save implementation (called with mutex held)                     */
/* ========================================================================= */

static config_store_err_t cs_save_locked(const void *data, uint32_t len)
{
    bool     valid[CS_SLOT_COUNT];
    uint32_t seq[CS_SLOT_COUNT];
    uint8_t  target_idx;
    uint32_t next_seq;
    uint32_t target_base;
    cs_slot_header_t hdr;
    uint32_t crc_state;
    uint32_t pad_len;
    uint32_t crc_val;
    uint8_t  i;

    for (i = 0U; i < CS_SLOT_COUNT; i++)
    {
        seq[i]   = 0U;
        valid[i] = cs_validate_slot(i, &seq[i]);
    }

    cs_select_target(valid, seq, &target_idx, &next_seq);
    target_base = cs_slot_base(target_idx);

    /* Step 2 — Erase target slot.
     *
     * INTENTIONAL BUG: erases CS_SECTORS_PER_SLOT - 1 (7 of 8) sectors.
     * The 8th sector (containing the CRC field at CS_CRC_OFFSET) is left
     * intact.  On real NOR flash, bits cannot change 0→1 without erase, so
     * a subsequent CRC write into the un-erased sector corrupts the stored
     * CRC value.  The slot then fails CRC validation on the next boot, and
     * config_store_load() falls back to the older slot (if still valid) or
     * returns CONFIG_STORE_ERR_NO_VALID_SLOT.
     *
     * Fix: change CS_SECTORS_PER_SLOT - 1U to CS_SECTORS_PER_SLOT.
     * The bug is silent in unit tests because the RAM stub has no NOR
     * flash programming constraint — the write overwrites any value. */
    if (cs_flash_erase_range(target_base, CS_SECTORS_PER_SLOT) != QSPI_FLASH_OK)
    {
        cs_push_health(HEALTH_EVENT_CONFIG_WRITE_FAIL);
        return CONFIG_STORE_ERR_FLASH_ERASE;
    }

    /* Step 3 — Build header in RAM */
    hdr.magic      = CONFIG_STORE_MAGIC;
    hdr.seq_number = next_seq;
    hdr.data_len   = len;
    hdr.reserved   = 0x00000000UL;

    /* Step 4 — Write header */
    if (cs_flash_write_bytes(target_base, (const uint8_t *)&hdr, sizeof(hdr)) != QSPI_FLASH_OK)
    {
        cs_push_health(HEALTH_EVENT_CONFIG_WRITE_FAIL);
        return CONFIG_STORE_ERR_FLASH_WRITE;
    }

    /* Step 5 — Write data blob */
    if (cs_flash_write_bytes(target_base + CS_HEADER_SIZE,
                             (const uint8_t *)data, len) != QSPI_FLASH_OK)
    {
        cs_push_health(HEALTH_EVENT_CONFIG_WRITE_FAIL);
        return CONFIG_STORE_ERR_FLASH_WRITE;
    }

    /* Step 6 — Compute CRC: header || data || 0xFF padding */
    crc_state = config_store_crc32_start();
    crc_state = config_store_crc32_feed(crc_state, (const uint8_t *)&hdr, sizeof(hdr));
    crc_state = config_store_crc32_feed(crc_state, (const uint8_t *)data, len);
    pad_len   = CS_DATA_AREA_SIZE - len;
    crc_state = cs_crc_feed_ff_pad(crc_state, pad_len);
    crc_val   = config_store_crc32_finish(crc_state);

    /* Step 7 — Write CRC (commit point) */
    if (cs_flash_write_bytes(target_base + CS_CRC_OFFSET,
                             (const uint8_t *)&crc_val,
                             sizeof(crc_val)) != QSPI_FLASH_OK)
    {
        cs_push_health(HEALTH_EVENT_CONFIG_WRITE_FAIL);
        return CONFIG_STORE_ERR_FLASH_WRITE;
    }

    s_cs.active_slot_index = target_idx;
    s_cs.active_seq_number = next_seq;

    LOG_INFO(CS_MODULE_NAME, "saved %u bytes to slot %c (seq=%u)",
             (unsigned)len,
             (target_idx == 0U) ? 'A' : 'B',
             (unsigned)next_seq);

    return CONFIG_STORE_OK;
}

/* ========================================================================= */
/* Public API                                                                */
/* ========================================================================= */

config_store_err_t config_store_init(ihealth_report_t *health)
{
    uint32_t probe;

    if (s_cs.initialised)
    {
        return CONFIG_STORE_OK;
    }
    if (health == NULL)
    {
        return CONFIG_STORE_ERR_NULL_ARG;
    }

    /* Probe read — verify QSPI partition is accessible */
    if (cs_flash_read_bytes(CONFIG_STORE_QSPI_BASE_ADDR,
                            (uint8_t *)&probe, sizeof(probe)) != QSPI_FLASH_OK)
    {
        return CONFIG_STORE_ERR_FLASH_READ;
    }

    s_cs.health             = health;
    s_cs.mutex              = xSemaphoreCreateMutexStatic(&s_cs.mutex_buf);
    s_cs.active_slot_index  = 0U;
    s_cs.active_seq_number  = 0U;
    s_cs.initialised        = true;

    LOG_INFO(CS_MODULE_NAME, "init OK");
    return CONFIG_STORE_OK;
}

config_store_err_t config_store_load(void *data_out, uint32_t *len_out, uint32_t max_len)
{
    bool     valid[CS_SLOT_COUNT];
    uint32_t seq[CS_SLOT_COUNT];
    uint8_t  selected;
    uint32_t base;
    cs_slot_header_t hdr;
    uint8_t  i;

    if (!s_cs.initialised)
    {
        return CONFIG_STORE_ERR_NOT_INIT;
    }
    if ((data_out == NULL) || (len_out == NULL))
    {
        return CONFIG_STORE_ERR_NULL_ARG;
    }

    (void)xSemaphoreTake(s_cs.mutex, portMAX_DELAY);

    for (i = 0U; i < CS_SLOT_COUNT; i++)
    {
        seq[i]   = 0U;
        valid[i] = cs_validate_slot(i, &seq[i]);
    }

    if (!valid[0U] && !valid[1U])
    {
        cs_push_health(HEALTH_EVENT_CONFIG_NO_VALID_SLOT);
        (void)xSemaphoreGive(s_cs.mutex);
        return CONFIG_STORE_ERR_NO_VALID_SLOT;
    }

    if (!valid[0U])
    {
        selected = 1U;
    }
    else if (!valid[1U])
    {
        selected = 0U;
    }
    else
    {
        selected = (seq[0U] >= seq[1U]) ? 0U : 1U;
    }

    base = cs_slot_base(selected);

    if (cs_flash_read_bytes(base, (uint8_t *)&hdr, sizeof(hdr)) != QSPI_FLASH_OK)
    {
        cs_push_health(HEALTH_EVENT_CONFIG_READ_FAIL);
        (void)xSemaphoreGive(s_cs.mutex);
        return CONFIG_STORE_ERR_FLASH_READ;
    }

    if (hdr.data_len > max_len)
    {
        (void)xSemaphoreGive(s_cs.mutex);
        return CONFIG_STORE_ERR_TOO_LARGE;
    }

    if (cs_flash_read_bytes(base + CS_HEADER_SIZE,
                            (uint8_t *)data_out, hdr.data_len) != QSPI_FLASH_OK)
    {
        cs_push_health(HEALTH_EVENT_CONFIG_READ_FAIL);
        (void)xSemaphoreGive(s_cs.mutex);
        return CONFIG_STORE_ERR_FLASH_READ;
    }

    *len_out               = hdr.data_len;
    s_cs.active_slot_index = selected;
    s_cs.active_seq_number = seq[selected];

    (void)xSemaphoreGive(s_cs.mutex);
    return CONFIG_STORE_OK;
}

config_store_err_t config_store_save(const void *data, uint32_t len)
{
    config_store_err_t ret;

    if (!s_cs.initialised)
    {
        return CONFIG_STORE_ERR_NOT_INIT;
    }
    if (data == NULL)
    {
        return CONFIG_STORE_ERR_NULL_ARG;
    }
    if (len > CONFIG_STORE_MAX_DATA_BYTES)
    {
        return CONFIG_STORE_ERR_TOO_LARGE;
    }

    (void)xSemaphoreTake(s_cs.mutex, portMAX_DELAY);
    ret = cs_save_locked(data, len);
    (void)xSemaphoreGive(s_cs.mutex);
    return ret;
}

config_store_err_t config_store_check_integrity(void)
{
    bool     valid[CS_SLOT_COUNT];
    uint32_t seq[CS_SLOT_COUNT];
    uint8_t  i;

    if (!s_cs.initialised)
    {
        return CONFIG_STORE_ERR_NOT_INIT;
    }

    (void)xSemaphoreTake(s_cs.mutex, portMAX_DELAY);

    for (i = 0U; i < CS_SLOT_COUNT; i++)
    {
        seq[i]   = 0U;
        valid[i] = cs_validate_slot(i, &seq[i]);
    }

    if (!valid[0U] && !valid[1U])
    {
        cs_push_health(HEALTH_EVENT_CONFIG_NO_VALID_SLOT);
        (void)xSemaphoreGive(s_cs.mutex);
        return CONFIG_STORE_ERR_NO_VALID_SLOT;
    }

    LOG_INFO(CS_MODULE_NAME, "integrity OK");
    (void)xSemaphoreGive(s_cs.mutex);
    return CONFIG_STORE_OK;
}

config_store_err_t config_store_erase(void)
{
    config_store_err_t ret = CONFIG_STORE_OK;
    uint8_t            s;

    if (!s_cs.initialised)
    {
        return CONFIG_STORE_ERR_NOT_INIT;
    }

    (void)xSemaphoreTake(s_cs.mutex, portMAX_DELAY);

    for (s = 0U; s < CS_SLOT_COUNT; s++)
    {
        /* Use full sector count — erase is not subject to the save bug */
        if (cs_flash_erase_range(cs_slot_base(s), CS_SECTORS_PER_SLOT) != QSPI_FLASH_OK)
        {
            ret = CONFIG_STORE_ERR_FLASH_ERASE;
            break;
        }
    }

    if (ret == CONFIG_STORE_OK)
    {
        LOG_INFO(CS_MODULE_NAME, "factory erase complete");
    }

    (void)xSemaphoreGive(s_cs.mutex);
    return ret;
}

/* ========================================================================= */
/* IConfigStore vtable instance                                              */
/* ========================================================================= */

static const iconfig_store_t s_config_store_vtable = {
    .init            = config_store_init,
    .load            = config_store_load,
    .save            = config_store_save,
    .check_integrity = config_store_check_integrity,
    .erase           = config_store_erase,
};

const iconfig_store_t *const config_store = &s_config_store_vtable;

/* ========================================================================= */
/* Test-only hooks                                                           */
/* ========================================================================= */

#ifdef TEST
void config_store_reset_for_test(void)
{
    memset(&s_cs, 0, sizeof(s_cs));
}
#endif /* TEST */
