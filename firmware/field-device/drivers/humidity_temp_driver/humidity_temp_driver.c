#include "humidity_temp_driver.h"

#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Module-level state                                                  */
/* ------------------------------------------------------------------ */

static int32_t s_temperature_x100 = HT_DEFAULT_TEMPERATURE_X100;
static uint32_t s_humidity_x100 = HT_DEFAULT_HUMIDITY_X100;
static bool s_fault_injected = false;

/* ------------------------------------------------------------------ */
/* Private helpers                                                     */
/* ------------------------------------------------------------------ */

HT_TEST_VISIBLE int32_t ht_random_delta(void)
{
    return (int32_t) (rand() % 5) - 2;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

ht_err_t humidity_temp_init(void)
{
    s_temperature_x100 = HT_DEFAULT_TEMPERATURE_X100;
    s_humidity_x100 = HT_DEFAULT_HUMIDITY_X100;
    s_fault_injected = false;
    return HT_ERR_OK;
}

ht_err_t humidity_temp_read(ht_reading_t *reading)
{
    if (NULL == reading)
    {
        return HT_ERR_FAULT;
    }

    if (s_fault_injected)
    {
        return HT_ERR_FAULT;
    }

    int32_t temp_delta = ht_random_delta();
    int32_t humi_delta = ht_random_delta();

    s_temperature_x100 += temp_delta;

    if (s_temperature_x100 < HT_TEMPERATURE_MIN_X100)
    {
        s_temperature_x100 = HT_TEMPERATURE_MIN_X100;
    }

    if (s_temperature_x100 > HT_TEMPERATURE_MAX_X100)
    {
        s_temperature_x100 = HT_TEMPERATURE_MAX_X100;
    }

    /* Humidity is unsigned; guard against wrapping before casting. */
    if (humi_delta < 0 && (uint32_t) (-humi_delta) > s_humidity_x100)
    {
        s_humidity_x100 = HT_HUMIDITY_MIN_X100;
    }
    else
    {
        s_humidity_x100 = (uint32_t) ((int32_t) s_humidity_x100 + humi_delta);
    }

    if (s_humidity_x100 > HT_HUMIDITY_MAX_X100)
    {
        s_humidity_x100 = HT_HUMIDITY_MAX_X100;
    }

    reading->temperature_x100 = s_temperature_x100;
    reading->humidity_x100 = s_humidity_x100;
    return HT_ERR_OK;
}

void humidity_temp_inject_fault(bool inject)
{
    s_fault_injected = inject;
}

/* ------------------------------------------------------------------ */
/* Test hooks                                                          */
/* ------------------------------------------------------------------ */

#ifdef TEST
void humidity_temp_reset_for_test(void)
{
    s_temperature_x100 = HT_DEFAULT_TEMPERATURE_X100;
    s_humidity_x100 = HT_DEFAULT_HUMIDITY_X100;
    s_fault_injected = false;
}

void humidity_temp_set_temp_for_test(int32_t temperature_x100)
{
    s_temperature_x100 = temperature_x100;
}

void humidity_temp_set_humidity_for_test(uint32_t humidity_x100)
{
    s_humidity_x100 = humidity_x100;
}

void humidity_temp_get_state_for_test(int32_t *out_temp, uint32_t *out_humidity, bool *out_fault)
{
    if (NULL != out_temp)
    {
        *out_temp = s_temperature_x100;
    }
    if (NULL != out_humidity)
    {
        *out_humidity = s_humidity_x100;
    }
    if (NULL != out_fault)
    {
        *out_fault = s_fault_injected;
    }
}
#endif /* TEST */
