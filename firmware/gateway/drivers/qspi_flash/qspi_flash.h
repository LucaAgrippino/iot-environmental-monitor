/**
 * @file qspi_flash.h
 * @brief QSPI NOR flash driver — read, page-program, and sector-erase (Gateway).
 *
 * Provides IQspiFlash (per components.md). Uses the CMSIS QUADSPI peripheral
 * in indirect mode (1-1-1 SPI) only. No memory-mapped mode; no quad mode.
 * No internal synchronisation — callers serialise concurrent access
 * (companion §3.3, QSPID-O1).
 *
 * Board: B-L475E-IOT01A (STM32L475), MX25R6435F 8 MB on PE10..PE15 (AF10).
 * The Field Device counterpart
 * (firmware/field-device/drivers/qspi_flash_driver/qspi_flash_driver.h)
 * implements the same API for the STM32F469 / MT25QL128ABA — separate
 * CubeIDE projects, separate files (same split as RtcDriver).
 *
 * Singleton module (companion QSPID-D6): one flash device per board, so
 * the Gateway ADT pattern (opaque handle + pool) is not applied.
 *
 * @note See docs/lld/drivers/qspi-flash-driver.md for the full design.
 */

#ifndef QSPI_FLASH_H
#define QSPI_FLASH_H

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Geometry constants — shared with middleware consumers               */
/* ------------------------------------------------------------------ */

/** Flash capacity of the MX25R6435F in bytes (8 MB). */
#define QSPI_FLASH_DEVICE_SIZE_BYTES (8UL * 1024UL * 1024UL)

/** Page-program granularity: a single write may not cross this boundary. */
#define QSPI_FLASH_PAGE_SIZE_BYTES (256U)

/** Sector-erase granularity. */
#define QSPI_FLASH_SECTOR_SIZE_BYTES (4096U)

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Error codes returned by all QspiFlashDriver operations.
 *
 * Naming follows the cross-cutting convention in lld.md §3.2.
 * NOT_INITIALISED and NULL_POINTER were added by Pass H (QSPID-O7).
 */
typedef enum
{
    QSPI_FLASH_OK = 0,                  /**< Operation succeeded. */
    QSPI_FLASH_ERR_BUSY = 1,            /**< QUADSPI peripheral busy or flash WIP set. */
    QSPI_FLASH_ERR_TIMEOUT = 2,         /**< A flag poll exceeded its bounded window. */
    QSPI_FLASH_ERR_ADDR = 3,            /**< Address (or addr + len) exceeds device capacity. */
    QSPI_FLASH_ERR_LEN = 4,             /**< len == 0, len > 256, or write crosses a page. */
    QSPI_FLASH_ERR_DEVICE = 5,          /**< RDID response does not match expected ID. */
    QSPI_FLASH_ERR_NOT_INITIALISED = 6, /**< qspi_flash_init() has not succeeded yet. */
    QSPI_FLASH_ERR_NULL_POINTER = 7     /**< A required pointer argument was NULL. */
} qspi_flash_err_t;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise the QUADSPI peripheral and verify the flash device.
 *
 * Enables the QUADSPI and GPIOE clocks, configures PE10..PE15 as AF10,
 * configures the QUADSPI peripheral (prescaler, flash size, CS high
 * time), issues Reset Enable / Reset Memory (0x66 / 0x99), then a Read ID
 * (RDID, 0x9F) and verifies the 3-byte response against the expected
 * manufacturer + device type + capacity identifier (0xC22817).
 * Returns QSPI_FLASH_ERR_DEVICE if the response does not match — catches
 * wrong device population or open-circuit flash at boot.
 *
 * Idempotent: a second call after success returns QSPI_FLASH_OK without
 * touching the hardware.
 *
 * Must be called once from main() before the FreeRTOS scheduler starts.
 * Operates in indirect mode (1-1-1 SPI). Does not activate quad mode.
 *
 * @return QSPI_FLASH_OK on success; QSPI_FLASH_ERR_DEVICE on ID
 *         mismatch; QSPI_FLASH_ERR_TIMEOUT if the peripheral does not
 *         respond.
 * @note Threading: task-context only, non-blocking. Must be called before
 *       the scheduler starts.
 */
qspi_flash_err_t qspi_flash_init(void);

/**
 * @brief Read bytes from the flash device.
 *
 * Issues a Read Data command (0x03) in indirect mode. Reads any number
 * of bytes starting at addr; wraps at the device boundary are not
 * supported (QSPI_FLASH_ERR_ADDR if addr + len exceeds device capacity).
 *
 * Caller serialises concurrent calls — see companion §3.3 and QSPID-O1.
 *
 * @param addr  Byte address within the flash (0 .. device_size - 1).
 * @param buf   Destination buffer (must not be NULL; must be >= len bytes).
 * @param len   Number of bytes to read (must be >= 1).
 * @return QSPI_FLASH_OK on success; QSPI_FLASH_ERR_NOT_INITIALISED or
 *         QSPI_FLASH_ERR_NULL_POINTER on guard failure; QSPI_FLASH_ERR_ADDR
 *         or QSPI_FLASH_ERR_LEN on constraint violation;
 *         QSPI_FLASH_ERR_BUSY or QSPI_FLASH_ERR_TIMEOUT on hardware error.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
qspi_flash_err_t qspi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

/**
 * @brief Program up to 256 bytes within a single flash page.
 *
 * Issues Write Enable (0x06) then Page Program (0x02). Polls WIP until
 * the device completes the write (typically < 1 ms; max 10 ms per
 * MX25R6435F datasheet in low-power mode).
 *
 * Constraints enforced by the driver:
 *   - len must be >= 1 and <= 256.
 *   - addr and addr + len - 1 must lie within the same 256-byte page
 *     (i.e. (addr & ~0xFF) == ((addr + len - 1) & ~0xFF)).
 *     Returns QSPI_FLASH_ERR_LEN if violated.
 *   - addr + len must not exceed device capacity.
 *
 * NOR flash can only change 1 to 0. Bytes that already contain the target
 * value are written harmlessly; bits that need 0 to 1 require a prior
 * sector erase. This is a hardware constraint — the driver does not
 * verify or enforce it.
 *
 * @param addr  Byte address of the first byte to program.
 * @param data  Pointer to data to write (must not be NULL).
 * @param len   Number of bytes to program (1 .. 256, within one page).
 * @return QSPI_FLASH_OK on success; error code on failure.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
qspi_flash_err_t qspi_flash_write_page(uint32_t addr, const uint8_t *data, uint16_t len);

/**
 * @brief Erase the 4 KB sector containing the given address.
 *
 * Issues Write Enable (0x06) then Sector Erase (0x20). Polls WIP until
 * the erase completes (typically 40 ms; max 240 ms per MX25R6435F).
 *
 * After erase, all bytes in the sector read as 0xFF. The address may be
 * any byte within the 4 KB sector — the driver aligns to the sector
 * boundary internally.
 *
 * @param addr  Any byte address within the target 4 KB sector.
 * @return QSPI_FLASH_OK on success; QSPI_FLASH_ERR_TIMEOUT if WIP
 *         does not clear within the bounded window (~500 ms);
 *         QSPI_FLASH_ERR_ADDR if addr exceeds device capacity.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
qspi_flash_err_t qspi_flash_erase_sector(uint32_t addr);

/* ------------------------------------------------------------------ */
/* IQspiFlash vtable (P2 — Dependency Inversion)                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Vtable exposing the QspiFlashDriver operational surface.
 *
 * ConfigStore, CircularFlashLog and FirmwareStore depend on this
 * interface; the concrete driver is injected as a const pointer to a
 * single static instance. qspi_flash_init() is called directly at
 * startup — it is not part of this vtable.
 */
typedef struct
{
    qspi_flash_err_t (*read)(uint32_t addr, uint8_t *buf, uint32_t len);
    qspi_flash_err_t (*write_page)(uint32_t addr, const uint8_t *data, uint16_t len);
    qspi_flash_err_t (*erase_sector)(uint32_t addr);
} iqspi_flash_t;

/** Singleton pointer to the QspiFlashDriver vtable instance. */
extern const iqspi_flash_t *const qspi_flash_driver;

/* ------------------------------------------------------------------ */
/* Test-only hooks (#ifdef TEST)                                       */
/* ------------------------------------------------------------------ */

#ifdef TEST
/**
 * @brief Reset module state for unit tests.
 *
 * Clears s_initialised and s_device_size to post-BSS values. Test-only.
 */
void qspi_flash_reset_for_test(void);
#endif

#endif /* QSPI_FLASH_H */
