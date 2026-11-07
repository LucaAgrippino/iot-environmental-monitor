#ifndef SIM_PROVIDERS_H
#define SIM_PROVIDERS_H

/**
 * @file sim_providers.h
 * @brief Scenario control of the simulator's synthetic services.
 */

#include <stdint.h>

typedef enum
{
    SIM_NORMAL = 0,   /**< All sensors valid. */
    SIM_WAITING = 1,  /**< No reading yet. */
    SIM_STALE = 2,    /**< Pressure reading overdue. */
    SIM_ERROR = 3,    /**< Temperature fails after a few seconds. */
    SIM_ALARM = 4,    /**< Temperature HIGH and pressure LOW alarms. */
    SIM_DEGRADED = 5, /**< Modbus link down, CRC errors. */
    SIM_GAP = 6       /**< Humidity invalid for 30 s (a gap in the trend). */
} sim_scenario_t;

/** Set the virtual time that the synthetic data follows. */
void sim_set_time_ms(uint32_t ms);

/** Select a scenario. */
void sim_set_scenario(sim_scenario_t s);

/** @return The scenario for a name such as "alarm"; SIM_NORMAL if unknown. */
sim_scenario_t sim_scenario_from_name(const char *name);

#endif /* SIM_PROVIDERS_H */
