/**
 * @file imu_driver.c
 * @brief ImuDriver (Gateway) — LSM6DSL implementation.
 *
 * See imu_driver.h and docs/lld/drivers/gw-sensor-drivers-group-b-lld.md.
 */

#include "imu_driver.h"
#include "imu_driver_internal.h"

#include <stddef.h>
#include <string.h>

#include "exti/exti_driver.h"
#include "gpio/gpio_driver.h"

/* ===================================================================== */
/* Hardware constants (LSM6DSL datasheet, UM2153)                         */
/* ===================================================================== */

/** 7-bit I2C address (UM2153 Table 3). */
#define LSM6DSL_I2C_ADDR (0x6AU)

#define LSM6DSL_REG_INT1_CTRL (0x0DU)
#define LSM6DSL_REG_CTRL1_XL (0x10U)
#define LSM6DSL_REG_CTRL2_G (0x11U)
#define LSM6DSL_REG_CTRL3_C (0x12U)
#define LSM6DSL_REG_STATUS (0x1EU)
#define LSM6DSL_REG_OUTX_L_G (0x22U)

/** CTRL3_C: BDU (bit 6) = 1, IF_INC (bit 2) = 1 (auto-increment on multi-byte access). */
#define LSM6DSL_CTRL3_C_VALUE (0x44U)
/** INT1_CTRL: INT1_DRDY_G (bit 1) and INT1_DRDY_XL (bit 0) routed to INT1. */
#define LSM6DSL_INT1_CTRL_VALUE (0x03U)
/** CTRL1_XL: ODR_XL[3:0] = 0100 (104 Hz), FS_XL = 00 (+/-2 g). */
#define LSM6DSL_CTRL1_XL_VALUE (0x40U)
/** CTRL2_G: ODR_G[3:0] = 0100 (104 Hz), FS_G = 00 (+/-250 dps). */
#define LSM6DSL_CTRL2_G_VALUE (0x40U)

/** STATUS_REG: GDA (bit 1) and XLDA (bit 0) must both be set. */
#define LSM6DSL_STATUS_DATA_READY_MASK (0x03U)

#define LSM6DSL_OUT_LEN (12U)
/** Register address + value. */
#define LSM6DSL_WRITE_LEN (2U)
#define BYTE_SHIFT (8U)

/* Offsets into the 12-byte burst: gyro X/Y/Z first, then accel X/Y/Z. */
#define OFF_GYRO_X (0U)
#define OFF_GYRO_Y (2U)
#define OFF_GYRO_Z (4U)
#define OFF_ACCEL_X (6U)
#define OFF_ACCEL_Y (8U)
#define OFF_ACCEL_Z (10U)

/** INT1 is wired to PD11 / EXTI line 11 (UM2153; GPB-O3). */
#define IMU_INT1_PORT (GPIO_PORT_D)
#define IMU_INT1_EXTI_PORT (EXTI_PORT_D)
#define IMU_INT1_PIN (11U)
#define IMU_INT1_LINE (11U)

/** NVIC priority: numerically >= configMAX_SYSCALL_INTERRUPT_PRIORITY (5), as the callback
 *  calls *FromISR() APIs. Same value as the other Gateway ISRs that notify tasks. */
#define IMU_INT1_NVIC_PRIORITY (6U)

/* ===================================================================== */
/* Private state                                                          */
/* ===================================================================== */

typedef struct
{
    imu_int1_cb_t int1_cb; /**< Registered INT1 callback. */
    void *int1_ctx;        /**< Opaque context for the callback. */
    bool ready;            /**< True after successful Phase 1 init. */
} imu_state_t;

static imu_state_t s_imu;

/* ===================================================================== */
/* Private helpers                                                        */
/* ===================================================================== */

static imu_err_t prv_write_reg(uint8_t reg, uint8_t value)
{
    const uint8_t cmd[LSM6DSL_WRITE_LEN] = {reg, value};

    if (i2c_driver->write(LSM6DSL_I2C_ADDR, cmd, LSM6DSL_WRITE_LEN) != I2C_ERR_OK)
    {
        return IMU_ERR_I2C;
    }
    return IMU_ERR_OK;
}

static imu_err_t prv_read_output(uint8_t *raw)
{
    const uint8_t out_reg = LSM6DSL_REG_OUTX_L_G;

    if (i2c_driver->write_read(LSM6DSL_I2C_ADDR, &out_reg, 1U, raw, LSM6DSL_OUT_LEN) != I2C_ERR_OK)
    {
        return IMU_ERR_I2C;
    }
    return IMU_ERR_OK;
}

static int16_t prv_le_i16(const uint8_t *p)
{
    return (int16_t) (((uint16_t) p[1] << BYTE_SHIFT) | (uint16_t) p[0]);
}

static imu_err_t prv_configure_int1_pin(void)
{
    const gpio_pin_config_t pin = {
        .port = IMU_INT1_PORT,
        .pin = IMU_INT1_PIN,
        .mode = GPIO_MODE_INPUT,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE, /* the sensor drives the line actively */
        .alternate = 0U,
    };
    exti_err_t exti_err;

    if (gpio_configure_pin(&pin) != GPIO_OK)
    {
        return IMU_ERR_HW;
    }

    exti_err = exti_configure(IMU_INT1_LINE, IMU_INT1_EXTI_PORT, EXTI_EDGE_RISING);
    /* A second init (sensor re-initialisation, REQ-NF-205) finds the line already set up. */
    if ((exti_err != EXTI_ERR_OK) && (exti_err != EXTI_ERR_CONFLICT))
    {
        return IMU_ERR_HW;
    }
    return IMU_ERR_OK;
}

/** Read the output registers once to release an INT1 that went high before the line was armed. */
static imu_err_t prv_release_stale_int1(void)
{
    gpio_level_t level = GPIO_LEVEL_LOW;

    if (gpio_read_pin(IMU_INT1_PORT, IMU_INT1_PIN, &level) != GPIO_OK)
    {
        return IMU_ERR_HW;
    }
    if (level == GPIO_LEVEL_HIGH)
    {
        uint8_t raw[LSM6DSL_OUT_LEN];

        return prv_read_output(raw);
    }
    return IMU_ERR_OK;
}

/* ===================================================================== */
/* Public API                                                             */
/* ===================================================================== */

imu_err_t imu_init(void)
{
    imu_err_t err;

    s_imu.ready = false;

    err = prv_write_reg(LSM6DSL_REG_CTRL3_C, LSM6DSL_CTRL3_C_VALUE);
    if (err == IMU_ERR_OK)
    {
        err = prv_write_reg(LSM6DSL_REG_INT1_CTRL, LSM6DSL_INT1_CTRL_VALUE);
    }
    if (err == IMU_ERR_OK)
    {
        err = prv_write_reg(LSM6DSL_REG_CTRL1_XL, LSM6DSL_CTRL1_XL_VALUE);
    }
    if (err == IMU_ERR_OK)
    {
        /* Last: setting the ODRs starts conversions with everything configured. */
        err = prv_write_reg(LSM6DSL_REG_CTRL2_G, LSM6DSL_CTRL2_G_VALUE);
    }
    if (err == IMU_ERR_OK)
    {
        err = prv_configure_int1_pin();
    }
    if (err == IMU_ERR_OK)
    {
        s_imu.ready = true;
    }
    return err;
}

imu_err_t imu_attach_int1_callback(imu_int1_cb_t cb, void *ctx)
{
    if (cb == NULL)
    {
        return IMU_ERR_INVALID_ARG;
    }
    if (!s_imu.ready)
    {
        return IMU_ERR_NOT_READY;
    }

    s_imu.int1_cb = cb;
    s_imu.int1_ctx = ctx;

    if (exti_enable(IMU_INT1_LINE, IMU_INT1_NVIC_PRIORITY) != EXTI_ERR_OK)
    {
        return IMU_ERR_HW;
    }

    /* Armed first, so any edge from now on is seen; then release a level that was
     * already high before arming. */
    return prv_release_stale_int1();
}

imu_err_t imu_read(imu_data_t *out)
{
    const uint8_t status_reg = LSM6DSL_REG_STATUS;
    uint8_t status = 0U;
    uint8_t raw[LSM6DSL_OUT_LEN];

    if (out == NULL)
    {
        return IMU_ERR_INVALID_ARG;
    }
    if (!s_imu.ready)
    {
        return IMU_ERR_NOT_READY;
    }

    if (i2c_driver->write_read(LSM6DSL_I2C_ADDR, &status_reg, 1U, &status, 1U) != I2C_ERR_OK)
    {
        return IMU_ERR_I2C;
    }
    if ((status & LSM6DSL_STATUS_DATA_READY_MASK) != LSM6DSL_STATUS_DATA_READY_MASK)
    {
        return IMU_ERR_NOT_READY;
    }

    if (prv_read_output(raw) != IMU_ERR_OK)
    {
        return IMU_ERR_I2C;
    }

    out->gyro_x = prv_le_i16(&raw[OFF_GYRO_X]);
    out->gyro_y = prv_le_i16(&raw[OFF_GYRO_Y]);
    out->gyro_z = prv_le_i16(&raw[OFF_GYRO_Z]);
    out->accel_x = prv_le_i16(&raw[OFF_ACCEL_X]);
    out->accel_y = prv_le_i16(&raw[OFF_ACCEL_Y]);
    out->accel_z = prv_le_i16(&raw[OFF_ACCEL_Z]);
    return IMU_ERR_OK;
}

void imu_int1_irq_handler(void)
{
    if (s_imu.int1_cb != NULL)
    {
        s_imu.int1_cb(s_imu.int1_ctx);
    }
}

static const iimu_t s_imu_vtable = {
    .init = imu_init,
    .attach_int1_callback = imu_attach_int1_callback,
    .read = imu_read,
};

const iimu_t *const imu_driver = &s_imu_vtable;

#ifdef TEST
void imu_reset_for_test(void)
{
    (void) memset(&s_imu, 0, sizeof(s_imu));
}
#endif
