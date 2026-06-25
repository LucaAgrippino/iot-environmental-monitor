/**
 * @file modbus_slave.c
 * @brief ModbusSlave implementation — Modbus RTU protocol state machine.
 *
 * Layout:
 *   §1  Includes
 *   §2  Private constants and module state
 *   §3  Internal helpers (response builders)
 *   §4  ISR callback
 *   §5  Public API
 *   §6  Test-only hooks
 */

/* ===================================================================== */
/* §1. Includes                                                          */
/* ===================================================================== */

#include "modbus_slave.h"
#include "modbus_crc.h"

#include <string.h>
#include "FreeRTOS.h"
#include "task.h"

#ifndef TEST
#include "modbus_uart_driver/modbus_uart_driver.h"
#include "logger/logger.h"
#define LOG_MODULE ("MSLAVE")
#else
/* Stub driver header swapped in under TEST — avoids auto-linking the real driver. */
#include "modbus_uart_driver_stub.h"
/* Logger no-ops — avoids auto-linking logger.c in the test build. */
#define LOG_INFO(mod, fmt, ...) ((void) 0)
#define LOG_WARN(mod, fmt, ...) ((void) 0)
#define LOG_ERROR(mod, fmt, ...) ((void) 0)
#endif

/* ===================================================================== */
/* §2. Private constants and module state                                */
/* ===================================================================== */

#define MODBUS_MAX_FRAME_BYTES (256U)

#define MODBUS_FC_READ_HOLDING (0x03U)
#define MODBUS_FC_READ_INPUT (0x04U)
#define MODBUS_FC_WRITE_SINGLE (0x06U)
#define MODBUS_FC_WRITE_MULTIPLE (0x10U)

#define MODBUS_EXC_ILLEGAL_FC (0x01U)
#define MODBUS_EXC_ILLEGAL_ADDR (0x02U)
#define MODBUS_EXC_ILLEGAL_VALUE (0x03U)
#define MODBUS_EXC_DEVICE_FAIL (0x04U)

#define MODBUS_ADDR_MIN (1U)
#define MODBUS_ADDR_MAX (247U)

typedef struct
{
    bool initialised;
    uint8_t slave_addr;
    uint8_t rx_buf[MODBUS_MAX_FRAME_BYTES];
    uint16_t rx_len;
    uint8_t tx_buf[MODBUS_MAX_FRAME_BYTES];
    uint16_t tx_len;
    const IModbusRegisterMap *reg_map;
    TaskHandle_t task_handle;
    modbus_slave_stats_t stats;
} modbus_slave_t;

static modbus_slave_t s_slave;

/* ===================================================================== */
/* §3. Internal helpers — response builders                              */
/* ===================================================================== */

static void append_crc(uint8_t *frame, uint16_t len)
{
    uint16_t crc = modbus_crc16(frame, len);
    frame[len] = (uint8_t) (crc & 0x00FFU);
    frame[len + 1U] = (uint8_t) (crc >> 8U);
}

static void build_exception_response(uint8_t fc, uint8_t exception_code)
{
    s_slave.tx_buf[0] = s_slave.slave_addr;
    s_slave.tx_buf[1] = fc | 0x80U;
    s_slave.tx_buf[2] = exception_code;
    append_crc(s_slave.tx_buf, 3U);
    s_slave.tx_len = 5U;
    s_slave.stats.exception_responses++;
}

static modbus_slave_err_t build_read_response(uint8_t fc, uint16_t start_addr, uint16_t reg_count)
{
    if (reg_count == 0U || reg_count > (MODBUS_MAX_FRAME_BYTES / 2U))
    {
        build_exception_response(fc, MODBUS_EXC_ILLEGAL_ADDR);
        return MODBUS_SLAVE_ERR_INVALID_ADDR;
    }

    uint8_t byte_count = (uint8_t) (reg_count * 2U);
    s_slave.tx_buf[0] = s_slave.slave_addr;
    s_slave.tx_buf[1] = fc;
    s_slave.tx_buf[2] = byte_count;

    for (uint16_t i = 0U; i < reg_count; i++)
    {
        uint16_t value = 0U;
        modbus_slave_err_t err;

        if (fc == MODBUS_FC_READ_INPUT)
        {
            err = s_slave.reg_map->read_input(start_addr + i, &value);
        }
        else
        {
            err = s_slave.reg_map->read_holding(start_addr + i, &value);
        }

        if (err == MODBUS_SLAVE_ERR_INVALID_ADDR)
        {
            build_exception_response(fc, MODBUS_EXC_ILLEGAL_ADDR);
            return err;
        }
        if (err == MODBUS_SLAVE_ERR_DEVICE_FAIL)
        {
            build_exception_response(fc, MODBUS_EXC_DEVICE_FAIL);
            return err;
        }

        s_slave.tx_buf[3U + i * 2U] = (uint8_t) (value >> 8U);
        s_slave.tx_buf[3U + i * 2U + 1U] = (uint8_t) (value & 0x00FFU);
    }

    uint16_t pdu_len = 3U + (uint16_t) byte_count;
    append_crc(s_slave.tx_buf, pdu_len);
    s_slave.tx_len = pdu_len + 2U;
    s_slave.stats.successful_responses++;
    return MODBUS_SLAVE_ERR_OK;
}

static modbus_slave_err_t build_write_single_response(void)
{
    uint16_t reg_addr = ((uint16_t) s_slave.rx_buf[2] << 8U) | (uint16_t) s_slave.rx_buf[3];
    uint16_t value = ((uint16_t) s_slave.rx_buf[4] << 8U) | (uint16_t) s_slave.rx_buf[5];

    modbus_slave_err_t err = s_slave.reg_map->write_holding(reg_addr, value);

    if (err == MODBUS_SLAVE_ERR_INVALID_ADDR)
    {
        build_exception_response(MODBUS_FC_WRITE_SINGLE, MODBUS_EXC_ILLEGAL_ADDR);
        return err;
    }
    if (err == MODBUS_SLAVE_ERR_INVALID_VALUE)
    {
        build_exception_response(MODBUS_FC_WRITE_SINGLE, MODBUS_EXC_ILLEGAL_VALUE);
        return err;
    }
    if (err == MODBUS_SLAVE_ERR_DEVICE_FAIL)
    {
        build_exception_response(MODBUS_FC_WRITE_SINGLE, MODBUS_EXC_DEVICE_FAIL);
        return err;
    }

    /* Echo the request as the ACK response (Modbus convention for FC06). */
    s_slave.tx_buf[0] = s_slave.slave_addr;
    s_slave.tx_buf[1] = MODBUS_FC_WRITE_SINGLE;
    s_slave.tx_buf[2] = (uint8_t) (reg_addr >> 8U);
    s_slave.tx_buf[3] = (uint8_t) (reg_addr & 0x00FFU);
    s_slave.tx_buf[4] = (uint8_t) (value >> 8U);
    s_slave.tx_buf[5] = (uint8_t) (value & 0x00FFU);
    append_crc(s_slave.tx_buf, 6U);
    s_slave.tx_len = 8U;
    s_slave.stats.successful_responses++;
    return MODBUS_SLAVE_ERR_OK;
}

static modbus_slave_err_t build_write_multiple_response(void)
{
    uint16_t start_addr = ((uint16_t) s_slave.rx_buf[2] << 8U) | (uint16_t) s_slave.rx_buf[3];
    uint16_t reg_count = ((uint16_t) s_slave.rx_buf[4] << 8U) | (uint16_t) s_slave.rx_buf[5];
    uint8_t byte_count = s_slave.rx_buf[6];

    /* Validate byte count == register_count × 2. */
    if (byte_count != (uint8_t) (reg_count * 2U))
    {
        build_exception_response(MODBUS_FC_WRITE_MULTIPLE, MODBUS_EXC_ILLEGAL_VALUE);
        return MODBUS_SLAVE_ERR_INVALID_VALUE;
    }

    for (uint16_t i = 0U; i < reg_count; i++)
    {
        uint16_t value = ((uint16_t) s_slave.rx_buf[7U + i * 2U] << 8U) |
                         (uint16_t) s_slave.rx_buf[7U + i * 2U + 1U];

        modbus_slave_err_t err = s_slave.reg_map->write_holding(start_addr + i, value);

        if (err == MODBUS_SLAVE_ERR_INVALID_ADDR)
        {
            build_exception_response(MODBUS_FC_WRITE_MULTIPLE, MODBUS_EXC_ILLEGAL_ADDR);
            return err;
        }
        if (err == MODBUS_SLAVE_ERR_INVALID_VALUE)
        {
            build_exception_response(MODBUS_FC_WRITE_MULTIPLE, MODBUS_EXC_ILLEGAL_VALUE);
            return err;
        }
        if (err == MODBUS_SLAVE_ERR_DEVICE_FAIL)
        {
            build_exception_response(MODBUS_FC_WRITE_MULTIPLE, MODBUS_EXC_DEVICE_FAIL);
            return err;
        }
    }

    /* ACK response: addr, FC, start_addr(2), reg_count(2), CRC(2). */
    s_slave.tx_buf[0] = s_slave.slave_addr;
    s_slave.tx_buf[1] = MODBUS_FC_WRITE_MULTIPLE;
    s_slave.tx_buf[2] = (uint8_t) (start_addr >> 8U);
    s_slave.tx_buf[3] = (uint8_t) (start_addr & 0x00FFU);
    s_slave.tx_buf[4] = (uint8_t) (reg_count >> 8U);
    s_slave.tx_buf[5] = (uint8_t) (reg_count & 0x00FFU);
    append_crc(s_slave.tx_buf, 6U);
    s_slave.tx_len = 8U;
    s_slave.stats.successful_responses++;
    return MODBUS_SLAVE_ERR_OK;
}

/* ===================================================================== */
/* §4. ISR callback                                                      */
/* ===================================================================== */

static void on_frame_complete(modbus_uart_event_t event, void *context)
{
    (void) context;
    BaseType_t higher_priority_woken = pdFALSE;
    /* Notify ModbusTask — event value encodes RX_DONE or RX_ERROR. */
    xTaskNotifyFromISR(s_slave.task_handle, (uint32_t) event, eSetValueWithOverwrite,
                       &higher_priority_woken);
    portYIELD_FROM_ISR(higher_priority_woken);
}

/* ===================================================================== */
/* §5. ISR callback                                                      */
/* ===================================================================== */
static const imodbus_slave_t s_modbus_slave_vtable = {
    .set_address = modbus_slave_set_address,
};

const imodbus_slave_t *const modbus_slave = &s_modbus_slave_vtable;
/* ===================================================================== */
/* §6. Public API                                                        */
/* ===================================================================== */

modbus_slave_err_t modbus_slave_init(const IModbusRegisterMap *reg_map, uint8_t slave_addr,
                                     TaskHandle_t task_handle)
{
    if (reg_map == NULL || task_handle == NULL)
    {
        return MODBUS_SLAVE_ERR_NULL_ARG;
    }
    if (slave_addr < MODBUS_ADDR_MIN || slave_addr > MODBUS_ADDR_MAX)
    {
        return MODBUS_SLAVE_ERR_INVALID_ADDR;
    }

    s_slave.reg_map = reg_map;
    s_slave.slave_addr = slave_addr;
    s_slave.task_handle = task_handle;
    s_slave.initialised = true;

    modbus_uart_attach_rx(on_frame_complete, NULL);
    return MODBUS_SLAVE_ERR_OK;
}

modbus_slave_err_t modbus_slave_process(void)
{
    if (!s_slave.initialised)
    {
        return MODBUS_SLAVE_ERR_NOT_INIT;
    }

    s_slave.tx_len = 0U;
    (void) modbus_uart_get_rx_frame(s_slave.rx_buf, &s_slave.rx_len);

    if (s_slave.rx_len < 4U)
    {
        /* Frame too short to contain addr + FC + CRC. Silent drop. */
        return MODBUS_SLAVE_ERR_OK;
    }

    /* Step 1: address filter. */
    if (s_slave.rx_buf[0] != s_slave.slave_addr)
    {
        s_slave.stats.address_mismatches++;
        return MODBUS_SLAVE_ERR_OK;
    }

    /* Step 2: CRC check. */
    uint16_t expected_crc = modbus_crc16(s_slave.rx_buf, s_slave.rx_len - 2U);
    uint16_t received_crc = (uint16_t) s_slave.rx_buf[s_slave.rx_len - 2U] |
                            ((uint16_t) s_slave.rx_buf[s_slave.rx_len - 1U] << 8U);

    if (expected_crc != received_crc)
    {
        s_slave.stats.crc_errors++;
        LOG_WARN(LOG_MODULE, "CRC mismatch exp=0x%04X got=0x%04X", (unsigned) expected_crc,
                 (unsigned) received_crc);
        return MODBUS_SLAVE_ERR_OK;
    }

    /* Step 3: valid frame. */
    s_slave.stats.valid_frames++;

    /* Step 4: FC dispatch. */
    uint8_t fc = s_slave.rx_buf[1];
    uint16_t start_addr = ((uint16_t) s_slave.rx_buf[2] << 8U) | (uint16_t) s_slave.rx_buf[3];
    uint16_t reg_count = ((uint16_t) s_slave.rx_buf[4] << 8U) | (uint16_t) s_slave.rx_buf[5];

    modbus_slave_err_t err = MODBUS_SLAVE_ERR_OK;

    switch (fc)
    {
    case MODBUS_FC_READ_HOLDING:
        err = build_read_response(MODBUS_FC_READ_HOLDING, start_addr, reg_count);
        break;
    case MODBUS_FC_READ_INPUT:
        err = build_read_response(MODBUS_FC_READ_INPUT, start_addr, reg_count);
        break;
    case MODBUS_FC_WRITE_SINGLE:
        err = build_write_single_response();
        break;
    case MODBUS_FC_WRITE_MULTIPLE:
        err = build_write_multiple_response();
        break;
    default:
        build_exception_response(fc, MODBUS_EXC_ILLEGAL_FC);
        s_slave.stats.unsupported_fc++;
        err = MODBUS_SLAVE_ERR_OK;
        break;
    }

    /* Step 5: transmit. */
    if (s_slave.tx_len > 0U)
    {
        (void) modbus_uart_transmit(s_slave.tx_buf, s_slave.tx_len);
    }

    (void) err;
    return MODBUS_SLAVE_ERR_OK;
}

modbus_slave_err_t modbus_slave_set_address(uint8_t new_addr)
{
    if (!s_slave.initialised)
    {
        return MODBUS_SLAVE_ERR_NOT_INIT;
    }
    if (new_addr < MODBUS_ADDR_MIN || new_addr > MODBUS_ADDR_MAX)
    {
        return MODBUS_SLAVE_ERR_INVALID_ADDR;
    }

    /* DEVIATION (MBS-BUG-01): missing taskENTER_CRITICAL / taskEXIT_CRITICAL
     * around this write. Concurrent ModbusTask read of slave_addr is not
     * protected. Invisible to unit tests because taskENTER/EXIT_CRITICAL()
     * are no-ops in the host build and tests are single-threaded. */
    taskENTER_CRITICAL();
    s_slave.slave_addr = new_addr;
    taskEXIT_CRITICAL();

    return MODBUS_SLAVE_ERR_OK;
}

modbus_slave_err_t modbus_slave_get_stats(modbus_slave_stats_t *stats_out)
{
    if (stats_out == NULL)
    {
        return MODBUS_SLAVE_ERR_NULL_ARG;
    }

    taskENTER_CRITICAL();
    (void) memcpy(stats_out, &s_slave.stats, sizeof(modbus_slave_stats_t));
    taskEXIT_CRITICAL();

    return MODBUS_SLAVE_ERR_OK;
}

modbus_slave_err_t modbus_slave_reset_stats(void)
{
    taskENTER_CRITICAL();
    (void) memset(&s_slave.stats, 0, sizeof(modbus_slave_stats_t));
    taskEXIT_CRITICAL();

    return MODBUS_SLAVE_ERR_OK;
}

/* ===================================================================== */
/* §7. Test-only hooks                                                   */
/* ===================================================================== */

#ifdef TEST
void modbus_slave_reset_for_test(void)
{
    (void) memset(&s_slave, 0, sizeof(s_slave));
}
#endif /* TEST */
