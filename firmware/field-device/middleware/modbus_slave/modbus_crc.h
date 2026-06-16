/**
 * @file modbus_crc.h
 * @brief CRC-16/IBM (Modbus) interface.
 *
 * Polynomial 0x8005 (reflected: 0xA001), initial value 0xFFFF, input and
 * output bytes reflected. CRC bytes are appended low byte first per Modbus
 * RTU convention.
 *
 * @note See modbus-slave.md §7 for algorithm details.
 */

#ifndef MODBUS_CRC_H
#define MODBUS_CRC_H

#include <stdint.h>

/**
 * @brief Compute CRC-16/IBM over a byte buffer.
 *
 * @param buf  Pointer to the first byte of the data to protect.
 * @param len  Number of bytes to process (must not include the CRC field).
 * @return     16-bit CRC. Append as: buf[len] = crc & 0xFF; buf[len+1] = crc >> 8.
 */
uint16_t modbus_crc16(const uint8_t *buf, uint16_t len);

#endif /* MODBUS_CRC_H */
