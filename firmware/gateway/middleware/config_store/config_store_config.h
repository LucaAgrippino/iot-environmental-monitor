/**
 * @file config_store_config.h
 * @brief Partition geometry and slot layout constants for ConfigStore (Gateway).
 *
 * Addresses are QspiFlashDriver device offsets, not mapped addresses: the
 * driver's read/program/erase take offsets from 0, so the partition the
 * companion places at 0x9000_0000 is at offset 0 here (the same convention as
 * CircularFlashLog).
 *
 * Slot layout, little-endian:
 *   0x0000  magic       (4)
 *   0x0004  seq_number  (4)
 *   0x0008  data_len    (4)
 *   0x000C  reserved    (4, written as 0)
 *   0x0010  data        (data_len bytes)
 *   0x7FF8  crc32       (4)   CRC32/ISO-HDLC over bytes 0x0000..0x7FF7
 *
 * @see docs/lld/middleware/config-store-lld.md §6
 */

#ifndef CONFIG_STORE_CONFIG_H
#define CONFIG_STORE_CONFIG_H

#include <stdint.h>

/** Device offset of the partition (mapped at 0x9000_0000). */
#define CONFIG_STORE_PARTITION_BASE (0x00000000UL)

/** Number of A/B slots. */
#define CONFIG_STORE_SLOT_COUNT (2U)

/** Size of one slot. */
#define CONFIG_STORE_SLOT_SIZE (32U * 1024U)

/** Total partition: two slots. */
#define CONFIG_STORE_PARTITION_SIZE (CONFIG_STORE_SLOT_COUNT * CONFIG_STORE_SLOT_SIZE)

/** QSPI erase granularity. */
#define CONFIG_STORE_SECTOR_SIZE (4U * 1024U)

/** Erase sectors in one slot. */
#define CONFIG_STORE_SECTORS_PER_SLOT (CONFIG_STORE_SLOT_SIZE / CONFIG_STORE_SECTOR_SIZE)

/** Slot header size: magic, seq_number, data_len, reserved. */
#define CONFIG_STORE_HEADER_SIZE (16U)

/** Offset of the CRC32 within a slot; it covers every byte before it. */
#define CONFIG_STORE_CRC_OFFSET (0x7FF8U)

/** Size of the stored CRC32. */
#define CONFIG_STORE_CRC_SIZE (4U)

/** Bytes between the header and the CRC: the largest blob a slot can hold. */
#define CONFIG_STORE_DATA_AREA_SIZE (CONFIG_STORE_CRC_OFFSET - CONFIG_STORE_HEADER_SIZE)

#endif /* CONFIG_STORE_CONFIG_H */
