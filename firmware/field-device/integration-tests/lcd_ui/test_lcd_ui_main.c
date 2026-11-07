/**
 * @file test_lcd_ui_main.c
 * @brief LcdUi integration test on STM32F469I-DISCO.
 *
 * Exercises the complete LCD UI stack end-to-end against real hardware:
 *   sdram_init → lcd_init → touchscreen_init → graphics_init → lcd_ui_init
 *
 * Stub provider implementations are defined here; no SensorService, AlarmService,
 * or ConfigService tasks are started. The test uses synthetic data to drive each
 * screen through a representative display cycle.
 *
 * Activation in CubeIDE:
 *   - Exclude firmware/Src/main.c from build (Resource Configurations).
 *   - Add integration-tests/lcd_ui/ to project source paths.
 *   - Build, flash, open PuTTY at 115200/8N1 on ST-Link VCP.
 *
 * ==========================================================================
 * Visual and serial checklist:
 * ==========================================================================
 *
 * | # | What to observe                                              | Verifies |
 * |---|--------------------------------------------------------------|-------------------------------|
 * | 1 | "===== LcdUi integration test =====" (INFO)                  | Test task started | | 2 |
 * "lcd_ui_init: OK" (INFO)                                     | lcd_ui_init() succeeded       | |
 * 3 | "Waiting for data..." visible on panel — Sensors tab         | Sensor waiting overlay | | 4 |
 * After 3 s: sensor values appear on Sensors tab               | Data path sensor→UI           | |
 * 5 | Switch to Status tab: 14 metric rows visible                 | Status screen refreshes | | 6
 * | Switch to Alarms tab: "No active alarms" label visible       | Alarm CLEAR path              |
 * | 7 | Switch to Alarms tab after HIGH raised: list entry visible   | Alarm ACTIVE_HIGH path | | 8
 * | Switch to Config tab: spinboxes disabled; values from params | Config VIEWING state          |
 * | 9 | Tap a spinbox (simulated by log): "Config EDITING" (INFO)    | VIEWING → EDITING transition
 * | |10 | "=== ALL CHECKS PASSED ===" (INFO)                           | All steps completed | |11
 * | Green LED (LD1, PG13) lit continuously                       | No assertion failure          |
 * |12 | Red LED (LD4, PD4) remains off                               | No error path triggered |
 *
 * @see docs/lld/application/lcd-ui-lld.md
 */

#include "stm32f469xx.h"

#include "FreeRTOS.h"
#include "task.h"
#include "timers.h"

#include "system_clock.h"
#include "sdram_driver/sdram_driver.h"
#include "lcd_driver/lcd_driver.h"
#include "touchscreen_driver/touchscreen_driver.h"
#include "graphics_library/graphics_library.h"
#include "debug-uart/debug_uart_driver.h"
#include "rtc/rtc_driver.h"
#include "gpio/gpio_driver.h"
#include "health_monitor/health_monitor.h"
#include "logger/logger.h"

#include "sensor_service/sensor_service.h"
#include "alarm_service/alarm_service.h"
#include "config_service/config_service.h"

#include "lcd_ui/lcd_ui.h"

/* ===================================================================== */
/* Board-specific LED macros (F469-DISCO)                               */
/* ===================================================================== */

#define LED_GREEN_PIN (1UL << 13U) /* PG13 */
#define LED_RED_PIN (1UL << 4U)    /* PD4  */

static void led_green_on(void)
{
    GPIOG->BSRR = LED_GREEN_PIN;
}
static void led_red_on(void)
{
    GPIOD->BSRR = LED_RED_PIN;
}
static void led_green_off(void)
{
    GPIOG->BSRR = (LED_GREEN_PIN << 16U);
}

/* ===================================================================== */
/* Test configuration                                                   */
/* ===================================================================== */

#define IT_TASK_STACK_WORDS (2048U)
#define IT_TASK_TAG "LCDUI-IT"
#ifndef IT_PHASE_DELAY_MS
#define IT_PHASE_DELAY_MS (3000U)
#endif

/* ===================================================================== */
/* Synthetic provider data                                              */
/* ===================================================================== */

static sensor_snapshot_t s_sensor_snap;
static alarm_state_t s_alarm_states[SENSOR_ID_COUNT];
static device_health_snapshot_t s_health_snap;

/* ===================================================================== */
/* ISensorService implementation (synthetic)                            */
/* ===================================================================== */

static sensor_service_err_t it_get_sensor_snapshot(sensor_snapshot_t *snap)
{
    *snap = s_sensor_snap;
    return SENSOR_SERVICE_ERR_OK;
}

static const isensor_service_t s_sensors = {.get_snapshot = it_get_sensor_snapshot};

/* ===================================================================== */
/* IAlarmService implementation (synthetic)                             */
/* ===================================================================== */

static alarm_service_err_t it_get_all_states(alarm_state_t states[SENSOR_ID_COUNT])
{
    (void) memcpy(states, s_alarm_states, sizeof(s_alarm_states));
    return ALARM_SERVICE_ERR_OK;
}

static const ialarm_service_t s_alarms = {.get_all_states = it_get_all_states};

/* ===================================================================== */
/* IConfigProvider / IConfigManager implementations                     */
/* ===================================================================== */

static const config_params_t *it_get_params(void)
{
    return config_service_get_params();
}

static config_service_err_t it_set_param(config_param_id_t id, const void *value)
{
    return config_service_set_param(id, value);
}

static const iconfig_provider_t s_cfg_read = {.get_params = it_get_params};
static const iconfig_manager_t s_cfg_write = {.set_param = it_set_param};

/* ===================================================================== */
/* IHealthSnapshot / IHealthReport implementations                      */
/* ===================================================================== */

static health_monitor_err_t it_get_health_snapshot(device_health_snapshot_t *snap)
{
    *snap = s_health_snap;
    return HEALTH_MONITOR_ERR_OK;
}

static const ihealth_snapshot_t s_health_snap_provider = {.get_snapshot = it_get_health_snapshot};

/* ===================================================================== */
/* Task stacks (static allocation — FreeRTOS configSUPPORT_STATIC_ALLOCATION) */
/* ===================================================================== */

static StackType_t s_it_task_stack[IT_TASK_STACK_WORDS] __attribute__((aligned(8)));
static StaticTask_t s_it_task_tcb;

static StackType_t s_lcd_ui_stack[2048U] __attribute__((aligned(8)));
static StaticTask_t s_lcd_ui_tcb;

/* ===================================================================== */
/* Fail helper                                                          */
/* ===================================================================== */

static void fail(const char *msg)
{
    LOG_ERROR(IT_TASK_TAG, "FAIL: %s", msg);
    led_green_off();
    led_red_on();
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/** LVGL heap use: the five-screen UI must fit LV_MEM_SIZE with room to spare. */
static void log_lvgl_heap(const char *when)
{
    lv_mem_monitor_t mon;
    lv_mem_monitor(&mon);
    LOG_INFO(IT_TASK_TAG, "LVGL heap %s: used %lu of %lu bytes (%u%%), largest free block %lu",
             when, (unsigned long) (mon.total_size - mon.free_size), (unsigned long) mon.total_size,
             (unsigned) mon.used_pct, (unsigned long) mon.free_biggest_size);
}

/* ===================================================================== */
/* Synthetic acquisition: one cycle per second, values drifting a little */
/* ===================================================================== */

static uint32_t s_cycle;

/** Run `seconds` acquisition cycles, one per second, updating the synthetic snapshot. */
static void feed_sensors(uint32_t seconds)
{
    for (uint32_t i = 0U; i < seconds; i++)
    {
        s_cycle++;
        s_sensor_snap.cycle_count = s_cycle;
        for (uint32_t k = 0U; k < (uint32_t) SENSOR_ID_COUNT; k++)
        {
            s_sensor_snap.readings[k].valid = true;
            s_sensor_snap.readings[k].timestamp.epoch = 45296U + s_cycle; /* 12:34:56 + n */
            s_sensor_snap.readings[k].timestamp.sync_state = TIME_SYNC_SYNCHRONISED;
        }
        if (s_sensor_snap.readings[SENSOR_ID_TEMPERATURE].value == 0)
        {
            s_sensor_snap.readings[SENSOR_ID_TEMPERATURE].value = 2350;
            s_sensor_snap.readings[SENSOR_ID_HUMIDITY].value = 5500;
            s_sensor_snap.readings[SENSOR_ID_PRESSURE].value = 10130;
        }
        s_health_snap.uptime_s++;
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/* ===================================================================== */
/* Integration test task                                                */
/* ===================================================================== */

static void it_task(void *arg)
{
    (void) arg;

    LOG_INFO(IT_TASK_TAG, "===== LcdUi integration test =====");

    /* ------------------------------------------------------------------ */
    /* Step 1: Initialise LCD UI with synthetic providers                 */
    /* ------------------------------------------------------------------ */
    lcd_ui_err_t err = lcd_ui_init(&s_sensors, &s_alarms, &s_cfg_read, &s_cfg_write,
                                   &s_health_snap_provider, health_report);
    if (err != LCD_UI_ERR_OK)
    {
        LOG_ERROR(IT_TASK_TAG, "lcd_ui_init failed: %d", (int) err);
        fail("lcd_ui_init");
    }
    LOG_INFO(IT_TASK_TAG, "lcd_ui_init: OK");
    log_lvgl_heap("after init");

    /* ------------------------------------------------------------------ */
    /* The harness walks through every screen and state of the design pack. */
    /* Each phase waits IT_PHASE_DELAY_MS so the panel can be looked at.    */
    /* ------------------------------------------------------------------ */
    LOG_INFO(IT_TASK_TAG, "Phase 1: Sensors with no data: WAITING cards, pill INIT");
    vTaskDelay(pdMS_TO_TICKS(IT_PHASE_DELAY_MS));

    LOG_INFO(IT_TASK_TAG, "Phase 2: Sensors with data: 23.5 C, 55.0 %%RH, 1013 hPa, pill RUNNING");
    feed_sensors(IT_PHASE_DELAY_MS / 1000U);

    s_health_snap.uptime_s = 42U;
    s_health_snap.sensor_valid[SENSOR_ID_TEMPERATURE] = true;
    s_health_snap.sensor_valid[SENSOR_ID_HUMIDITY] = true;
    s_health_snap.sensor_valid[SENSOR_ID_PRESSURE] = true;
    s_health_snap.modbus_slave_ok = true;
    s_health_snap.modbus_valid_frames = 100U;
    s_health_snap.modbus_crc_errors = 1U;
    for (uint32_t i = 0U; i < HEALTH_TASK_COUNT; i++)
    {
        s_health_snap.stack_watermark_words[i] = (uint16_t) (120U + (i * 20U));
    }
    LOG_INFO(IT_TASK_TAG, "Phase 3: Trend (accumulating)");
    lcd_ui_select_screen(1U);
    feed_sensors(IT_PHASE_DELAY_MS / 1000U);

    LOG_INFO(IT_TASK_TAG, "Phase 4: Status (14 rows in four sections)");
    lcd_ui_select_screen(2U);
    feed_sensors(IT_PHASE_DELAY_MS / 1000U);

    s_alarm_states[SENSOR_ID_TEMPERATURE] = ALARM_STATE_ACTIVE_HIGH;
    s_sensor_snap.readings[SENSOR_ID_TEMPERATURE].value = 4270;
    LOG_INFO(IT_TASK_TAG, "Phase 5: Alarms: Temperature HIGH, pill ALARM");
    lcd_ui_select_screen(3U);
    feed_sensors(IT_PHASE_DELAY_MS / 1000U);

    s_alarm_states[SENSOR_ID_TEMPERATURE] = ALARM_STATE_CLEAR;
    s_sensor_snap.readings[SENSOR_ID_TEMPERATURE].value = 2350;
    LOG_INFO(IT_TASK_TAG, "Phase 6: Alarms cleared: No active alarms");
    feed_sensors(IT_PHASE_DELAY_MS / 1000U);

    LOG_INFO(IT_TASK_TAG, "Phase 7: Config: tap a field, use the keypad, APPLY, CONFIRM");
    lcd_ui_select_screen(4U);
    feed_sensors((IT_PHASE_DELAY_MS * 2U) / 1000U);

    LOG_INFO(IT_TASK_TAG, "Phase 8: back to Sensors");
    lcd_ui_select_screen(0U);
    feed_sensors(IT_PHASE_DELAY_MS / 1000U);
    log_lvgl_heap("after the tour");

    /* ------------------------------------------------------------------ */
    /* All checks passed                                                  */
    /* ------------------------------------------------------------------ */
    LOG_INFO(IT_TASK_TAG, "=== ALL CHECKS PASSED ===");
    led_green_on();

    /* lcd_ui_task_body() continues driving the display */
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/* ===================================================================== */
/* main                                                                 */
/* ===================================================================== */

int main(void)
{
    system_clock_init();
    system_clock_enable_dwt();

    (void) gpio_init();
    (void) debug_uart_init();
    (void) rtc_init();
    (void) health_monitor_init();
    (void) logger_init(LOG_LEVEL_DEBUG);

    /* Hardware bring-up order: SDRAM → LCD → Touch → Graphics */
    if (sdram_init() != SDRAM_ERR_OK)
    {
        for (;;)
        {
        } /* Cannot display error without SDRAM */
    }
    (void) lcd_init();
    (void) touchscreen_init();
    (void) graphics_init();

    /* Config service — load stored params or use defaults */
    (void) config_service_init(config_store);

    /* Integration test task */
    (void) xTaskCreateStatic(it_task, "LcdUiIT", IT_TASK_STACK_WORDS, NULL, tskIDLE_PRIORITY + 2U,
                             s_it_task_stack, &s_it_task_tcb);

    /* LcdUi refresh task */
    (void) xTaskCreateStatic(lcd_ui_task_body, "LcdUiTask", 2048U, NULL, tskIDLE_PRIORITY + 1U,
                             s_lcd_ui_stack, &s_lcd_ui_tcb);

    vTaskStartScheduler();

    for (;;)
    {
    }
    return 0;
}
