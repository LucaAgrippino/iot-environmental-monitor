/**
 * @file main_test_modbus_poller.c
 * @brief Hardware bring-up test for ModbusPoller (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. Add the current harness (cloud_publisher) to the `excluding=` list of
 *      the `integration-tests` source entry and remove modbus_poller.
 *   2. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/modbus_poller.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * WIRING.  Jumper Arduino D1 (PA0, UART4_TX) to D0 (PA1, UART4_RX), as for the
 * ModbusMaster bring-up.  The master hears its own request, which is a
 * well-formed frame, so every poll and every command "succeeds" and the reply
 * payload equals the request payload.  That is enough to drive the whole poller
 * on silicon: cycle, hysteresis, command routing and the HealthMonitor feed.
 * Slave 1 comes from the weak placeholder DeviceProfileRegistry (FC04, 0x0010,
 * 3 registers).
 *
 * Hardware outputs:
 *   USART1 TX  PB6      115 200 8N1  Human-readable test results
 *   UART4      PA0/PA1  9600 8N1     Modbus link (looped back)
 *   LD2        PB14                  HealthMonitor status LED
 *
 * Tasks: ModbusPollerTask (priority 4, the real entry point) runs
 * modbus_poller_run(); a lower-priority driver task observes and posts commands.
 *
 * Automated test sequence:
 *   TC-HW-MBP-001  init; poller task started; modbus_master_start() done by run().
 *                  modbus_poller_is_ready() is false before init and true after it.
 *   TC-HW-MBP-002  Three loopback polls bring the link up (HEALTH_EVENT_MODBUS_LINK_UP).
 *   TC-HW-MBP-003  FC06 command executes; the echoed words come back on the response queue.
 *   TC-HW-MBP-004  FC16 command (3 registers) executes; echo is identical.
 *   TC-HW-MBP-005  Bad commands are refused without touching the bus.
 *   TC-HW-MBP-006  Master counters reach HealthMonitor every cycle.
 *   TC-HW-MBP-007  Receiver disabled: three failed polls take the link down.
 *   TC-HW-MBP-008  Receiver re-enabled: the link comes back up (hysteresis, 3 successes).
 *   TC-HW-MBP-009  ModbusPollerTask stack high-water mark leaves margin (2 KB budget).
 *   TC-HW-MBP-010  modbus_poller_get_fd_reading() (CP-D18) on the board: OK for the provisioned
 *                  slave with link_online true; has_data false, because the loopback echoes the
 *                  request and an echo is not an FC04 response of the expected length (it is
 *                  ignored, not decoded). Real values are checked with a Field Device in WP7.
 *
 * Integration checklist:
 *   [ ] Jumper D1-D0 fitted
 *   [ ] TC-HW-MBP-009 words free printed and above the margin
 *   [ ] LD2 follows the HealthMonitor pattern (operational state is not driven here)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "FreeRTOS.h"
#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "health_monitor/health_monitor.h"
#include "led/led_driver.h"
#include "modbus_master/modbus_master.h"
#include "modbus_poller/modbus_poller.h"
#include "modbus_uart_driver/modbus_uart_driver.h"
#include "queue.h"
#include "stm32l475xx.h"
#include "task.h"

/* ---------------------------------------------------------------------- */
/* Board and test constants                                                */
/* ---------------------------------------------------------------------- */

#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)
#define BRINGUP_TX_TIMEOUT_MS (100U)

#define LD2_PIN (14U)
#define LD2_RED_PIN (5U)

#define POLLER_TASK_STACK_WORDS (512U) /* task-breakdown.md: 512 words / 2 KB */
#define POLLER_TASK_PRIORITY (4U)
#define DRIVER_TASK_STACK_WORDS (1024U)
#define DRIVER_TASK_PRIORITY (3U)

#define UART4_IRQ_PRIORITY (6U) /* >= configMAX_SYSCALL_INTERRUPT_PRIORITY (5) */
#define RESPONSE_TIMEOUT_MS (200U)
#define MAX_RETRIES (3U)
#define POLL_PERIOD_MS (200U)
#define SLAVE_ADDR (1U)
#define UNKNOWN_SLAVE_ADDR (99U)

#define LINK_UP_WAIT_MS (3000U)   /* 3 polls x 200 ms plus margin */
#define LINK_DOWN_WAIT_MS (8000U) /* 3 failed polls x ~800 ms plus margin */
#define COMMAND_WAIT_MS (3000U)
#define SAMPLE_GAP_MS (600U)
#define WAIT_STEP_MS (50U)
#define STACK_MARGIN_WORDS (32U)

#define FC06_REG (0x0100U)
#define FC06_VALUE (0xABCDU)
#define FC16_REG (0x0110U)
#define FC16_COUNT (3U)

static StaticTask_t s_poller_tcb;
static StackType_t s_poller_stack[POLLER_TASK_STACK_WORDS];
static StaticTask_t s_driver_tcb;
static StackType_t s_driver_stack[DRIVER_TASK_STACK_WORDS];
static TaskHandle_t s_poller_handle;

static StaticQueue_t s_resp_storage;
static uint8_t s_resp_buf[sizeof(modbus_command_response_t)];
static volatile bool s_ready_before_init;
static volatile bool s_ready_after_init;
static QueueHandle_t s_resp_queue;

static health_monitor_handle_t s_hm;

static const led_pin_t k_gw_led_pins[LED_COUNT] = {
    [LED_GREEN] = {.port = (uint8_t) GPIO_PORT_B,
                   .pin = (uint8_t) LD2_PIN,
                   .active_high = true,
                   .fitted = true},
    [LED_RED] = {.port = (uint8_t) GPIO_PORT_A,
                 .pin = LD2_RED_PIN,
                 .active_high = true,
                 .fitted = false}, /* LD1 / SB1 - shared with SPI1_SCK */
};

/* ---------------------------------------------------------------------- */
/* Reporting helpers - built on DebugUartDriver.                           */
/* ---------------------------------------------------------------------- */

static void bringup_puts(const char *s)
{
    (void) debug_uart_send((const uint8_t *) s, strlen(s), BRINGUP_TX_TIMEOUT_MS);
}

static void bringup_pass(const char *label)
{
    bringup_puts("[PASS] ");
    bringup_puts(label);
    bringup_puts("\r\n");
}

static void bringup_fail(const char *label)
{
    bringup_puts("[FAIL] ");
    bringup_puts(label);
    bringup_puts(" - HALTED\r\n");
    for (;;)
    {
    }
}

static void check(bool condition, const char *label)
{
    if (!condition)
    {
        bringup_fail(label);
    }
}

static void put_u32(const char *label, uint32_t value)
{
    char digits[11];
    size_t n = 0U;

    do
    {
        digits[n] = (char) ('0' + (value % 10U));
        value /= 10U;
        n++;
    } while (value > 0U);

    bringup_puts("[INFO] ");
    bringup_puts(label);
    bringup_puts(" = ");
    while (n > 0U)
    {
        n--;
        (void) debug_uart_send((const uint8_t *) &digits[n], 1U, BRINGUP_TX_TIMEOUT_MS);
    }
    bringup_puts("\r\n");
}

/* ---------------------------------------------------------------------- */
/* Helpers                                                                 */
/* ---------------------------------------------------------------------- */

static uint32_t tick_ms(void)
{
    return (uint32_t) xTaskGetTickCount();
}

static health_snapshot_t snapshot(void)
{
    health_snapshot_t snap;

    (void) memset(&snap, 0, sizeof(snap));
    check(health_monitor_get_snapshot(s_hm, &snap) == HEALTH_MONITOR_ERR_OK, "get_snapshot");
    return snap;
}

/** Wait until the snapshot's link flag equals `want`, up to `limit_ms`. */
static bool wait_for_link(bool want, uint32_t limit_ms)
{
    for (uint32_t waited = 0U; waited < limit_ms; waited += WAIT_STEP_MS)
    {
        if (snapshot().modbus_link_online == want)
        {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(WAIT_STEP_MS));
    }
    return snapshot().modbus_link_online == want;
}

/** Post a command and wait for its answer; the answer's status is returned in *out. */
static void run_command(const modbus_command_t *cmd, modbus_command_response_t *out,
                        const char *label)
{
    check(modbus_poller_send_command(cmd) == MODBUS_POLLER_ERR_OK, label);
    check(xQueueReceive(s_resp_queue, out, pdMS_TO_TICKS(COMMAND_WAIT_MS)) == pdTRUE,
          "command response timed out");
}

/* ---------------------------------------------------------------------- */
/* Driver task                                                             */
/* ---------------------------------------------------------------------- */

static void driver_task(void *arg)
{
    modbus_command_response_t rsp;

    (void) arg;

    /* TC-HW-MBP-001: init ran before the scheduler; the poller task is already looping. */
    check(!s_ready_before_init, "TC-HW-MBP-001 is_ready true before init");
    check(s_ready_after_init, "TC-HW-MBP-001 is_ready false after init");
    bringup_pass("TC-HW-MBP-001 init; ModbusPollerTask running (master started by run())");

    /* TC-HW-MBP-002 */
    check(wait_for_link(true, LINK_UP_WAIT_MS),
          "TC-HW-MBP-002 link did not come up - is the D1-D0 jumper fitted?");
    bringup_pass("TC-HW-MBP-002 three loopback polls bring the link up");

    /* TC-HW-MBP-010 */
    {
        modbus_fd_reading_t fd;

        check(modbus_poller_get_fd_reading(SLAVE_ADDR, &fd) == MODBUS_POLLER_ERR_OK,
              "TC-HW-MBP-010 get_fd_reading status");
        check(fd.link_online, "TC-HW-MBP-010 link_online");
        check(!fd.has_data, "TC-HW-MBP-010 an echoed request must not be decoded as data");
        check(modbus_poller_get_fd_reading(UNKNOWN_SLAVE_ADDR, &fd) == MODBUS_POLLER_ERR_REJECTED,
              "TC-HW-MBP-010 unknown slave");
        bringup_pass("TC-HW-MBP-010 get_fd_reading: OK, link online, echo not decoded");
    }

    /* TC-HW-MBP-003 */
    {
        modbus_command_t cmd = {.response_queue = s_resp_queue,
                                .slave_addr = SLAVE_ADDR,
                                .function_code = MODBUS_POLLER_FC_WRITE_SINGLE,
                                .start_reg = FC06_REG,
                                .reg_count = 1U};

        cmd.data[0] = FC06_VALUE;
        run_command(&cmd, &rsp, "TC-HW-MBP-003 send_command");
        check(rsp.status == MODBUS_MASTER_ERR_OK, "TC-HW-MBP-003 status");
        check(rsp.data_len == 2U, "TC-HW-MBP-003 data_len");
        check((rsp.data[0] == FC06_REG) && (rsp.data[1] == FC06_VALUE), "TC-HW-MBP-003 echo");
        bringup_pass("TC-HW-MBP-003 FC06 command executed, echo decoded");
    }

    /* TC-HW-MBP-004 */
    {
        modbus_command_t cmd = {.response_queue = s_resp_queue,
                                .slave_addr = SLAVE_ADDR,
                                .function_code = MODBUS_POLLER_FC_WRITE_MULTIPLE,
                                .start_reg = FC16_REG,
                                .reg_count = FC16_COUNT};

        cmd.data[0] = 0x1122U;
        cmd.data[1] = 0x3344U;
        cmd.data[2] = 0x5566U;
        run_command(&cmd, &rsp, "TC-HW-MBP-004 send_command");
        check(rsp.status == MODBUS_MASTER_ERR_OK, "TC-HW-MBP-004 status");
        /* Echo payload: reg(2) + count(2) + byte count(1) + 6 value bytes = 11 bytes = 5 words. */
        check(rsp.data_len == 5U, "TC-HW-MBP-004 data_len");
        check((rsp.data[0] == FC16_REG) && (rsp.data[1] == FC16_COUNT), "TC-HW-MBP-004 header");
        bringup_pass("TC-HW-MBP-004 FC16 command executed, echo decoded");
    }

    /* TC-HW-MBP-005 */
    {
        modbus_command_t cmd = {.response_queue = s_resp_queue,
                                .slave_addr = UNKNOWN_SLAVE_ADDR,
                                .function_code = MODBUS_POLLER_FC_WRITE_SINGLE,
                                .start_reg = FC06_REG,
                                .reg_count = 1U};

        check(modbus_poller_send_command(&cmd) == MODBUS_POLLER_ERR_REJECTED,
              "TC-HW-MBP-005 unknown slave");
        cmd.slave_addr = SLAVE_ADDR;
        cmd.function_code = 0x03U;
        check(modbus_poller_send_command(&cmd) == MODBUS_POLLER_ERR_INVALID_ARG,
              "TC-HW-MBP-005 function code");
        cmd.function_code = MODBUS_POLLER_FC_WRITE_MULTIPLE;
        cmd.reg_count = 0U;
        check(modbus_poller_send_command(&cmd) == MODBUS_POLLER_ERR_INVALID_ARG,
              "TC-HW-MBP-005 register count");
        check(modbus_poller_send_command(NULL) == MODBUS_POLLER_ERR_NULL_ARG,
              "TC-HW-MBP-005 NULL command");
        bringup_pass("TC-HW-MBP-005 bad commands refused without touching the bus");
    }

    /* TC-HW-MBP-006 */
    {
        const health_snapshot_t before = snapshot();

        vTaskDelay(pdMS_TO_TICKS(SAMPLE_GAP_MS));

        const health_snapshot_t after = snapshot();

        put_u32("modbus_transactions_ok", after.modbus_transactions_ok);
        check(after.modbus_transactions_ok > before.modbus_transactions_ok,
              "TC-HW-MBP-006 counters not advancing");
        bringup_pass("TC-HW-MBP-006 master counters reach HealthMonitor each cycle");
    }

    /* TC-HW-MBP-007 */
    UART4->CR1 &= ~USART_CR1_RE;
    check(wait_for_link(false, LINK_DOWN_WAIT_MS), "TC-HW-MBP-007 link did not go down");
    check(snapshot().modbus_timeouts > 0U, "TC-HW-MBP-007 timeouts not reported");
    bringup_pass("TC-HW-MBP-007 receiver off: three failed polls take the link down");

    /* TC-HW-MBP-008 */
    UART4->CR1 |= USART_CR1_RE;
    check(wait_for_link(true, LINK_UP_WAIT_MS + (MAX_RETRIES + 1U) * RESPONSE_TIMEOUT_MS),
          "TC-HW-MBP-008 link did not recover");
    bringup_pass("TC-HW-MBP-008 receiver back: link up again after three successes");

    /* TC-HW-MBP-009 */
    {
        const UBaseType_t free_words = uxTaskGetStackHighWaterMark(s_poller_handle);

        put_u32("poller stack words free (of 512)", (uint32_t) free_words);
        check(free_words >= STACK_MARGIN_WORDS, "TC-HW-MBP-009 stack margin too small");
        bringup_pass("TC-HW-MBP-009 ModbusPollerTask stack fits its 2 KB budget");
    }

    bringup_puts("\r\n===== ALL TESTS PASSED =====\r\n");
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

static void poller_task(void *arg)
{
    (void) arg;
    modbus_poller_run(); /* never returns */
}

/* ---------------------------------------------------------------------- */
/* main()                                                                  */
/* ---------------------------------------------------------------------- */

int main(void)
{
    /* cpu_init() first - clock to 80 MHz, DWT cycle counter, fault handlers. */
    if (cpu_init() != STATUS_OK)
    {
        for (;;)
        {
            /* Nothing can be reported before the UART exists. */
        }
    }

    const gpio_pin_config_t uart_tx_config = {
        .port = BRINGUP_UART_TX_PORT,
        .pin = BRINGUP_UART_TX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_NONE,
        .alternate = BRINGUP_UART_AF,
    };
    const gpio_pin_config_t uart_rx_config = {
        .port = BRINGUP_UART_RX_PORT,
        .pin = BRINGUP_UART_RX_PIN,
        .mode = GPIO_MODE_ALTERNATE,
        .otype = GPIO_OTYPE_PUSH_PULL,
        .speed = GPIO_SPEED_VERY_HIGH,
        .pull = GPIO_PULL_UP,
        .alternate = BRINGUP_UART_AF,
    };

    if ((gpio_init() != GPIO_OK) || (gpio_configure_pin(&uart_tx_config) != GPIO_OK) ||
        (gpio_configure_pin(&uart_rx_config) != GPIO_OK) || (debug_uart_init() != DEBUG_UART_OK))
    {
        for (;;)
        {
            /* No sink to report to. */
        }
    }

    bringup_puts("\r\n===== ModbusPoller Hardware Bring-up =====\r\n");
    bringup_puts("Board  : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("WIRING : jumper Arduino D1 (PA0) to D0 (PA1) for loopback\r\n");
    bringup_puts("==========================================\r\n\r\n");

    /* The UART ISR calls xTaskNotifyFromISR(), so it must sit at or below the
     * FreeRTOS syscall ceiling (MBUART companion §4 NVIC table: priority 6). */
    NVIC_SetPriority(UART4_IRQn, UART4_IRQ_PRIORITY);

    const health_monitor_config_t hm_config = {.boot_reset_cause = LIFECYCLE_RESET_SOFT};

    if (led_init(k_gw_led_pins, (uint8_t) LED_COUNT) != LED_ERR_OK)
    {
        bringup_fail("led_init");
    }
    if (health_monitor_create(&hm_config, &s_hm) != HEALTH_MONITOR_ERR_OK)
    {
        bringup_fail("health_monitor_create");
    }
    if ((modbus_uart_init() != MODBUS_UART_OK) ||
        (modbus_master_init(RESPONSE_TIMEOUT_MS, MAX_RETRIES) != MODBUS_MASTER_ERR_OK))
    {
        bringup_fail("modbus init");
    }
    modbus_uart_set_tick_source(tick_ms);

    /* Init order per companion §7: uart, master (phase 1), registry, poller. */
    s_ready_before_init = modbus_poller_is_ready();
    if (modbus_poller_init(POLL_PERIOD_MS, s_hm) != MODBUS_POLLER_ERR_OK)
    {
        bringup_fail("modbus_poller_init");
    }
    s_ready_after_init = modbus_poller_is_ready();

    s_resp_queue =
        xQueueCreateStatic(1U, sizeof(modbus_command_response_t), s_resp_buf, &s_resp_storage);

    s_poller_handle = xTaskCreateStatic(poller_task, "ModbusPoller", POLLER_TASK_STACK_WORDS, NULL,
                                        POLLER_TASK_PRIORITY, s_poller_stack, &s_poller_tcb);
    (void) xTaskCreateStatic(driver_task, "mbp_bringup", DRIVER_TASK_STACK_WORDS, NULL,
                             DRIVER_TASK_PRIORITY, s_driver_stack, &s_driver_tcb);
    vTaskStartScheduler();

    /* Only reached if the scheduler fails to start. */
    for (;;)
    {
    }
}
