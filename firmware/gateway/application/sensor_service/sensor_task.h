/**
 * @file sensor_task.h
 * @brief SensorTask (Gateway) — schedules SensorService and AlarmService every 100 ms.
 *
 * Hosts SensorService and AlarmService (SS-D1). A 100 ms software timer sends a
 * direct-to-task notification and the task runs one acquisition cycle per tick
 * (SS-D6). Task parameters follow task-breakdown.md section 5.2.
 *
 * @note Companion: docs/lld/application/sensor-alarm-service.md (Gateway sections)
 */

#ifndef SENSOR_TASK_H
#define SENSOR_TASK_H

#include <stdint.h>

#include "health_monitor/health_monitor.h"

/** Error codes returned by sensor_task_create(). */
typedef enum
{
    SENSOR_TASK_ERR_OK = 0,
    SENSOR_TASK_ERR_NULL_ARG = 1, /**< health handle is NULL. */
    SENSOR_TASK_ERR_INIT = 2,     /**< SensorService or AlarmService init failed. */
    SENSOR_TASK_ERR_RTOS = 3,     /**< Task could not be created. */
} sensor_task_err_t;

/** Acquisition period (REQ-SA-070). */
#define SENSOR_TASK_PERIOD_MS (100U)

/** Priority above ModbusPollerTask so a retry storm cannot starve acquisition (section 5.3). */
#define SENSOR_TASK_PRIORITY (5U)

/** Stack depth in words (LLD section 11.2: about 300 estimated, 384 provisioned). */
#define SENSOR_TASK_STACK_WORDS (384U)

/**
 * @brief Initialise SensorService and AlarmService, then create SensorTask.
 *
 * Initialisation happens here, before the scheduler starts, because the Group A and
 * Group B driver headers require it (SS-D9). The task creates and starts the 100 ms
 * timer on its first run, so no acquisition happens until the scheduler is running.
 *
 * @param[in] health  HealthMonitor handle handed to SensorService.
 * @return SENSOR_TASK_ERR_OK; SENSOR_TASK_ERR_NULL_ARG; SENSOR_TASK_ERR_INIT;
 *         SENSOR_TASK_ERR_RTOS.
 * @note Threading: call once, before the scheduler starts, after i2c_init() and gpio_init().
 */
sensor_task_err_t sensor_task_create(health_monitor_handle_t health);

#endif /* SENSOR_TASK_H */
