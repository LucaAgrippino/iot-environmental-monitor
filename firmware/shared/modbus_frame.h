/**
 * @file modbus_frame.h
 * @brief Application-level Modbus PDU shared by ModbusMaster (Gateway) and the slave side.
 *
 * Wire encoding (address prefix, CRC suffix) is internal to ModbusMaster.
 */

#ifndef MODBUS_FRAME_H
#define MODBUS_FRAME_H

#include <stdint.h>

/** Maximum Modbus RTU ADU length in bytes. */
#define MODBUS_MAX_FRAME_BYTES (256U)

/** Maximum PDU payload: 256 - address (1) - function code (1) - CRC (2). */
#define MODBUS_MAX_DATA_BYTES (252U)

/** @brief One Modbus request or response, without address-prefix framing or CRC. */
typedef struct
{
    uint8_t slave_addr;                  /**< Slave address, 1..247. */
    uint8_t function_code;               /**< Function code (bit 7 set in an exception response). */
    uint8_t data[MODBUS_MAX_DATA_BYTES]; /**< Payload bytes following the function code. */
    uint16_t data_len;                   /**< Valid bytes in data[]. */
} modbus_frame_t;

#endif /* MODBUS_FRAME_H */
