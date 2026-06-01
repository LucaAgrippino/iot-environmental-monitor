/**
 * @file i2c_driver.h
 * @brief I2C bus driver — serialised register-addressed transactions.
 *
 * Provides II2c (per components.md): write-only, read-only, and combined
 * write-read transactions on the board's I2C bus. On the Field Device the
 * peripheral is I2C1 (consumer: TouchscreenDriver). On the Gateway the
 * peripheral is I2C2 (consumers: MagnetometerDriver, ImuDriver,
 * BarometerDriver, HumidityTempDriver).
 *
 * The driver depends only on CMSIS. It does NOT depend on FreeRTOS or any
 * other RTOS. Callers serialise themselves; the driver is not internally
 * synchronised (see docs/lld/drivers/i2c-driver.md §3 Synchronisation).
 *
 * Two implementation files share this header:
 *   i2c_driver_f4.c — STM32F469 (I2C v1: DR, SR1, SR2, CCR, TRISE)
 *   i2c_driver_l4.c — STM32L475 (I2C v2: TIMINGR, ISR, ICR, TXDR, RXDR)
 *
 * @note See docs/lld/drivers/i2c-driver.md for the full design.
 */

#ifndef I2C_DRIVER_H
#define I2C_DRIVER_H

#include <stdint.h>

/* --------------------------------------------------------------------- */
/* Error codes                                                            */
/* --------------------------------------------------------------------- */

/**
 * @brief Error codes returned by all I2cDriver operations.
 *
 * Naming follows the cross-cutting convention established in lld.md §3.2:
 * module_err_t, not _status_t.
 */
typedef enum
{
    I2C_ERR_OK = 0,       /**< Operation succeeded. */
    I2C_ERR_NACK = 1,     /**< Device did not acknowledge address or data byte. */
    I2C_ERR_TIMEOUT = 2,  /**< A flag did not assert within the timeout window. */
    I2C_ERR_BUS_BUSY = 3, /**< BUSY flag set at transaction start. */
} i2c_err_t;

/* --------------------------------------------------------------------- */
/* Public API                                                             */
/* --------------------------------------------------------------------- */

/**
 * @brief Initialise the I2C peripheral and configure bus pins.
 *
 * Configures SCL and SDA as alternate-function open-drain outputs. Sets the
 * clock speed to 400 kHz (fast mode). Enables the peripheral. Must be called
 * once from main() before the scheduler starts and before any consumer driver
 * calls i2c_write, i2c_read, or i2c_write_read.
 *
 * Peripheral: I2C1 on Field Device; I2C2 on Gateway.
 * Timing register values are placeholders pending clock-config.md resolution
 * (open items I2CD-O1, I2CD-O2 — see companion §8).
 *
 * @return I2C_ERR_OK on success.
 * @note Threading: task-context only, non-blocking.
 */
i2c_err_t i2c_init(void);

/**
 * @brief Perform a write-only I2C transaction.
 *
 * Generates START → address (write) → data bytes → STOP.
 * Used for writing configuration registers on sensor or touchscreen devices.
 *
 * @param dev_addr  7-bit device address (not shifted).
 * @param data      Pointer to bytes to transmit (must not be NULL).
 * @param len       Number of bytes to transmit (must be >= 1).
 * @return I2C_ERR_OK, I2C_ERR_NACK, I2C_ERR_TIMEOUT, or I2C_ERR_BUS_BUSY.
 * @note Threading: task-context only, may block. Not ISR-safe.
 * @note Pre-condition (debug builds): i2c_init() has been called successfully.
 */
i2c_err_t i2c_write(uint8_t dev_addr, const uint8_t *data, uint16_t len);

/**
 * @brief Perform a read-only I2C transaction.
 *
 * Generates START → address (read) → data bytes → STOP.
 * Used when a device advances its internal pointer automatically.
 *
 * @param dev_addr  7-bit device address (not shifted).
 * @param buf       Pointer to receive buffer (must not be NULL).
 * @param len       Number of bytes to receive (must be >= 1).
 * @return I2C_ERR_OK, I2C_ERR_NACK, I2C_ERR_TIMEOUT, or I2C_ERR_BUS_BUSY.
 * @note Threading: task-context only, may block. Not ISR-safe.
 * @note Pre-condition (debug builds): i2c_init() has been called successfully.
 */
i2c_err_t i2c_read(uint8_t dev_addr, uint8_t *buf, uint16_t len);

/**
 * @brief Perform a combined write-then-read I2C transaction.
 *
 * Generates START → address (write) → tx_data bytes → repeated START →
 * address (read) → rx_len bytes → STOP.
 *
 * The repeated START is issued atomically by the driver; the caller does not
 * manage it. This is the primary transaction type for register-addressed
 * sensor reads (P1: hardware knowledge stays in the driver).
 *
 * @param dev_addr  7-bit device address (not shifted).
 * @param tx_data   Bytes to transmit in write phase (must not be NULL).
 * @param tx_len    Number of bytes in write phase (must be >= 1).
 * @param rx_buf    Receive buffer for read phase (must not be NULL).
 * @param rx_len    Number of bytes to receive in read phase (must be >= 1).
 * @return I2C_ERR_OK, I2C_ERR_NACK, I2C_ERR_TIMEOUT, or I2C_ERR_BUS_BUSY.
 * @note Threading: task-context only, may block. Not ISR-safe.
 * @note Pre-condition (debug builds): i2c_init() has been called successfully.
 */
i2c_err_t i2c_write_read(uint8_t dev_addr, const uint8_t *tx_data, uint16_t tx_len, uint8_t *rx_buf,
                         uint16_t rx_len);

/* --------------------------------------------------------------------- */
/* Singleton vtable (LLD-D10)                                             */
/* --------------------------------------------------------------------- */

/**
 * @brief Vtable interface for I2cDriver (LLD-D10 singleton pattern).
 *
 * All consumers depend on this interface, not on the concrete driver.
 * The target-selected implementation file defines the singleton instance;
 * this header declares the extern pointer.
 */
typedef struct
{
    /* cppcheck-suppress unusedStructMember; used via i2c_driver singleton */
    i2c_err_t (*init)(void);
    /* cppcheck-suppress unusedStructMember; used via i2c_driver singleton */
    i2c_err_t (*write)(uint8_t dev_addr, const uint8_t *data, uint16_t len);
    /* cppcheck-suppress unusedStructMember; used via i2c_driver singleton */
    i2c_err_t (*read)(uint8_t dev_addr, uint8_t *buf, uint16_t len);
    /* cppcheck-suppress unusedStructMember; used via i2c_driver singleton */
    i2c_err_t (*write_read)(uint8_t dev_addr, const uint8_t *tx_data, uint16_t tx_len,
                            uint8_t *rx_buf, uint16_t rx_len);
} ii2c_t;

/** Singleton pointer — defined in the target-selected implementation file. */
extern const ii2c_t *const i2c_driver;

/* --------------------------------------------------------------------- */
/* Test-only hooks (#ifdef TEST)                                          */
/* --------------------------------------------------------------------- */

#ifdef TEST
/**
 * @brief Reset module state for unit tests.
 *
 * Clears s_i2c to its post-bss value so each Unity test case starts from
 * a clean slate. Test-only; not compiled into firmware builds. Follows the
 * project-wide convention established in DebugUartDriver and RtcDriver.
 */
void i2c_reset_for_test(void);
#endif

#endif /* I2C_DRIVER_H */
