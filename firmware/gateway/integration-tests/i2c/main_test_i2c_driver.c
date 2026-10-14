/**
 * @file main_test_i2c_driver.c
 * @brief Hardware bring-up test for I2cDriver (B-L475E-IOT01A, Gateway, I2C2).
 *
 * NOT compiled by default.  To activate:
 *   1. In CubeIDE, add integration-tests/i2c to the source entries and remove
 *      it from the "excluding=" list of the integration-tests entry (and add
 *      the harness it replaces), so that exactly ONE harness is built.
 *   2. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   3. Flash and run with a debugger attached (ST-LINK).
 *
 * Hardware outputs:
 *   USART1 TX  PB6   115 200 8N1  Human-readable test results
 *   LD2 green  PA5                Heartbeat on completion, fast blink on halt
 *   I2C2       PB10 SCL / PB11 SDA  (board pull-ups), 400 kHz target
 *
 * Bare metal: the driver polls and has no RTOS dependency, so no scheduler is
 * started.  The four on-board I2C sensors are used as known targets, each
 * identified through its WHO_AM_I register (0x0F).
 *
 * Automated test sequence:
 *   TC-HW-I2C-001  i2c_init() succeeds and a second call is idempotent.
 *   TC-HW-I2C-002  WHO_AM_I via i2c_write_read(): HTS221 0xBC @0x5F, LPS22HB 0xB1
 *                   @0x5D, LIS3MDL 0x3D @0x1E, LSM6DSL 0x6A @0x6A.  Proves PB10/PB11
 *                   routing, AF4, and that the placeholder TIMINGR yields a working bus.
 *   TC-HW-I2C-003  i2c_write() then i2c_write_read() read-back: LPS22HB CTRL_REG1
 *                   written 0x12 reads back 0x12.
 *   TC-HW-I2C-004  i2c_write() of a register pointer followed by a plain i2c_read():
 *                   LPS22HB WHO_AM_I again 0xB1 (exercises the read-only path).
 *   TC-HW-I2C-005  Burst read: HTS221 calibration 0x30..0x3F (16 bytes, auto-increment
 *                   bit 7): bytes are not all identical and H1_rH_x2 > H0_rH_x2.
 *   TC-HW-I2C-006  Absent address 0x2A returns I2C_ERR_NACK, and the bus is still
 *                   usable immediately afterwards (STOP was completed).
 *   TC-HW-I2C-007  Invalid arguments return I2C_ERR_INVALID_ARG without bus traffic.
 *   TC-HW-I2C-008  DWT-timed 1-byte-register read (average of 100): reported and checked
 *                   to sit between 60 us (faster than 400 kHz is impossible) and 200 us
 *                   (slower means the bus is running near 100 kHz).  Evidence for I2CD-O1.
 *
 * Not covered here: the 9-clock bus recovery (I2CD-O4) needs a stuck slave and a
 * logic analyser; it is verified on the bench by holding SDA low externally.
 *
 * Integration checklist:
 *   [ ] TC-HW-I2C-001/002 pass -> I2C2 pins and clocking confirmed
 *   [ ] TC-HW-I2C-008 average time recorded; scope SCL once to close I2CD-O1
 *   [ ] TC-HW-I2C-006: bus recovers after the NACK (no BUS_BUSY on the next call)
 *   [ ] Record results in docs/lld/drivers/i2c-driver.md (§7.6)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
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

#define ADDR_HTS221 (0x5FU)
#define ADDR_LPS22HB (0x5DU)
#define ADDR_LIS3MDL (0x1EU)
#define ADDR_LSM6DSL (0x6AU)
#define ADDR_ABSENT (0x2AU)

#define REG_WHO_AM_I (0x0FU)
#define WHO_HTS221 (0xBCU)
#define WHO_LPS22HB (0xB1U)
#define WHO_LIS3MDL (0x3DU)
#define WHO_LSM6DSL (0x6AU)

#define LPS22HB_CTRL_REG1 (0x10U)
#define LPS22HB_CTRL_REG1_VALUE (0x12U)

#define HTS221_CALIB_REG (0x30U | 0x80U)
#define HTS221_CALIB_LEN (16U)

#define TIMING_ITERATIONS (100U)
#define TIMING_MIN_US (60U)
#define TIMING_MAX_US (200U)

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

/** Halt unless err matches the expected code; reports both on mismatch. */
static void expect_err(const char *label, i2c_err_t got, i2c_err_t expected)
{
    if (got != expected)
    {
        char msg[96];
        (void) snprintf(msg, sizeof(msg), "%s (err=%d, expected=%d)", label, (int) got,
                        (int) expected);
        bringup_fail(msg);
    }
}

static void expect_byte(const char *label, uint8_t got, uint8_t expected)
{
    if (got != expected)
    {
        char msg[96];
        (void) snprintf(msg, sizeof(msg), "%s (got=0x%02X, expected=0x%02X)", label, (unsigned) got,
                        (unsigned) expected);
        bringup_fail(msg);
    }
}

/** Read one register with a combined write-read, halting on any bus error. */
static uint8_t read_reg(uint8_t dev, uint8_t reg, const char *label)
{
    uint8_t value = 0U;
    expect_err(label, i2c_driver->write_read(dev, &reg, 1U, &value, 1U), I2C_ERR_OK);
    return value;
}

/* ---------------------------------------------------------------------- */
/* Test cases                                                              */
/* ---------------------------------------------------------------------- */

/* TC-HW-I2C-001 */
static void test_init(void)
{
    expect_err("TC-HW-I2C-001 i2c_init", i2c_driver->init(), I2C_ERR_OK);
    expect_err("TC-HW-I2C-001 second i2c_init", i2c_driver->init(), I2C_ERR_OK);
    bringup_pass("TC-HW-I2C-001 I2C2 initialised, init idempotent");
}

/* TC-HW-I2C-002 */
static void test_who_am_i(void)
{
    expect_byte("TC-HW-I2C-002 HTS221 WHO_AM_I @0x5F",
                read_reg(ADDR_HTS221, REG_WHO_AM_I, "HTS221"), WHO_HTS221);
    expect_byte("TC-HW-I2C-002 LPS22HB WHO_AM_I @0x5D",
                read_reg(ADDR_LPS22HB, REG_WHO_AM_I, "LPS22HB"), WHO_LPS22HB);
    expect_byte("TC-HW-I2C-002 LIS3MDL WHO_AM_I @0x1E",
                read_reg(ADDR_LIS3MDL, REG_WHO_AM_I, "LIS3MDL"), WHO_LIS3MDL);
    expect_byte("TC-HW-I2C-002 LSM6DSL WHO_AM_I @0x6A",
                read_reg(ADDR_LSM6DSL, REG_WHO_AM_I, "LSM6DSL"), WHO_LSM6DSL);
    bringup_pass("TC-HW-I2C-002 all four sensors answer WHO_AM_I");
}

/* TC-HW-I2C-003 */
static void test_write_then_readback(void)
{
    const uint8_t cmd[2] = {LPS22HB_CTRL_REG1, LPS22HB_CTRL_REG1_VALUE};

    expect_err("TC-HW-I2C-003 i2c_write", i2c_driver->write(ADDR_LPS22HB, cmd, 2U), I2C_ERR_OK);
    expect_byte("TC-HW-I2C-003 CTRL_REG1 read-back",
                read_reg(ADDR_LPS22HB, LPS22HB_CTRL_REG1, "CTRL_REG1"), LPS22HB_CTRL_REG1_VALUE);
    bringup_pass("TC-HW-I2C-003 write then read-back");
}

/* TC-HW-I2C-004 */
static void test_plain_read(void)
{
    const uint8_t reg = REG_WHO_AM_I;
    uint8_t value = 0U;

    expect_err("TC-HW-I2C-004 set register pointer", i2c_driver->write(ADDR_LPS22HB, &reg, 1U),
               I2C_ERR_OK);
    expect_err("TC-HW-I2C-004 i2c_read", i2c_driver->read(ADDR_LPS22HB, &value, 1U), I2C_ERR_OK);
    expect_byte("TC-HW-I2C-004 WHO_AM_I via plain read", value, WHO_LPS22HB);
    bringup_pass("TC-HW-I2C-004 plain i2c_read after register-pointer write");
}

/* TC-HW-I2C-005 */
static void test_burst_read(void)
{
    const uint8_t reg = HTS221_CALIB_REG;
    uint8_t cal[HTS221_CALIB_LEN];
    bool all_same = true;

    expect_err("TC-HW-I2C-005 burst read",
               i2c_driver->write_read(ADDR_HTS221, &reg, 1U, cal, HTS221_CALIB_LEN), I2C_ERR_OK);
    for (uint32_t i = 1U; i < HTS221_CALIB_LEN; i++)
    {
        if (cal[i] != cal[0])
        {
            all_same = false;
        }
    }
    if (all_same)
    {
        bringup_fail("TC-HW-I2C-005 burst returned one byte repeated (auto-increment?)");
    }
    if (cal[1] <= cal[0])
    {
        bringup_fail("TC-HW-I2C-005 H1_rH_x2 not above H0_rH_x2 (calibration implausible)");
    }
    bringup_pass("TC-HW-I2C-005 16-byte burst read");
}

/* TC-HW-I2C-006 */
static void test_absent_device(void)
{
    const uint8_t data[1] = {0x00U};

    expect_err("TC-HW-I2C-006 absent address", i2c_driver->write(ADDR_ABSENT, data, 1U),
               I2C_ERR_NACK);
    expect_byte("TC-HW-I2C-006 bus usable after NACK",
                read_reg(ADDR_HTS221, REG_WHO_AM_I, "post-NACK"), WHO_HTS221);
    bringup_pass("TC-HW-I2C-006 NACK reported, bus recovered");
}

/* TC-HW-I2C-007 */
static void test_invalid_arguments(void)
{
    const uint8_t data[1] = {0x00U};
    uint8_t buf[1] = {0U};

    expect_err("TC-HW-I2C-007 NULL write", i2c_driver->write(ADDR_HTS221, NULL, 1U),
               I2C_ERR_INVALID_ARG);
    expect_err("TC-HW-I2C-007 zero-length read", i2c_driver->read(ADDR_HTS221, buf, 0U),
               I2C_ERR_INVALID_ARG);
    expect_err("TC-HW-I2C-007 256-byte write_read",
               i2c_driver->write_read(ADDR_HTS221, data, 1U, buf, 256U), I2C_ERR_INVALID_ARG);
    bringup_pass("TC-HW-I2C-007 invalid arguments rejected");
}

/* TC-HW-I2C-008 */
static void test_timing(void)
{
    const uint8_t reg = REG_WHO_AM_I;
    uint8_t value = 0U;
    const uint32_t cycles_per_us = cpu_get_sysclk_hz() / 1000000U;
    const uint32_t start = DWT->CYCCNT;

    for (uint32_t i = 0U; i < TIMING_ITERATIONS; i++)
    {
        expect_err("TC-HW-I2C-008 timed read",
                   i2c_driver->write_read(ADDR_HTS221, &reg, 1U, &value, 1U), I2C_ERR_OK);
    }

    const uint32_t avg_us = ((DWT->CYCCNT - start) / cycles_per_us) / TIMING_ITERATIONS;
    char msg[96];
    (void) snprintf(msg, sizeof(msg), "[INFO] 1-byte register read averages %lu us\r\n",
                    (unsigned long) avg_us);
    bringup_puts(msg);

    if ((avg_us < TIMING_MIN_US) || (avg_us > TIMING_MAX_US))
    {
        bringup_fail("TC-HW-I2C-008 transaction time outside 60..200 us (bus not at ~400 kHz)");
    }
    bringup_pass("TC-HW-I2C-008 transaction time consistent with 400 kHz");
}

/* ---------------------------------------------------------------------- */
/* Entry point                                                             */
/* ---------------------------------------------------------------------- */

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
     * The I2C2 pins are owned by I2cDriver itself. */
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

    bringup_puts("\r\n===== I2cDriver Hardware Bring-up =====\r\n");
    bringup_puts("Board : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("Bus   : I2C2 PB10 SCL / PB11 SDA, 400 kHz target\r\n");
    bringup_puts("=======================================\r\n\r\n");

    test_init();
    test_who_am_i();
    test_write_then_readback();
    test_plain_read();
    test_burst_read();
    test_absent_device();
    test_invalid_arguments();
    test_timing();

    bringup_puts("\r\n[PASS] All automated tests complete.\r\n");
    bringup_puts(
        "[INFO] Scope SCL once to close I2CD-O1; hold SDA low to exercise I2CD-O4.\r\n\r\n");

    for (;;)
    {
        gpio_toggle_pin(BRINGUP_LED_PORT, BRINGUP_LED_PIN);
        cpu_delay_ms(1000U);
    }
}
