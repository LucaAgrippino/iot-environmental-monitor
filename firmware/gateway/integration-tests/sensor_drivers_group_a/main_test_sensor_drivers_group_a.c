/**
 * @file main_test_sensor_drivers_group_a.c
 * @brief Hardware bring-up test for HumidityTempDriver (HTS221) and
 *        BarometerDriver (LPS22HB) — B-L475E-IOT01A, Gateway.
 *
 * NOT compiled by default.  To activate:
 *   1. In CubeIDE, add integration-tests/sensor_drivers_group_a to the
 *      source entries and add it to (or remove it from) the "excluding=" list
 *      of the integration-tests entry so that exactly ONE harness is built.
 *   2. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   3. Flash and run with a debugger attached (ST-LINK).
 *
 * Hardware outputs:
 *   USART1 TX  PB6   115 200 8N1  Human-readable test results
 *   LD2 green  PA5                Heartbeat on completion, fast blink on halt
 *   I2C2       PB10 SCL / PB11 SDA  HTS221 @ 0x5F, LPS22HB @ 0x5D
 *
 * PREREQUISITE — the Gateway I2cDriver (i2c_driver_l4.c) is NOT written yet.
 * Until it is, i2c_driver_placeholder.c answers every transaction with
 * I2C_ERR_NACK, TC-HW-SA-001 reports it and the run halts.  The harness is
 * kept complete so that it runs unchanged the moment the L475 driver lands.
 *
 * The drivers call vTaskDelay() for the STATUS_REG retry, so the sequence runs
 * inside a FreeRTOS task after the scheduler starts.
 *
 * Automated test sequence:
 *   TC-HW-SA-001  i2c_driver->init() succeeds (I2C2 up, bus idle).
 *   TC-HW-SA-002  humidity_temp_init() returns OK: both CTRL writes ACKed and the
 *                  16-byte calibration burst read back.
 *   TC-HW-SA-003  barometer_init() returns OK: both CTRL writes ACKed.
 *   TC-HW-SA-004  NULL arguments are rejected without touching the bus.
 *   TC-HW-SA-005  Five 1 Hz samples through the vtable singletons.  Every
 *                  sample must be plausible for a room: 0..50 degC, 5..95 %RH,
 *                  900..1100 hPa, barometer temperature 0..50 degC.  A pressure
 *                  outside the window is the signature of a broken burst read
 *                  (IF_ADD_INC cleared) or a wrong sign extension.
 *   TC-HW-SA-006  The two temperature sensors agree within 5 degC — a coarse
 *                  cross-check that catches a wrong T0/T1 MSB assembly.
 *
 * Integration checklist:
 *   [ ] TC-HW-SA-001 passes (needs the L475 I2cDriver; closes GPA-O3 once the
 *       TIMINGR value is settled)
 *   [ ] Humidity moves when breathed on; pressure moves ~0.1 hPa per metre
 *   [ ] No _ERR_NOT_READY across the five 1 Hz samples (ODR matches the poll rate)
 *   [ ] Record observed values in docs/lld/drivers/gw-sensor-drivers-group-a-lld.md
 *       (closes GPA-O4 once averaging is judged against the noise seen)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "barometer_driver/barometer_driver.h"
#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "humidity_temp_driver/humidity_temp_driver.h"
#include "i2c/i2c_driver.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Board constants                                                         */
/* ---------------------------------------------------------------------- */

#define BRINGUP_LED_PORT GPIO_PORT_A
#define BRINGUP_LED_PIN (5U)
#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)
#define BRINGUP_TX_TIMEOUT_MS (100U)

#define TEST_TASK_STACK_WORDS (512U)
#define TEST_TASK_PRIORITY (2U)

#define SAMPLE_COUNT (5U)
#define SAMPLE_PERIOD_MS (1000U)

/* Plausibility windows for a room-temperature bench (value x scale). */
#define TEMP_MIN_X100 (0)
#define TEMP_MAX_X100 (5000)
#define HUMIDITY_MIN_X100 (500)
#define HUMIDITY_MAX_X100 (9500)
#define PRESSURE_MIN_X10 (9000)
#define PRESSURE_MAX_X10 (11000)
#define TEMP_AGREEMENT_X100 (500)

static StaticTask_t s_test_tcb;
static StackType_t s_test_stack[TEST_TASK_STACK_WORDS];

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

static bool in_range(int32_t value, int32_t lo, int32_t hi)
{
    return (value >= lo) && (value <= hi);
}

static int32_t abs_diff(int32_t a, int32_t b)
{
    return (a > b) ? (a - b) : (b - a);
}

/* ---------------------------------------------------------------------- */
/* Test cases                                                              */
/* ---------------------------------------------------------------------- */

/* TC-HW-SA-001 */
static void test_i2c_up(void)
{
    const i2c_err_t err = i2c_driver->init();
    if (err != I2C_ERR_OK)
    {
        char msg[96];
        (void) snprintf(msg, sizeof(msg),
                        "TC-HW-SA-001 i2c init err=%d (Gateway I2cDriver implemented?)", (int) err);
        bringup_fail(msg);
    }
    bringup_pass("TC-HW-SA-001 I2C2 initialised");
}

/* TC-HW-SA-002 / -003 */
static void test_inits(void)
{
    const humidity_temp_err_t ht = humidity_temp_driver->init();
    const barometer_err_t ba = barometer_driver->init();

    expect_true("TC-HW-SA-002 humidity_temp_init (HTS221 @ 0x5F ACK + calibration)",
                ht == HUMIDITY_TEMP_ERR_OK);
    bringup_pass("TC-HW-SA-002 HTS221 initialised, calibration read");
    expect_true("TC-HW-SA-003 barometer_init (LPS22HB @ 0x5D ACK)", ba == BAROMETER_ERR_OK);
    bringup_pass("TC-HW-SA-003 LPS22HB initialised");
}

/* TC-HW-SA-004 */
static void test_null_arguments(void)
{
    expect_true("TC-HW-SA-004 humidity NULL",
                humidity_temp_read(NULL) == HUMIDITY_TEMP_ERR_INVALID_ARG);
    expect_true("TC-HW-SA-004 barometer NULL", barometer_read(NULL) == BAROMETER_ERR_INVALID_ARG);
    bringup_pass("TC-HW-SA-004 NULL arguments rejected");
}

/* TC-HW-SA-005 / -006 */
static void test_samples(void)
{
    for (uint32_t i = 0U; i < SAMPLE_COUNT; i++)
    {
        humidity_temp_data_t ht = {0};
        barometer_data_t ba = {0};
        char msg[128];

        /* The sensors run at 1 Hz ODR; give the first conversion time to finish. */
        vTaskDelay(pdMS_TO_TICKS(SAMPLE_PERIOD_MS));

        const humidity_temp_err_t ht_err = humidity_temp_driver->read(&ht);
        const barometer_err_t ba_err = barometer_driver->read(&ba);

        (void) snprintf(msg, sizeof(msg), "sample %lu: ht_err=%d ba_err=%d", (unsigned long) i,
                        (int) ht_err, (int) ba_err);
        expect_true(msg, (ht_err == HUMIDITY_TEMP_ERR_OK) && (ba_err == BAROMETER_ERR_OK));

        (void) snprintf(msg, sizeof(msg),
                        "[INFO] #%lu  HTS221 %ld.%02ld degC %ld.%02ld %%RH | LPS22HB %ld.%01ld hPa "
                        "%ld.%02ld degC\r\n",
                        (unsigned long) i, (long) (ht.temperature_x100 / 100),
                        (long) (ht.temperature_x100 % 100), (long) (ht.humidity_x100 / 100),
                        (long) (ht.humidity_x100 % 100), (long) (ba.pressure_x10 / 10),
                        (long) (ba.pressure_x10 % 10), (long) (ba.temperature_x100 / 100),
                        (long) (ba.temperature_x100 % 100));
        bringup_puts(msg);

        expect_true("TC-HW-SA-005 HTS221 temperature plausible",
                    in_range(ht.temperature_x100, TEMP_MIN_X100, TEMP_MAX_X100));
        expect_true("TC-HW-SA-005 HTS221 humidity plausible",
                    in_range(ht.humidity_x100, HUMIDITY_MIN_X100, HUMIDITY_MAX_X100));
        expect_true("TC-HW-SA-005 LPS22HB pressure plausible (IF_ADD_INC / sign extension)",
                    in_range(ba.pressure_x10, PRESSURE_MIN_X10, PRESSURE_MAX_X10));
        expect_true("TC-HW-SA-005 LPS22HB temperature plausible",
                    in_range(ba.temperature_x100, TEMP_MIN_X100, TEMP_MAX_X100));
        expect_true("TC-HW-SA-006 temperatures agree within 5 degC (T0/T1 MSB assembly)",
                    abs_diff(ht.temperature_x100, ba.temperature_x100) <= TEMP_AGREEMENT_X100);
    }
    bringup_pass("TC-HW-SA-005 five plausible 1 Hz samples");
    bringup_pass("TC-HW-SA-006 HTS221 and LPS22HB temperatures agree");
}

/* ---------------------------------------------------------------------- */
/* Task and entry point                                                    */
/* ---------------------------------------------------------------------- */

static void test_task(void *arg)
{
    (void) arg;

    bringup_puts("\r\n===== Sensor Drivers Group A Hardware Bring-up =====\r\n");
    bringup_puts("Board  : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("Sensors: HTS221 @ 0x5F, LPS22HB @ 0x5D on I2C2 (PB10/PB11)\r\n");
    bringup_puts("====================================================\r\n\r\n");

    test_i2c_up();
    test_inits();
    test_null_arguments();
    test_samples();

    bringup_puts("\r\n[PASS] All automated tests complete.\r\n");

    for (;;)
    {
        gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
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

    /* gpio_init() + DebugUartDriver (USART1) — reporting channel only.
     * The I2C2 pins are owned by the I2cDriver itself. */
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
    (void) debug_uart_init();

    (void) xTaskCreateStatic(test_task, "sensors_bringup", TEST_TASK_STACK_WORDS, NULL,
                             TEST_TASK_PRIORITY, s_test_stack, &s_test_tcb);
    vTaskStartScheduler();

    /* Only reached if the scheduler fails to start. */
    for (;;)
    {
    }
}
