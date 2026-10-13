/**
 * @file device_profile_registry.c
 * @brief Weak placeholder for DeviceProfileRegistry — one hard-coded Field Device profile.
 *
 * Replace this file when the real DeviceProfileRegistry is implemented.  The
 * functions are weak so a bring-up harness or the real module can override
 * them without touching this file.
 */

#include "device_profile_registry.h"

#include <stddef.h>

#define DEFAULT_FD_SLAVE_ADDR (1U)           /* config_params.h: modbus_slave_addr default */
#define DEFAULT_FD_INPUT_REG_START (0x0010U) /* temperature, humidity, pressure */
#define DEFAULT_FD_INPUT_REG_COUNT (3U)
#define DEFAULT_PROFILE_COUNT (1U)

static const device_profile_t g_default_profiles[DEFAULT_PROFILE_COUNT] = {
    {
        .slave_addr = DEFAULT_FD_SLAVE_ADDR,
        .input_reg_start = DEFAULT_FD_INPUT_REG_START,
        .input_reg_count = DEFAULT_FD_INPUT_REG_COUNT,
    },
};

__attribute__((weak)) bool device_profile_get_next(const device_profile_t **profile, uint8_t *index)
{
    if ((profile == NULL) || (index == NULL) || (*index >= DEFAULT_PROFILE_COUNT))
    {
        return false;
    }

    *profile = &g_default_profiles[*index];
    (*index)++;
    return true;
}

__attribute__((weak)) bool device_profile_is_registered(uint8_t slave_addr)
{
    for (uint8_t i = 0U; i < DEFAULT_PROFILE_COUNT; i++)
    {
        if (g_default_profiles[i].slave_addr == slave_addr)
        {
            return true;
        }
    }
    return false;
}
