/**
 * @file qspi_flash_driver.h
 * @brief QSPI NOR flash driver — read, page-program, and sector-erase.
 *
 * Provides IQspiFlash (per components.md). Uses CMSIS QUADSPI peripheral
 * in indirect mode (1-1-1 SPI) only. No memory-mapped mode; no quad mode.
 * No internal synchronisation — callers serialise concurrent access.
 *
 * Board scope: Field Device (STM32F469, MT25QL128ABA 16 MB) and
 *              Gateway      (STM32L475, MX25R6435F   8 MB).
 *
 * @note See docs/lld/drivers/qspi-flash-driver.md for the full design.
 */

#ifndef QSPI_FLASH_DRIVER_H
#define QSPI_FLASH_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Error codes                                                         */
/* ------------------------------------------------------------------ */

/**
 * @brief Error codes returned by all QspiFlashDriver operations.
 *
 * Naming follows the cross-cutting convention in lld.md §3.2.
 */
typedef enum
{
    QSPI_FLASH_OK = 0,          /**< Operation succeeded. */
    QSPI_FLASH_ERR_BUSY = 1,    /**< QUADSPI peripheral busy or flash WIP set. */
    QSPI_FLASH_ERR_TIMEOUT = 2, /**< WIP polling exceeded timeout (erase/write). */
    QSPI_FLASH_ERR_ADDR = 3,    /**< Address exceeds device capacity. */
    QSPI_FLASH_ERR_LEN = 4,     /**< len == 0, or write crosses a page boundary. */
    QSPI_FLASH_ERR_DEVICE = 5,  /**< RDID response does not match expected ID. */
    QSPI_FLASH_ERR_NOT_INIT = 6 /**< Driver not initialised — call qspi_flash_init() first. */
} qspi_flash_err_t;

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

/**
 * @brief Initialise the QUADSPI peripheral and verify the flash device.
 *
 * Configures the QUADSPI peripheral (prescaler, flash size, CS high time).
 * Issues a Read ID (RDID, 0x9F) command and verifies the 3-byte response
 * against the expected manufacturer + device type + capacity identifier.
 * Returns QSPI_FLASH_ERR_DEVICE if the response does not match — catches
 * wrong device population or open-circuit flash at boot.
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
 * Caller serialises concurrent calls — see §3.3 and QSPID-O1 (§8).
 *
 * @param addr  Byte address within the flash (0 .. device_size - 1).
 * @param buf   Destination buffer (must not be NULL; must be >= len bytes).
 * @param len   Number of bytes to read (must be >= 1).
 * @return QSPI_FLASH_OK on success; QSPI_FLASH_ERR_ADDR or
 *         QSPI_FLASH_ERR_LEN on constraint violation;
 *         QSPI_FLASH_ERR_BUSY or QSPI_FLASH_ERR_TIMEOUT on hardware error.
 * @note Threading: task-context only, non-blocking. Not ISR-safe.
 */
qspi_flash_err_t qspi_flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

/**
 * @brief Program up to 256 bytes within a single flash page.
 *
 * Issues Write Enable (0x06) then Page Program (0x02). Polls WIP until
 * the device completes the write (typically < 1 ms; max 5 ms per
 * MX25R6435F datasheet).
 *
 * Constraints enforced by the driver:
 *   - len must be >= 1 and <= 256.
 *   - addr and addr + len - 1 must lie within the same 256-byte page
 *     (i.e. (addr & ~0xFF) == ((addr + len - 1) & ~0xFF)).
 *     Returns QSPI_FLASH_ERR_LEN if violated.
 *   - addr must not exceed device capacity.
 *
 * NOR flash can only change 1 to 0. Bytes that already contain the target
 * value are written harmlessly; bits that need 0 to 1 require a prior
 * sector erase. This is a hardware constraint — the driver does not
 * verify or enforce it.
 *
 * @param addr  Byte address of the first byte to program.
 * @param data  Pointer to data to write (must not be NULL).
 * @param len   Number of bytes to program (1 .. 256, page-aligned).
 * @return QSPI_FLASH_OK on success; error code on failure.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
qspi_flash_err_t qspi_flash_write_page(uint32_t addr, const uint8_t *data, uint16_t len);

/**
 * @brief Erase the 4 KB sector containing the given address.
 *
 * Issues Write Enable (0x06) then Sector Erase (0x20). Polls WIP until
 * the erase completes (typically 120 ms; max 300 ms per MX25R6435F).
 *
 * After erase, all bytes in the sector read as 0xFF. The address may be
 * any byte within the 4 KB sector — the driver aligns to the sector
 * boundary internally.
 *
 * @param addr  Any byte address within the target 4 KB sector.
 * @return QSPI_FLASH_OK on success; QSPI_FLASH_ERR_TIMEOUT if WIP
 *         does not clear within 500 ms; QSPI_FLASH_ERR_ADDR if addr
 *         exceeds device capacity.
 * @note Threading: task-context only, may block. Not ISR-safe.
 */
qspi_flash_err_t qspi_flash_erase_sector(uint32_t addr);

/* ------------------------------------------------------------------ */
/* IQspiFlash vtable (P2 — Dependency Inversion)                      */
/* ------------------------------------------------------------------ */

/**
 * @brief Vtable exposing the QspiFlashDriver operational surface.
 *
 * ConfigStore (and other middleware) depends on this interface; the concrete
 * driver is injected as a const pointer to a single static instance.
 * qspi_flash_init() is called directly at startup — it is not part of this
 * vtable.
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
#define QSPI_FLASH_TEST_VISIBLE
#else
#define QSPI_FLASH_TEST_VISIBLE static
#endif

#ifdef TEST
/**
 * @brief Reset module state for unit tests.
 *
 * Clears s_initialised and s_device_size to post-BSS values. Test-only.
 */
void qspi_flash_reset_for_test(void);
#endif

#endif /* QSPI_FLASH_DRIVER_H */
