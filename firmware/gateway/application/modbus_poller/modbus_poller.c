/**
 * @file modbus_poller.c
 * @brief ModbusPoller implementation (Gateway).
 *
 * Machine 4 model variables (consecutive_successes / consecutive_failures /
 * link_online) live in the slave state table and are updated by
 * record_poll_outcome() (companion §5.3).
 *
 * @see docs/lld/application/modbus-poller-lld.md
 */

#include "modbus_poller.h"

#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "device_profile_registry/device_profile_registry.h"
#include "modbus_frame.h"

#ifdef TEST
#define LOG_INFO(m, f, ...) ((void) 0)
#define LOG_WARN(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

#define MODULE_TAG "MBP"

#define MODBUS_MAX_SLAVES (4U)      /* max provisioned slaves */
#define MODBUS_CMD_QUEUE_DEPTH (4U) /* provisional; confirm at integration (MBP-O1) */
#define FC_READ_INPUT_REGS (0x04U)

#define HYSTERESIS_THRESHOLD (3U) /* consecutive results before a link transition */
#define BITS_PER_BYTE (8U)
#define BYTE_MASK (0xFFU)
#define BYTES_PER_REG (2U)
#define FC06_DATA_BYTES (4U)         /* reg(2) + value(2) */
#define FC16_HEADER_BYTES (5U)       /* reg(2) + count(2) + byte count(1) */
#define POLL_REQUEST_DATA_BYTES (4U) /* start(2) + count(2) */

/* Field Device sensor registers (docs/hld/modbus-register-map.md 6.2) and their invalid sentinels.
 */
#define FD_REG_TEMPERATURE (0x0010U)
#define FD_REG_HUMIDITY (0x0011U)
#define FD_REG_PRESSURE (0x0012U)
#define FD_SENTINEL_INT16 (0x8000U)
#define FD_SENTINEL_UINT16 (0xFFFFU)
#define FC04_BYTE_COUNT_BYTES (1U) /* the byte count that precedes the register words */

/* ========================================================================= */
/* Module state                                                              */
/* ========================================================================= */

static struct
{
    uint32_t poll_period_ms;
    health_monitor_handle_t health;
    bool initialised;
} s_poller;

static modbus_slave_state_t s_slave_states[MODBUS_MAX_SLAVES];

/** Decoded readings, one per slave slot (same index as s_slave_states); no heap (CP-D18). */
typedef struct
{
    int16_t temperature_centi_c;
    uint16_t humidity_centi_pct;
    uint16_t pressure_deci_hpa;
    bool temperature_valid;
    bool humidity_valid;
    bool pressure_valid;
    bool has_data;
    uint32_t updated_tick;
} fd_cache_t;

static fd_cache_t s_fd_cache[MODBUS_MAX_SLAVES];

/* Static queue storage — no heap allocation after init (BARR-C, REQ-NF-201). */
static QueueHandle_t s_cmd_queue;
static StaticQueue_t s_cmd_queue_storage;
static uint8_t s_cmd_queue_buf[MODBUS_CMD_QUEUE_DEPTH * sizeof(modbus_command_t)];

/* ========================================================================= */
/* Helpers                                                                   */
/* ========================================================================= */

/** Find the state slot of a slave, claiming a free one on first sight. */
static modbus_slave_state_t *find_or_claim_slot(uint8_t slave_addr)
{
    modbus_slave_state_t *free_slot = NULL;

    for (uint8_t i = 0U; i < MODBUS_MAX_SLAVES; i++)
    {
        if (s_slave_states[i].slave_addr == slave_addr)
        {
            return &s_slave_states[i];
        }
        if ((free_slot == NULL) && (s_slave_states[i].slave_addr == 0U))
        {
            free_slot = &s_slave_states[i];
        }
    }

    if (free_slot != NULL)
    {
        free_slot->slave_addr = slave_addr;
    }
    return free_slot;
}

static void emit_link_up(uint8_t slave_addr)
{
    (void) health_monitor_push_event(s_poller.health, HEALTH_EVENT_MODBUS_LINK_UP, slave_addr);
    LOG_INFO(MODULE_TAG, "slave %u link up", (unsigned) slave_addr);
}

static void emit_node_offline(uint8_t slave_addr)
{
    (void) health_monitor_push_event(s_poller.health, HEALTH_EVENT_MODBUS_LINK_DOWN, slave_addr);
    LOG_WARN(MODULE_TAG, "slave %u node offline", (unsigned) slave_addr);
}

/** Update the hysteresis counters and emit a link event on a transition (§5.3). */
static void record_poll_outcome(modbus_slave_state_t *slave, modbus_master_err_t result)
{
    if (result == MODBUS_MASTER_ERR_OK)
    {
        slave->consecutive_failures = 0U;
        if (slave->consecutive_successes < HYSTERESIS_THRESHOLD)
        {
            slave->consecutive_successes++;
        }
        if ((slave->consecutive_successes == HYSTERESIS_THRESHOLD) && !slave->link_online)
        {
            slave->link_online = true;
            emit_link_up(slave->slave_addr);
        }
    }
    else
    {
        slave->consecutive_successes = 0U;
        if (slave->consecutive_failures < HYSTERESIS_THRESHOLD)
        {
            slave->consecutive_failures++;
        }
        if ((slave->consecutive_failures == HYSTERESIS_THRESHOLD) && slave->link_online)
        {
            slave->link_online = false;
            emit_node_offline(slave->slave_addr);
        }
    }
}

/** Build an FC04 request from the profile's register span (§5.7). */
static void build_poll_request(const device_profile_t *profile, modbus_frame_t *out)
{
    out->slave_addr = profile->slave_addr;
    out->function_code = FC_READ_INPUT_REGS;
    out->data[0] = (uint8_t) (profile->input_reg_start >> BITS_PER_BYTE);
    out->data[1] = (uint8_t) (profile->input_reg_start & BYTE_MASK);
    out->data[2] = (uint8_t) (profile->input_reg_count >> BITS_PER_BYTE);
    out->data[3] = (uint8_t) (profile->input_reg_count & BYTE_MASK);
    out->data_len = POLL_REQUEST_DATA_BYTES;
}

/** Index of a slave's slot, or MODBUS_MAX_SLAVES if it has none yet. */
static uint8_t find_slot_index(uint8_t slave_addr)
{
    for (uint8_t i = 0U; i < MODBUS_MAX_SLAVES; i++)
    {
        if (s_slave_states[i].slave_addr == slave_addr)
        {
            return i;
        }
    }

    return MODBUS_MAX_SLAVES;
}

/** Big-endian word of one input register of an FC04 response, or false if the span lacks it. */
static bool read_register_word(const device_profile_t *profile, const modbus_frame_t *response,
                               uint16_t reg, uint16_t *word_out)
{
    if ((reg < profile->input_reg_start) ||
        (reg >= (uint16_t) (profile->input_reg_start + profile->input_reg_count)))
    {
        return false;
    }

    const uint16_t at = (uint16_t) (FC04_BYTE_COUNT_BYTES +
                                    ((uint16_t) (reg - profile->input_reg_start) * BYTES_PER_REG));

    *word_out =
        (uint16_t) (((uint16_t) response->data[at] << BITS_PER_BYTE) | response->data[at + 1U]);
    return true;
}

/** Decode the Field Device sensor registers of a poll response into the cache (MBP-O3, CP-D18). */
static void forward_response_to_consumers(const device_profile_t *profile,
                                          const modbus_frame_t *response)
{
    const uint16_t expected =
        (uint16_t) (FC04_BYTE_COUNT_BYTES + ((uint16_t) profile->input_reg_count * BYTES_PER_REG));
    const uint8_t slot = find_slot_index(profile->slave_addr);

    if (slot >= MODBUS_MAX_SLAVES)
    {
        return;
    }

    if (response->data_len != expected)
    {
        LOG_WARN(MODULE_TAG, "slave %u: response length %u, expected %u",
                 (unsigned) profile->slave_addr, (unsigned) response->data_len,
                 (unsigned) expected);
        return;
    }

    fd_cache_t next = {0};
    uint16_t word = 0U;

    if (read_register_word(profile, response, FD_REG_TEMPERATURE, &word))
    {
        next.temperature_valid = (word != FD_SENTINEL_INT16);
        next.temperature_centi_c = (int16_t) word;
    }
    if (read_register_word(profile, response, FD_REG_HUMIDITY, &word))
    {
        next.humidity_valid = (word != FD_SENTINEL_UINT16);
        next.humidity_centi_pct = word;
    }
    if (read_register_word(profile, response, FD_REG_PRESSURE, &word))
    {
        next.pressure_valid = (word != FD_SENTINEL_UINT16);
        next.pressure_deci_hpa = word;
    }
    next.has_data = true;
    next.updated_tick = (uint32_t) xTaskGetTickCount();

    taskENTER_CRITICAL();
    s_fd_cache[slot] = next;
    taskEXIT_CRITICAL();
}

/** Build the wire request of a command (§5.5). */
static void build_command_request(const modbus_command_t *cmd, modbus_frame_t *request)
{
    request->slave_addr = cmd->slave_addr;
    request->function_code = cmd->function_code;
    request->data[0] = (uint8_t) (cmd->start_reg >> BITS_PER_BYTE);
    request->data[1] = (uint8_t) (cmd->start_reg & BYTE_MASK);

    if (cmd->function_code == MODBUS_POLLER_FC_WRITE_SINGLE)
    {
        /* FC06: [reg_hi][reg_lo][val_hi][val_lo] */
        request->data[2] = (uint8_t) (cmd->data[0] >> BITS_PER_BYTE);
        request->data[3] = (uint8_t) (cmd->data[0] & BYTE_MASK);
        request->data_len = FC06_DATA_BYTES;
    }
    else
    {
        /* FC16: [reg_hi][reg_lo][count_hi][count_lo][byte_count][val0_hi][val0_lo]... */
        request->data[2] = (uint8_t) (cmd->reg_count >> BITS_PER_BYTE);
        request->data[3] = (uint8_t) (cmd->reg_count & BYTE_MASK);
        request->data[4] = (uint8_t) (cmd->reg_count * BYTES_PER_REG);
        for (uint16_t i = 0U; i < cmd->reg_count; i++)
        {
            const uint16_t at = (uint16_t) (FC16_HEADER_BYTES + (i * BYTES_PER_REG));

            request->data[at] = (uint8_t) (cmd->data[i] >> BITS_PER_BYTE);
            request->data[at + 1U] = (uint8_t) (cmd->data[i] & BYTE_MASK);
        }
        request->data_len = (uint16_t) (FC16_HEADER_BYTES + (cmd->reg_count * BYTES_PER_REG));
    }
}

/** True if a dequeued command can be encoded without overrunning the frame (MBP-D6). */
static bool command_is_encodable(const modbus_command_t *cmd)
{
    if (cmd->function_code == MODBUS_POLLER_FC_WRITE_SINGLE)
    {
        return true;
    }
    return (cmd->function_code == MODBUS_POLLER_FC_WRITE_MULTIPLE) && (cmd->reg_count != 0U) &&
           (cmd->reg_count <= MODBUS_POLLER_FC16_MAX_COUNT);
}

/*
 * execute_command() is split in three so the 256-byte request, response and result
 * buffers are never all on the stack at once: the task has a 2 KB budget and
 * modbus_master_transact() needs about 680 B of it.  noinline keeps the compiler
 * from merging the frames back together.
 */

/** Encode and send one command; the reply lands in *response. */
__attribute__((noinline)) static modbus_master_err_t transact_command(const modbus_command_t *cmd,
                                                                      modbus_frame_t *response)
{
    modbus_frame_t request = {0};

    /* send_command() already checked this; the queue is the trust boundary, so check again. */
    if (!command_is_encodable(cmd))
    {
        return MODBUS_MASTER_ERR_INVALID_ARG;
    }

    build_command_request(cmd, &request);

    /* Blocking; includes ModbusMaster's retry loop. */
    return modbus_master_transact(&request, response);
}

/** Decode the reply into register words and post it to the caller's queue. */
__attribute__((noinline)) static void post_command_result(const modbus_command_t *cmd,
                                                          modbus_master_err_t status,
                                                          const modbus_frame_t *response)
{
    modbus_command_response_t result = {.status = status, .data_len = 0U};

    if (status == MODBUS_MASTER_ERR_OK)
    {
        uint16_t words = (uint16_t) (response->data_len / BYTES_PER_REG);

        if (words > MODBUS_POLLER_CMD_MAX_REGS)
        {
            words = MODBUS_POLLER_CMD_MAX_REGS;
        }
        for (uint16_t i = 0U; i < words; i++)
        {
            result.data[i] =
                (uint16_t) (((uint16_t) response->data[i * BYTES_PER_REG] << BITS_PER_BYTE) |
                            (uint16_t) response->data[(i * BYTES_PER_REG) + 1U]);
        }
        result.data_len = words;
    }

    /* Best effort: a caller whose queue is full or absent just misses the answer. */
    if (cmd->response_queue != NULL)
    {
        (void) xQueueSend(cmd->response_queue, &result, 0);
    }
}

/** Run one queued command and post the result to its caller (§5.5). */
static void execute_command(const modbus_command_t *cmd)
{
    modbus_frame_t response = {0};
    const modbus_master_err_t status = transact_command(cmd, &response);

    post_command_result(cmd, status, &response);
}

/** Push the Modbus counters to HealthMonitor (§5.6, changed by MBP-D6). */
static void report_health_metrics(void)
{
    modbus_master_stats_t stats;

    if (modbus_master_get_stats(&stats) == MODBUS_MASTER_ERR_OK)
    {
        (void) health_monitor_update_modbus_master_stats(s_poller.health, &stats);
    }
}

/** Run every queued command (§5.2, step 1).  Its own frame, so it is off the stack during polls. */
static void drain_commands(void)
{
    modbus_command_t cmd;

    while (xQueueReceive(s_cmd_queue, &cmd, 0) == pdTRUE)
    {
        execute_command(&cmd);
    }
}

/** Poll every provisioned slave once (§5.2, step 2). */
static void poll_slaves(void)
{
    const device_profile_t *profile = NULL;
    uint8_t index = 0U;

    while (device_profile_get_next(&profile, &index))
    {
        modbus_slave_state_t *slave = find_or_claim_slot(profile->slave_addr);
        modbus_frame_t request = {0};
        modbus_frame_t response = {0};

        if (slave == NULL)
        {
            LOG_WARN(MODULE_TAG, "slave table full, skipping %u", (unsigned) profile->slave_addr);
            continue;
        }

        build_poll_request(profile, &request);

        const modbus_master_err_t result = modbus_master_transact(&request, &response);

        record_poll_outcome(slave, result);
        if (result == MODBUS_MASTER_ERR_OK)
        {
            forward_response_to_consumers(profile, &response);
        }
    }
}

/** One drain / poll / report cycle (§5.2, steps 1..3). */
static void poll_cycle(void)
{
    drain_commands(); /* commands first (MBP-D4) */
    poll_slaves();
    report_health_metrics(); /* every cycle (MBP-D5) */
}

/* ========================================================================= */
/* Public API                                                                */
modbus_poller_err_t modbus_poller_get_fd_reading(uint8_t slave_addr, modbus_fd_reading_t *out)
{
    if (!s_poller.initialised)
    {
        return MODBUS_POLLER_ERR_NOT_INIT;
    }
    if (out == NULL)
    {
        return MODBUS_POLLER_ERR_NULL_ARG;
    }
    if (!device_profile_is_registered(slave_addr))
    {
        return MODBUS_POLLER_ERR_REJECTED;
    }

    modbus_fd_reading_t reading = {0}; /* a slave not polled yet: no data, every channel invalid */
    const uint8_t slot = find_slot_index(slave_addr);

    if (slot < MODBUS_MAX_SLAVES)
    {
        taskENTER_CRITICAL();
        const fd_cache_t cached = s_fd_cache[slot];
        const bool online = s_slave_states[slot].link_online;
        taskEXIT_CRITICAL();

        reading.temperature_centi_c = cached.temperature_centi_c;
        reading.humidity_centi_pct = cached.humidity_centi_pct;
        reading.pressure_deci_hpa = cached.pressure_deci_hpa;
        reading.temperature_valid = cached.temperature_valid;
        reading.humidity_valid = cached.humidity_valid;
        reading.pressure_valid = cached.pressure_valid;
        reading.has_data = cached.has_data;
        reading.updated_tick = cached.updated_tick;
        reading.link_online = online;
    }

    *out = reading;
    return MODBUS_POLLER_ERR_OK;
}

/* ========================================================================= */

modbus_poller_err_t modbus_poller_init(uint32_t poll_period_ms, health_monitor_handle_t health)
{
    if (health == NULL)
    {
        return MODBUS_POLLER_ERR_NULL_ARG;
    }
    if (poll_period_ms == 0U)
    {
        return MODBUS_POLLER_ERR_INVALID_ARG;
    }

    s_cmd_queue = xQueueCreateStatic(MODBUS_CMD_QUEUE_DEPTH, sizeof(modbus_command_t),
                                     s_cmd_queue_buf, &s_cmd_queue_storage);
    configASSERT(s_cmd_queue != NULL);

    (void) memset(s_slave_states, 0, sizeof(s_slave_states));
    (void) memset(s_fd_cache, 0, sizeof(s_fd_cache));
    s_poller.poll_period_ms = poll_period_ms;
    s_poller.health = health;
    s_poller.initialised = true;
    return MODBUS_POLLER_ERR_OK;
}

bool modbus_poller_is_ready(void)
{
    return s_poller.initialised;
}

modbus_poller_err_t modbus_poller_send_command(const modbus_command_t *cmd)
{
    if (!s_poller.initialised)
    {
        return MODBUS_POLLER_ERR_NOT_INIT;
    }
    if (cmd == NULL)
    {
        return MODBUS_POLLER_ERR_NULL_ARG;
    }

    const bool single = (cmd->function_code == MODBUS_POLLER_FC_WRITE_SINGLE);
    const bool multiple = (cmd->function_code == MODBUS_POLLER_FC_WRITE_MULTIPLE);

    if (!single && !multiple)
    {
        return MODBUS_POLLER_ERR_INVALID_ARG;
    }
    if (multiple && ((cmd->reg_count == 0U) || (cmd->reg_count > MODBUS_POLLER_FC16_MAX_COUNT)))
    {
        return MODBUS_POLLER_ERR_INVALID_ARG;
    }
    if (!device_profile_is_registered(cmd->slave_addr))
    {
        return MODBUS_POLLER_ERR_REJECTED;
    }
    if (xQueueSend(s_cmd_queue, cmd, 0) != pdTRUE)
    {
        return MODBUS_POLLER_ERR_QUEUE_FULL;
    }
    return MODBUS_POLLER_ERR_OK;
}

void modbus_poller_run(void)
{
    /* Phase 2 of ModbusMaster's init: the task handle must be ours (MBP-D3). */
    const modbus_master_err_t start_err = modbus_master_start();

    configASSERT(start_err == MODBUS_MASTER_ERR_OK);
    configASSERT(s_poller.initialised);
    (void) start_err;

    TickType_t last_wake = xTaskGetTickCount();

    for (;;)
    {
        poll_cycle();

        /* MBP-D2: vTaskDelayUntil, not a timer, so no notification-bit collision. */
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(s_poller.poll_period_ms));
    }
}

#ifdef TEST
void modbus_poller_reset_for_test(void)
{
    if (s_cmd_queue != NULL)
    {
        vQueueDelete(s_cmd_queue);
        s_cmd_queue = NULL;
    }
    (void) memset(s_slave_states, 0, sizeof(s_slave_states));
    (void) memset(s_fd_cache, 0, sizeof(s_fd_cache));
    (void) memset(&s_poller, 0, sizeof(s_poller));
}

void modbus_poller_cycle_for_test(void)
{
    poll_cycle();
}

const modbus_slave_state_t *modbus_poller_slave_state_for_test(uint8_t slot)
{
    return (slot < MODBUS_MAX_SLAVES) ? &s_slave_states[slot] : NULL;
}
#endif /* TEST */
