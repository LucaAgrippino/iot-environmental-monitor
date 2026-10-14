/**
 * @file magnetometer_driver.c
 * @brief MagnetometerDriver (Gateway) — LIS3MDL implementation.
 *
 * See magnetometer_driver.h and docs/lld/drivers/gw-sensor-drivers-group-b-lld.md.
 */

#include "magnetometer_driver.h"
#include "magnetometer_driver_internal.h"

#include <stddef.h>
#include <string.h>

#include "exti/exti_driver.h"
#include "gpio/gpio_driver.h"

/* ===================================================================== */
/* Hardware constants (LIS3MDL datasheet, UM2153)                         */
/* ===================================================================== */

/** 7-bit I2C address (UM2153 Table 3). */
#define LIS3MDL_I2C_ADDR (0x1EU)

/** Setting bit 7 of the register address enables multi-byte auto-increment. */
#define LIS3MDL_AUTO_INC (0x80U)

#define LIS3MDL_REG_CTRL_REG1 (0x20U)
#define LIS3MDL_REG_CTRL_REG2 (0x21U)
#define LIS3MDL_REG_CTRL_REG3 (0x22U)
#define LIS3MDL_REG_CTRL_REG4 (0x23U)
#define LIS3MDL_REG_CTRL_REG5 (0x24U)
#define LIS3MDL_REG_STATUS (0x27U)
#define LIS3MDL_REG_OUT_X_L (0x28U)

/** CTRL_REG1: TEMP_EN=0, OM[1:0]=11 (ultra-high XY), DO[2:0]=100 (10 Hz), FAST_ODR=0, ST=0. */
#define LIS3MDL_CTRL_REG1_VALUE (0x70U)
/** CTRL_REG2: FS[1:0]=00 (+/-4 gauss), no reboot, no soft reset. */
#define LIS3MDL_CTRL_REG2_VALUE (0x00U)
/** CTRL_REG3: MD[1:0]=00 (continuous conversion). Power-on default is power-down (0x03). */
#define LIS3MDL_CTRL_REG3_VALUE (0x00U)
/** CTRL_REG4: OMZ[1:0]=11 (ultra-high Z), BLE=0 (little endian). */
#define LIS3MDL_CTRL_REG4_VALUE (0x0CU)
/** CTRL_REG5: BDU=1 (block data update). */
#define LIS3MDL_CTRL_REG5_VALUE (0x40U)

/** STATUS_REG: ZYXDA (bit 3) — new data on all three axes. */
#define LIS3MDL_STATUS_ZYXDA (0x08U)

#define LIS3MDL_OUT_LEN (6U)
/** Register address + value. */
#define LIS3MDL_WRITE_LEN (2U)
#define BYTE_SHIFT (8U)

/** DRDY is wired to PC8 / EXTI line 8 (UM2153; GPB-O3). */
#define MAG_DRDY_PORT (GPIO_PORT_C)
#define MAG_DRDY_EXTI_PORT (EXTI_PORT_C)
#define MAG_DRDY_PIN (8U)
#define MAG_DRDY_LINE (8U)

/** NVIC priority: numerically >= configMAX_SYSCALL_INTERRUPT_PRIORITY (5), as the callback
 *  calls *FromISR() APIs. Same value as the other Gateway ISRs that notify tasks. */
#define MAG_DRDY_NVIC_PRIORITY (6U)

/* ===================================================================== */
/* Private state                                                          */
/* ===================================================================== */

typedef struct
{
    magnetometer_drdy_cb_t drdy_cb; /**< Registered DRDY callback. */
    void *drdy_ctx;                 /**< Opaque context for the callback. */
    bool ready;                     /**< True after successful Phase 1 init. */
} magnetometer_state_t;

static magnetometer_state_t s_mag;

/* ===================================================================== */
/* Private helpers                                                        */
/* ===================================================================== */

static magnetometer_err_t prv_write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t cmd[LIS3MDL_WRITE_LEN] = {reg, value};

    if (i2c_driver->write(LIS3MDL_I2C_ADDR, cmd, LIS3MDL_WRITE_LEN) != I2C_ERR_OK)
    {
        return MAGNETOMETER_ERR_I2C;
    }
    return MAGNETOMETER_ERR_OK;
}

static magnetometer_err_t prv_read_output(uint8_t *raw)
{
    const uint8_t out_reg = (uint8_t) (LIS3MDL_REG_OUT_X_L | LIS3MDL_AUTO_INC);

    if (i2c_driver->write_read(LIS3MDL_I2C_ADDR, &out_reg, 1U, raw, LIS3MDL_OUT_LEN) != I2C_ERR_OK)
    {
        return MAGNETOMETER_ERR_I2C;
    }
    return MAGNETOMETER_ERR_OK;
}

static int16_t prv_le_i16(const uint8_t *p)
{
    return (int16_t) (((uint16_t) p[1] << BYTE_SHIFT) | (uint16_t) p[0]);
}

static magnetometer_err_t prv_configure_drdy_pin(void)
{
    const gpio_pin_config_t pin = {
        .port = MAG_DRDY_PORT,
        .pin = MAG_DRDY_PIN,
        .mode = GPIO_MODE_INPUT,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE, /* the sensor drives the line actively */
        .alternate = 0U,
    };
    exti_err_t exti_err;

    if (gpio_configure_pin(&pin) != GPIO_OK)
    {
        return MAGNETOMETER_ERR_HW;
    }

    exti_err = exti_configure(MAG_DRDY_LINE, MAG_DRDY_EXTI_PORT, EXTI_EDGE_RISING);
    /* A second init (sensor re-initialisation, REQ-NF-205) finds the line already set up. */
    if ((exti_err != EXTI_ERR_OK) && (exti_err != EXTI_ERR_CONFLICT))
    {
        return MAGNETOMETER_ERR_HW;
    }
    return MAGNETOMETER_ERR_OK;
}

/** Read the output registers once to release a DRDY that went high before the line was armed. */
static magnetometer_err_t prv_release_stale_drdy(void)
{
    gpio_level_t level = GPIO_LEVEL_LOW;

    if (gpio_read_pin(MAG_DRDY_PORT, MAG_DRDY_PIN, &level) != GPIO_OK)
    {
        return MAGNETOMETER_ERR_HW;
    }
    if (level == GPIO_LEVEL_HIGH)
    {
        uint8_t raw[LIS3MDL_OUT_LEN];

        return prv_read_output(raw);
    }
    return MAGNETOMETER_ERR_OK;
}

/* ===================================================================== */
/* Public API                                                             */
/* ===================================================================== */

magnetometer_err_t magnetometer_init(void)
{
    magnetometer_err_t err;

    s_mag.ready = false;

    err = prv_write_reg(LIS3MDL_REG_CTRL_REG1, LIS3MDL_CTRL_REG1_VALUE);
    if (err == MAGNETOMETER_ERR_OK)
    {
        err = prv_write_reg(LIS3MDL_REG_CTRL_REG2, LIS3MDL_CTRL_REG2_VALUE);
    }
    if (err == MAGNETOMETER_ERR_OK)
    {
        err = prv_write_reg(LIS3MDL_REG_CTRL_REG4, LIS3MDL_CTRL_REG4_VALUE);
    }
    if (err == MAGNETOMETER_ERR_OK)
    {
        err = prv_write_reg(LIS3MDL_REG_CTRL_REG5, LIS3MDL_CTRL_REG5_VALUE);
    }
    if (err == MAGNETOMETER_ERR_OK)
    {
        /* Last: leaving power-down starts conversions with everything configured. */
        err = prv_write_reg(LIS3MDL_REG_CTRL_REG3, LIS3MDL_CTRL_REG3_VALUE);
    }
    if (err == MAGNETOMETER_ERR_OK)
    {
        err = prv_configure_drdy_pin();
    }
    if (err == MAGNETOMETER_ERR_OK)
    {
        s_mag.ready = true;
    }
    return err;
}

magnetometer_err_t magnetometer_attach_drdy_callback(magnetometer_drdy_cb_t cb, void *ctx)
{
    magnetometer_err_t err;

    if (cb == NULL)
    {
        return MAGNETOMETER_ERR_INVALID_ARG;
    }
    if (!s_mag.ready)
    {
        return MAGNETOMETER_ERR_NOT_READY;
    }

    s_mag.drdy_cb = cb;
    s_mag.drdy_ctx = ctx;

    if (exti_enable(MAG_DRDY_LINE, MAG_DRDY_NVIC_PRIORITY) != EXTI_ERR_OK)
    {
        return MAGNETOMETER_ERR_HW;
    }

    /* Armed first, so any edge from now on is seen; then release a level that was
     * already high before arming. */
    err = prv_release_stale_drdy();
    return err;
}

magnetometer_err_t magnetometer_read(magnetometer_data_t *out)
{
    const uint8_t status_reg = LIS3MDL_REG_STATUS;
    uint8_t status = 0U;
    uint8_t raw[LIS3MDL_OUT_LEN];

    if (out == NULL)
    {
        return MAGNETOMETER_ERR_INVALID_ARG;
    }
    if (!s_mag.ready)
    {
        return MAGNETOMETER_ERR_NOT_READY;
    }

    if (i2c_driver->write_read(LIS3MDL_I2C_ADDR, &status_reg, 1U, &status, 1U) != I2C_ERR_OK)
    {
        return MAGNETOMETER_ERR_I2C;
    }
    if ((status & LIS3MDL_STATUS_ZYXDA) == 0U)
    {
        return MAGNETOMETER_ERR_NOT_READY;
    }

    if (prv_read_output(raw) != MAGNETOMETER_ERR_OK)
    {
        return MAGNETOMETER_ERR_I2C;
    }

    out->x = prv_le_i16(&raw[0]);
    out->y = prv_le_i16(&raw[2]);
    out->z = prv_le_i16(&raw[4]);
    return MAGNETOMETER_ERR_OK;
}

void magnetometer_drdy_irq_handler(void)
{
    if (s_mag.drdy_cb != NULL)
    {
        s_mag.drdy_cb(s_mag.drdy_ctx);
    }
}

static const imagnetometer_t s_magnetometer_vtable = {
    .init = magnetometer_init,
    .attach_drdy_callback = magnetometer_attach_drdy_callback,
    .read = magnetometer_read,
};

const imagnetometer_t *const magnetometer_driver = &s_magnetometer_vtable;

#ifdef TEST
void magnetometer_reset_for_test(void)
{
    (void) memset(&s_mag, 0, sizeof(s_mag));
}
#endif
