/**
 * @file main_test_sensor_drivers_group_b.c
 * @brief Hardware bring-up test for MagnetometerDriver (LIS3MDL) and ImuDriver (LSM6DSL)
 *        — B-L475E-IOT01A, Gateway.
 *
 * NOT compiled by default.  To activate:
 *   1. In CubeIDE, add integration-tests/sensor_drivers_group_b to the source entries and
 *      remove it from the "excluding=" list of the integration-tests entry (and add the
 *      harness it replaces), so that exactly ONE harness is built.
 *   2. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   3. Flash and run with a debugger attached (ST-LINK).
 *
 * Hardware outputs:
 *   USART1 TX  PB6   115 200 8N1  Human-readable test results
 *   LD2 green  PA5                Heartbeat on completion, fast blink on halt
 *   I2C2       PB10 SCL / PB11 SDA  LIS3MDL @ 0x1E, LSM6DSL @ 0x6A
 *   LIS3MDL DRDY  PC8  (EXTI8)    LSM6DSL INT1  PD11 (EXTI11)
 *
 * PREREQUISITE — run integration-tests/i2c first: it proves the bus and the WHO_AM_I of
 * both sensors.  Keep the board STILL and flat for TC-HW-SB-006.
 *
 * The EXTI vectors are the production ones in Src/stm32l4xx_it.c: each clears the pending
 * bit and calls the driver's internal ISR entry point, whose callback notifies this task.
 * So this harness also exercises the real interrupt wiring.
 *
 * Automated test sequence:
 *   TC-HW-SB-001  magnetometer_init() and imu_init() succeed (Phase 1).
 *   TC-HW-SB-002  Both attach calls succeed from task context (Phase 2).
 *   TC-HW-SB-003  A DRDY and an INT1 notification each arrive within 500 ms.  Proves PC8 /
 *                  PD11 routing, the EXTI mapping and the stale-line release in attach.
 *   TC-HW-SB-004  Interrupt rates over 2 s: DRDY 8..12 Hz (10 Hz configured), INT1 80..130 Hz
 *                  (104 Hz configured).  Catches a wrong ODR field (CTRL_REG1 0x7C would give
 *                  80 Hz DRDY).
 *   TC-HW-SB-005  Magnetometer reads: field magnitude 0.1..1.2 gauss, which rules out a
 *                  repeated-register burst (auto-increment bit) and a byte-order error.
 *   TC-HW-SB-006  IMU reads: acceleration magnitude 0.75..1.25 g, each gyro axis below
 *                  2000 LSB (about 17 dps) while still.  Checks gyro/accel ordering.
 *   TC-HW-SB-007  Twenty notified reads in a row: no NOT_READY (STATUS_REG gate agrees with
 *                  the interrupts).
 *   TC-HW-SB-008  NULL arguments are rejected without touching the bus.
 *   TC-HW-SB-009  Re-initialisation while attached (REQ-NF-205 path): both init calls return
 *                  OK and notifications keep arriving.
 *
 * Integration checklist:
 *   [ ] TC-HW-SB-003 passes -> GPB-O3 (PC8 / PD11) confirmed against the real board
 *   [ ] TC-HW-SB-004 rates recorded; sensors really run at 10 Hz and 104 Hz
 *   [ ] Rotate the board: gyro values move; tilt: accel axes redistribute 1 g
 *   [ ] Record results in docs/lld/drivers/gw-sensor-drivers-group-b-lld.md (§13.1)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "FreeRTOS.h"
#include "task.h"

#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "i2c/i2c_driver.h"
#include "imu_driver/imu_driver.h"
#include "magnetometer_driver/magnetometer_driver.h"

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

#define MAG_DRDY_LINE (8U)
#define IMU_INT1_LINE (11U)

#define NOTIFY_BIT_MAG (1UL << 0)
#define NOTIFY_BIT_IMU (1UL << 1)

#define FIRST_EVENT_TIMEOUT_MS (500U)
#define RATE_WINDOW_MS (2000U)
#define MAG_RATE_MIN_HZ (8U)
#define MAG_RATE_MAX_HZ (12U)
#define IMU_RATE_MIN_HZ (80U)
#define IMU_RATE_MAX_HZ (130U)
#define NOTIFIED_READS (20U)
#define NOTIFIED_READ_TIMEOUT_MS (200U)

/* Magnitude windows (raw LSB, squared to avoid a square root). */
#define MAG_LSB_PER_GAUSS (6842)
#define MAG_MIN_LSB (MAG_LSB_PER_GAUSS / 10)        /* 0.1 gauss */
#define MAG_MAX_LSB ((MAG_LSB_PER_GAUSS * 12) / 10) /* 1.2 gauss */
#define ACCEL_LSB_PER_G (16384)
#define ACCEL_MIN_LSB ((ACCEL_LSB_PER_G * 3) / 4) /* 0.75 g */
#define ACCEL_MAX_LSB ((ACCEL_LSB_PER_G * 5) / 4) /* 1.25 g */
#define GYRO_STILL_MAX_LSB (2000)

static StaticTask_t s_test_tcb;
static StackType_t s_test_stack[TEST_TASK_STACK_WORDS];
static TaskHandle_t s_test_task;

static volatile uint32_t s_mag_irq_count;
static volatile uint32_t s_imu_irq_count;

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

/* ---------------------------------------------------------------------- */
/* Callbacks (the vectors themselves are in Src/stm32l4xx_it.c)           */
/* ---------------------------------------------------------------------- */

static void notify_task_from_isr(void *ctx, uint32_t bit)
{
    BaseType_t woken = pdFALSE;
    (void) xTaskNotifyFromISR((TaskHandle_t) ctx, bit, eSetBits, &woken);
    portYIELD_FROM_ISR(woken);
}

static void mag_drdy_cb(void *ctx)
{
    s_mag_irq_count++;
    notify_task_from_isr(ctx, NOTIFY_BIT_MAG);
}

static void imu_int1_cb(void *ctx)
{
    s_imu_irq_count++;
    notify_task_from_isr(ctx, NOTIFY_BIT_IMU);
}

/** Wait for any of the bits; returns the bits received (0 on timeout). */
static uint32_t wait_notify(uint32_t bits, uint32_t timeout_ms)
{
    uint32_t value = 0U;
    if (xTaskNotifyWait(0U, bits, &value, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
    {
        return 0U;
    }
    return value & bits;
}

static uint64_t sq(int32_t v)
{
    return (uint64_t) ((int64_t) v * (int64_t) v);
}

/* ---------------------------------------------------------------------- */
/* Test cases                                                              */
/* ---------------------------------------------------------------------- */

/* TC-HW-SB-001 */
static void test_phase1(void)
{
    expect_true("TC-HW-SB-001 i2c init", i2c_driver->init() == I2C_ERR_OK);
    expect_true("TC-HW-SB-001 magnetometer_init",
                magnetometer_driver->init() == MAGNETOMETER_ERR_OK);
    expect_true("TC-HW-SB-001 imu_init", imu_driver->init() == IMU_ERR_OK);
    bringup_pass("TC-HW-SB-001 Phase 1 init of both sensors");
}

/* TC-HW-SB-002 */
static void test_phase2(void)
{
    expect_true("TC-HW-SB-002 magnetometer attach",
                magnetometer_driver->attach_drdy_callback(mag_drdy_cb, s_test_task) ==
                    MAGNETOMETER_ERR_OK);
    expect_true("TC-HW-SB-002 imu attach",
                imu_driver->attach_int1_callback(imu_int1_cb, s_test_task) == IMU_ERR_OK);
    bringup_pass("TC-HW-SB-002 Phase 2 callbacks attached, EXTI armed");
}

/* TC-HW-SB-003 */
static void test_first_events(void)
{
    uint32_t got = 0U;
    const TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(FIRST_EVENT_TIMEOUT_MS);

    while (((got & (NOTIFY_BIT_MAG | NOTIFY_BIT_IMU)) != (NOTIFY_BIT_MAG | NOTIFY_BIT_IMU)) &&
           (xTaskGetTickCount() < deadline))
    {
        got |= wait_notify(NOTIFY_BIT_MAG | NOTIFY_BIT_IMU, 50U);
    }
    expect_true("TC-HW-SB-003 no DRDY within 500 ms (PC8 / EXTI8 / stale-line release?)",
                (got & NOTIFY_BIT_MAG) != 0U);
    expect_true("TC-HW-SB-003 no INT1 within 500 ms (PD11 / EXTI11?)",
                (got & NOTIFY_BIT_IMU) != 0U);
    bringup_pass("TC-HW-SB-003 DRDY and INT1 interrupts arrive");
}

/* TC-HW-SB-004 */
static void test_rates(void)
{
    const uint32_t mag0 = s_mag_irq_count;
    const uint32_t imu0 = s_imu_irq_count;
    char msg[96];

    vTaskDelay(pdMS_TO_TICKS(RATE_WINDOW_MS));

    const uint32_t mag_hz = (s_mag_irq_count - mag0) / (RATE_WINDOW_MS / 1000U);
    const uint32_t imu_hz = (s_imu_irq_count - imu0) / (RATE_WINDOW_MS / 1000U);
    (void) snprintf(msg, sizeof(msg),
                    "[INFO] DRDY %lu Hz (10 configured), INT1 %lu Hz (104 configured)\r\n",
                    (unsigned long) mag_hz, (unsigned long) imu_hz);
    bringup_puts(msg);

    expect_true("TC-HW-SB-004 DRDY rate outside 8..12 Hz (CTRL_REG1 ODR field?)",
                (mag_hz >= MAG_RATE_MIN_HZ) && (mag_hz <= MAG_RATE_MAX_HZ));
    expect_true("TC-HW-SB-004 INT1 rate outside 80..130 Hz",
                (imu_hz >= IMU_RATE_MIN_HZ) && (imu_hz <= IMU_RATE_MAX_HZ));
    bringup_pass("TC-HW-SB-004 output data rates match configuration");
}

/** Read the magnetometer once after a DRDY, halting on any error. */
static magnetometer_data_t read_mag_notified(const char *label)
{
    magnetometer_data_t m = {0, 0, 0};
    expect_true(label, wait_notify(NOTIFY_BIT_MAG, NOTIFIED_READ_TIMEOUT_MS) != 0U);
    expect_true(label, magnetometer_driver->read(&m) == MAGNETOMETER_ERR_OK);
    return m;
}

static imu_data_t read_imu_notified(const char *label)
{
    imu_data_t d = {0, 0, 0, 0, 0, 0};
    expect_true(label, wait_notify(NOTIFY_BIT_IMU, NOTIFIED_READ_TIMEOUT_MS) != 0U);
    expect_true(label, imu_driver->read(&d) == IMU_ERR_OK);
    return d;
}

/* TC-HW-SB-005 */
static void test_magnetometer_values(void)
{
    const magnetometer_data_t m = read_mag_notified("TC-HW-SB-005 notified magnetometer read");
    const uint64_t mag_sq = sq(m.x) + sq(m.y) + sq(m.z);
    char msg[96];

    (void) snprintf(msg, sizeof(msg), "[INFO] mag raw x=%d y=%d z=%d\r\n", (int) m.x, (int) m.y,
                    (int) m.z);
    bringup_puts(msg);
    expect_true("TC-HW-SB-005 field magnitude outside 0.1..1.2 gauss",
                (mag_sq >= sq(MAG_MIN_LSB)) && (mag_sq <= sq(MAG_MAX_LSB)));
    bringup_pass("TC-HW-SB-005 magnetic field magnitude plausible");
}

/* TC-HW-SB-006 */
static void test_imu_values(void)
{
    const imu_data_t d = read_imu_notified("TC-HW-SB-006 notified IMU read");
    const uint64_t acc_sq = sq(d.accel_x) + sq(d.accel_y) + sq(d.accel_z);
    char msg[120];

    (void) snprintf(msg, sizeof(msg), "[INFO] accel x=%d y=%d z=%d  gyro x=%d y=%d z=%d\r\n",
                    (int) d.accel_x, (int) d.accel_y, (int) d.accel_z, (int) d.gyro_x,
                    (int) d.gyro_y, (int) d.gyro_z);
    bringup_puts(msg);
    expect_true("TC-HW-SB-006 acceleration magnitude outside 0.75..1.25 g (board moving?)",
                (acc_sq >= sq(ACCEL_MIN_LSB)) && (acc_sq <= sq(ACCEL_MAX_LSB)));
    expect_true("TC-HW-SB-006 gyro not near zero while still (axis order?)",
                (sq(d.gyro_x) < sq(GYRO_STILL_MAX_LSB)) &&
                    (sq(d.gyro_y) < sq(GYRO_STILL_MAX_LSB)) &&
                    (sq(d.gyro_z) < sq(GYRO_STILL_MAX_LSB)));
    bringup_pass("TC-HW-SB-006 acceleration ~1 g, gyro quiet");
}

/* TC-HW-SB-007 */
static void test_notified_reads(void)
{
    for (uint32_t i = 0U; i < NOTIFIED_READS; i++)
    {
        (void) read_mag_notified("TC-HW-SB-007 magnetometer read after DRDY");
        (void) read_imu_notified("TC-HW-SB-007 IMU read after INT1");
    }
    bringup_pass("TC-HW-SB-007 twenty notified reads, no NOT_READY");
}

/* TC-HW-SB-008 */
static void test_null_arguments(void)
{
    expect_true("TC-HW-SB-008 magnetometer_read NULL",
                magnetometer_read(NULL) == MAGNETOMETER_ERR_INVALID_ARG);
    expect_true("TC-HW-SB-008 imu_read NULL", imu_read(NULL) == IMU_ERR_INVALID_ARG);
    expect_true("TC-HW-SB-008 attach NULL magnetometer",
                magnetometer_attach_drdy_callback(NULL, NULL) == MAGNETOMETER_ERR_INVALID_ARG);
    expect_true("TC-HW-SB-008 attach NULL imu",
                imu_attach_int1_callback(NULL, NULL) == IMU_ERR_INVALID_ARG);
    bringup_pass("TC-HW-SB-008 NULL arguments rejected");
}

/* TC-HW-SB-009 */
static void test_reinit(void)
{
    expect_true("TC-HW-SB-009 magnetometer re-init", magnetometer_init() == MAGNETOMETER_ERR_OK);
    expect_true("TC-HW-SB-009 imu re-init", imu_init() == IMU_ERR_OK);
    (void) read_mag_notified("TC-HW-SB-009 DRDY after re-init");
    (void) read_imu_notified("TC-HW-SB-009 INT1 after re-init");
    bringup_pass("TC-HW-SB-009 re-initialisation keeps the interrupts working");
}

/* ---------------------------------------------------------------------- */
/* Task and entry point                                                    */
/* ---------------------------------------------------------------------- */

static void test_task(void *arg)
{
    (void) arg;
    s_test_task = xTaskGetCurrentTaskHandle();

    bringup_puts("\r\n===== Sensor Drivers Group B Hardware Bring-up =====\r\n");
    bringup_puts("Board  : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("Sensors: LIS3MDL @ 0x1E (DRDY PC8), LSM6DSL @ 0x6A (INT1 PD11)\r\n");
    bringup_puts("Keep the board still and flat.\r\n");
    bringup_puts("====================================================\r\n\r\n");

    test_phase1();
    test_phase2();
    test_first_events();
    test_rates();
    test_magnetometer_values();
    test_imu_values();
    test_notified_reads();
    test_null_arguments();
    test_reinit();

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

    /* gpio_init() + DebugUartDriver (USART1) — reporting channel only. */
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

    (void) xTaskCreateStatic(test_task, "sensorsB_bringup", TEST_TASK_STACK_WORDS, NULL,
                             TEST_TASK_PRIORITY, s_test_stack, &s_test_tcb);
    vTaskStartScheduler();

    /* Only reached if the scheduler fails to start. */
    for (;;)
    {
    }
}
