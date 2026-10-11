/**
 * @file config_store_crc.h
 * @brief CRC32/ISO-HDLC for ConfigStore slot integrity (Gateway).
 *
 * Polynomial 0xEDB88320 (reflected), initial value 0xFFFFFFFF, final XOR
 * 0xFFFFFFFF: the zlib / Ethernet CRC32, checkable on any PC. Private to
 * ConfigStore; not shared with the Modbus CRC (different polynomial).
 *
 * The incremental form lets slot verification feed 256-byte chunks, so no
 * 32 KB buffer is needed (CS-D7).
 *
 * @see docs/lld/middleware/config-store-lld.md §9
 */

#ifndef CONFIG_STORE_CRC_H
#define CONFIG_STORE_CRC_H

#include <stdint.h>

/**
 * @brief Start an incremental CRC32.
 * @return The initial running value.
 */
uint32_t config_store_crc32_begin(void);

/**
 * @brief Fold a buffer into a running CRC32.
 *
 * @param[in] running  Value from config_store_crc32_begin() or a previous feed.
 * @param[in] buf      Data to fold in; may be NULL only when len is 0.
 * @param[in] len      Byte count.
 * @return The updated running value.
 */
uint32_t config_store_crc32_feed(uint32_t running, const uint8_t *buf, uint32_t len);

/**
 * @brief Finish an incremental CRC32.
 * @param[in] running  The running value after the last feed.
 * @return The final CRC32.
 */
uint32_t config_store_crc32_end(uint32_t running);

/**
 * @brief Compute the CRC32 of one buffer.
 *
 * @param[in] buf  Data; may be NULL only when len is 0.
 * @param[in] len  Byte count.
 * @return The CRC32 (0 for an empty buffer).
 */
uint32_t config_store_crc32(const uint8_t *buf, uint32_t len);

#endif /* CONFIG_STORE_CRC_H */
