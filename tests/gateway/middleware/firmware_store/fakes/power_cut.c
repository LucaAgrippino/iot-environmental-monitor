/**
 * @file power_cut.c
 * @brief Power-loss injector shared by the flash fakes (FirmwareStore tests).
 */

#include "power_cut.h"

#define NOT_ARMED (0xFFFFFFFFUL)

static uint32_t s_count;
static uint32_t s_cut_at = NOT_ARMED;
static bool s_dead;

void power_cut_reset(void)
{
    s_count = 0U;
    s_cut_at = NOT_ARMED;
    s_dead = false;
}

void power_cut_arm(uint32_t operation_index)
{
    s_cut_at = operation_index;
}

void power_cut_restore(void)
{
    s_dead = false;
    s_cut_at = NOT_ARMED;
}

bool power_cut_is_dead(void)
{
    return s_dead;
}

uint32_t power_cut_operations(void)
{
    return s_count;
}

power_cut_verdict_t power_cut_tick(void)
{
    if (s_dead)
    {
        return POWER_CUT_DEAD;
    }

    const uint32_t index = s_count;

    s_count++;

    if (index == s_cut_at)
    {
        s_dead = true;
        return POWER_CUT_TORN;
    }

    return POWER_CUT_PASS;
}
