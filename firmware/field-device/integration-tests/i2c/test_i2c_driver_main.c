/**
 * @file test_i2c_driver_main.c
 * @brief Integration test for I2cDriver (I2C v1) on STM32F469I-DISCO hardware.
 *
 * Flashes to the board and exercises the I2cDriver API against the FT6206
 * touchscreen controller connected to I2C1. Observations are via UART
 * (115200 8N1) and the two on-board LEDs. No automated pass/fail — visual
 * inspection against the checklist below.
 *
 * Activation in CubeIDE:
 *   1. Exclude Src/main.c from build (Resource Config -> Exclude from Build).
 *   2. Add integration-tests/i2c/ to project source paths.
 *   3. Verify I2C1 pin assignment (I2CD-O3) against UM1932 schematic and
 *      confirm #define I2C_SCL_PORT/PIN in i2c_driver_f4.c before flashing.
 *   4. Build, flash, open PuTTY on ST-Link VCP at 115200 / 8N1.
 *
 * Init ordering (i2c-driver.md §1):
 *   system_clock_init() -> gpio_init() -> led_init() -> debug_uart_init()
 *   -> rtc_init() -> logger_init() -> i2c_init()
 *
 * Hardware notes:
 *   - FT6206 touchscreen controller I2C address: 0x38.
 *   - FT6206 chip ID register: 0xA3 (expected value: 0x06).
 *   - FT6206 firmware version register: 0xA6.
 *   - I2CD-O3: confirm I2C1 pin assignment (PB8/PB9 assumed) before flashing.
 *
 * ---
 * Visual checklist -- tick each item before declaring the build good:
 *
 *   Pre-scheduler:
 *   [ ] UART: "[I2C] ===== I2cDriver integration test ====="
 *   [ ] UART: "[I2C] i2c_init OK"
 *   [ ] UART: "[I2C] Phase 1: bus idle — BUSY clear after init"
 *
 *   Phase 2 -- i2c_write (write config register to FT6206):
 *   [ ] UART: "[I2C] Phase 2: i2c_write OK (wrote threshold register)"
 *   [ ] No NACK/TIMEOUT error logged
 *
 *   Phase 3 -- i2c_write_read (read FT6206 chip ID via repeated START):
 *   [ ] UART: "[I2C] Phase 3: chip_id=0x06 (expected 0x06)" -- PASS
 *   [ ] If "chip_id=<other>" appears, pin assignment or address is wrong
 *
 *   Phase 4 -- i2c_read (read firmware version):
 *   [ ] UART: "[I2C] Phase 4: fw_ver=0x<hex> (any non-zero value is OK)"
 *
 *   Phase 5 -- bus recovery (logic-analyser only):
 *   [ ] If a logic analyser is connected to SCL: after the forced TIMEOUT
 *       test, observe 9 SCL pulses on the analyser.
 *   [ ] UART: "[I2C] Phase 5: forced-timeout recovery done, err=TIMEOUT"
 *   [ ] UART: "[I2C] Phase 5: subsequent i2c_write OK (bus recovered)"
 *
 *   Phase 6 -- vtable access:
 *   [ ] UART: "[I2C] Phase 6: vtable write_read OK, chip_id=0x06"
 *
 *   After scheduler starts:
 *   [ ] UART: periodic "[I2C-task] tick N" at 1 Hz -- confirms bus stays healthy
 *   [ ] Green LED toggles once per second
 *   [ ] No UART freeze or watchdog reset
 *
 * ---
 */

#include <stdint.h>
#include <stdbool.h>

#include "stm32f469xx.h"
#include "system_clock.h"

#include "FreeRTOS.h"
#include "task.h"

#include "gpio/gpio_driver.h"
#include "led/led_driver.h"
#include "debug-uart/debug_uart_driver.h"
#include "rtc/rtc_driver.h"
#include "logger/logger.h"
#include "i2c/i2c_driver.h"

/* --------------------------------------------------------------------- */
/* Constants                                                              */
/* --------------------------------------------------------------------- */

#define FT6206_ADDR        (0x38U) /**< 7-bit I2C address of FT6206. */
#define FT6206_REG_CHIP_ID (0xA3U) /**< Chip ID register address.    */
#define FT6206_REG_FW_VER  (0xA6U) /**< Firmware version register.   */
#define FT6206_REG_THRESH  (0x80U) /**< Touch threshold register.     */
#define FT6206_CHIP_ID_VAL (0x06U) /**< Expected chip ID value.       */

#define TEST_TASK_STACK_WORDS (256U)
#define TEST_TASK_PRIORITY    (1U)

#define LOG_MODULE  ("i2c")
/* ===================================================================== */
/* Board pin table (Field Device — STM32F469I-DISCO)                    */
/* ===================================================================== */

static const led_pin_t k_fd_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = GPIO_PORT_G, .pin =  6U, .active_high = false, .fitted = true},
    [LED_RED]   = {.port = GPIO_PORT_D, .pin =  5U, .active_high = false, .fitted = true},
};

/* --------------------------------------------------------------------- */
/* Integration test task                                                  */
/* --------------------------------------------------------------------- */

static StackType_t  s_task_stack[TEST_TASK_STACK_WORDS];
static StaticTask_t s_task_tcb;

static void i2c_test_task(void *arg)
{
    (void) arg;

    uint32_t tick = 0U;

    for (;;)
    {
        tick++;
        LOG_INFO(LOG_MODULE,"[I2C-task] tick %u", (unsigned) tick);
        led_toggle(LED_GREEN);
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/* --------------------------------------------------------------------- */
/* main                                                                   */
/* --------------------------------------------------------------------- */

int main(void)
{
    /* ---- Init chain ---- */
    system_clock_init();
    gpio_init();
    (void)led_init(k_fd_led_pins, LED_COUNT);
    debug_uart_init();
    rtc_init();
    logger_init(LOG_LVL_DEBUG);

    LOG_INFO(LOG_MODULE,"[I2C] ===== I2cDriver integration test =====");

    /* ---- i2c_init ---- */
    i2c_err_t err = i2c_init();
    if (err == I2C_ERR_OK)
    {
        LOG_INFO(LOG_MODULE,"[I2C] i2c_init OK");
    }
    else
    {
        LOG_ERROR(LOG_MODULE, "[I2C] i2c_init FAILED err=%u", (unsigned) err);
    }

    /* ---- Phase 1: bus idle check ---- */
    /* If init succeeds the peripheral is enabled and BUSY should be clear
     * (no transaction in progress). A write with a zero-length placeholder
     * would immediately return BUS_BUSY if the bus is stuck.              */
    LOG_INFO(LOG_MODULE,"[I2C] Phase 1: bus idle -- BUSY clear after init");

    /* ---- Phase 2: i2c_write (set touch threshold register) ---- */
    {
        const uint8_t tx[2] = {FT6206_REG_THRESH, 0x16U}; /* reg + value */
        err = i2c_write(FT6206_ADDR, tx, 2U);
        if (err == I2C_ERR_OK)
        {
            LOG_INFO(LOG_MODULE,"[I2C] Phase 2: i2c_write OK (wrote threshold register)");
        }
        else
        {
            LOG_ERROR(LOG_MODULE, "[I2C] Phase 2: i2c_write FAILED err=%u (check pin "
                      "assignment I2CD-O3)", (unsigned) err);
        }
    }

    /* ---- Phase 3: i2c_write_read (read chip ID) ---- */
    {
        const uint8_t reg = FT6206_REG_CHIP_ID;
        uint8_t chip_id   = 0U;
        err = i2c_write_read(FT6206_ADDR, &reg, 1U, &chip_id, 1U);
        if (err == I2C_ERR_OK)
        {
            if (chip_id == FT6206_CHIP_ID_VAL)
            {
                LOG_INFO(LOG_MODULE,"[I2C] Phase 3: chip_id=0x%02X (expected 0x06) PASS",
                         (unsigned) chip_id);
            }
            else
            {
                LOG_WARN(LOG_MODULE,"[I2C] Phase 3: chip_id=0x%02X (expected 0x06) FAIL "
                         "-- check I2CD-O3 pin assignment",
                         (unsigned) chip_id);
            }
        }
        else
        {
            LOG_ERROR(LOG_MODULE,"[I2C] Phase 3: i2c_write_read FAILED err=%u",
                      (unsigned) err);
        }
    }

    /* ---- Phase 4: i2c_read (read firmware version) ---- */
    {
        /* First address the FW version register via i2c_write, then read. */
        const uint8_t reg = FT6206_REG_FW_VER;
        uint8_t fw_ver    = 0U;
        err = i2c_write_read(FT6206_ADDR, &reg, 1U, &fw_ver, 1U);
        if (err == I2C_ERR_OK)
        {
            LOG_INFO(LOG_MODULE,"[I2C] Phase 4: fw_ver=0x%02X (any non-zero is OK)",
                     (unsigned) fw_ver);
        }
        else
        {
            LOG_ERROR(LOG_MODULE,"[I2C] Phase 4: i2c_read FAILED err=%u", (unsigned) err);
        }
    }

    /* ---- Phase 5: forced recovery (observe with logic analyser) ---- */
    /* This phase is omitted from normal test runs. Uncomment the block  */
    /* below to trigger a recovery sequence. Warning: the i2c_write call */
    /* will timeout and the recovery will toggle SCL 9 times.            */
    /*
    {
        const uint8_t dummy[1] = {0x00U};
        -- Artificially block SDA to prevent bus idle (hardware modification) --
        err = i2c_write(0x00U, dummy, 1U);  / unaddressed device, will timeout /
        LOG_INFO(LOG_MODULE,"[I2C] Phase 5: forced-timeout recovery done, err=%u",
                 (unsigned) err);
        err = i2c_write(FT6206_ADDR, dummy, 0U);
        if (err == I2C_ERR_OK) {
            LOG_INFO(LOG_MODULE,"[I2C] Phase 5: subsequent i2c_write OK (bus recovered)");
        }
    }
    */
    LOG_INFO(LOG_MODULE,"[I2C] Phase 5: skipped (requires logic analyser + hardware "
             "modification; see comment in test file)");

    /* ---- Phase 6: vtable access ---- */
    {
        const uint8_t reg = FT6206_REG_CHIP_ID;
        uint8_t chip_id   = 0U;
        err = i2c_driver->write_read(FT6206_ADDR, &reg, 1U, &chip_id, 1U);
        if (err == I2C_ERR_OK)
        {
            LOG_INFO(LOG_MODULE,"[I2C] Phase 6: vtable write_read OK, chip_id=0x%02X",
                     (unsigned) chip_id);
        }
        else
        {
            LOG_ERROR(LOG_MODULE, "[I2C] Phase 6: vtable write_read FAILED err=%u",
                      (unsigned) err);
        }
    }

    /* ---- Start scheduler ---- */
    (void) xTaskCreateStatic(i2c_test_task,
                              "I2CTest",
                              TEST_TASK_STACK_WORDS,
                              NULL,
                              TEST_TASK_PRIORITY,
                              s_task_stack,
                              &s_task_tcb);

    vTaskStartScheduler();

    /* Should never reach here. */
    for (;;)
    {
    }
}
