/**
 * @file spi_driver.h
 * @brief SPI driver — full-duplex word transfer over SPI3 (Gateway).
 *
 * Provides ISpi (per components.md): create an SPI3 instance and exchange
 * 16-bit words with an SPI-connected peripheral. The sole consumer is
 * WifiDriver, which uses this driver to talk to the ISM43362-M3G-L44 WiFi
 * module. NSS (chip-select) and DATARDY are managed by WifiDriver via
 * GpioDriver — this driver never touches them.
 *
 * @note See docs/lld/drivers/spi-driver.md for the full design specification.
 */

#ifndef SPI_DRIVER_H
#define SPI_DRIVER_H

#include <stdbool.h>
#include <stdint.h>

#include "stm32l475xx.h"

/**
 * @brief Opaque handle to an SPI driver instance.
 */
typedef struct spi_inst *spi_handle_t;

/**
 * @brief Error codes returned by all SpiDriver operations.
 */
typedef enum
{
    SPI_ERR_OK = 0,          /**< Operation succeeded. */
    SPI_ERR_TIMEOUT = 1,     /**< TXE, RXNE, or BSY flag did not assert/clear
                                   within the timeout window. */
    SPI_ERR_NULL_PTR = 2,    /**< A required pointer argument was NULL. */
    SPI_ERR_NO_RESOURCE = 3, /**< Static instance pool exhausted. */
} spi_err_t;

/**
 * @brief SPI instance configuration.
 */
typedef struct
{
    SPI_TypeDef *instance; /**< SPI peripheral (e.g. SPI3). */
} spi_config_t;

/**
 * @brief Create and initialise an SPI driver instance.
 *
 * Enables the SPI peripheral clock, configures SCK, MOSI, MISO as
 * alternate-function outputs (AF6 for SPI3 on the L475).
 * Mode 0 (CPOL=0, CPHA=0), 16-bit data frame (DS=1111), MSB first.
 * Sets FRXTH=0 in CR2 so RXNE fires after a full 16-bit word is
 * received (see companion §3.3).
 * Clock speed: 10 MHz (BR=010, PCLK1/8; see companion §4.2).
 * Does NOT configure or assert NSS; NSS is managed by WifiDriver
 * via GpioDriver.
 *
 * Must be called once from main() before any spi_transceive call.
 *
 * @param[in]  config  SPI peripheral to use.
 * @param[out] handle  Receives the created handle on success.
 * @return SPI_ERR_OK on success; SPI_ERR_NULL_PTR if config or handle
 *         is NULL; SPI_ERR_NO_RESOURCE if the static pool is exhausted.
 * @note Threading: task-context only. Must be called before the
 *       scheduler starts.
 */
spi_err_t spi_create(const spi_config_t *config, spi_handle_t *handle);

/**
 * @brief Exchange 16-bit words over SPI.
 *
 * Full-duplex transfer: for each word, one 16-bit word is shifted out
 * on MOSI and one is shifted in on MISO simultaneously.
 *
 * If tx_buf is NULL, dummy words (0x0000) are transmitted.
 * If rx_buf is NULL, received words are discarded.
 * Both tx_buf and rx_buf NULL is a caller error and returns
 * SPI_ERR_NULL_PTR.
 *
 * NSS must be asserted by the caller (WifiDriver via GpioDriver)
 * before calling this function, and de-asserted after it returns.
 * SpiDriver never touches NSS.
 *
 * @param[in]  handle  Handle from spi_create().
 * @param[in]  tx_buf  Pointer to words to transmit, or NULL for dummy.
 * @param[out] rx_buf  Pointer to receive buffer, or NULL to discard.
 * @param[in]  len     Number of 16-bit words to exchange.
 * @return SPI_ERR_OK on success; SPI_ERR_NULL_PTR if handle is NULL or
 *         both buffers are NULL; SPI_ERR_TIMEOUT if a flag does not
 *         assert/clear within the timeout window.
 * @note Threading: task-context only, blocking. Not ISR-safe.
 */
spi_err_t spi_transceive(spi_handle_t handle, const uint16_t *tx_buf, uint16_t *rx_buf,
                         uint16_t len);

#endif /* SPI_DRIVER_H */
