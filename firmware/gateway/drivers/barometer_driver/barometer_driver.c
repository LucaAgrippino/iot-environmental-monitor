/**
 * @file barometer_driver.c
 * @brief BarometerDriver (Gateway) — LPS22HB implementation.
 *
 * See barometer_driver.h and docs/lld/drivers/gw-sensor-drivers-group-a-lld.md.
 */

#include "barometer_driver.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

/* ===================================================================== */
/* Hardware constants (LPS22HB datasheet)                                 */
/* ===================================================================== */

/** 7-bit I2C address (UM2153 Table 3). */
#define LPS22HB_I2C_ADDR (0x5DU)

#define LPS22HB_REG_CTRL_REG1 (0x10U)
#define LPS22HB_REG_CTRL_REG2 (0x11U)
#define LPS22HB_REG_STATUS (0x27U)
#define LPS22HB_REG_PRESS_OUT_XL (0x28U)

/** CTRL_REG1: ODR[2:0] (bits 6:4) = 001 (1 Hz), BDU (bit 1) = 1. */
#define LPS22HB_CTRL_REG1_VALUE (0x12U)
/**
 * CTRL_REG2: IF_ADD_INC (bit 4) = 1, everything else 0 (normal mode, no
 * one-shot, no reset). IF_ADD_INC is the power-on default and MUST stay set:
 * the LPS22HB has no address bit 7 auto-increment, so without it the 5-byte
 * burst read returns the same register five times.
 */
#define LPS22HB_CTRL_REG2_VALUE (0x10U)

/** STATUS_REG: P_DA (bit 0) and T_DA (bit 1) must both be set. */
#define LPS22HB_STATUS_DATA_READY_MASK (0x03U)

#define LPS22HB_OUT_LEN (5U)
/** Register address + value. */
#define LPS22HB_WRITE_LEN (2U)

#define BYTE_SHIFT (8U)
#define PRESS_MID_SHIFT (8U)
#define PRESS_HIGH_SHIFT (16U)
#define PRESS_SIGN_BIT (0x00800000L)
#define PRESS_SIGN_EXTEND (0xFF000000UL)

/** Pressure LSB is 1/4096 hPa; the output scale is x10. */
#define PRESS_COUNTS_PER_HPA (4096)
#define PRESS_OUT_SCALE (10)

/** Retry delay when STATUS_REG reports data not ready. */
#define BAROMETER_RETRY_DELAY_MS (2U)

/* ===================================================================== */
/* Private state                                                          */
/* ===================================================================== */

typedef struct
{
    bool ready; /* set true after successful init */
} barometer_state_t;

static barometer_state_t s_baro;

/* ===================================================================== */
/* Private helpers                                                        */
/* ===================================================================== */

static barometer_err_t prv_read_status(bool *ready)
{
    const uint8_t reg = LPS22HB_REG_STATUS;
    uint8_t status = 0U;

    if (i2c_driver->write_read(LPS22HB_I2C_ADDR, &reg, 1U, &status, 1U) != I2C_ERR_OK)
    {
        return BAROMETER_ERR_I2C;
    }
    *ready = ((status & LPS22HB_STATUS_DATA_READY_MASK) == LPS22HB_STATUS_DATA_READY_MASK);
    return BAROMETER_ERR_OK;
}

/** Assemble the 24-bit two's-complement pressure word and sign-extend it. */
static int32_t prv_pressure_raw(const uint8_t *buf)
{
    uint32_t word = ((uint32_t) buf[2] << PRESS_HIGH_SHIFT) |
                    ((uint32_t) buf[1] << PRESS_MID_SHIFT) | (uint32_t) buf[0];

    if ((word & (uint32_t) PRESS_SIGN_BIT) != 0U)
    {
        word |= PRESS_SIGN_EXTEND;
    }
    return (int32_t) word;
}

/* ===================================================================== */
/* Public API                                                             */
/* ===================================================================== */

barometer_err_t barometer_init(void)
{
    const uint8_t ctrl1[LPS22HB_WRITE_LEN] = {LPS22HB_REG_CTRL_REG1, LPS22HB_CTRL_REG1_VALUE};
    const uint8_t ctrl2[LPS22HB_WRITE_LEN] = {LPS22HB_REG_CTRL_REG2, LPS22HB_CTRL_REG2_VALUE};

    s_baro.ready = false;

    if (i2c_driver->write(LPS22HB_I2C_ADDR, ctrl1, LPS22HB_WRITE_LEN) != I2C_ERR_OK)
    {
        return BAROMETER_ERR_I2C;
    }
    if (i2c_driver->write(LPS22HB_I2C_ADDR, ctrl2, LPS22HB_WRITE_LEN) != I2C_ERR_OK)
    {
        return BAROMETER_ERR_I2C;
    }

    s_baro.ready = true;
    return BAROMETER_ERR_OK;
}

barometer_err_t barometer_read(barometer_data_t *out)
{
    const uint8_t out_reg = LPS22HB_REG_PRESS_OUT_XL;
    uint8_t raw[LPS22HB_OUT_LEN];
    bool data_ready = false;
    barometer_err_t err;

    if (out == NULL)
    {
        return BAROMETER_ERR_INVALID_ARG;
    }
    if (!s_baro.ready)
    {
        return BAROMETER_ERR_NOT_READY;
    }

    err = prv_read_status(&data_ready);
    if ((err == BAROMETER_ERR_OK) && !data_ready)
    {
        vTaskDelay(pdMS_TO_TICKS(BAROMETER_RETRY_DELAY_MS));
        err = prv_read_status(&data_ready);
    }
    if (err != BAROMETER_ERR_OK)
    {
        return err;
    }
    if (!data_ready)
    {
        return BAROMETER_ERR_NOT_READY;
    }

    if (i2c_driver->write_read(LPS22HB_I2C_ADDR, &out_reg, 1U, raw, LPS22HB_OUT_LEN) != I2C_ERR_OK)
    {
        return BAROMETER_ERR_I2C;
    }

    /* Divide (not shift): truncates toward zero, as TC-BA-002 specifies. */
    out->pressure_x10 = (prv_pressure_raw(raw) * PRESS_OUT_SCALE) / PRESS_COUNTS_PER_HPA;
    out->temperature_x100 =
        (int32_t) (int16_t) (((uint16_t) raw[4] << BYTE_SHIFT) | (uint16_t) raw[3]);
    return BAROMETER_ERR_OK;
}

static const ibarometer_t s_barometer_vtable = {
    .init = barometer_init,
    .read = barometer_read,
};

const ibarometer_t *const barometer_driver = &s_barometer_vtable;

#ifdef TEST
void barometer_reset_for_test(void)
{
    (void) memset(&s_baro, 0, sizeof(s_baro));
}
#endif
