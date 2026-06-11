/**
 * @file test_modbus_slave_main.c
 * @brief Integration test for ModbusSlave on STM32F469I-DISCO hardware.
 *
 * Exercises the ModbusSlave middleware end-to-end with a real ModbusUartDriver.
 * A USB-to-RS-485 adapter (e.g. CH340 + MAX485) is connected to USART6 via the
 * RS-485 transceiver on the target.  A PC-side Modbus master tool (e.g. ModRSsim2
 * or a Python minimalmodbus script) sends RTU frames at 9600 baud, 8N1.
 *
 * Activation in CubeIDE:
 *   1. Exclude Src/main.c from build (Resource Config -> Exclude from Build).
 *   2. Add integration-tests/modbus_slave/ to project source paths.
 *   3. Connect USB-RS485 adapter: A/B to transceiver, DE/RE tied to PG14 DE pin.
 *   4. Build, flash, open PuTTY on ST-Link VCP at 115200 / 8N1 for log output.
 *   5. On PC, open ModRSsim2 (or equivalent) targeting slave address 0x01.
 *
 * Init ordering (modbus-slave.md §2.4):
 *   system_clock_init() -> debug_uart_init() -> rtc_init() -> logger_init()
 *   -> modbus_uart_init()   [pre-scheduler]
 *   -> xTaskCreateStatic()  [ModbusTask created — handle valid before init]
 *   -> vTaskStartScheduler()
 *   -> modbus_slave_init()  [inside ModbusTask — phase 2]
 *   -> ulTaskNotifyTake()   [blocks waiting for ISR notification]
 *   -> modbus_slave_process() [called on each notification]
 *
 * ---
 * Visual checklist — tick each item before declaring the build good:
 *
 *   Pre-scheduler:
 *   [ ] VCP: "[MSLAVE] ===== ModbusSlave integration test ====="
 *   [ ] VCP: "[MSLAVE] modbus_uart_init OK"
 *   [ ] VCP: "[MSLAVE] starting scheduler..."
 *
 *   Phase 1 — FC03 read holding register (PC master → target):
 *   [ ] Send FC03 request: 01 03 00 00 00 01 <CRC>
 *   [ ] VCP: "[MSLAVE] Phase 1: FC03 frame processed — response TX'd"
 *   [ ] PC master log: received response with value 0x1234
 *
 *   Phase 2 — FC06 write single register:
 *   [ ] Send FC06 request: 01 06 00 00 12 34 <CRC>
 *   [ ] VCP: "[MSLAVE] Phase 2: FC06 write addr=0x0000 val=0x1234"
 *   [ ] PC master log: ACK echo response received
 *
 *   Phase 3 — address mismatch (frame to slave 2):
 *   [ ] Send FC03 request: 02 03 00 00 00 01 <CRC>
 *   [ ] VCP: "[MSLAVE] Phase 3: address mismatch — silent drop (expected)"
 *
 *   Phase 4 — stats snapshot:
 *   [ ] VCP: "[MSLAVE] Phase 4: valid=2 crc_errors=0 mismatches=1 exceptions=0"
 *
 *   Periodic tick:
 *   [ ] VCP: periodic "[MSLAVE-task] tick N" at 1 Hz
 *   [ ] No VCP freeze or watchdog reset
 *
 * ---
 */

#include <stdbool.h>
#include <stdint.h>

#include "stm32f469xx.h"
#include "system_clock.h"

#include "FreeRTOS.h"
#include "task.h"

#include "debug-uart/debug_uart_driver.h"
#include "rtc/rtc_driver.h"
#include "logger/logger.h"
#include "modbus_uart_driver/modbus_uart_driver.h"
#include "modbus_slave/modbus_slave.h"

/* ===================================================================== */
/* Configuration                                                         */
/* ===================================================================== */

#define LOG_MODULE                ("MSLAVE")

#define MODBUS_TASK_STACK_WORDS   (512U)
#define MODBUS_TASK_PRIORITY      (tskIDLE_PRIORITY + 2U)

#define STATS_TASK_STACK_WORDS    (256U)
#define STATS_TASK_PRIORITY       (tskIDLE_PRIORITY + 1U)

/** Slave address used for the integration test. */
#define TEST_SLAVE_ADDR           (0x01U)

/** Holding register value returned for all read requests. */
#define TEST_HOLDING_VALUE        (0x1234U)

/** Notify bit set by ISR callback. */
#define MODBUS_NOTIFY_RX_DONE     (1UL)

/* ===================================================================== */
/* Minimal register map — stub for integration test                     */
/* ===================================================================== */

static modbus_slave_err_t it_read_input(uint16_t addr, uint16_t *out)
{
    (void) addr;
    *out = TEST_HOLDING_VALUE;
    return MODBUS_SLAVE_ERR_OK;
}

static modbus_slave_err_t it_read_holding(uint16_t addr, uint16_t *out)
{
    (void) addr;
    *out = TEST_HOLDING_VALUE;
    return MODBUS_SLAVE_ERR_OK;
}

static modbus_slave_err_t it_write_holding(uint16_t addr, uint16_t value)
{
    LOG_INFO(LOG_MODULE, "Phase 2: FC06 write addr=0x%04X val=0x%04X",
             (unsigned) addr, (unsigned) value);
    return MODBUS_SLAVE_ERR_OK;
}

static const IModbusRegisterMap k_reg_map = {
    .read_input    = it_read_input,
    .read_holding  = it_read_holding,
    .write_holding = it_write_holding,
};

/* ===================================================================== */
/* ModbusTask                                                            */
/* ===================================================================== */

static StaticTask_t s_modbus_tcb;
static StackType_t  s_modbus_stack[MODBUS_TASK_STACK_WORDS] __attribute__((aligned(8)));

static void modbus_task(void *arg)
{
    (void) arg;

    /* Two-phase init: call modbus_slave_init() here so task_handle is valid. */
    modbus_slave_err_t err = modbus_slave_init(&k_reg_map,
                                               TEST_SLAVE_ADDR,
                                               xTaskGetCurrentTaskHandle());
    if (err != MODBUS_SLAVE_ERR_OK)
    {
        LOG_ERROR(LOG_MODULE, "modbus_slave_init FAILED err=%u", (unsigned) err);
        vTaskDelete(NULL);
    }

    LOG_INFO(LOG_MODULE, "modbus_slave_init OK — slave addr=0x%02X",
             (unsigned) TEST_SLAVE_ADDR);
    LOG_INFO(LOG_MODULE, "Listening for Modbus RTU frames on USART6 (PG14 TX / PG9 RX)");

    for (;;)
    {
        /* Block until ISR notifies (xTaskNotifyFromISR in on_frame_complete). */
        (void) ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        modbus_slave_err_t process_err = modbus_slave_process();
        (void) process_err;
    }
}

/* ===================================================================== */
/* StatsTask — periodic diagnostics                                     */
/* ===================================================================== */

static StaticTask_t s_stats_tcb;
static StackType_t  s_stats_stack[STATS_TASK_STACK_WORDS] __attribute__((aligned(8)));

static void stats_task(void *arg)
{
    (void) arg;
    uint32_t tick = 0U;

    for (;;)
    {
        tick++;

        modbus_slave_stats_t stats;
        (void) modbus_slave_get_stats(&stats);

        LOG_INFO(LOG_MODULE, "[MSLAVE-task] tick %lu  valid=%lu crc_err=%lu",
        					(unsigned long) tick,
        	                (unsigned long) stats.valid_frames,
        	                (unsigned long) stats.crc_errors);

        LOG_INFO(LOG_MODULE, "[MSLAVE-task] mismatch=%lu exc=%lu unsup=%lu ok=%lu",
        					 (unsigned long) stats.address_mismatches,
							 (unsigned long) stats.exception_responses,
							 (unsigned long) stats.unsupported_fc,
							 (unsigned long) stats.successful_responses);

        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/* ===================================================================== */
/* Entry point                                                           */
/* ===================================================================== */

int main(void)
{
    /* 1. Clock tree -> 180 MHz. */
    system_clock_init();

    /* 2. Debug peripherals. */
    (void) debug_uart_init();
    (void) rtc_init();
    (void) logger_init(LOG_LEVEL_DEBUG);

    /* 3. ModbusUartDriver — phase 1 (pre-scheduler). */
    (void) modbus_uart_init();

    /* 4. Pre-scheduler banner. */
    LOG_INFO(LOG_MODULE, "===== ModbusSlave integration test =====");
    LOG_INFO(LOG_MODULE, "modbus_uart_init OK");
    LOG_INFO(LOG_MODULE, "starting scheduler...");

    /* 5. Create tasks — ModbusTask first so its handle is valid before
     *    modbus_slave_init() is called inside the task body. */
    (void) xTaskCreateStatic(modbus_task,
                             "ModbusTask",
                             MODBUS_TASK_STACK_WORDS,
                             NULL,
                             MODBUS_TASK_PRIORITY,
                             s_modbus_stack,
                             &s_modbus_tcb);

    (void) xTaskCreateStatic(stats_task,
                             "StatsTask",
                             STATS_TASK_STACK_WORDS,
                             NULL,
                             STATS_TASK_PRIORITY,
                             s_stats_stack,
                             &s_stats_tcb);

    /* 6. Start scheduler. Does not return under normal operation. */
    vTaskStartScheduler();

    for (;;)
    {
    }
}
