/**
 * @file humidity_temp_driver.c
 * @brief HumidityTempDriver (Gateway) — HTS221 implementation.
 *
 * See humidity_temp_driver.h and docs/lld/drivers/gw-sensor-drivers-group-a-lld.md.
 */

#include "humidity_temp_driver.h"

#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

/* ===================================================================== */
/* Hardware constants (HTS221 datasheet)                                  */
/* ===================================================================== */

/** 7-bit I2C address (UM2153 Table 3). */
#define HTS221_I2C_ADDR (0x5FU)

/** Setting bit 7 of the register address enables multi-byte auto-increment. */
#define HTS221_AUTO_INC (0x80U)

#define HTS221_REG_AV_CONF (0x10U)
#define HTS221_REG_CTRL_REG1 (0x20U)
#define HTS221_REG_STATUS (0x27U)
#define HTS221_REG_HUMIDITY_OUT_L (0x28U)
#define HTS221_REG_CALIB_START (0x30U)

/** CTRL_REG1: PD (bit 7) = active, BDU (bit 2) = 1, ODR[1:0] = 01 (1 Hz). */
#define HTS221_CTRL_REG1_VALUE (0x85U)
/** AV_CONF: AVGT = 16 samples, AVGH = 32 samples (datasheet default). */
#define HTS221_AV_CONF_VALUE (0x1BU)

/** STATUS_REG: H_DA (bit 1) and T_DA (bit 0) must both be set. */
#define HTS221_STATUS_DATA_READY_MASK (0x03U)

/** Calibration block 0x30..0x3F. */
#define HTS221_CALIB_LEN (16U)
#define HTS221_OUT_LEN (4U)
/** Register address + value. */
#define HTS221_WRITE_LEN (2U)

/* Offsets into the calibration burst (register address minus 0x30). */
#define CAL_H0_RH_X2 (0U)
#define CAL_H1_RH_X2 (1U)
#define CAL_T0_DEGC_X8 (2U)
#define CAL_T1_DEGC_X8 (3U)
#define CAL_T1_T0_MSB (5U)
#define CAL_H0_T0_OUT (6U)
#define CAL_H1_T0_OUT (10U)
#define CAL_T0_OUT (12U)
#define CAL_T1_OUT (14U)

/** T0/T1 MSB masks and shifts in register 0x35. */
#define T0_MSB_MASK (0x03U)
#define T1_MSB_MASK (0x0CU)
#define T0_MSB_SHIFT (8U)
#define T1_MSB_SHIFT (6U)

#define BYTE_SHIFT (8U)

/** Retry delay when STATUS_REG reports data not ready. */
#define HUMIDITY_TEMP_RETRY_DELAY_MS (2U)

/* Fixed-point scaling. */
#define HUMIDITY_X2_TO_X100 (50) /* H*_rH_x2 holds 2 x %RH; x100 / 2 = x50. */
#define TEMP_X8_TO_X100_NUM (100)
#define TEMP_X8_DIVISOR (8)
#define HUMIDITY_MIN_X100 (0)
#define HUMIDITY_MAX_X100 (10000)

/* ===================================================================== */
/* Private state                                                          */
/* ===================================================================== */

typedef struct
{
    int16_t h0_rh_x2;
    int16_t h1_rh_x2;
    int16_t t0_degc_x8;
    int16_t t1_degc_x8;
    int16_t h0_t0_out;
    int16_t h1_t0_out;
    int16_t t0_out;
    int16_t t1_out;
} hts221_cal_t;

typedef struct
{
    hts221_cal_t cal;
    bool ready; /* set true after successful init */
} humidity_temp_state_t;

static humidity_temp_state_t s_ht;

/* ===================================================================== */
/* Private helpers                                                        */
/* ===================================================================== */

static int16_t prv_le_i16(const uint8_t *p)
{
    return (int16_t) (((uint16_t) p[1] << BYTE_SHIFT) | (uint16_t) p[0]);
}

static int32_t prv_clamp_humidity(int32_t value)
{
    int32_t result = value;

    if (result < HUMIDITY_MIN_X100)
    {
        result = HUMIDITY_MIN_X100;
    }
    else if (result > HUMIDITY_MAX_X100)
    {
        result = HUMIDITY_MAX_X100;
    }
    else
    {
        /* in range */
    }
    return result;
}

/**
 * Humidity in %RH x 100. H*_rH_x2 holds twice the %RH, so the x100 scale is
 * x50 per count — this avoids the 0.5 %RH lost by halving first. A zero
 * calibration span (corrupt OTP) returns the H0 point instead of dividing.
 */
static int32_t prv_compensate_humidity(int16_t h_raw)
{
    const hts221_cal_t *c = &s_ht.cal;
    const int32_t span = (int32_t) c->h1_t0_out - (int32_t) c->h0_t0_out;
    const int32_t base = (int32_t) c->h0_rh_x2 * HUMIDITY_X2_TO_X100;
    int32_t result = base;

    if (span != 0)
    {
        const int32_t rise = ((int32_t) c->h1_rh_x2 - (int32_t) c->h0_rh_x2) * HUMIDITY_X2_TO_X100;
        result = ((rise * ((int32_t) h_raw - (int32_t) c->h0_t0_out)) / span) + base;
    }
    return prv_clamp_humidity(result);
}

/**
 * Temperature in degC x 100. T*_degC_x8 holds eight times the degC.
 * Intermediate (dT_x8 x 100 x dRaw) stays inside int32_t for any calibration
 * the part ships with (companion §5.2); a zero span returns the T0 point.
 */
static int32_t prv_compensate_temperature(int16_t t_raw)
{
    const hts221_cal_t *c = &s_ht.cal;
    const int32_t span = (int32_t) c->t1_out - (int32_t) c->t0_out;
    const int32_t base = ((int32_t) c->t0_degc_x8 * TEMP_X8_TO_X100_NUM) / TEMP_X8_DIVISOR;
    int32_t result = base;

    if (span != 0)
    {
        const int32_t rise =
            ((int32_t) c->t1_degc_x8 - (int32_t) c->t0_degc_x8) * TEMP_X8_TO_X100_NUM;
        result =
            ((rise * ((int32_t) t_raw - (int32_t) c->t0_out)) / (TEMP_X8_DIVISOR * span)) + base;
    }
    return result;
}

static humidity_temp_err_t prv_read_status(bool *ready)
{
    const uint8_t reg = HTS221_REG_STATUS;
    uint8_t status = 0U;

    if (i2c_driver->write_read(HTS221_I2C_ADDR, &reg, 1U, &status, 1U) != I2C_ERR_OK)
    {
        return HUMIDITY_TEMP_ERR_I2C;
    }
    *ready = ((status & HTS221_STATUS_DATA_READY_MASK) == HTS221_STATUS_DATA_READY_MASK);
    return HUMIDITY_TEMP_ERR_OK;
}

static void prv_parse_calibration(const uint8_t *buf)
{
    const uint8_t msb = buf[CAL_T1_T0_MSB];

    s_ht.cal.h0_rh_x2 = (int16_t) buf[CAL_H0_RH_X2];
    s_ht.cal.h1_rh_x2 = (int16_t) buf[CAL_H1_RH_X2];
    s_ht.cal.t0_degc_x8 = (int16_t) ((uint16_t) buf[CAL_T0_DEGC_X8] |
                                     (uint16_t) ((msb & T0_MSB_MASK) << T0_MSB_SHIFT));
    s_ht.cal.t1_degc_x8 = (int16_t) ((uint16_t) buf[CAL_T1_DEGC_X8] |
                                     (uint16_t) ((msb & T1_MSB_MASK) << T1_MSB_SHIFT));
    s_ht.cal.h0_t0_out = prv_le_i16(&buf[CAL_H0_T0_OUT]);
    s_ht.cal.h1_t0_out = prv_le_i16(&buf[CAL_H1_T0_OUT]);
    s_ht.cal.t0_out = prv_le_i16(&buf[CAL_T0_OUT]);
    s_ht.cal.t1_out = prv_le_i16(&buf[CAL_T1_OUT]);
}

/* ===================================================================== */
/* Public API                                                             */
/* ===================================================================== */

humidity_temp_err_t humidity_temp_init(void)
{
    const uint8_t ctrl[2] = {HTS221_REG_CTRL_REG1, HTS221_CTRL_REG1_VALUE};
    const uint8_t avg[2] = {HTS221_REG_AV_CONF, HTS221_AV_CONF_VALUE};
    const uint8_t calib_reg = (uint8_t) (HTS221_REG_CALIB_START | HTS221_AUTO_INC);
    uint8_t calib[HTS221_CALIB_LEN];

    s_ht.ready = false;

    if (i2c_driver->write(HTS221_I2C_ADDR, ctrl, HTS221_WRITE_LEN) != I2C_ERR_OK)
    {
        return HUMIDITY_TEMP_ERR_I2C;
    }
    if (i2c_driver->write(HTS221_I2C_ADDR, avg, HTS221_WRITE_LEN) != I2C_ERR_OK)
    {
        return HUMIDITY_TEMP_ERR_I2C;
    }
    if (i2c_driver->write_read(HTS221_I2C_ADDR, &calib_reg, 1U, calib, HTS221_CALIB_LEN) !=
        I2C_ERR_OK)
    {
        return HUMIDITY_TEMP_ERR_I2C;
    }

    prv_parse_calibration(calib);
    s_ht.ready = true;
    return HUMIDITY_TEMP_ERR_OK;
}

humidity_temp_err_t humidity_temp_read(humidity_temp_data_t *out)
{
    const uint8_t out_reg = (uint8_t) (HTS221_REG_HUMIDITY_OUT_L | HTS221_AUTO_INC);
    uint8_t raw[HTS221_OUT_LEN];
    bool data_ready = false;
    humidity_temp_err_t err;

    if (out == NULL)
    {
        return HUMIDITY_TEMP_ERR_INVALID_ARG;
    }
    if (!s_ht.ready)
    {
        return HUMIDITY_TEMP_ERR_NOT_READY;
    }

    err = prv_read_status(&data_ready);
    if ((err == HUMIDITY_TEMP_ERR_OK) && !data_ready)
    {
        vTaskDelay(pdMS_TO_TICKS(HUMIDITY_TEMP_RETRY_DELAY_MS));
        err = prv_read_status(&data_ready);
    }
    if (err != HUMIDITY_TEMP_ERR_OK)
    {
        return err;
    }
    if (!data_ready)
    {
        return HUMIDITY_TEMP_ERR_NOT_READY;
    }

    if (i2c_driver->write_read(HTS221_I2C_ADDR, &out_reg, 1U, raw, HTS221_OUT_LEN) != I2C_ERR_OK)
    {
        return HUMIDITY_TEMP_ERR_I2C;
    }

    out->humidity_x100 = prv_compensate_humidity(prv_le_i16(&raw[0]));
    out->temperature_x100 = prv_compensate_temperature(prv_le_i16(&raw[2]));
    return HUMIDITY_TEMP_ERR_OK;
}

static const ihumidity_temp_t s_humidity_temp_vtable = {
    .init = humidity_temp_init,
    .read = humidity_temp_read,
};

const ihumidity_temp_t *const humidity_temp_driver = &s_humidity_temp_vtable;

#ifdef TEST
void humidity_temp_reset_for_test(void)
{
    (void) memset(&s_ht, 0, sizeof(s_ht));
}
#endif
