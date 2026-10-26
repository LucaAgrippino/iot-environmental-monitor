/**
 * @file model.h
 * @brief The console's read-only view of the system, pulled from the injected services.
 *
 * The console owns no sensor, alarm, configuration or health data. model_refresh() copies the
 * current state out of the services (P7, pull-based) into one structure that rendering and
 * commands read. Thresholds therefore come from ConfigService every frame, so a `set` is
 * reflected by the chart without any notification.
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §5, §8.5
 */

#ifndef MODEL_H
#define MODEL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "chart.h"
#include "console_service/console_types.h"

/** @brief Fixed description of one chart channel. */
typedef struct
{
    const char *label; /**< "TEMPERATURE".                                    */
    const char *name;  /**< "temp".                                           */
    const char *unit;  /**< "°C".                                             */
    sensor_id_t id;    /**< SensorService channel.                            */
    int32_t scale;     /**< Units per 1.0 in SensorService / ConfigService.   */
    int32_t band;      /**< Proximity window, same units.                     */
    uint8_t decimals;  /**< Decimals shown.                                   */
} channel_info_t;

/** @brief Snapshot of everything the console shows. */
typedef struct
{
    bool sensors_ok;
    sensor_snapshot_t snap;
    bool health_ok;
    health_snapshot_t health;
    bool alarms_ok;
    alarm_state_t alarm_state[SENSOR_ID_COUNT];
    const config_params_t *cfg; /**< Live parameters; NULL until ConfigService is up. */
} console_model_t;

/** @brief Description of chart channel @p ch (0 temp, 1 humid, 2 press). */
const channel_info_t *model_channel(uint8_t ch);

/** @brief Pull the current state out of the services. Failed pulls clear their *_ok flag. */
void model_refresh(const console_deps_t *deps, console_model_t *m);

/** @brief Latest value of a chart channel; false if the reading is not valid. */
bool model_value(const console_model_t *m, uint8_t ch, int32_t *value_out);

/** @brief Alarm limits of a chart channel from the live configuration. */
chart_limits_t model_limits(const console_model_t *m, uint8_t ch);

/** @brief Number of channels (of temp / humid / press) currently in alarm. */
uint8_t model_alarm_count(const console_model_t *m);

/** @brief Number of the four sensor devices with at least one valid channel. */
uint8_t model_sensors_online(const console_model_t *m);

/** @brief Uptime as "Nd HH:MM" (or "HH:MM" under a day) for the banner. */
void model_uptime_short(const console_model_t *m, char *buf, size_t size);

/** @brief Uptime as "Nd HH:MM:SS" for the status screens. */
void model_uptime_long(const console_model_t *m, char *buf, size_t size);

/** @brief "HH:MM:SS" of a reading's timestamp (UTC when synchronised, uptime otherwise). */
void model_time_of(const sensor_reading_t *r, char *buf, size_t size);

#endif /* MODEL_H */
