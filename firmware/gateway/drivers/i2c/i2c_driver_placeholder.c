/**
 * @file i2c_driver_placeholder.c
 * @brief Weak stand-in for the unbuilt Gateway I2cDriver (STM32L475, I2C2).
 *
 * HumidityTempDriver and BarometerDriver call through the `i2c_driver`
 * singleton. The L475 implementation (i2c_driver_l4.c, docs/lld/drivers/
 * i2c-driver.md) does not exist yet, so without this file the Gateway image
 * would fail to link with an undefined `i2c_driver`.
 *
 * Every definition here is weak: the real i2c_driver_l4.c replaces it
 * simply by being added to the build. Until then every transaction reports
 * I2C_ERR_NACK, which the sensor drivers surface as *_ERR_I2C — an honest
 * "no sensor reachable" rather than fabricated data.
 *
 * Deliberately NOT named i2c_driver.c: Ceedling auto-links a .c whose
 * basename matches an #included header, and unit tests supply their own
 * i2c_driver vtable.
 */

#include "i2c/i2c_driver.h"

#include <stddef.h>

static i2c_err_t placeholder_init(void)
{
    return I2C_ERR_NACK;
}

static i2c_err_t placeholder_write(uint8_t dev_addr, const uint8_t *data, uint16_t len)
{
    (void) dev_addr;
    (void) data;
    (void) len;
    return I2C_ERR_NACK;
}

static i2c_err_t placeholder_read(uint8_t dev_addr, uint8_t *buf, uint16_t len)
{
    (void) dev_addr;
    (void) buf;
    (void) len;
    return I2C_ERR_NACK;
}

static i2c_err_t placeholder_write_read(uint8_t dev_addr, const uint8_t *tx_data, uint16_t tx_len,
                                        uint8_t *rx_buf, uint16_t rx_len)
{
    (void) dev_addr;
    (void) tx_data;
    (void) tx_len;
    (void) rx_buf;
    (void) rx_len;
    return I2C_ERR_NACK;
}

static const ii2c_t s_placeholder_vtable = {
    .init = placeholder_init,
    .write = placeholder_write,
    .read = placeholder_read,
    .write_read = placeholder_write_read,
};

__attribute__((weak)) const ii2c_t *const i2c_driver = &s_placeholder_vtable;
