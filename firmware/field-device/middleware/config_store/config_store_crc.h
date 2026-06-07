/**
 * @file config_store_crc.h
 * @brief CRC32/ISO-HDLC (zlib/Ethernet) for ConfigStore slot integrity.
 *
 * Polynomial 0xEDB88320 (reflected), initial value 0xFFFFFFFF,
 * output XOR 0xFFFFFFFF. Table-driven; 256-entry ROM table (1 KB).
 *
 * Streaming API allows incremental computation over large buffers
 * without heap allocation.  One-shot wrapper for convenience.
 */

#ifndef CONFIG_STORE_CRC_H
#define CONFIG_STORE_CRC_H

#include <stdint.h>

/**
 * @brief Initialise a streaming CRC computation.
 * @return Initial running CRC state (before first feed).
 */
uint32_t config_store_crc32_start(void);

/**
 * @brief Feed bytes into a running CRC computation.
 * @param crc  Running state from _start() or a previous _feed().
 * @param buf  Data buffer (must not be NULL).
 * @param len  Byte count.
 * @return Updated running CRC state.
 */
uint32_t config_store_crc32_feed(uint32_t crc, const uint8_t *buf, uint32_t len);

/**
 * @brief Finalise a streaming CRC computation.
 * @param crc  Running state from the last _feed().
 * @return Final CRC32 value (after output XOR).
 */
uint32_t config_store_crc32_finish(uint32_t crc);

/**
 * @brief One-shot CRC32 over a contiguous buffer.
 * @param buf  Data buffer (must not be NULL).
 * @param len  Byte count.
 * @return CRC32/ISO-HDLC of the buffer.
 */
uint32_t config_store_crc32(const uint8_t *buf, uint32_t len);

#endif /* CONFIG_STORE_CRC_H */
