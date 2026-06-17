/**
 * @file touchscreen_driver.c
 * @brief TouchscreenDriver -- FT6x06 over I2C1, IRQ on PJ5 (EXTI5).
 *
 * @see docs/lld/drivers/touchscreen-driver.md
 */

#include "touchscreen_driver/touchscreen_driver.h"
#include "i2c/i2c_driver.h"
#include "gpio/gpio_driver.h"

#include "stm32f469xx.h"

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/* ===================================================================== */
/* FT6x06 register addresses                                            */
/* ===================================================================== */

#define FT6X06_I2C_ADDR (0x38U) /**< 7-bit I2C address of FT6206. */
#define FT6X06_REG_DEV_MODE (0x00U)
#define FT6X06_REG_TD_STATUS (0x02U)
#define FT6X06_REG_P1_XH (0x03U)
#define FT6X06_REG_TH_GROUP (0x80U)
#define FT6X06_REG_INT_MODE (0xA4U)
#define FT6X06_INT_TRIGGER (0x00U)
#define FT6X06_TH_DEFAULT (0x16U)

/* ===================================================================== */
/* GPIO / EXTI / SYSCFG constants                                       */
/* ===================================================================== */

#define TS_IRQ_PIN (5U)
#define SYSCFG_EXTICR1_EXTI5_Pos (4U)
#define SYSCFG_EXTICR1_EXTI5_MASK (0xFUL << SYSCFG_EXTICR1_EXTI5_Pos)
#define SYSCFG_EXTICR1_EXTI5_PJ (0x9UL << SYSCFG_EXTICR1_EXTI5_Pos)
#define FT6X06_IRQ_PRIORITY (10U)

/* ===================================================================== */
/* Private state                                                         */
/* ===================================================================== */

typedef struct
{
    ts_irq_cb_t irq_cb;
    void *irq_ctx;
    bool initialised;
} touchscreen_driver_t;

static touchscreen_driver_t s_ts;

/* ===================================================================== */
/* Private helpers                                                       */
/* ===================================================================== */

static i2c_err_t ft6x06_write_reg(uint8_t reg, uint8_t value)
{
    uint8_t buf[2U] = {reg, value};
    return i2c_write(FT6X06_I2C_ADDR, buf, 2U);
}

static i2c_err_t ft6x06_read_regs(uint8_t reg, uint8_t *buf, uint16_t len)
{
    return i2c_write_read(FT6X06_I2C_ADDR, &reg, 1U, buf, len);
}
/* ===================================================================== */
/* Public API                                                            */
/* ===================================================================== */

ts_err_t touchscreen_init(void)
{
    // configure PJ5 as input pin
    const gpio_pin_config_t cfg = {.port = GPIO_PORT_J,
                                   .pin = TS_IRQ_PIN,
                                   .mode = GPIO_MODE_INPUT,
                                   .otype = GPIO_OTYPE_PUSH_PULL,
                                   .speed = GPIO_SPEED_HIGH,
                                   .pull = GPIO_PULL_NONE,
                                   .alternate = 0U};

    if (gpio_configure_pin(&cfg) != GPIO_OK)
    {
        return TS_ERR_GPIO;
    }

    // enable SYSCFG clock
    RCC->APB2ENR |= RCC_APB2ENR_SYSCFGEN;
    // dummy read, just to stabilise the clock
    (void) RCC->APB2ENR;

    // enable EXTI for PJ5 pin
    SYSCFG->EXTICR[1U] &= ~SYSCFG_EXTICR1_EXTI5_MASK;
    SYSCFG->EXTICR[1U] |= SYSCFG_EXTICR1_EXTI5_PJ;

    // set falling trigger
    EXTI->FTSR |= (1UL << TS_IRQ_PIN);
    // clear rising trigger
    EXTI->RTSR &= ~(1UL << TS_IRQ_PIN);
    // clear EXTI PJ5 interrupt flag
    EXTI->IMR &= ~(1UL << TS_IRQ_PIN);

    /* 4. NVIC — EXTI line 5 shares EXTI9_5_IRQn vector */
    NVIC_SetPriority(EXTI9_5_IRQn, FT6X06_IRQ_PRIORITY);
    NVIC_EnableIRQ(EXTI9_5_IRQn);

    // enable I2C1 peripheral
    if (i2c_init() != I2C_ERR_OK)
    {
        return TS_ERR_I2C;
    }

    if (ft6x06_write_reg(FT6X06_REG_INT_MODE, FT6X06_INT_TRIGGER) != I2C_ERR_OK)
    {
        return TS_ERR_I2C;
    }

    if (ft6x06_write_reg(FT6X06_REG_TH_GROUP, FT6X06_TH_DEFAULT) != I2C_ERR_OK)
    {
        return TS_ERR_I2C;
    }

    uint8_t dev_mode = 0U;
    if (ft6x06_read_regs(FT6X06_REG_DEV_MODE, &dev_mode, 1U) != I2C_ERR_OK)
    {
        return TS_ERR_I2C;
    }

    s_ts.initialised = true;
    return TS_ERR_OK;
}

ts_err_t touchscreen_attach_irq(ts_irq_cb_t callback, void *context)
{
    if (callback == NULL)
    {
        return TS_ERR_NULL;
    }

    s_ts.irq_cb = callback;
    s_ts.irq_ctx = context;

    EXTI->PR = (1UL << TS_IRQ_PIN); /* write-1-to-clear any pending */
    EXTI->IMR |= (1UL << TS_IRQ_PIN);

    return TS_ERR_OK;
}

ts_err_t touchscreen_read(ts_touch_t *touch)
{
    if (touch == NULL)
    {
        return TS_ERR_NULL;
    }

    uint8_t td_status = 0U;
    if (ft6x06_read_regs(FT6X06_REG_TD_STATUS, &td_status, 1U) != I2C_ERR_OK)
    {
        return TS_ERR_I2C;
    }

    if ((td_status & 0x0FU) == 0U)
    {
        return TS_ERR_NO_DATA;
    }

    uint8_t coords[4U];
    if (ft6x06_read_regs(FT6X06_REG_P1_XH, coords, 4U) != I2C_ERR_OK)
    {
        return TS_ERR_I2C;
    }

    touch->x = (uint16_t) (((uint16_t) (coords[0U] & 0x0FU) << 8U) | (uint16_t) coords[1U]);
    touch->y = (uint16_t) (((uint16_t) (coords[2U] & 0x0FU) << 8U) | (uint16_t) coords[3U]);

    uint8_t ev = (uint8_t) ((coords[0U] >> 6U) & 0x03U);
    if (ev == 0U)
    {
        touch->event = TS_EVENT_PRESS;
    }
    else if (ev == 1U)
    {
        touch->event = TS_EVENT_RELEASE;
    }
    else
    {
        touch->event = TS_EVENT_CONTACT;
    }

    return TS_ERR_OK;
}

/* ===================================================================== */
/* ISR                                                                   */
/* ===================================================================== */

void EXTI9_5_IRQHandler(void)
{
    if ((EXTI->PR & (1UL << TS_IRQ_PIN)) != 0U)
    {
        EXTI->PR = (1UL << TS_IRQ_PIN);
        if (s_ts.irq_cb != NULL)
        {
            s_ts.irq_cb(s_ts.irq_ctx);
        }
    }
}

#ifdef TEST
void touchscreen_reset_for_test(void)
{
    s_ts.irq_cb = NULL;
    s_ts.irq_ctx = NULL;
    s_ts.initialised = false;
}
#endif /* TEST */
