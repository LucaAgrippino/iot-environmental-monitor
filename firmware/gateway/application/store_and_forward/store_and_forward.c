/**
 * @file store_and_forward.c
 * @brief StoreAndForward implementation (Gateway application layer).
 *
 * See docs/lld/application/store-and-forward-lld.md. Everything here is policy
 * layered over CircularFlashLog: serialisation of topic + QoS + payload, the
 * drop-oldest loop, two-phase dequeue/confirm and occupancy reporting.
 */

#include "store_and_forward.h"

#include "health_monitor/health_monitor.h"

#include <stddef.h>
#include <string.h>

#include "logger/logger.h"

#define SAF_LOG_MODULE "SAF"

#define SAF_MAX_INSTANCES (1U)
#define SAF_STAGE_BUF_SIZE (CFL_MAX_RECORD_BYTES)

/** Serialised-blob layout (companion §4). */
#define SAF_BLOB_TOPIC_LEN_SZ (1U)
#define SAF_BLOB_QOS_SZ (1U)
#define SAF_BLOB_PAYLOAD_LEN_SZ (2U)
#define SAF_BLOB_MIN_SZ                                                                            \
    (SAF_BLOB_TOPIC_LEN_SZ + 1U + SAF_BLOB_QOS_SZ + SAF_BLOB_PAYLOAD_LEN_SZ + 1U)

#define SAF_BYTE_SHIFT (8U)
#define SAF_BYTE_MASK (0xFFU)

/** @brief Internal instance state — hidden from consumers. */
struct saf_inst
{
    /* Injected dependencies */
    cfl_handle_t cfl;
    health_monitor_handle_t health;

    /* Configuration and tracking */
    uint32_t capacity;
    uint32_t entry_count;

    /* Two-phase drain state */
    bool has_pending;
    saf_entry_t pending_hdr;
    uint8_t pending_payload[SAF_PAYLOAD_MAX];

    /* Serialisation / deserialisation scratch */
    uint8_t stage_buf[SAF_STAGE_BUF_SIZE];

    bool in_use;
};

static struct saf_inst g_pool[SAF_MAX_INSTANCES];
static uint8_t g_count;

/* ====================================================================== */
/* Helpers                                                                */
/* ====================================================================== */

static saf_err_t saf_check_handle(saf_handle_t handle)
{
    if (handle == NULL)
    {
        return SAF_ERR_NULL_PTR;
    }

    if (!handle->in_use)
    {
        return SAF_ERR_NOT_INIT;
    }

    return SAF_ERR_OK;
}

/**
 * @brief Push the current entry count to HealthMonitor.
 *
 * A NULL health handle is allowed (bring-up harnesses): the push is then skipped. The capacity is
 * a planning figure kept for saf_get_occupancy(); the health snapshot has no field for it.
 */
static void saf_report_occupancy(const struct saf_inst *inst)
{
    if (inst->health == NULL)
    {
        return;
    }

    (void) health_monitor_update_buffer_occupancy(inst->health, inst->entry_count);
}

/** @brief Decrement the entry count without wrapping below zero. */
static void saf_entry_removed(struct saf_inst *inst)
{
    if (inst->entry_count > 0U)
    {
        inst->entry_count--;
    }
}

/**
 * @brief Serialise an entry into the stage buffer.
 *
 * @return Blob length in bytes.
 */
static uint32_t saf_serialise(struct saf_inst *inst, const char *topic, uint8_t topic_len,
                              const uint8_t *buf, uint16_t len, uint8_t qos)
{
    uint8_t *out = inst->stage_buf;
    uint32_t pos = 0U;

    out[pos] = topic_len;
    pos += SAF_BLOB_TOPIC_LEN_SZ;

    (void) memcpy(&out[pos], topic, topic_len);
    pos += topic_len;

    out[pos] = qos;
    pos += SAF_BLOB_QOS_SZ;

    out[pos] = (uint8_t) (len & SAF_BYTE_MASK);
    out[pos + 1U] = (uint8_t) ((len >> SAF_BYTE_SHIFT) & SAF_BYTE_MASK);
    pos += SAF_BLOB_PAYLOAD_LEN_SZ;

    (void) memcpy(&out[pos], buf, len);
    pos += len;

    return pos;
}

/**
 * @brief Parse the blob in the stage buffer into the pending entry.
 *
 * Every length is checked against the blob size, so a record that passed its
 * CRC but carries nonsense is rejected rather than read out of bounds.
 *
 * @return true when the blob was well formed.
 */
static bool saf_deserialise(struct saf_inst *inst, uint32_t raw_len)
{
    const uint8_t *in = inst->stage_buf;
    uint32_t pos = 0U;
    uint32_t topic_len;
    uint32_t payload_len;

    if (raw_len < SAF_BLOB_MIN_SZ)
    {
        return false;
    }

    topic_len = in[pos];
    pos += SAF_BLOB_TOPIC_LEN_SZ;

    if ((topic_len == 0U) || (topic_len > SAF_TOPIC_MAX) ||
        ((pos + topic_len + SAF_BLOB_QOS_SZ + SAF_BLOB_PAYLOAD_LEN_SZ) > raw_len))
    {
        return false;
    }

    (void) memcpy(inst->pending_hdr.topic, &in[pos], topic_len);
    inst->pending_hdr.topic[topic_len] = '\0';
    pos += topic_len;

    inst->pending_hdr.qos = in[pos];
    pos += SAF_BLOB_QOS_SZ;

    payload_len = (uint32_t) in[pos] | ((uint32_t) in[pos + 1U] << SAF_BYTE_SHIFT);
    pos += SAF_BLOB_PAYLOAD_LEN_SZ;

    if ((payload_len == 0U) || (payload_len > SAF_PAYLOAD_MAX) || ((pos + payload_len) != raw_len))
    {
        return false;
    }

    (void) memcpy(inst->pending_payload, &in[pos], payload_len);
    inst->pending_hdr.payload_len = (uint16_t) payload_len;
    inst->pending_hdr.payload = inst->pending_payload;

    return true;
}

/* ====================================================================== */
/* Public API                                                             */
/* ====================================================================== */

saf_err_t saf_create(const saf_config_t *config, saf_handle_t *handle)
{
    if ((config == NULL) || (handle == NULL) || (config->cfl == NULL))
    {
        return SAF_ERR_NULL_PTR;
    }

    if (g_count >= SAF_MAX_INSTANCES)
    {
        return SAF_ERR_NO_RESOURCE;
    }

    struct saf_inst *inst = &g_pool[g_count];
    uint32_t entries = 0U;
    uint32_t bytes = 0U;

    /* CircularFlashLog rejects a NULL byte_count, so the payload byte total
     * it also reports is collected and discarded. */
    if (cfl_get_occupancy(config->cfl, &entries, &bytes) != CFL_ERR_OK)
    {
        LOG_ERROR(SAF_LOG_MODULE, "cfl_get_occupancy failed at create");
        return SAF_ERR_FLASH;
    }

    (void) memset(inst, 0, sizeof(*inst));
    inst->cfl = config->cfl;
    inst->health = config->health;
    inst->capacity = config->capacity;
    inst->entry_count = entries;
    inst->has_pending = false;
    inst->in_use = true;
    g_count++;
    *handle = inst;

    saf_report_occupancy(inst);
    LOG_INFO(SAF_LOG_MODULE, "%lu / %lu entries at boot", (unsigned long) inst->entry_count,
             (unsigned long) inst->capacity);

    return SAF_ERR_OK;
}

saf_err_t saf_enqueue(saf_handle_t handle, const char *topic, const uint8_t *buf, uint16_t len,
                      uint8_t qos)
{
    saf_err_t err = saf_check_handle(handle);

    if (err != SAF_ERR_OK)
    {
        return err;
    }

    if ((topic == NULL) || (buf == NULL))
    {
        return SAF_ERR_NULL_PTR;
    }

    size_t topic_len = strnlen(topic, SAF_TOPIC_MAX + 1U);

    if ((len == 0U) || (len > SAF_PAYLOAD_MAX) || (topic_len == 0U) || (topic_len > SAF_TOPIC_MAX))
    {
        LOG_WARN(SAF_LOG_MODULE, "entry rejected: topic %lu B, payload %u B",
                 (unsigned long) topic_len, (unsigned) len);
        return SAF_ERR_TOO_LARGE;
    }

    uint32_t raw_len = saf_serialise(handle, topic, (uint8_t) topic_len, buf, len, qos);
    cfl_err_t rc = cfl_append(handle->cfl, handle->stage_buf, raw_len);

    /* REQ-BF-020: drop the oldest entries until the append fits. */
    while (rc == CFL_ERR_FULL)
    {
        cfl_err_t drop_rc = cfl_drop_oldest(handle->cfl);

        if (drop_rc == CFL_ERR_EMPTY)
        {
            break;
        }

        if (drop_rc != CFL_ERR_OK)
        {
            LOG_ERROR(SAF_LOG_MODULE, "drop-oldest failed (%d)", (int) drop_rc);
            return SAF_ERR_FLASH;
        }

        /* The oldest entry is gone; if it was the one a caller is holding,
         * a later confirm would otherwise consume the record after it. */
        handle->has_pending = false;
        saf_entry_removed(handle);
        LOG_WARN(SAF_LOG_MODULE, "buffer full - oldest entry dropped");

        rc = cfl_append(handle->cfl, handle->stage_buf, raw_len);
    }

    if (rc != CFL_ERR_OK)
    {
        LOG_ERROR(SAF_LOG_MODULE, "flash append failed (%d)", (int) rc);
        return SAF_ERR_FLASH;
    }

    handle->entry_count++;
    saf_report_occupancy(handle);

    return SAF_ERR_OK;
}

saf_err_t saf_dequeue(saf_handle_t handle, const saf_entry_t **out)
{
    saf_err_t err = saf_check_handle(handle);

    if (err != SAF_ERR_OK)
    {
        return err;
    }

    if (out == NULL)
    {
        return SAF_ERR_NULL_PTR;
    }

    if (handle->has_pending)
    {
        *out = &handle->pending_hdr;
        return SAF_ERR_OK;
    }

    for (;;)
    {
        uint32_t raw_len = 0U;
        cfl_err_t rc =
            cfl_peek_oldest(handle->cfl, handle->stage_buf, &raw_len, SAF_STAGE_BUF_SIZE);

        if (rc == CFL_ERR_EMPTY)
        {
            return SAF_ERR_EMPTY;
        }

        if (rc == CFL_ERR_OK)
        {
            if (saf_deserialise(handle, raw_len))
            {
                break;
            }

            LOG_WARN(SAF_LOG_MODULE, "skipped malformed record");
        }
        else if (rc == CFL_ERR_CORRUPT)
        {
            LOG_WARN(SAF_LOG_MODULE, "skipped corrupt CFL record");
        }
        else
        {
            LOG_ERROR(SAF_LOG_MODULE, "flash peek failed (%d)", (int) rc);
            return SAF_ERR_FLASH;
        }

        /* Skip the unusable record. A failed consume must stop the loop, or
         * the same record would be peeked forever. */
        if (cfl_consume_oldest(handle->cfl) != CFL_ERR_OK)
        {
            LOG_ERROR(SAF_LOG_MODULE, "could not consume unusable record");
            return SAF_ERR_FLASH;
        }

        saf_entry_removed(handle);
    }

    handle->has_pending = true;
    *out = &handle->pending_hdr;

    return SAF_ERR_OK;
}

saf_err_t saf_confirm(saf_handle_t handle)
{
    saf_err_t err = saf_check_handle(handle);

    if (err != SAF_ERR_OK)
    {
        return err;
    }

    if (!handle->has_pending)
    {
        return SAF_ERR_OK;
    }

    if (cfl_consume_oldest(handle->cfl) != CFL_ERR_OK)
    {
        LOG_ERROR(SAF_LOG_MODULE, "flash consume failed");
        return SAF_ERR_FLASH;
    }

    handle->has_pending = false;
    saf_entry_removed(handle);
    saf_report_occupancy(handle);

    return SAF_ERR_OK;
}

saf_err_t saf_is_empty(saf_handle_t handle, bool *empty_out)
{
    saf_err_t err = saf_check_handle(handle);

    if (err != SAF_ERR_OK)
    {
        return err;
    }

    if (empty_out == NULL)
    {
        return SAF_ERR_NULL_PTR;
    }

    *empty_out = (handle->entry_count == 0U);

    return SAF_ERR_OK;
}

saf_err_t saf_get_occupancy(saf_handle_t handle, uint32_t *entry_count, uint32_t *capacity)
{
    saf_err_t err = saf_check_handle(handle);

    if (err != SAF_ERR_OK)
    {
        return err;
    }

    if ((entry_count == NULL) || (capacity == NULL))
    {
        return SAF_ERR_NULL_PTR;
    }

    *entry_count = handle->entry_count;
    *capacity = handle->capacity;

    return SAF_ERR_OK;
}

#ifdef TEST
void saf_reset_for_test(void)
{
    (void) memset(g_pool, 0, sizeof(g_pool));
    g_count = 0U;
}
#endif
