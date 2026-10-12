/**
 * @file modbus_master.c
 * @brief ModbusMaster implementation (Gateway).
 *
 * The four Machine 4 protocol states map to code positions inside
 * modbus_master_transact() (companion §5.4): Transmitting is the blocking
 * modbus_uart_transmit() call, AwaitingResponse is xTaskNotifyWait(), and
 * ProcessingResponse is the validation sequence.
 *
 * @see docs/lld/middleware/modbus-master-poller.md
 */

#include "modbus_master.h"

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#ifdef TEST
#define LOG_WARN(m, f, ...) ((void) 0)
#define LOG_ERROR(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

/* The real driver header is safe here: Ceedling auto-links only from the test TU's own includes. */
#include "modbus_uart_driver/modbus_uart_driver.h"

#define MODULE_TAG "MBM"

/* Task-notification bits set by the UART RX callback. */
#define NOTIFY_RX_DONE (1UL << 0)
#define NOTIFY_RX_ERROR (1UL << 1)
#define NOTIFY_ALL_BITS (0xFFFFFFFFUL)

#define MBM_CRC_BYTES (2U)
#define MBM_HEADER_BYTES (2U)                                  /* address + function code */
#define MBM_MIN_FRAME_BYTES (MBM_HEADER_BYTES + MBM_CRC_BYTES) /* smallest valid response */
#define MBM_EXCEPTION_FRAME_BYTES (MBM_MIN_FRAME_BYTES + 1U)   /* + exception code */
#define MBM_EXCEPTION_FLAG (0x80U)
#define MBM_BITS_PER_BYTE (8U)

#define CRC16_INIT (0xFFFFU)
#define CRC16_POLY (0xA001U)

/* ========================================================================= */
/* Module state                                                              */
/* ========================================================================= */

static struct
{
    uint32_t response_timeout_ms;
    uint8_t max_retries;
    TaskHandle_t owner_task;
    modbus_master_stats_t stats;
    bool initialised;
    bool started;
} s_mm;

/* ========================================================================= */
/* Helpers                                                                   */
/* ========================================================================= */

/** CRC-16/MODBUS, bitwise (poly 0xA001, init 0xFFFF).  Slow but tiny; 9600 baud does not care. */
static uint16_t mbm_crc16(const uint8_t *data, uint16_t len)
{
    uint16_t crc = CRC16_INIT;

    for (uint16_t i = 0U; i < len; i++)
    {
        crc ^= data[i];
        for (uint8_t bit = 0U; bit < MBM_BITS_PER_BYTE; bit++)
        {
            crc =
                ((crc & 1U) != 0U) ? (uint16_t) ((crc >> 1U) ^ CRC16_POLY) : (uint16_t) (crc >> 1U);
        }
    }
    return crc;
}

/** Build addr + FC + data + CRC (low byte first).  @return wire length. */
static uint16_t mbm_build_wire_frame(const modbus_frame_t *request, uint8_t *wire)
{
    uint16_t len = 0U;

    wire[len++] = request->slave_addr;
    wire[len++] = request->function_code;
    if (request->data_len > 0U)
    {
        (void) memcpy(&wire[len], request->data, request->data_len);
        len = (uint16_t) (len + request->data_len);
    }

    const uint16_t crc = mbm_crc16(wire, len);

    wire[len++] = (uint8_t) (crc & 0xFFU);
    wire[len++] = (uint8_t) (crc >> MBM_BITS_PER_BYTE);
    return len;
}

/**
 * @brief Discard anything left over from an earlier attempt.
 *
 * xTaskNotifyStateClear() would not do: it resets the pending state but not
 * the accumulated bits, so a late RX_DONE from a timed-out attempt could be
 * taken for the next response (MBMP-D5, revised).  The UART buffer is
 * drained as well, or a late frame would be prepended to the next one.
 */
static void mbm_flush_stale_rx(uint8_t *scratch)
{
    uint32_t stale_bits = 0U;
    uint16_t stale_len = 0U;

    (void) xTaskNotifyWait(0U, NOTIFY_ALL_BITS, &stale_bits, 0U);
    (void) modbus_uart_get_rx_frame(scratch, &stale_len);
}

/** UART RX callback — ISR context.  Maps the driver event to a notification bit. */
static void mbm_uart_rx_callback(modbus_uart_event_t event, void *context)
{
    BaseType_t higher_woken = pdFALSE;
    const uint32_t bits = (event == MODBUS_UART_EVENT_RX_DONE) ? NOTIFY_RX_DONE : NOTIFY_RX_ERROR;

    (void) context;
    if (s_mm.owner_task != NULL)
    {
        (void) xTaskNotifyFromISR(s_mm.owner_task, bits, eSetBits, &higher_woken);
        portYIELD_FROM_ISR(higher_woken);
    }
}

/* ========================================================================= */
/* Public API                                                                */
/* ========================================================================= */

modbus_master_err_t modbus_master_init(uint32_t response_timeout_ms, uint8_t max_retries)
{
    if (response_timeout_ms == 0U)
    {
        return MODBUS_MASTER_ERR_INVALID_ARG;
    }

    (void) memset(&s_mm, 0, sizeof(s_mm));
    s_mm.response_timeout_ms = response_timeout_ms;
    s_mm.max_retries = max_retries;
    s_mm.initialised = true;
    return MODBUS_MASTER_ERR_OK;
}

modbus_master_err_t modbus_master_start(void)
{
    if (!s_mm.initialised)
    {
        return MODBUS_MASTER_ERR_NOT_INIT;
    }
    if (s_mm.started)
    {
        return MODBUS_MASTER_ERR_ALREADY_STARTED;
    }

    s_mm.owner_task = xTaskGetCurrentTaskHandle();
    modbus_uart_attach_rx(mbm_uart_rx_callback, NULL);
    s_mm.started = true;
    return MODBUS_MASTER_ERR_OK;
}

modbus_master_err_t modbus_master_transact(const modbus_frame_t *request, modbus_frame_t *response)
{
    uint8_t wire_buf[MODBUS_MAX_FRAME_BYTES];
    uint8_t rx_buf[MODBUS_MAX_FRAME_BYTES];
    modbus_master_err_t last_err = MODBUS_MASTER_ERR_TIMEOUT;

    if (!s_mm.started)
    {
        return MODBUS_MASTER_ERR_NOT_INIT;
    }
    if ((request == NULL) || (response == NULL))
    {
        return MODBUS_MASTER_ERR_NULL_ARG;
    }
    if (request->data_len > MODBUS_MAX_DATA_BYTES)
    {
        return MODBUS_MASTER_ERR_INVALID_ARG;
    }

    const uint16_t wire_len = mbm_build_wire_frame(request, wire_buf);

    for (uint8_t attempt = 0U; attempt <= s_mm.max_retries; attempt++)
    {
        uint32_t bits = 0U;
        uint16_t rx_len = 0U;

        s_mm.stats.transactions_sent++;

        /* ---- Transmitting ---- */
        mbm_flush_stale_rx(rx_buf);
        if (modbus_uart_transmit(wire_buf, wire_len) != MODBUS_UART_OK)
        {
            /* A failed TX is indistinguishable from a lost frame (MBMP-D7). */
            s_mm.stats.timeouts++;
            last_err = MODBUS_MASTER_ERR_TIMEOUT;
            continue;
        }

        /* ---- AwaitingResponse ----
         * Bits are NOT cleared on entry: a fast slave may already have raised
         * RX_DONE, and the stale bits were flushed before the TX. */
        if (xTaskNotifyWait(0U, NOTIFY_RX_DONE | NOTIFY_RX_ERROR, &bits,
                            pdMS_TO_TICKS(s_mm.response_timeout_ms)) == pdFALSE)
        {
            s_mm.stats.timeouts++;
            last_err = MODBUS_MASTER_ERR_TIMEOUT;
            continue;
        }

        /* ---- ProcessingResponse ---- */
        if ((bits & NOTIFY_RX_ERROR) != 0U)
        {
            /* UART framing / overrun error: transient, so retried. */
            s_mm.stats.rx_errors++;
            last_err = MODBUS_MASTER_ERR_RX_ERROR;
            continue;
        }

        if ((modbus_uart_get_rx_frame(rx_buf, &rx_len) != MODBUS_UART_OK) ||
            (rx_len < MBM_MIN_FRAME_BYTES))
        {
            /* Runt frame: transient, so retried. */
            s_mm.stats.bad_responses++;
            last_err = MODBUS_MASTER_ERR_BAD_RESPONSE;
            continue;
        }

        const uint16_t body_len = (uint16_t) (rx_len - MBM_CRC_BYTES);
        const uint16_t recv_crc =
            (uint16_t) (rx_buf[body_len] | ((uint16_t) rx_buf[body_len + 1U] << MBM_BITS_PER_BYTE));

        if (mbm_crc16(rx_buf, body_len) != recv_crc)
        {
            s_mm.stats.crc_errors++;
            last_err = MODBUS_MASTER_ERR_CRC;
            continue;
        }

        /* Address and function-code mismatches are bus errors: not retried. */
        if (rx_buf[0] != request->slave_addr)
        {
            s_mm.stats.bad_responses++;
            LOG_ERROR(MODULE_TAG, "Response from slave %u, expected %u", (unsigned) rx_buf[0],
                      (unsigned) request->slave_addr);
            return MODBUS_MASTER_ERR_BAD_RESPONSE;
        }

        if (rx_buf[1] == (uint8_t) (request->function_code | MBM_EXCEPTION_FLAG))
        {
            if (rx_len < MBM_EXCEPTION_FRAME_BYTES)
            {
                s_mm.stats.bad_responses++;
                return MODBUS_MASTER_ERR_BAD_RESPONSE;
            }
            s_mm.stats.exception_responses++;
            response->slave_addr = rx_buf[0];
            response->function_code = rx_buf[1];
            response->data[0] = rx_buf[2];
            response->data_len = 1U;
            LOG_WARN(MODULE_TAG, "Slave %u FC %u exception 0x%02X", (unsigned) rx_buf[0],
                     (unsigned) request->function_code, (unsigned) rx_buf[2]);
            return MODBUS_MASTER_ERR_EXCEPTION;
        }

        if (rx_buf[1] != request->function_code)
        {
            s_mm.stats.bad_responses++;
            LOG_ERROR(MODULE_TAG, "Response FC %u, expected %u", (unsigned) rx_buf[1],
                      (unsigned) request->function_code);
            return MODBUS_MASTER_ERR_BAD_RESPONSE;
        }

        response->slave_addr = rx_buf[0];
        response->function_code = rx_buf[1];
        response->data_len = (uint16_t) (body_len - MBM_HEADER_BYTES);
        (void) memcpy(response->data, &rx_buf[MBM_HEADER_BYTES], response->data_len);

        s_mm.stats.transactions_ok++;
        return MODBUS_MASTER_ERR_OK;
    }

    LOG_WARN(MODULE_TAG, "Slave %u: retries exhausted (err %d)", (unsigned) request->slave_addr,
             (int) last_err);
    return last_err;
}

modbus_master_err_t modbus_master_get_stats(modbus_master_stats_t *stats_out)
{
    if (stats_out == NULL)
    {
        return MODBUS_MASTER_ERR_NULL_ARG;
    }
    if (!s_mm.initialised)
    {
        return MODBUS_MASTER_ERR_NOT_INIT;
    }

    *stats_out = s_mm.stats;
    return MODBUS_MASTER_ERR_OK;
}

modbus_master_err_t modbus_master_reset_stats(void)
{
    if (!s_mm.initialised)
    {
        return MODBUS_MASTER_ERR_NOT_INIT;
    }

    (void) memset(&s_mm.stats, 0, sizeof(s_mm.stats));
    return MODBUS_MASTER_ERR_OK;
}

#ifdef TEST
void modbus_master_reset_for_test(void)
{
    (void) memset(&s_mm, 0, sizeof(s_mm));
}
#endif /* TEST */
