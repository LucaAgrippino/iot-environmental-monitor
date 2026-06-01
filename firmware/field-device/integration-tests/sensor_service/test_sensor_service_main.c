/**
 * @file test_sensor_service_main.c
 * @brief Integration test for SensorService + AlarmService — STM32F469I-DISCO.
 *
 * Flash to the Field Device and observe the following via SWO / UART:
 *
 * VISUAL CHECKLIST
 * ================
 * | # | What to observe                                        | Verifies                              |
 * |---|-------------------------------------------------------|---------------------------------------|
 * | 1 | "[SS] SensorService initialised" printed at startup   | sensor_service_init() succeeds         |
 * | 2 | "[AS] AlarmService initialised" printed at startup    | alarm_service_init() succeeds          |
 * | 3 | "[IT] Cycle <N> T=xx.xx H=xx.xx P=xxxx.x" at ~5 Hz   | Pipeline running (bug: 5Hz not 10Hz)   |
 * | 4 | Readings change slowly (IIR filter visible)           | IIR filter applied                     |
 * | 5 | "[IT] ALARM RAISED HIGH TEMPERATURE" when T > 35°C   | AlarmService threshold detection       |
 * | 6 | "[IT] ALARM CLEARED TEMPERATURE" after T drops < 33°C | Hysteresis working correctly           |
 * | 7 | "is_ready: 1" appears after first clean cycle         | sensor_service_is_ready() returns true |
 *
 * NOTE: Readings update at ~5 Hz instead of the specified 10 Hz because the
 *       poll timer period is misconfigured (see bug-log). At 5 Hz the IIR
 *       filter time constant is approximately 2 s instead of 1 s — readings
 *       will appear sluggish when the sensor is warmed by hand.
 *
 * Hardware: STM32F469I-DISCO. SWO ITM on pin PB3 at 2 MHz.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#include "Inc/system_clock.h"
#include "logger/logger.h"
#include "health_monitor/health_monitor.h"
#include "time_provider/time_provider.h"
#include "sensor_service/sensor_service.h"
#include "alarm_service/alarm_service.h"

/* ======================================================================= */
/* Constants                                                                */
/* ======================================================================= */

#define IT_TAG "IT"

#define SENSOR_TASK_STACK_WORDS  (256U)
#define SENSOR_TASK_PRIORITY     (2U)

/* ======================================================================= */
/* Static task storage                                                      */
/* ======================================================================= */

static StaticTask_t  s_sensor_task_tcb;
static StackType_t   s_sensor_task_stack[SENSOR_TASK_STACK_WORDS];

/* ======================================================================= */
/* Alarm subscriber                                                         */
/* ======================================================================= */

static void alarm_event_cb(sensor_id_t             sensor,
                            alarm_event_t           event,
                            const sensor_reading_t *reading)
{
    const char *sensor_name = (sensor == SENSOR_ID_TEMPERATURE) ? "TEMPERATURE"
                            : (sensor == SENSOR_ID_HUMIDITY)    ? "HUMIDITY"
                            : (sensor == SENSOR_ID_PRESSURE)    ? "PRESSURE"
                            : "UNKNOWN";

    if (event == ALARM_EVENT_RAISED_HIGH)
    {
        LOG_ERROR(IT_TAG, "ALARM RAISED HIGH %s (val=%.2f)",
                  sensor_name, (double)reading->value);
    }
    else if (event == ALARM_EVENT_RAISED_LOW)
    {
        LOG_ERROR(IT_TAG, "ALARM RAISED LOW %s (val=%.2f)",
                  sensor_name, (double)reading->value);
    }
    else
    {
        LOG_INFO(IT_TAG, "ALARM CLEARED %s (val=%.2f)",
                 sensor_name, (double)reading->value);
    }
}

/* ======================================================================= */
/* SensorTask                                                               */
/* ======================================================================= */

static void vSensorTask(void *pvParameters)
{
    (void)pvParameters;

    sensor_service_init();
    alarm_service_init();
    alarm_service_subscribe(alarm_event_cb);

    LOG_INFO(IT_TAG, "SensorTask running — waiting for poll timer");

    for (;;)
    {
        /* Block until the 200 ms poll timer fires (bug: should be 100 ms). */
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        sensor_service_run_cycle();

        sensor_snapshot_t snap;
        if (sensor_service_get_snapshot(&snap) == SENSOR_SERVICE_ERR_OK)
        {
            LOG_INFO(IT_TAG,
                     "Cycle %lu  T=%.2f  H=%.2f  P=%.1f  rdy=%d",
                     (unsigned long)snap.cycle_count,
                     (double)snap.readings[SENSOR_ID_TEMPERATURE].value,
                     (double)snap.readings[SENSOR_ID_HUMIDITY].value,
                     (double)snap.readings[SENSOR_ID_PRESSURE].value,
                     (int)sensor_service_is_ready());
        }
    }
}

/* ======================================================================= */
/* main                                                                     */
/* ======================================================================= */

int main(void)
{
    system_clock_init();
    logger_init(LOG_LEVEL_DEBUG);
    health_monitor_init();
    time_provider_init(health_report);

    LOG_INFO(IT_TAG, "=== SensorService + AlarmService integration test ===");
    LOG_INFO(IT_TAG, "Board: STM32F469I-DISCO (Field Device)");

    (void)xTaskCreateStatic(
        vSensorTask, "SensorTask",
        SENSOR_TASK_STACK_WORDS, NULL,
        SENSOR_TASK_PRIORITY,
        s_sensor_task_stack, &s_sensor_task_tcb);

    vTaskStartScheduler();

    /* Should never reach here */
    for (;;) {}
    return 0;
}
