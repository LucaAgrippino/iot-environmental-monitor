/**
 * @file config_store_config.h
 * @brief Board-specific partition constants for ConfigStore.
 *
 * Both STM32F469 (FD) and STM32L475 (GW) alias QSPI to 0x90000000.
 * Values are identical on both boards — see docs/lld/middleware/config-store.md §11.
 */

#ifndef CONFIG_STORE_CONFIG_H
#define CONFIG_STORE_CONFIG_H

/** QSPI aliased base address (same on FD and GW). */
#define CONFIG_STORE_QSPI_BASE_ADDR 0x00000000UL

/** Total config partition: 64 KB spanning two A/B slots. */
#define CONFIG_STORE_PARTITION_SIZE (64U * 1024U)

/** Each A/B slot: 32 KB. */
#define CONFIG_STORE_SLOT_SIZE (32U * 1024U)

/** QSPI erase granularity: 4 KB sector. */
#define CONFIG_STORE_SECTOR_SIZE (4U * 1024U)

#endif /* CONFIG_STORE_CONFIG_H */
