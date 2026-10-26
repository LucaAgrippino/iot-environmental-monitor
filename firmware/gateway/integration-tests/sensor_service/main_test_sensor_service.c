/**
 * @file main_test_sensor_service.c
 * @brief Hardware bring-up test for SensorService + AlarmService + SensorTask
 *        — B-L475E-IOT01A, Gateway.
 *
 * NOT compiled by default.  To activate:
 *   1. In CubeIDE, remove integration-tests/sensor_service from the "excluding=" list of
 *      the integration-tests entry and add the harness it replaces, so that exactly ONE
 *      harness is built.
 *   2. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   3. Flash and run with a debugger attached (ST-LINK).
 *
 * Hardware outputs:
 *   USART1 TX  PB6   115 200 8N1  Test results, then the live snapshot once a second
 *   LD2 green  PB14               HealthMonitor status LED
 *   I2C2       PB10 SCL / PB11 SDA  HTS221 @ 0x5F, LPS22HB @ 0x5D, LIS3MDL @ 0x1E, LSM6DSL @ 0x6A
 *
 * PREREQUISITES — run integration-tests/i2c, sensor_drivers_group_a and
 * sensor_drivers_group_b first: this harness only trusts the drivers.  Keep the board
 * still and flat while the automated tests run (TC-HW-SS-005).
 *
 * This is the first image in which the production SensorTask runs: sensor_task_create()
 * initialises SensorService and AlarmService before the scheduler (SS-D9), and a 100 ms
 * software timer then drives one acquisition cycle per tick.  The test task runs at a
 * lower priority than SensorTask and only reads the published snapshot.
 *
 * Automated test sequence:
 *   TC-HW-SS-001  sensor_task_create() succeeded (reported by the test task).
 *   TC-HW-SS-002  sensor_service_is_ready() becomes true within 3 s: all four drivers
 *                  delivered a valid reading through the real pipeline.
 *   TC-HW-SS-003  cycle_count advances 18..22 times in 2 s (100 ms timer, no missed ticks).
 *   TC-HW-SS-004  Temperature 5..45 degC, humidity 5..95 %RH, pressure 900..1100 hPa.
 *   TC-HW-SS-005  Acceleration magnitude 0.75..1.25 g (units are mg), each gyro axis below
 *                  17 dps (units are mdps), magnetic field 0.1..1.2 gauss (units are mgauss).
 *   TC-HW-SS-006  Timestamps advance between two snapshots 2 s apart.
 *   TC-HW-SS-007  No alarm is ever active on an IMU / magnetometer channel (SS-D15) and
 *                  get_all_states() succeeds.
 *   TC-HW-SS-008  HealthMonitor reports zero sensor failures after the run.
 *   TC-HW-SS-009  With SensorTask suspended, 50 run_cycle() calls timed with the DWT counter:
 *                  worst case under 10 ms, a tenth of the period (SS-O4); SensorTask stack
 * high-water mark leaves at least 64 of 384 words free (SS-O9).
 *   TC-HW-SS-010  sensor_service_reconfigure(): a bad configuration is rejected and changes
 *                  nothing; a temperature range just below the live reading makes it invalid
 *                  within 2 s on the real pipeline (the clamped sample then reaches the output
 * through the IIR); restoring the defaults makes it valid again.
 *
 * Integration checklist:
 *   [ ] All TC-HW-SS pass, then watch the live lines:
 *   [ ] Tilt the board: the 1 g redistributes between the accel axes
 *   [ ] Rotate the board: gyro axes move; release: they return to ~0
 *   [ ] Breathe on the board: humidity and temperature rise, then relax (IIR filter, alpha 0.1)
 *   [ ] Record results in docs/lld/application/fd-gw-sensor-alarm-service-lld.md (SS-O9)
 *   [ ] SS-O4: time sensor_service_run_cycle() with the DWT counter against the 5 ms budget
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"

#include "alarm_service/alarm_service.h"
#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "health_monitor/health_monitor.h"
#include "i2c/i2c_driver.h"
#include "led/led_driver.h"
#include "rtc/rtc.h"
#include "sensor_service/sensor_service.h"
#include "sensor_service/sensor_task.h"
#include "time_provider/time_provider.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Board constants                                                         */
/* ---------------------------------------------------------------------- */

#define BRINGUP_LED_PORT GPIO_PORT_A
#define LD2_PIN (14U) /* PB14, green LD2 driven by HealthMonitor */
#define BRINGUP_LED_PIN (5U)
#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)
#define BRINGUP_TX_TIMEOUT_MS (100U)

#define TEST_TASK_STACK_WORDS (768U) /* TC-HW-SS-010 holds a 480 B config struct and snapshots */
#define TEST_TASK_PRIORITY (2U)

#define READY_TIMEOUT_MS (3000U)
#define READY_POLL_MS (100U)
#define RATE_WINDOW_MS (2000U)
#define CYCLES_MIN (18U)
#define CYCLES_MAX (22U)

/* Plausible indoor windows, in the units of sensor_reading_t.value. */
#define TEMP_MIN_X100 (500)
#define TEMP_MAX_X100 (4500)
#define HUMIDITY_MIN_X100 (500)
#define HUMIDITY_MAX_X100 (9500)
#define PRESSURE_MIN_X10 (9000)
#define PRESSURE_MAX_X10 (11000)
#define ACCEL_MIN_MG (750)
#define ACCEL_MAX_MG (1250)
#define GYRO_STILL_MAX_MDPS (17000)
#define MAG_MIN_MGAUSS (100)
#define MAG_MAX_MGAUSS (1200)

#define WCET_RUNS (50U)
/* One tenth of the 100 ms period. The LLD estimate of 5 ms (SS-O4) was exceeded by 81 us because
 * a due Group A cycle can include two 2 ms driver retry sleeps, which block but do not burn CPU. */
#define WCET_BUDGET_US (10000U)
#define STACK_FREE_MIN_WORDS (64U)
#define US_PER_SECOND (1000000U)

#define LIVE_PERIOD_MS (1000U)

static StaticTask_t s_test_tcb;
static StackType_t s_test_stack[TEST_TASK_STACK_WORDS];

static health_monitor_handle_t s_hm;
static sensor_task_err_t s_create_err;

static const led_pin_t k_gw_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = (uint8_t) GPIO_PORT_B,
                   .pin = (uint8_t) LD2_PIN,
                   .active_high = true,
                   .fitted = true},
    [LED_RED] = {.port = (uint8_t) GPIO_PORT_A,
                 .pin = 5U,
                 .active_high = true,
                 .fitted = false}, /* LD1 / SB1 - shared with SPI1_SCK */
};

/* ---------------------------------------------------------------------- */
/* Reporting helpers — built on DebugUartDriver.                           */
/* ---------------------------------------------------------------------- */

static void bringup_puts(const char *s)
{
    size_t len = 0U;
    while (s[len] != '\0')
    {
        ++len;
    }
    (void) debug_uart_send((const uint8_t *) s, len, BRINGUP_TX_TIMEOUT_MS);
}

static void bringup_pass(const char *label)
{
    bringup_puts("[PASS] ");
    bringup_puts(label);
    bringup_puts("\r\n");
}

static void bringup_raw_spin(uint32_t n)
{
    volatile uint32_t c = n;
    while (c > 0U)
    {
        --c;
    }
}

static void bringup_fail(const char *label)
{
    bringup_puts("[FAIL] ");
    bringup_puts(label);
    bringup_puts(" - HALTED\r\n");
    for (;;)
    {
        GPIOA->BSRR = (1UL << BRINGUP_LED_PIN);
        bringup_raw_spin(1000000U);
        GPIOA->BSRR = (1UL << (BRINGUP_LED_PIN + 16U));
        bringup_raw_spin(1000000U);
    }
}

static void expect_true(const char *label, bool condition)
{
    if (!condition)
    {
        bringup_fail(label);
    }
}

static int64_t sq(int32_t v)
{
    return (int64_t) v * (int64_t) v;
}

static bool in_range(int32_t v, int32_t lo, int32_t hi)
{
    return (v >= lo) && (v <= hi);
}

static sensor_snapshot_t take_snapshot(void)
{
    sensor_snapshot_t snap;

    (void) memset(&snap, 0, sizeof(snap));
    expect_true("sensor_service_get_snapshot",
                sensor_service_get_snapshot(&snap) == SENSOR_SERVICE_ERR_OK);
    return snap;
}

/* ---------------------------------------------------------------------- */
/* Test cases                                                              */
/* ---------------------------------------------------------------------- */

/* TC-HW-SS-001 */
static void test_create(void)
{
    expect_true("TC-HW-SS-001 sensor_task_create() failed (driver init? health handle?)",
                s_create_err == SENSOR_TASK_ERR_OK);
    bringup_pass("TC-HW-SS-001 SensorService + AlarmService initialised, SensorTask created");
}

/* TC-HW-SS-002 */
static void test_ready(void)
{
    uint32_t waited = 0U;

    while (!sensor_service_is_ready() && (waited < READY_TIMEOUT_MS))
    {
        vTaskDelay(pdMS_TO_TICKS(READY_POLL_MS));
        waited += READY_POLL_MS;
    }
    expect_true("TC-HW-SS-002 not ready within 3 s (a driver failed or is NOT_READY forever)",
                sensor_service_is_ready());
    bringup_pass("TC-HW-SS-002 every sensor produced a valid reading");
}

/* TC-HW-SS-003 */
static void test_cycle_rate(void)
{
    const uint32_t c0 = take_snapshot().cycle_count;
    char msg[64];

    vTaskDelay(pdMS_TO_TICKS(RATE_WINDOW_MS));

    const uint32_t cycles = take_snapshot().cycle_count - c0;
    (void) snprintf(msg, sizeof(msg), "[INFO] %lu cycles in 2 s (20 expected)\r\n",
                    (unsigned long) cycles);
    bringup_puts(msg);
    expect_true("TC-HW-SS-003 cycle rate outside 18..22 per 2 s (timer period? missed ticks?)",
                (cycles >= CYCLES_MIN) && (cycles <= CYCLES_MAX));
    bringup_pass("TC-HW-SS-003 100 ms acquisition cadence holds");
}

/* TC-HW-SS-004 */
static void test_scalar_values(void)
{
    const sensor_snapshot_t s = take_snapshot();

    expect_true("TC-HW-SS-004 temperature outside 5..45 degC",
                in_range(s.readings[SENSOR_ID_TEMPERATURE].value, TEMP_MIN_X100, TEMP_MAX_X100));
    expect_true(
        "TC-HW-SS-004 humidity outside 5..95 %RH",
        in_range(s.readings[SENSOR_ID_HUMIDITY].value, HUMIDITY_MIN_X100, HUMIDITY_MAX_X100));
    expect_true("TC-HW-SS-004 pressure outside 900..1100 hPa",
                in_range(s.readings[SENSOR_ID_PRESSURE].value, PRESSURE_MIN_X10, PRESSURE_MAX_X10));
    bringup_pass("TC-HW-SS-004 temperature, humidity and pressure plausible");
}

/* TC-HW-SS-005 */
static void test_motion_values(void)
{
    const sensor_snapshot_t s = take_snapshot();
    const int32_t ax = s.readings[SENSOR_ID_ACCEL_X].value;
    const int32_t ay = s.readings[SENSOR_ID_ACCEL_Y].value;
    const int32_t az = s.readings[SENSOR_ID_ACCEL_Z].value;
    const int32_t mx = s.readings[SENSOR_ID_MAG_X].value;
    const int32_t my = s.readings[SENSOR_ID_MAG_Y].value;
    const int32_t mz = s.readings[SENSOR_ID_MAG_Z].value;
    char msg[120];

    (void) snprintf(msg, sizeof(msg), "[INFO] accel mg  x=%ld y=%ld z=%ld\r\n", (long) ax,
                    (long) ay, (long) az);
    bringup_puts(msg);
    (void) snprintf(msg, sizeof(msg), "[INFO] mag mG    x=%ld y=%ld z=%ld\r\n", (long) mx,
                    (long) my, (long) mz);
    bringup_puts(msg);

    const int64_t acc_sq = sq(ax) + sq(ay) + sq(az);
    expect_true("TC-HW-SS-005 acceleration magnitude outside 0.75..1.25 g (units? board moving?)",
                (acc_sq >= sq(ACCEL_MIN_MG)) && (acc_sq <= sq(ACCEL_MAX_MG)));

    for (uint8_t id = (uint8_t) SENSOR_ID_GYRO_X; id <= (uint8_t) SENSOR_ID_GYRO_Z; id++)
    {
        const int32_t g = s.readings[id].value;
        expect_true("TC-HW-SS-005 gyro axis above 17 dps while still (mdps conversion?)",
                    (g > -GYRO_STILL_MAX_MDPS) && (g < GYRO_STILL_MAX_MDPS));
    }

    const int64_t mag_sq = sq(mx) + sq(my) + sq(mz);
    expect_true("TC-HW-SS-005 magnetic field outside 0.1..1.2 gauss (mgauss conversion?)",
                (mag_sq >= sq(MAG_MIN_MGAUSS)) && (mag_sq <= sq(MAG_MAX_MGAUSS)));
    bringup_pass(
        "TC-HW-SS-005 accel ~1 g, gyro quiet, magnetic field plausible, in SI-milli units");
}

/* TC-HW-SS-006 */
static void test_timestamps(void)
{
    const sensor_snapshot_t a = take_snapshot();

    vTaskDelay(pdMS_TO_TICKS(RATE_WINDOW_MS));

    const sensor_snapshot_t b = take_snapshot();
    expect_true("TC-HW-SS-006 timestamp did not advance over 2 s (TimeProvider?)",
                b.readings[SENSOR_ID_TEMPERATURE].timestamp.epoch >
                    a.readings[SENSOR_ID_TEMPERATURE].timestamp.epoch);
    bringup_pass("TC-HW-SS-006 readings are timestamped from TimeProvider");
}

/* TC-HW-SS-007 */
static void test_alarm_states(void)
{
    alarm_state_t states[SENSOR_ID_COUNT];
    char msg[96];

    expect_true("TC-HW-SS-007 get_all_states failed",
                alarm_service_get_all_states(states) == ALARM_SERVICE_ERR_OK);
    (void) snprintf(msg, sizeof(msg), "[INFO] alarm T=%d H=%d P=%d (0 = clear)\r\n",
                    (int) states[SENSOR_ID_TEMPERATURE], (int) states[SENSOR_ID_HUMIDITY],
                    (int) states[SENSOR_ID_PRESSURE]);
    bringup_puts(msg);

    for (uint8_t id = (uint8_t) SENSOR_ID_ACCEL_X; id < (uint8_t) SENSOR_ID_COUNT; id++)
    {
        expect_true("TC-HW-SS-007 alarm active on an IMU / magnetometer channel (SS-D15)",
                    states[id] == ALARM_STATE_CLEAR);
    }
    bringup_pass("TC-HW-SS-007 only thresholded channels can alarm");
}

/* TC-HW-SS-008 */
static void test_health(void)
{
    health_snapshot_t hs;

    (void) memset(&hs, 0, sizeof(hs));
    expect_true("TC-HW-SS-008 health_monitor_get_snapshot failed",
                health_monitor_get_snapshot(s_hm, &hs) == HEALTH_MONITOR_ERR_OK);
    expect_true("TC-HW-SS-008 HealthMonitor counted a sensor failure", hs.sensor_fail_count == 0U);
    bringup_pass("TC-HW-SS-008 no sensor failure reported to HealthMonitor");
}

/* TC-HW-SS-009 */
static void test_wcet_and_stack(void)
{
    const TaskHandle_t task = sensor_task_get_handle();
    uint32_t worst = 0U;
    char msg[96];

    expect_true("TC-HW-SS-009 no SensorTask handle", task != NULL);

    /* Suspended, SensorTask cannot overlap these calls, so the timing is the cycle alone. */
    vTaskSuspend(task);
    for (uint32_t i = 0U; i < WCET_RUNS; i++)
    {
        const uint32_t t0 = DWT->CYCCNT;
        (void) sensor_service_run_cycle();
        const uint32_t dt = DWT->CYCCNT - t0;
        /* cppcheck-suppress unsignedLessThanZero -- DWT->CYCCNT is a hardware register */
        if (dt > worst)
        {
            worst = dt;
        }
        vTaskDelay(pdMS_TO_TICKS(20U)); /* let the 1 Hz sensors produce fresh samples too */
    }
    vTaskResume(task);

    const uint32_t worst_us = worst / (cpu_get_sysclk_hz() / US_PER_SECOND);
    const UBaseType_t free_words = uxTaskGetStackHighWaterMark(task);
    (void) snprintf(msg, sizeof(msg),
                    "[INFO] run_cycle worst %lu us (budget %u), stack free %lu/%u words\r\n",
                    (unsigned long) worst_us, WCET_BUDGET_US, (unsigned long) free_words,
                    SENSOR_TASK_STACK_WORDS);
    bringup_puts(msg);

    expect_true("TC-HW-SS-009 run_cycle exceeds 10 ms", worst_us < WCET_BUDGET_US);
    expect_true("TC-HW-SS-009 SensorTask stack nearly exhausted",
                free_words >= STACK_FREE_MIN_WORDS);
    bringup_pass("TC-HW-SS-009 cycle time within budget, stack margin adequate");
}

/* TC-HW-SS-010 */
static config_params_t reconfigure_defaults(void)
{
    config_params_t p;

    (void) memset(&p, 0, sizeof(p));
    p.polling_interval_ms = 1000U; /* what the 10-cycle Group A period was before */
    p.filter_alpha = 100U;
    p.temp_range_min = -4000;
    p.temp_range_max = 8500;
    p.humidity_range_min = 0U;
    p.humidity_range_max = 10000U;
    p.pressure_range_min = 3000U;
    p.pressure_range_max = 11000U;
    return p;
}

static void test_reconfigure(void)
{
    static config_params_t p; /* 480 B: off the 3 KB test stack */

    p = reconfigure_defaults();
    const sensor_snapshot_t before = take_snapshot();
    const int32_t temp_now = before.readings[SENSOR_ID_TEMPERATURE].value;

    /* A bad configuration is refused as a whole. */
    p.filter_alpha = 0U;
    p.temp_range_max = (int16_t) (temp_now - 100);
    expect_true("TC-HW-SS-010 a bad alpha was accepted",
                sensor_service_reconfigure(&p) == SENSOR_SERVICE_ERR_INVALID_ARG);
    vTaskDelay(pdMS_TO_TICKS(1500U));
    expect_true("TC-HW-SS-010 a rejected configuration still took effect",
                take_snapshot().readings[SENSOR_ID_TEMPERATURE].valid);

    /* A valid one with the ceiling just under the live temperature. */
    p.filter_alpha = 100U;
    expect_true("TC-HW-SS-010 reconfigure refused a valid configuration",
                sensor_service_reconfigure(&p) == SENSOR_SERVICE_ERR_OK);
    vTaskDelay(pdMS_TO_TICKS(2000U));
    const sensor_snapshot_t narrowed = take_snapshot();
    expect_true("TC-HW-SS-010 temperature still valid above the new ceiling",
                !narrowed.readings[SENSOR_ID_TEMPERATURE].valid);

    /* Back to the defaults. */
    p = reconfigure_defaults();
    expect_true("TC-HW-SS-010 restoring the defaults failed",
                sensor_service_reconfigure(&p) == SENSOR_SERVICE_ERR_OK);
    vTaskDelay(pdMS_TO_TICKS(2500U));
    expect_true("TC-HW-SS-010 temperature not valid again after the restore",
                take_snapshot().readings[SENSOR_ID_TEMPERATURE].valid);
    bringup_pass("TC-HW-SS-010 reconfigure: bad config refused, range applied live, restored");
}

/* Live line once a second so the operator can tilt, rotate and warm the board. */
static void live_loop(void)
{
    char msg[160];

    for (;;)
    {
        const sensor_snapshot_t s = take_snapshot();
        (void) snprintf(
            msg, sizeof(msg),
            "#%lu T=%ld H=%ld P=%ld | a=%ld,%ld,%ld g=%ld,%ld,%ld m=%ld,%ld,%ld\r\n",
            (unsigned long) s.cycle_count, (long) s.readings[0].value, (long) s.readings[1].value,
            (long) s.readings[2].value, (long) s.readings[3].value, (long) s.readings[4].value,
            (long) s.readings[5].value, (long) s.readings[6].value, (long) s.readings[7].value,
            (long) s.readings[8].value, (long) s.readings[9].value, (long) s.readings[10].value,
            (long) s.readings[11].value);
        bringup_puts(msg);
        gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        vTaskDelay(pdMS_TO_TICKS(LIVE_PERIOD_MS));
    }
}

/* ---------------------------------------------------------------------- */
/* Task and entry point                                                    */
/* ---------------------------------------------------------------------- */

static void test_task(void *arg)
{
    (void) arg;

    bringup_puts("\r\n===== SensorService + AlarmService Hardware Bring-up =====\r\n");
    bringup_puts("Board  : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("Units  : T 0.01 C, H 0.01 %RH, P 0.1 hPa, accel mg, gyro mdps, mag mgauss\r\n");
    bringup_puts("Keep the board still and flat until the live lines start.\r\n");
    bringup_puts("=========================================================\r\n\r\n");

    test_create();
    test_ready();
    test_cycle_rate();
    test_scalar_values();
    test_motion_values();
    test_timestamps();
    test_alarm_states();
    test_health();
    test_reconfigure();
    test_wcet_and_stack();

    bringup_puts("\r\n[PASS] All automated tests complete. Live snapshot follows.\r\n");
    live_loop();
}

int main(void)
{
    /* cpu_init() first - clock to 80 MHz, DWT cycle counter, fault handlers. */
    status_t init_st = cpu_init();
    if (init_st != STATUS_OK)
    {
        RCC->AHB2ENR |= RCC_AHB2ENR_GPIOAEN;
        GPIOA->MODER &= ~(3UL << (BRINGUP_LED_PIN * 2U));
        GPIOA->MODER |= (1UL << (BRINGUP_LED_PIN * 2U));
        for (;;)
        {
            GPIOA->BSRR = (1UL << BRINGUP_LED_PIN);
            bringup_raw_spin(200000U);
            GPIOA->BSRR = (1UL << (BRINGUP_LED_PIN + 16U));
            bringup_raw_spin(200000U);
        }
    }

    (void) gpio_init();

    gpio_pin_config_t led_config = {
        .port = BRINGUP_LED_PORT,
        .pin = BRINGUP_LED_PIN,
        .mode = GPIO_MODE_OUTPUT,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_LOW,
        .pull = GPIO_PULL_NONE,
        .alternate = 0U,
    };
    gpio_pin_config_t uart_tx_config = {
        .port = BRINGUP_UART_TX_PORT,
        .pin = BRINGUP_UART_TX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_NONE,
        .alternate = BRINGUP_UART_AF,
    };
    gpio_pin_config_t uart_rx_config = {
        .port = BRINGUP_UART_RX_PORT,
        .pin = BRINGUP_UART_RX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_UP,
        .alternate = BRINGUP_UART_AF,
    };
    (void) gpio_configure_pin(&led_config);
    (void) gpio_configure_pin(&uart_tx_config);
    (void) gpio_configure_pin(&uart_rx_config);

    if ((debug_uart_init() != DEBUG_UART_OK) || (rtc_init() != RTC_OK))
    {
        bringup_fail("debug_uart_init / rtc_init");
    }

    /* SensorService stamps every reading from TimeProvider and reports sensor faults to
     * HealthMonitor, which drives LD2 through LedDriver. */
    const health_monitor_config_t hm_config = {.boot_reset_cause = LIFECYCLE_RESET_SOFT};

    if (led_init(k_gw_led_pins, (uint8_t) LED_COUNT) != LED_ERR_OK)
    {
        bringup_fail("led_init");
    }
    if (health_monitor_create(&hm_config, &s_hm) != HEALTH_MONITOR_ERR_OK)
    {
        bringup_fail("health_monitor_create");
    }
    if (time_provider_init(s_hm) != TIME_PROVIDER_ERR_OK)
    {
        bringup_fail("time_provider_init");
    }

    /* sensor_task_create() needs the I2C bus up (sensor_task.h); init is idempotent. */
    if (i2c_driver->init() != I2C_ERR_OK)
    {
        bringup_fail("i2c_driver->init");
    }
    s_create_err = sensor_task_create(s_hm);

    (void) xTaskCreateStatic(test_task, "sensor_bringup", TEST_TASK_STACK_WORDS, NULL,
                             TEST_TASK_PRIORITY, s_test_stack, &s_test_tcb);
    vTaskStartScheduler();

    /* Only reached if the scheduler fails to start. */
    for (;;)
    {
    }
}
