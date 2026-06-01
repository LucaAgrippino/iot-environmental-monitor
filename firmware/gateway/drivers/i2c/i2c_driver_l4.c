/**
 * @file i2c_driver_l4.c
 * @brief I2cDriver implementation for STM32L475 (I2C v2 peripheral, I2C2).
 *
 * Implements the II2c interface using the I2C v2 register model (TIMINGR,
 * ISR, ICR, TXDR, RXDR). See docs/lld/drivers/i2c-driver.md §3.4 for the
 * exact transaction sequences implemented here.
 *
 * Bus pins: PB10 (I2C2_SCL) and PB11 (I2C2_SDA) per UM2153 schematic.
 * TIMINGR value is a placeholder pending clock-config.md resolution (I2CD-O1).
 */

#include "i2c_driver.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* --------------------------------------------------------------------- */
/* Configuration                                                          */
/* --------------------------------------------------------------------- */

/* I2CD-O1: TIMINGR for I2C2 on L475 at 400 kHz depends on I2CCLK source. */
/* Placeholder: value for HSI16 (16 MHz) input, 400 kHz fast mode.         */
/* Use STM32CubeIDE I2C timing tool once clock-config.md is finalised.     */
#define I2C_TIMINGR_VALUE (0x00303D5BU)

/* I2C2 bus pins per UM2153. AF4 = I2C2 on L475. */
#define I2C_SCL_PORT GPIOB
#define I2C_SCL_PIN (10U)
#define I2C_SDA_PORT GPIOB
#define I2C_SDA_PIN (11U)
#define I2C_GPIO_AF (4U)

/* Number of clock pulses for bus recovery. */
#define I2C_RECOVERY_PULSES (9U)

/* Polling-loop iteration limit for all flag-wait loops. */
#ifdef TEST
#define I2C_TIMEOUT_COUNT (3U)
#else
#define I2C_TIMEOUT_COUNT (50000U)
#endif

/* MODER field values per pin (2 bits). */
#define GPIO_MODER_OUTPUT (1U)
#define GPIO_MODER_AF (2U)

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

/** Configure one GPIO pin as alternate-function, open-drain, no pull. */
static void gpio_set_af_od(GPIO_TypeDef *port, uint8_t pin, uint8_t af)
{
    port->MODER &= ~(3UL << (pin * 2U));
    port->MODER |= (GPIO_MODER_AF << (pin * 2U));
    port->OTYPER |= (1UL << pin); /* open-drain */
    port->PUPDR &= ~(3UL << (pin * 2U));

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

/** Configure one GPIO pin as a push-pull output (bus recovery). */
static void gpio_set_output_pp(GPIO_TypeDef *port, uint8_t pin)
{
    port->MODER &= ~(3UL << (pin * 2U));
    port->MODER |= (GPIO_MODER_OUTPUT << (pin * 2U));
    port->OTYPER &= ~(1UL << pin); /* push-pull */
}

/**
 * Bus recovery per §3.5. Called after any TIMEOUT before returning the
 * error to the caller.
 */
static void i2c_bus_recovery(void)
{
    I2C2->CR1 &= ~I2C_CR1_PE;

    gpio_set_output_pp(I2C_SCL_PORT, I2C_SCL_PIN);

    for (uint8_t i = 0U; i < I2C_RECOVERY_PULSES; ++i)
    {
        I2C_SCL_PORT->BSRR = (1UL << I2C_SCL_PIN);
        I2C_SCL_PORT->BSRR = (1UL << (I2C_SCL_PIN + 16U));
    }

    gpio_set_af_od(I2C_SCL_PORT, I2C_SCL_PIN, I2C_GPIO_AF);

    I2C2->CR1 |= I2C_CR1_PE;
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

    /* Enable GPIOB and I2C2 clocks. */
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_I2C2EN;

    /* Configure PB10 (SCL) and PB11 (SDA) as AF4, open-drain. */
    gpio_set_af_od(I2C_SCL_PORT, I2C_SCL_PIN, I2C_GPIO_AF);
    gpio_set_af_od(I2C_SDA_PORT, I2C_SDA_PIN, I2C_GPIO_AF);

    /* Disable peripheral before configuring timing. */
    I2C2->CR1 &= ~I2C_CR1_PE;

    /* Set timing register (placeholder — I2CD-O1). */
    I2C2->TIMINGR = I2C_TIMINGR_VALUE;

    /* Enable the peripheral. */
    I2C2->CR1 |= I2C_CR1_PE;

    s_i2c.initialised = true;
    return I2C_ERR_OK;
}

i2c_err_t i2c_write(uint8_t dev_addr, const uint8_t *data, uint16_t len)
{
    assert(s_i2c.initialised);

    /* 1. Check BUSY. */
    if (I2C2->ISR & I2C_ISR_BUSY)
    {
        return I2C_ERR_BUS_BUSY;
    }

    /* 2. Configure CR2 and generate START.
     *    SADD = dev_addr << 1 (7-bit mode, address in bits [7:1]).
     *    NBYTES = len, RD_WRN = 0 (write), AUTOEND = 1, START = 1.    */
    I2C2->CR2 = ((uint32_t) (dev_addr << 1U) & I2C_CR2_SADD) |
                (((uint32_t) len << I2C_CR2_NBYTES_Pos) & I2C_CR2_NBYTES) | I2C_CR2_AUTOEND |
                I2C_CR2_START;

    /* 3. Send each data byte: poll TXIS or NACKF. */
    for (uint16_t i = 0U; i < len; ++i)
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C2->ISR & I2C_ISR_TXIS))
        {
            if (I2C2->ISR & I2C_ISR_NACKF)
            {
                I2C2->ICR = I2C_ICR_NACKCF;
                return I2C_ERR_NACK;
            }
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
        I2C2->TXDR = data[i];
    }

    /* 4. Poll STOPF (AUTOEND=1 generates STOP automatically after last byte). */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C2->ISR & I2C_ISR_STOPF))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    /* 5. Clear STOPF. */
    I2C2->ICR = I2C_ICR_STOPCF;

    return I2C_ERR_OK;
}

i2c_err_t i2c_read(uint8_t dev_addr, uint8_t *buf, uint16_t len)
{
    assert(s_i2c.initialised);

    /* 1. Check BUSY. */
    if (I2C2->ISR & I2C_ISR_BUSY)
    {
        return I2C_ERR_BUS_BUSY;
    }

    /* 2. Configure CR2 and generate START.
     *    RD_WRN = 1 (read), AUTOEND = 1.                               */
    I2C2->CR2 = ((uint32_t) (dev_addr << 1U) & I2C_CR2_SADD) |
                (((uint32_t) len << I2C_CR2_NBYTES_Pos) & I2C_CR2_NBYTES) | I2C_CR2_RD_WRN |
                I2C_CR2_AUTOEND | I2C_CR2_START;

    /* 3. Read each byte: poll RXNE. */
    for (uint16_t i = 0U; i < len; ++i)
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C2->ISR & I2C_ISR_RXNE))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
        buf[i] = (uint8_t) I2C2->RXDR;
    }

    /* 4. Poll STOPF (AUTOEND generated it after the last byte). */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C2->ISR & I2C_ISR_STOPF))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    /* 5. Clear STOPF. */
    I2C2->ICR = I2C_ICR_STOPCF;

    return I2C_ERR_OK;
}

i2c_err_t i2c_write_read(uint8_t dev_addr, const uint8_t *tx_data, uint16_t tx_len, uint8_t *rx_buf,
                         uint16_t rx_len)
{
    assert(s_i2c.initialised);

    /* 1. Check BUSY. */
    if (I2C2->ISR & I2C_ISR_BUSY)
    {
        return I2C_ERR_BUS_BUSY;
    }

    /* 2. Write phase: AUTOEND=0 holds the bus between phases. */
    I2C2->CR2 = ((uint32_t) (dev_addr << 1U) & I2C_CR2_SADD) |
                (((uint32_t) tx_len << I2C_CR2_NBYTES_Pos) & I2C_CR2_NBYTES) |
                I2C_CR2_START; /* RD_WRN=0, AUTOEND=0 */

    /* 3. Send tx bytes. */
    for (uint16_t i = 0U; i < tx_len; ++i)
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C2->ISR & I2C_ISR_TXIS))
        {
            if (I2C2->ISR & I2C_ISR_NACKF)
            {
                I2C2->ICR = I2C_ICR_NACKCF;
                return I2C_ERR_NACK;
            }
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
        I2C2->TXDR = tx_data[i];
    }

    /* 4. Poll TC (transfer complete, AUTOEND=0): bus held, no STOP yet. */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C2->ISR & I2C_ISR_TC))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    /* 5. Read phase: repeated START, RD_WRN=1, AUTOEND=1. */
    I2C2->CR2 = ((uint32_t) (dev_addr << 1U) & I2C_CR2_SADD) |
                (((uint32_t) rx_len << I2C_CR2_NBYTES_Pos) & I2C_CR2_NBYTES) | I2C_CR2_RD_WRN |
                I2C_CR2_AUTOEND | I2C_CR2_START;

    /* 6. Read rx bytes. */
    for (uint16_t i = 0U; i < rx_len; ++i)
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C2->ISR & I2C_ISR_RXNE))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
        rx_buf[i] = (uint8_t) I2C2->RXDR;
    }

    /* 7. Poll STOPF. */
    {
        uint32_t timeout = I2C_TIMEOUT_COUNT;
        while (!(I2C2->ISR & I2C_ISR_STOPF))
        {
            if (--timeout == 0U)
            {
                i2c_bus_recovery();
                return I2C_ERR_TIMEOUT;
            }
        }
    }

    /* 8. Clear STOPF. */
    I2C2->ICR = I2C_ICR_STOPCF;

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
