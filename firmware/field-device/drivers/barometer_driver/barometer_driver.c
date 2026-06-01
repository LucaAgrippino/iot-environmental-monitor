#include "barometer_driver.h"

#include <stdlib.h>

/* ------------------------------------------------------------------ */
/* Module-level state                                                  */
/* ------------------------------------------------------------------ */

static int32_t s_pressure_x10 = BARO_DEFAULT_PRESSURE_X10;
static bool s_fault_injected = false;

/* ------------------------------------------------------------------ */
/* Private helpers                                                     */
/* ------------------------------------------------------------------ */

BARO_TEST_VISIBLE int32_t random_delta(void)
{
    return (int32_t) (rand() % 5) - 2;
}

/* ------------------------------------------------------------------ */
/* Public API                                                          */
/* ------------------------------------------------------------------ */

baro_err_t barometer_init(void)
{
    s_pressure_x10 = BARO_DEFAULT_PRESSURE_X10;
    s_fault_injected = false;
    return BARO_ERR_OK;
}

baro_err_t barometer_read(baro_reading_t *reading)
{
    if (NULL == reading)
    {
        return BARO_ERR_FAULT;
    }

    if (s_fault_injected)
    {
        return BARO_ERR_FAULT;
    }

    s_pressure_x10 += random_delta();

    if (s_pressure_x10 < BARO_PRESSURE_MIN_X10)
    {
        s_pressure_x10 = BARO_PRESSURE_MIN_X10;
    }

    if (s_pressure_x10 > BARO_PRESSURE_MAX_X10)
    {
        s_pressure_x10 = BARO_PRESSURE_MAX_X10;
    }

    reading->pressure_x10 = s_pressure_x10;
    return BARO_ERR_OK;
}

void barometer_inject_fault(bool inject)
{
    s_fault_injected = inject;
}

/* ------------------------------------------------------------------ */
/* Test hooks                                                          */
/* ------------------------------------------------------------------ */

#ifdef TEST
void barometer_reset_for_test(void)
{
    s_pressure_x10 = BARO_DEFAULT_PRESSURE_X10;
    s_fault_injected = false;
}

void barometer_set_pressure_for_test(int32_t pressure_x10)
{
    s_pressure_x10 = pressure_x10;
}

void barometer_get_state_for_test(int32_t *out_pressure, bool *out_fault)
{
    if (NULL != out_pressure)
    {
        *out_pressure = s_pressure_x10;
    }
    if (NULL != out_fault)
    {
        *out_fault = s_fault_injected;
    }
}
#endif /* TEST */
