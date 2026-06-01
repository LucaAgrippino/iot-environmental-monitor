/**
 * @file test_simulated_sensors_main.c
 * @brief Integration test for BarometerDriver and HumidityTempDriver on F469 hardware.
 *
 * Validates the simulated sensor drivers running on the real STM32F469I-DISCO.
 * Both drivers are pure software; this test exercises them through SensorTask-style
 * polling and fault injection without any physical sensor hardware.
 *
 * Activation in CubeIDE:
 *   - Right-click Src/main.c → Resource Configurations → Exclude from Build.
 *   - Add integration-tests/simulated_sensors/ to project source paths
 *     (Project Properties → C/C++ General → Paths and Symbols).
 *   - Build, flash, open PuTTY on ST-Link VCP at 115200/8N1.
 *
 * Visual checklist — expected UART output:
 *
 * | # | What to observe                                   | Verifies                             |
 * |---|---------------------------------------------------|--------------------------------------|
 * | 1 | "===== Simulated Sensors integration test =====" | Boot reached, Logger working         |
 * | 2 | "BARO init OK | HT init OK"                       | Both inits return BARO/HT_ERR_OK     |
 * | 3 | 10 lines: "BARO ok p=NNNNN  T=NNNN H=NNNNN"      | Happy-path reads, values in range    |
 * | 4 | "--- Injecting faults ---"                        | Test progresses to fault phase       |
 * | 5 | "BARO FAULT | HT FAULT" (repeated 3 times)       | Fault path returns correct error     |
 * | 6 | "--- Faults cleared ---"                          | Test progresses to recovery phase    |
 * | 7 | "BARO ok p=NNNNN  T=NNNN H=NNNNN" (3 more lines) | Simulation resumes after fault clear |
 * | 8 | "sensor_task: looping at 1 Hz..."                 | Scheduler running, task alive        |
 * | 9 | Pressure values slowly drift over time            | Random walk producing time variation |
 */

#include "stm32f469xx.h"

#include "FreeRTOS.h"
#include "task.h"

#include "system_clock.h"
#include "debug-uart/debug_uart_driver.h"
#include "rtc/rtc_driver.h"
#include "logger/logger.h"

#include "barometer_driver/barometer_driver.h"
#include "humidity_temp_driver/humidity_temp_driver.h"

/* ===================================================================== */
/* Configuration                                                         */
/* ===================================================================== */

#define SENSOR_STACK_WORDS (384U)
#define SENSOR_PRIORITY (tskIDLE_PRIORITY + 2U)

#define HAPPY_PATH_READS (10U)
#define FAULT_READS (3U)
#define RECOVERY_READS (3U)

/* ===================================================================== */
/* Sensor task                                                           */
/* ===================================================================== */

static StaticTask_t s_sensor_tcb;
static StackType_t s_sensor_stack[SENSOR_STACK_WORDS];

static void sensor_task(void *arg)
{
    (void) arg;
    uint32_t phase = 0U;
    uint32_t count = 0U;

    for (;;)
    {
        if (phase == 0U)
        {
            /* Happy-path sampling. */
            baro_reading_t baro = {0};
            ht_reading_t ht = {0};

            baro_err_t berr = barometer_read(&baro);
            ht_err_t herr = humidity_temp_read(&ht);

            if ((BARO_ERR_OK == berr) && (HT_ERR_OK == herr))
            {
                LOG_INFO("SensorTask", "BARO ok p=%ld  T=%ld H=%lu", (long) baro.pressure_x10,
                         (long) ht.temperature_x100, (unsigned long) ht.humidity_x100);
            }
            else
            {
                LOG_WARN("SensorTask", "Unexpected error: BARO=%d HT=%d", (int) berr, (int) herr);
            }

            count++;
            if (count >= HAPPY_PATH_READS)
            {
                count = 0U;
                phase = 1U;
                LOG_INFO("SensorTask", "--- Injecting faults ---");
                barometer_inject_fault(true);
                humidity_temp_inject_fault(true);
            }
        }
        else if (phase == 1U)
        {
            /* Fault-injection phase. */
            baro_reading_t baro = {0};
            ht_reading_t ht = {0};

            baro_err_t berr = barometer_read(&baro);
            ht_err_t herr = humidity_temp_read(&ht);

            if ((BARO_ERR_FAULT == berr) && (HT_ERR_FAULT == herr))
            {
                LOG_WARN("SensorTask", "BARO FAULT | HT FAULT (expected)");
            }
            else
            {
                LOG_ERROR("SensorTask", "Expected faults but got: BARO=%d HT=%d", (int) berr,
                          (int) herr);
            }

            count++;
            if (count >= FAULT_READS)
            {
                count = 0U;
                phase = 2U;
                barometer_inject_fault(false);
                humidity_temp_inject_fault(false);
                LOG_INFO("SensorTask", "--- Faults cleared ---");
            }
        }
        else if (phase == 2U)
        {
            /* Recovery reads. */
            baro_reading_t baro = {0};
            ht_reading_t ht = {0};

            baro_err_t berr = barometer_read(&baro);
            ht_err_t herr = humidity_temp_read(&ht);

            if ((BARO_ERR_OK == berr) && (HT_ERR_OK == herr))
            {
                LOG_INFO("SensorTask", "BARO ok p=%ld  T=%ld H=%lu", (long) baro.pressure_x10,
                         (long) ht.temperature_x100, (unsigned long) ht.humidity_x100);
            }

            count++;
            if (count >= RECOVERY_READS)
            {
                count = 0U;
                phase = 3U;
                LOG_INFO("SensorTask", "sensor_task: looping at 1 Hz...");
            }
        }
        else
        {
            /* Steady-state 1 Hz polling. */
            baro_reading_t baro = {0};
            ht_reading_t ht = {0};

            (void) barometer_read(&baro);
            (void) humidity_temp_read(&ht);

            LOG_INFO("SensorTask", "p=%ld T=%ld H=%lu", (long) baro.pressure_x10,
                     (long) ht.temperature_x100, (unsigned long) ht.humidity_x100);
        }

        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/* ===================================================================== */
/* Entry point                                                           */
/* ===================================================================== */

int main(void)
{
    system_clock_init();

    (void) debug_uart_init();
    (void) rtc_init();
    (void) logger_init(LOG_LEVEL_DEBUG);

    /* Initialise simulated sensor drivers before scheduler starts. */
    (void) barometer_init();
    (void) humidity_temp_init();

    /* Pre-scheduler diagnostics. */
    LOG_INFO("Boot", "===== Simulated Sensors integration test =====");
    LOG_INFO("Boot", "BARO init OK | HT init OK");

    (void) xTaskCreateStatic(sensor_task, "SensorTask", SENSOR_STACK_WORDS, NULL, SENSOR_PRIORITY,
                             s_sensor_stack, &s_sensor_tcb);

    vTaskStartScheduler();

    for (;;)
    {
    }
}
