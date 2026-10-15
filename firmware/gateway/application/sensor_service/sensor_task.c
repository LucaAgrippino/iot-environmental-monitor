/**
 * @file sensor_task.c
 * @brief SensorTask implementation (Gateway).
 *
 * @see docs/lld/application/sensor-alarm-service.md
 */

#include "sensor_task.h"

#include <stddef.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#include "alarm_service/alarm_service.h"
#include "sensor_service/sensor_service.h"

#define TIMER_NAME "SensPoll"
#define TASK_NAME "SensorTask"

static StaticTask_t s_task_tcb;
static StackType_t s_task_stack[SENSOR_TASK_STACK_WORDS];
static StaticTimer_t s_timer_buf;
static TaskHandle_t s_task_handle;

/* Timer service task context: only sends the tick, never touches SensorService. */
static void prv_poll_timer_cb(TimerHandle_t timer)
{
    (void) timer;
    (void) xTaskNotifyGive(s_task_handle);
}

static void prv_sensor_task(void *params)
{
    (void) params;

    TimerHandle_t timer = xTimerCreateStatic(TIMER_NAME, pdMS_TO_TICKS(SENSOR_TASK_PERIOD_MS),
                                             pdTRUE, NULL, prv_poll_timer_cb, &s_timer_buf);
    if (timer != NULL)
    {
        (void) xTimerStart(timer, portMAX_DELAY);
    }

    for (;;)
    {
        /* Without the timer there is no tick; park instead of spinning. */
        (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        (void) sensor_service_run_cycle();
    }
}

sensor_task_err_t sensor_task_create(health_monitor_handle_t health)
{
    if (health == NULL)
    {
        return SENSOR_TASK_ERR_NULL_ARG;
    }

    if ((sensor_service_init(health) != SENSOR_SERVICE_ERR_OK) ||
        (alarm_service_init() != ALARM_SERVICE_ERR_OK))
    {
        return SENSOR_TASK_ERR_INIT;
    }

    s_task_handle = xTaskCreateStatic(prv_sensor_task, TASK_NAME, SENSOR_TASK_STACK_WORDS, NULL,
                                      SENSOR_TASK_PRIORITY, s_task_stack, &s_task_tcb);
    return (s_task_handle != NULL) ? SENSOR_TASK_ERR_OK : SENSOR_TASK_ERR_RTOS;
}
