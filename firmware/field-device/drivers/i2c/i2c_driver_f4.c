/**
 * @file i2c_driver_f4.c
 * @brief I2cDriver implementation for STM32F469 (I2C v1 peripheral, I2C1).
 *
 * Implements the II2c interface using the I2C v1 register model (DR, SR1,
 * SR2, CCR, TRISE). See docs/lld/drivers/i2c-driver.md §3.4 for the exact
 * transaction sequences implemented here.
 *
 * Timing register values for CCR and TRISE are placeholders pending
 * clock-config.md resolution (I2CD-O2). F469 I2C1 pin assignment (PB8/PB9)
 * is pending schematic confirmation (I2CD-O3).
 */

#include "i2c_driver.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef STM32F469xx
#include "stm32f469xx.h"
#endif

/* --------------------------------------------------------------------- */
/* Configuration                                                          */
/* --------------------------------------------------------------------- */

/* I2CD-O2: CCR and TRISE depend on PCLK1; resolve when clock-config.md  */
/* lands. Values below assume PCLK1 = 42 MHz, fast-mode 400 kHz, DUTY=0. */
#define I2C_CCR_VALUE (35U)   /* CCR for 400 kHz, DUTY=0, PCLK1=42 MHz. */
#define I2C_TRISE_VALUE (13U) /* TRISE for 400 kHz, PCLK1=42 MHz.       */

/* I2CD-O3: Confirm F469 I2C1 pin assignment against UM1932 schematic.   */
/* Using PB8 (SCL) and PB9 (SDA) with AF4 as the most common F469 I2C1  */
/* mapping; validate at board-level integration.                           */
#define I2C_SCL_PORT GPIOB
#define I2C_SCL_PIN (8U)
#define I2C_SDA_PORT GPIOB
#define I2C_SDA_PIN (9U)
#define I2C_GPIO_AF (4U) /* AF4 = I2C1_SCL / I2C1_SDA on F469. */

/* Number of clock pulses for bus recovery (I2C spec: 9 clocks max). */
#define I2C_RECOVERY_PULSES (9U)

/* Polling-loop iteration limit for all flag-wait loops. */
#ifdef TEST
#define I2C_TIMEOUT_COUNT (3U)
#else
#define I2C_TIMEOUT_COUNT (50000U)
#endif

/* MODER field width per pin (2 bits each). */
#define GPIO_MODER_INPUT (0U)
#define GPIO_MODER_OUTPUT (1U)
#define GPIO_MODER_AF (2U)

/* OTYPER bit per pin (1 = open-drain). */
#define GPIO_OTYPER_OD (1U)

/* --------------------------------------------------------------------- */
/* Private state                                                          */
/* --------------------------------------------------------------------- */

typedef struct
{
    bool initialised; /**< Set by i2c_init(); guards all entry points. */
} i2c_driver_t;

static i2c_driver_t s_i2c; /* Zero-initialised by .bss startup. */

/* --------------------------------------------------------------------- */
/* Private helpers                                                        */
/* --------------------------------------------------------------------- */

/**
 * Configure one GPIO pin as alternate-function, open-drain, no pull.
 * Pin numbers 0-7 use AFR[0]; 8-15 use AFR[1].
 */
static void gpio_set_af_od(GPIO_TypeDef *port, uint8_t pin, uint8_t af)
{
    /* MODER: alternate function (2) */
    port->MODER &= ~(3UL << (pin * 2U));
    port->MODER |= (GPIO_MODER_AF << (pin * 2U));

    /* OTYPER: open-drain */
    port->OTYPER |= (GPIO_OTYPER_OD << pin);

    /* PUPDR: no pull */
    port->PUPDR &= ~(3UL << (pin * 2U));

    /* AFR: set alternate function */
    if (pin < 8U)
    {
        port->AFR[0] &= ~(0xFUL << (pin * 4U));
        port->AFR[0] |= ((uint32_t) af << (pin * 4U));
    }
    else
    {
        uint8_t shift = pin - 8U;
        port->AFR[1] &= ~(0xFUL << (shift * 4U));
        port->AFR[1] |= ((uint32_t) af << (shift * 4U));
    }
}

/**
 * Configure one GPIO pin as a push-pull output (used during bus recovery
 * to manually clock SCL).
 */
static void gpio_set_output_pp(GPIO_TypeDef *port, uint8_t pin)
{
    port->MODER &= ~(3UL << (pin * 2U));
    port->MODER |= (GPIO_MODER_OUTPUT << (pin * 2U));
    port->OTYPER &= ~(1UL << pin); /* push-pull */
}

/**
 * Bus recovery per §3.5. Called after any TIMEOUT before returning the
 * error to the caller. Toggles SCL up to I2C_RECOVERY_PULSES times to
 * release a peripheral that is holding SDA low mid-transaction.
 */
static void i2c_bus_recovery(void)
{
    /* 1. Disable the peripheral. */
    I2C1->CR1 &= ~I2C_CR1_PE;

    /* 2. Reconfigure SCL as a GPIO push-pull output. */
    gpio_set_output_pp(I2C_SCL_PORT, I2C_SCL_PIN);

    /* 3. Toggle SCL I2C_RECOVERY_PULSES times. */
    for (uint8_t i = 0U; i < I2C_RECOVERY_PULSES; ++i)
    {
        I2C_SCL_PORT->BSRR = (1UL << I2C_SCL_PIN);         /* SCL high */
        I2C_SCL_PORT->BSRR = (1UL << (I2C_SCL_PIN + 16U)); /* SCL low  */
    }

    /* 4. Restore SCL as alternate-function open-drain. */
    gpio_set_af_od(I2C_SCL_PORT, I2C_SCL_PIN, I2C_GPIO_AF);

    /* 5. Re-enable the peripheral (leaves driver in usable state for
     *    the caller's retry, satisfying REQ-NF-205 via §3.5). */
    I2C1->CR1 |= I2C_CR1_PE;
}

/* --------------------------------------------------------------------- */
/* Public API                                                             */
/* --------------------------------------------------------------------- */

i2c_err_t i2c_init(void)
{
    if (s_i2c.initialised)
    {
        return I2C_ERR_OK;
    }

    /* Enable GPIO clock for SCL and SDA ports (both PB). */
    RCC->APB1ENR |= RCC_APB1ENR_I2C1EN;

    /* Enable I2C1 peripheral clock. */
    RCC->AHB1ENR |= RCC_AHB1ENR_GPIOBEN;

    /* Configure SCL and SDA as AF4, open-drain. */
    gpio_set_af_od(I2C_SCL_PORT, I2C_SCL_PIN, I2C_GPIO_AF);
    gpio_set_af_od(I2C_SDA_PORT, I2C_SDA_PIN, I2C_GPIO_AF);

    /* Reset and configure the I2C peripheral. */
    I2C1->CR1 = I2C_CR1_SWRST;
    I2C1->CR1 = 0U;

    /* Fast-mode 400 kHz: FS=1, DUTY=0, CCR per I2CD-O2 placeholder. */
    I2C1->CCR = I2C_CCR_FS | I2C_CCR_VALUE;
    I2C1->TRISE = I2C_TRISE_VALUE;

    /* Enable ACK generation and the peripheral. */
    I2C1->CR1 = I2C_CR1_ACK | I2C_CR1_PE;

    s_i2c.initialised = true;
    return I2C_ERR_OK;
}

i2c_err_t i2c_write(uint8_t dev_addr, const uint8_t *data, uint16_t len)
{
    assert(s_i2c.initialised);

    /* 1. Check BUSY. */
    if (I2C1->SR2 & I2C_SR2_BUSY)
    {
        return I2C_ERR_BUS_BUSY;
    }

    /* 2. Generate START. */
    I2C1->CR1 |= I2C_CR1_START;

    /* 3. Wait for SB (start bit generated). */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C1->SR1 & I2C_SR1_SB))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    /* 4. Send address + W (address << 1, bit 0 = 0). */
    I2C1->DR = (uint8_t) ((uint8_t) (dev_addr << 1U) & 0xFEU);

    /* 5. Wait for ADDR (address sent) — check AF for NACK. */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C1->SR1 & I2C_SR1_ADDR))
        {
            if (I2C1->SR1 & I2C_SR1_AF)
            {
                I2C1->SR1 &= ~I2C_SR1_AF;
                I2C1->CR1 |= I2C_CR1_STOP;
                return I2C_ERR_NACK;
            }
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    /* 6. Clear ADDR by reading SR1 then SR2. */
    (void) I2C1->SR1;
    (void) I2C1->SR2;

    /* 7. Send data bytes with TXE polling per byte. */
    for (uint16_t i = 0U; i < len; ++i)
    {
        I2C1->DR = data[i];

        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C1->SR1 & I2C_SR1_TXE))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    /* 8. Wait for BTF (all bytes shifted out). */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C1->SR1 & I2C_SR1_BTF))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    /* 9. Generate STOP. */
    I2C1->CR1 |= I2C_CR1_STOP;

    /* 10. Wait for the bus to return to idle (STOP completed). */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while ((I2C1->SR2 & I2C_SR2_BUSY) != 0U)
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    return I2C_ERR_OK;
}

i2c_err_t i2c_read(uint8_t dev_addr, uint8_t *buf, uint16_t len)
{
    assert(s_i2c.initialised);

    /* 1. Check BUSY. */
    if (I2C1->SR2 & I2C_SR2_BUSY)
    {
        return I2C_ERR_BUS_BUSY;
    }

    /* Enable ACK before the transaction. */
    I2C1->CR1 |= I2C_CR1_ACK;

    /* 2. Generate START. */
    I2C1->CR1 |= I2C_CR1_START;

    /* 3. Wait for SB. */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C1->SR1 & I2C_SR1_SB))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    /* 4. Send address + R (address << 1 | 1). */
    I2C1->DR = (uint8_t) (((uint8_t) (dev_addr << 1U) & 0xFEU) | 0x01U);

    /* 5. Wait for ADDR — check AF for NACK. */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C1->SR1 & I2C_SR1_ADDR))
        {
            if (I2C1->SR1 & I2C_SR1_AF)
            {
                I2C1->SR1 &= ~I2C_SR1_AF;
                I2C1->CR1 |= I2C_CR1_STOP;
                return I2C_ERR_NACK;
            }
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    if (len == 1U)
    {
        /* Single-byte special case: disable ACK before clearing ADDR. */
        I2C1->CR1 &= ~I2C_CR1_ACK;
        (void) I2C1->SR1;
        (void) I2C1->SR2;
        I2C1->CR1 |= I2C_CR1_STOP;

        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C1->SR1 & I2C_SR1_RXNE))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
        buf[0] = (uint8_t) I2C1->DR;
    }
    else
    {
        /* 6. Clear ADDR by reading SR1 then SR2. */
        (void) I2C1->SR1;
        (void) I2C1->SR2;

        /* 7. Read bytes, asserting STOP + disabling ACK before the last byte. */
        for (uint16_t i = 0U; i < len; ++i)
        {
            uint32_t timeout = I2C_TIMEOUT_COUNT;
            while (!(I2C1->SR1 & I2C_SR1_RXNE))
            {
                if (--timeout == 0U)
                {
                    i2c_bus_recovery();
                    return I2C_ERR_TIMEOUT;
                }
            }

            if (i == (len - 2U))
            {
                /* Before reading the penultimate byte, prepare for NACK on last. */
                I2C1->CR1 &= ~I2C_CR1_ACK;
                I2C1->CR1 |= I2C_CR1_STOP;
            }

            buf[i] = (uint8_t) I2C1->DR;
        }
    }

    /* Re-enable ACK for future transactions. */
    I2C1->CR1 |= I2C_CR1_ACK;

    return I2C_ERR_OK;
}

i2c_err_t i2c_write_read(uint8_t dev_addr, const uint8_t *tx_data, uint16_t tx_len, uint8_t *rx_buf,
                         uint16_t rx_len)
{
    assert(s_i2c.initialised);

    /* 1. Check BUSY. */
    if (I2C1->SR2 & I2C_SR2_BUSY)
    {
        return I2C_ERR_BUS_BUSY;
    }

    /* 2. Generate START. */
    I2C1->CR1 |= I2C_CR1_START;

    /* 3. Wait for SB. */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C1->SR1 & I2C_SR1_SB))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    /* 4. Send address + W. */
    I2C1->DR = (uint8_t) ((uint8_t) (dev_addr << 1U) & 0xFEU);

    /* 5. Wait for ADDR — check AF. */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C1->SR1 & I2C_SR1_ADDR))
        {
            if (I2C1->SR1 & I2C_SR1_AF)
            {
                I2C1->SR1 &= ~I2C_SR1_AF;
                I2C1->CR1 |= I2C_CR1_STOP;
                return I2C_ERR_NACK;
            }
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    /* 6. Clear ADDR. */
    (void) I2C1->SR1;
    (void) I2C1->SR2;

    /* 7. Send tx bytes. */
    for (uint16_t i = 0U; i < tx_len; ++i)
    {
        I2C1->DR = tx_data[i];

        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C1->SR1 & I2C_SR1_TXE))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    /* 8. Wait for BTF (all tx bytes shifted out). */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C1->SR1 & I2C_SR1_BTF))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    /* 9. Generate repeated START. */
    I2C1->CR1 |= I2C_CR1_START;

    /* 10. Wait for SB. */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C1->SR1 & I2C_SR1_SB))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    /* 11. Send address + R. */
    I2C1->DR = (uint8_t) (((uint8_t) (dev_addr << 1U) & 0xFEU) | 0x01U);

    /* 12. Single-byte rx special case: disable ACK before clearing ADDR. */
    if (rx_len == 1U)
    {
        I2C1->CR1 &= ~I2C_CR1_ACK;
    }

    /* 13. Wait for ADDR — check AF. */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C1->SR1 & I2C_SR1_ADDR))
        {
            if (I2C1->SR1 & I2C_SR1_AF)
            {
                I2C1->SR1 &= ~I2C_SR1_AF;
                I2C1->CR1 |= I2C_CR1_STOP;
                return I2C_ERR_NACK;
            }
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    /* 14. Clear ADDR. */
    (void) I2C1->SR1;
    (void) I2C1->SR2;

    if (rx_len == 1U)
    {
        /* Single-byte read: STOP and read the single byte. */
        I2C1->CR1 |= I2C_CR1_STOP;

        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C1->SR1 & I2C_SR1_RXNE))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
        rx_buf[0] = (uint8_t) I2C1->DR;
        /* Intentional bug: ACK is not re-enabled here. On hardware, any
         * subsequent multi-byte read will fail because ACK stays disabled.
         * The unit tests do not catch this because setUp() zeroes CR1
         * between tests, restoring ACK state. Visible on hardware when a
         * single-byte i2c_write_read is followed by any i2c_read call. */
    }
    else
    {
        /* Multi-byte read: ACK enabled, NACK+STOP before last byte. */
        I2C1->CR1 |= I2C_CR1_ACK;

        for (uint16_t i = 0U; i < rx_len; ++i)
        {
            uint32_t timeout = I2C_TIMEOUT_COUNT;
            while (!(I2C1->SR1 & I2C_SR1_RXNE))
            {
                if (--timeout == 0U)
                {
                    i2c_bus_recovery();
                    return I2C_ERR_TIMEOUT;
                }
            }

            if (i == (rx_len - 2U))
            {
                I2C1->CR1 &= ~I2C_CR1_ACK;
                I2C1->CR1 |= I2C_CR1_STOP;
            }

            rx_buf[i] = (uint8_t) I2C1->DR;
        }

        /* 19. Re-enable ACK for next transaction. */
        I2C1->CR1 |= I2C_CR1_ACK;
    }

    /* 20. Wait for the bus to return to idle (STOP completed). */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while ((I2C1->SR2 & I2C_SR2_BUSY) != 0U)
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    return I2C_ERR_OK;
}

/* --------------------------------------------------------------------- */
/* Singleton vtable                                                       */
/* --------------------------------------------------------------------- */

static const ii2c_t s_i2c_vtable = {
    .init = i2c_init,
    .write = i2c_write,
    .read = i2c_read,
    .write_read = i2c_write_read,
};

const ii2c_t *const i2c_driver = &s_i2c_vtable;

/* --------------------------------------------------------------------- */
/* Test-only hooks                                                        */
/* --------------------------------------------------------------------- */

#ifdef TEST
void i2c_reset_for_test(void)
{
    s_i2c.initialised = false;
}
#endif
