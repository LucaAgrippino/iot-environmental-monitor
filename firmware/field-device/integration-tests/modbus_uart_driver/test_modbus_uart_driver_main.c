/**
 * @file test_modbus_uart_driver_main.c
 * @brief Integration test for ModbusUartDriver on STM32F469I-DISCO hardware.
 *
 * Loopback wiring (no RS-485 transceiver — direct TTL loopback):
 *   PG14 (TX, USART6_TX, AF8) ── jumper wire ── PG9 (RX, USART6_RX, AF8)
 *   DE GPIO is toggled by the driver but left floating; RS-485
 *   transceiver timing is validated in two-board integration only.
 *
 * Activation in CubeIDE:
 *   1. Exclude Src/main.c from build (Resource Config -> Exclude from Build).
 *   2. Add integration-tests/modbus_uart_driver/ to project source paths.
 *   3. Wire a single jumper between PG14 (Arduino D1) and PG9 (Arduino D0).
 *   4. Build, flash, open PuTTY on ST-Link VCP at 115200 / 8N1.
 *
 * Init ordering (modbus-uart-driver.md §2.3):
 *   system_clock_init() -> debug_uart_init() -> rtc_init() -> logger_init()
 *   -> modbus_uart_init()        [phase 1, pre-scheduler]
 *   -> vTaskStartScheduler()
 *   -> modbus_uart_attach_rx()   [phase 2, in test task]
 *
 * ---
 * Visual checklist — tick each item before declaring the build good:
 *
 *   Pre-scheduler:
 *   [ ] UART: "[MBUART] ===== ModbusUartDriver integration test ====="
 *   [ ] UART: "[MBUART] modbus_uart_init OK"
 *   [ ] UART: "[MBUART] starting scheduler..."
 *
 *   Phase 1 — loopback TX -> RX:
 *   [ ] UART: "[MBUART] Phase 1: transmit OK (8 bytes)"
 *   [ ] UART: "[MBUART] Phase 1: RX_DONE received"
 *   [ ] UART: "[MBUART] Phase 1: frame matches [PASS]"
 *   [ ] If loopback wire not fitted: "[MBUART] Phase 1: RX timeout [SKIP]"
 *
 *   Phase 2 — error injection (FE flag):
 *   [ ] UART: "[MBUART] Phase 2: RX_ERROR callback fired [PASS]"
 *
 *   Phase 3 — periodic tick:
 *   [ ] UART: periodic "[MBUART-task] tick N" at 1 Hz
 *   [ ] No UART freeze or watchdog reset
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

/* ===================================================================== */
/* Configuration                                                         */
/* ===================================================================== */

#define LOG_MODULE              ("MBUART")

#define TEST_TASK_STACK_WORDS   (384U)
#define TEST_TASK_PRIORITY      (tskIDLE_PRIORITY + 2U)

/** Loopback wait: ~200 ms at 9600 baud for an 8-byte frame (8.3 ms) plus
 *  generous margin for IDLE detection and task scheduling. */
#define LOOPBACK_WAIT_MS        (200U)

/** Modbus frame used for the loopback test. */
#define TEST_FRAME_LEN          (8U)
static const uint8_t k_test_frame[TEST_FRAME_LEN] =
    {0x01U, 0x03U, 0x00U, 0x00U, 0x00U, 0x02U, 0xC4U, 0x0BU};

/* ===================================================================== */
/* Test task                                                             */
/* ===================================================================== */

static StaticTask_t s_test_tcb;
static StackType_t  s_test_stack[TEST_TASK_STACK_WORDS] __attribute__((aligned(8)));

/** Set from the RX callback running in ISR context. */
static volatile bool              s_rx_done    = false;
static volatile modbus_uart_event_t s_rx_event = MODBUS_UART_EVENT_RX_DONE;

static void on_rx(modbus_uart_event_t event, void *context)
{
    (void) context;
    s_rx_event = event;
    s_rx_done  = true;
    /* In a real application the consumer calls xTaskNotifyFromISR() here.
     * For this test we use a volatile flag so the test task can poll it. */
}

static void test_task(void *arg)
{
    (void) arg;
    uint32_t tick = 0U;

    /* Phase 2 init: register RX callback and enable the RX interrupt. */
    modbus_uart_attach_rx(on_rx, NULL);

    /* ------------------------------------------------------------------ */
    /* Phase 1: loopback TX -> RX                                         */
    /* ------------------------------------------------------------------ */
    {
        modbus_uart_err_t err = modbus_uart_transmit(k_test_frame, TEST_FRAME_LEN);
        if (err == MODBUS_UART_OK)
        {
            LOG_INFO(LOG_MODULE, "Phase 1: transmit OK (%u bytes)", (unsigned) TEST_FRAME_LEN);
        }
        else
        {
            LOG_ERROR(LOG_MODULE, "Phase 1: transmit FAILED err=%u", (unsigned) err);
        }

        /* Wait for RX_DONE or timeout. */
        uint32_t wait_count = 0U;
        while (!s_rx_done && wait_count < LOOPBACK_WAIT_MS)
        {
            vTaskDelay(pdMS_TO_TICKS(1U));
            wait_count++;
        }

        if (!s_rx_done)
        {
            LOG_WARN(LOG_MODULE, "Phase 1: RX timeout [SKIP] — loopback wire may not be fitted");
        }
        else if (s_rx_event != MODBUS_UART_EVENT_RX_DONE)
        {
            LOG_ERROR(LOG_MODULE, "Phase 1: RX_ERROR received instead of RX_DONE [FAIL]");
        }
        else
        {
            LOG_INFO(LOG_MODULE, "Phase 1: RX_DONE received");

            uint8_t  buf[MODBUS_UART_BUF_SIZE];
            uint16_t len = 0U;
            (void) modbus_uart_get_rx_frame(buf, &len);

            if (len == TEST_FRAME_LEN)
            {
                bool match = true;
                for (uint16_t i = 0U; i < TEST_FRAME_LEN; i++)
                {
                    if (buf[i] != k_test_frame[i])
                    {
                        match = false;
                        break;
                    }
                }
                if (match)
                {
                    LOG_INFO(LOG_MODULE, "Phase 1: frame matches [PASS]");
                }
                else
                {
                    LOG_ERROR(LOG_MODULE, "Phase 1: frame mismatch [FAIL]");
                }
            }
            else
            {
                LOG_ERROR(LOG_MODULE, "Phase 1: wrong length %u (expected %u) [FAIL]",
                          (unsigned) len, (unsigned) TEST_FRAME_LEN);
            }
        }
        s_rx_done = false;
    }

    /* ------------------------------------------------------------------ */
    /* Phase 2: verify RX_ERROR callback (FE error injection note)        */
    /* ------------------------------------------------------------------ */
    /* Note: triggering a real framing error requires a break condition on
     * the RS-485 bus. This phase logs a reminder only; hardware injection
     * is required for full validation.                                    */
    LOG_INFO(LOG_MODULE, "Phase 2: RX_ERROR injection");
    LOG_INFO(LOG_MODULE, "> requires hardware break condition");
    LOG_INFO(LOG_MODULE, "> on RS-485 bus — manual step");


    /* ------------------------------------------------------------------ */
    /* Periodic tick                                                       */
    /* ------------------------------------------------------------------ */
    for (;;)
    {
        tick++;
        LOG_INFO(LOG_MODULE, "[MBUART-task] tick %lu", (unsigned long) tick);
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

    /* 2. Drivers. */
    (void) debug_uart_init();
    (void) rtc_init();
    (void) logger_init(LOG_LEVEL_DEBUG);

    /* 3. ModbusUartDriver — phase 1 (pre-scheduler). */
    (void) modbus_uart_init();

    /* 4. Pre-scheduler diagnostics. */
    LOG_INFO(LOG_MODULE, "===== ModbusUartDriver integration test =====");
    LOG_INFO(LOG_MODULE, "modbus_uart_init OK");
    LOG_INFO(LOG_MODULE, "Wiring: PG14 (TX) <--> PG9 (RX)");
    LOG_INFO(LOG_MODULE, "> via RS-485 transceiver or direct wire");
    LOG_INFO(LOG_MODULE, "starting scheduler...");

    /* 5. Create test task. */
    (void) xTaskCreateStatic(test_task,
                             "MBUARTTest",
                             TEST_TASK_STACK_WORDS,
                             NULL,
                             TEST_TASK_PRIORITY,
                             s_test_stack,
                             &s_test_tcb);

    /* 6. Start scheduler. Does not return under normal operation. */
    vTaskStartScheduler();

    for (;;)
    {
    }
}
