/**
 * @file i2c_driver_l4.c
 * @brief I2cDriver implementation for STM32L475 (I2C v2 peripheral, I2C2).
 *
 * Implements the II2c interface using the I2C v2 register model (TIMINGR,
 * ISR, ICR, TXDR, RXDR) with polled, bounded flag waits and no RTOS
 * dependency. Transaction sequences follow docs/lld/drivers/i2c-driver.md
 * §3.4; bus recovery follows §3.5.
 *
 * Bus: I2C2 on PB10 (SCL) / PB11 (SDA), AF4, open-drain, external pull-ups.
 * Consumers: HTS221, LPS22HB, LIS3MDL, LSM6DSL, all from SensorTask.
 *
 * The Field Device counterpart (i2c_driver_f4.c, I2C v1) lives in a separate
 * CubeIDE project; the two share only the interface header's shape.
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

/**
 * I2CD-O1: placeholder TIMINGR for I2CCLK = PCLK1 = 80 MHz (RCC_CCIPR
 * I2C2SEL reset value), 400 kHz fast mode: PRESC=0 (12.5 ns), SCLDEL=7,
 * SDADEL=0, SCLH=41, SCLL=145. Confirm SCL frequency and edge timing on the
 * bench (TC-HW-I2C-005 times a transaction) before treating it as final.
 */
#define I2C_TIMINGR_VALUE (0x00702991UL)

#define I2C_SCL_PIN (10U)
#define I2C_SDA_PIN (11U)
#define I2C_GPIO_AF (4U) /* AF4 = I2C2_SCL / I2C2_SDA on L475. */

/** I2C v2 NBYTES is an 8-bit field. */
#define I2C_MAX_TRANSFER_BYTES (255U)
#define I2C_MAX_7BIT_ADDR (0x7FU)

/** Bus-recovery pulse count (I2C spec: 9 clocks release any slave). */
#define I2C_RECOVERY_PULSES (9U)

/** Consecutive BUSY-at-entry results before the bus is declared stuck. */
#define I2C_BUSY_STRIKES_LIMIT (2U)

/* Polling-loop iteration limit for every flag wait, and recovery half-period. */
#ifdef TEST
#define I2C_TIMEOUT_COUNT (3U)
#define I2C_RECOVERY_HALF_PERIOD_LOOPS (1U)
#else
#define I2C_TIMEOUT_COUNT (50000U)
/* ~50 us at 80 MHz => SCL ~10 kHz during recovery. */
#define I2C_RECOVERY_HALF_PERIOD_LOOPS (1000U)
#endif

/* GPIO field encodings. */
#define GPIO_MODER_INPUT (0UL)
#define GPIO_MODER_OUTPUT (1UL)
#define GPIO_MODER_AF (2UL)
#define GPIO_MODE_MASK (3UL)
#define GPIO_SPEED_VERY_HIGH (3UL)
#define GPIO_AFR_MASK (0xFUL)
#define GPIO_AFR_HIGH_PIN_BASE (8U)
#define GPIO_BSRR_RESET_SHIFT (16U)

#define I2C_ADDR_SHIFT (1U)
#define I2C_CR2_SADD_MASK (0xFEUL)

/* --------------------------------------------------------------------- */
/* Private state                                                          */
/* --------------------------------------------------------------------- */

typedef struct
{
    bool initialised;         /**< Set by i2c_init(); guards all entry points. */
    uint8_t busy_strikes;     /**< Consecutive BUSY-at-entry results. */
    uint32_t recovery_pulses; /**< SCL pulses in the most recent recovery. */
} i2c_driver_t;

static i2c_driver_t s_i2c; /* Zero-initialised by .bss startup. */

/* --------------------------------------------------------------------- */
/* GPIO helpers (SCL/SDA live on GPIOB)                                   */
/* --------------------------------------------------------------------- */

static void prv_pin_mode(uint8_t pin, uint32_t mode)
{
    GPIOB->MODER &= ~(GPIO_MODE_MASK << (pin * 2U));
    GPIOB->MODER |= (mode << (pin * 2U));
}

/** Alternate function, open-drain, very high speed, no pull (board has pull-ups). */
static void prv_pin_af_od(uint8_t pin, uint8_t af)
{
    const uint32_t idx = (pin < GPIO_AFR_HIGH_PIN_BASE) ? 0U : 1U;
    const uint8_t shift = (uint8_t) ((pin % GPIO_AFR_HIGH_PIN_BASE) * 4U);

    prv_pin_mode(pin, GPIO_MODER_AF);
    GPIOB->OTYPER |= (1UL << pin);
    GPIOB->OSPEEDR |= (GPIO_SPEED_VERY_HIGH << (pin * 2U));
    GPIOB->PUPDR &= ~(GPIO_MODE_MASK << (pin * 2U));
    GPIOB->AFR[idx] &= ~(GPIO_AFR_MASK << shift);
    GPIOB->AFR[idx] |= ((uint32_t) af << shift);
}

static void prv_delay_half_period(void)
{
    for (volatile uint32_t i = 0U; i < I2C_RECOVERY_HALF_PERIOD_LOOPS; ++i)
    {
    }
}

/* --------------------------------------------------------------------- */
/* Bus recovery (§3.5)                                                    */
/* --------------------------------------------------------------------- */

static bool prv_sda_is_high(void)
{
    return (GPIOB->IDR & (1UL << I2C_SDA_PIN)) != 0U;
}

/**
 * Release a slave that is holding SDA low mid-byte. Disables the peripheral
 * (which also clears its internal state machine), clocks SCL up to nine times
 * as an open-drain GPIO while sampling SDA, issues a manual STOP if SDA
 * released, then restores the pins and re-enables the peripheral so the
 * caller's retry (REQ-NF-205) finds a usable bus.
 */
static void prv_bus_recovery(void)
{
    const uint32_t scl_set = (1UL << I2C_SCL_PIN);
    const uint32_t scl_clr = (1UL << (I2C_SCL_PIN + GPIO_BSRR_RESET_SHIFT));
    const uint32_t sda_set = (1UL << I2C_SDA_PIN);
    const uint32_t sda_clr = (1UL << (I2C_SDA_PIN + GPIO_BSRR_RESET_SHIFT));
    uint32_t pulses = 0U;

    I2C2->CR1 &= ~I2C_CR1_PE;

    GPIOB->BSRR = scl_set; /* open-drain high = released, set before driving */
    prv_pin_mode(I2C_SCL_PIN, GPIO_MODER_OUTPUT);
    prv_pin_mode(I2C_SDA_PIN, GPIO_MODER_INPUT);

    while ((pulses < I2C_RECOVERY_PULSES) && !prv_sda_is_high())
    {
        GPIOB->BSRR = scl_clr;
        prv_delay_half_period();
        GPIOB->BSRR = scl_set;
        prv_delay_half_period();
        pulses++;
    }

    if (prv_sda_is_high())
    {
        /* Manual STOP: SDA low -> high while SCL is high. */
        GPIOB->BSRR = sda_clr;
        prv_pin_mode(I2C_SDA_PIN, GPIO_MODER_OUTPUT);
        prv_delay_half_period();
        GPIOB->BSRR = sda_set;
        prv_delay_half_period();
    }

    prv_pin_af_od(I2C_SCL_PIN, I2C_GPIO_AF);
    prv_pin_af_od(I2C_SDA_PIN, I2C_GPIO_AF);

    s_i2c.recovery_pulses = pulses;
    I2C2->CR1 |= I2C_CR1_PE;
}

static i2c_err_t prv_timeout(void)
{
    prv_bus_recovery();
    return I2C_ERR_TIMEOUT;
}

/**
 * A NACK makes the master send STOP by itself. Wait (bounded) for STOPF so a
 * stale flag cannot satisfy the next transaction's STOP wait, then clear both
 * flags and flush any byte still queued in TXDR.
 */
static i2c_err_t prv_nack(void)
{
    for (uint32_t t = I2C_TIMEOUT_COUNT; t > 0U; --t)
    {
        if ((I2C2->ISR & I2C_ISR_STOPF) != 0U)
        {
            break;
        }
    }
    I2C2->ICR = I2C_ICR_NACKCF | I2C_ICR_STOPCF;
    I2C2->ISR |= I2C_ISR_TXE;
    return I2C_ERR_NACK;
}

/** Wait for an ISR flag; NACKF aborts the wait, a timeout triggers recovery. */
static i2c_err_t prv_wait_flag(uint32_t flag)
{
    for (uint32_t t = I2C_TIMEOUT_COUNT; t > 0U; --t)
    {
        const uint32_t isr = I2C2->ISR;

        if ((isr & I2C_ISR_NACKF) != 0U)
        {
            return prv_nack();
        }
        if ((isr & flag) != 0U)
        {
            return I2C_ERR_OK;
        }
    }
    return prv_timeout();
}

/** Wait for STOPF after a transfer and clear it. */
static i2c_err_t prv_finish(void)
{
    const i2c_err_t err = prv_wait_flag(I2C_ISR_STOPF);

    if (err == I2C_ERR_OK)
    {
        I2C2->ICR = I2C_ICR_STOPCF;
    }
    return err;
}

/* --------------------------------------------------------------------- */
/* Transaction helpers                                                    */
/* --------------------------------------------------------------------- */

static i2c_err_t prv_check_bus_free(void)
{
    if ((I2C2->ISR & I2C_ISR_BUSY) != 0U)
    {
        s_i2c.busy_strikes++;
        if (s_i2c.busy_strikes >= I2C_BUSY_STRIKES_LIMIT)
        {
            /* Still busy on the consumer's retry: treat as a stuck bus (§6.1). */
            s_i2c.busy_strikes = 0U;
            return prv_timeout();
        }
        return I2C_ERR_BUS_BUSY;
    }
    s_i2c.busy_strikes = 0U;
    return I2C_ERR_OK;
}

/** Program CR2 for one phase and generate START (or repeated START). */
static void prv_start(uint8_t dev_addr, uint16_t nbytes, uint32_t flags)
{
    I2C2->CR2 = (((uint32_t) dev_addr << I2C_ADDR_SHIFT) & I2C_CR2_SADD_MASK) |
                ((uint32_t) nbytes << I2C_CR2_NBYTES_Pos) | flags | I2C_CR2_START;
}

static i2c_err_t prv_send_bytes(const uint8_t *data, uint16_t len)
{
    for (uint16_t i = 0U; i < len; ++i)
    {
        const i2c_err_t err = prv_wait_flag(I2C_ISR_TXIS);

        if (err != I2C_ERR_OK)
        {
            return err;
        }
        I2C2->TXDR = data[i];
    }
    return I2C_ERR_OK;
}

static i2c_err_t prv_receive_bytes(uint8_t *buf, uint16_t len)
{
    for (uint16_t i = 0U; i < len; ++i)
    {
        const i2c_err_t err = prv_wait_flag(I2C_ISR_RXNE);

        if (err != I2C_ERR_OK)
        {
            return err;
        }
        buf[i] = (uint8_t) I2C2->RXDR;
    }
    return I2C_ERR_OK;
}

static bool prv_args_valid(uint8_t dev_addr, const void *ptr, uint16_t len)
{
    return (dev_addr <= I2C_MAX_7BIT_ADDR) && (ptr != NULL) && (len >= 1U) &&
           (len <= I2C_MAX_TRANSFER_BYTES);
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

    /* Clocks first: GPIOB for the pins, I2C2 for the peripheral. */
    RCC->AHB2ENR |= RCC_AHB2ENR_GPIOBEN;
    RCC->APB1ENR1 |= RCC_APB1ENR1_I2C2EN;
    (void) RCC->APB1ENR1; /* read-back: let the enable take effect */

    prv_pin_af_od(I2C_SCL_PIN, I2C_GPIO_AF);
    prv_pin_af_od(I2C_SDA_PIN, I2C_GPIO_AF);

    /* TIMINGR may only be written while PE = 0. */
    I2C2->CR1 &= ~I2C_CR1_PE;
    I2C2->TIMINGR = I2C_TIMINGR_VALUE;
    I2C2->CR1 |= I2C_CR1_PE;

    s_i2c.initialised = true;
    return I2C_ERR_OK;
}

i2c_err_t i2c_write(uint8_t dev_addr, const uint8_t *data, uint16_t len)
{
    assert(s_i2c.initialised);

    if (!prv_args_valid(dev_addr, data, len))
    {
        return I2C_ERR_INVALID_ARG;
    }

    i2c_err_t err = prv_check_bus_free();

    if (err == I2C_ERR_OK)
    {
        prv_start(dev_addr, len, I2C_CR2_AUTOEND);
        err = prv_send_bytes(data, len);
    }
    if (err == I2C_ERR_OK)
    {
        err = prv_finish();
    }
    return err;
}

i2c_err_t i2c_read(uint8_t dev_addr, uint8_t *buf, uint16_t len)
{
    assert(s_i2c.initialised);

    if (!prv_args_valid(dev_addr, buf, len))
    {
        return I2C_ERR_INVALID_ARG;
    }

    i2c_err_t err = prv_check_bus_free();

    if (err == I2C_ERR_OK)
    {
        prv_start(dev_addr, len, I2C_CR2_RD_WRN | I2C_CR2_AUTOEND);
        err = prv_receive_bytes(buf, len);
    }
    if (err == I2C_ERR_OK)
    {
        err = prv_finish();
    }
    return err;
}

i2c_err_t i2c_write_read(uint8_t dev_addr, const uint8_t *tx_data, uint16_t tx_len, uint8_t *rx_buf,
                         uint16_t rx_len)
{
    assert(s_i2c.initialised);

    if (!prv_args_valid(dev_addr, tx_data, tx_len) || !prv_args_valid(dev_addr, rx_buf, rx_len))
    {
        return I2C_ERR_INVALID_ARG;
    }

    i2c_err_t err = prv_check_bus_free();

    if (err == I2C_ERR_OK)
    {
        /* Write phase: AUTOEND = 0 holds the bus for the repeated START. */
        prv_start(dev_addr, tx_len, 0U);
        err = prv_send_bytes(tx_data, tx_len);
    }
    if (err == I2C_ERR_OK)
    {
        err = prv_wait_flag(I2C_ISR_TC);
    }
    if (err == I2C_ERR_OK)
    {
        /* Read phase: repeated START, AUTOEND generates the final STOP. */
        prv_start(dev_addr, rx_len, I2C_CR2_RD_WRN | I2C_CR2_AUTOEND);
        err = prv_receive_bytes(rx_buf, rx_len);
    }
    if (err == I2C_ERR_OK)
    {
        err = prv_finish();
    }
    return err;
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
    s_i2c.busy_strikes = 0U;
    s_i2c.recovery_pulses = 0U;
}

uint32_t i2c_recovery_pulses_for_test(void)
{
    return s_i2c.recovery_pulses;
}
#endif
