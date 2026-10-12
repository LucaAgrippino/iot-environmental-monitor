/**
 * @file main_test_modbus_master.c
 * @brief Hardware bring-up test for ModbusMaster (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. Add the current harness (cloud_publisher) to the `excluding=` list of
 *      the `integration-tests` source entry and remove modbus_master.
 *   2. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/modbus_master.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * WIRING.  Jumper Arduino D1 (PA0, UART4_TX) to D0 (PA1, UART4_RX).  The master
 * then hears its own request.  A request is a well-formed frame (address,
 * function code, data, CRC), so it passes as a "response" whose payload equals
 * the request payload.  That drives the whole path on silicon: TX, IDLE-line RX,
 * the ISR callback, the task notification, the CRC and the validation.
 *
 * Hardware outputs:
 *   USART1 TX  PB6      115 200 8N1  Human-readable test results
 *   UART4      PA0/PA1  9600 8N1     Modbus link (looped back)
 *
 * Runs inside a FreeRTOS task: xTaskNotifyWait() needs the scheduler.
 *
 * Automated test sequence:
 *   TC-HW-MM-001  init / start; UART4 IRQ priority set below the syscall ceiling.
 *   TC-HW-MM-002  FC04 read request loops back: OK, echoed payload identical.
 *   TC-HW-MM-003  FC16 request with 100-byte payload loops back.
 *   TC-HW-MM-004  Maximum payload (252 bytes) loops back.
 *   TC-HW-MM-005  20 consecutive transactions all succeed; no stale-frame mix-ups.
 *   TC-HW-MM-006  Counters match the transactions made; reset_stats() zeroes them.
 *   TC-HW-MM-007  Receiver disabled: TIMEOUT after 4 attempts, ~4 x 200 ms.
 *   TC-HW-MM-008  Receiver re-enabled: the next transaction succeeds (recovery).
 *   TC-HW-MM-009  Invalid arguments are rejected without touching the bus.
 *
 * Integration checklist:
 *   [ ] Jumper D1-D0 fitted; every loopback payload read back byte-identical
 *   [ ] TC-HW-MM-007 elapsed time close to 4 x 200 ms plus 4 x TX time
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
#include "modbus_master/modbus_master.h"
#include "modbus_uart_driver/modbus_uart_driver.h"
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

#define MAIN_TASK_STACK_WORDS (1024U)
#define MAIN_TASK_PRIORITY (4U)

#define UART4_IRQ_PRIORITY (6U) /* >= configMAX_SYSCALL_INTERRUPT_PRIORITY (5) */
#define RESPONSE_TIMEOUT_MS (200U)
#define MAX_RETRIES (3U)
#define SLAVE_ADDR (1U)
#define FC_READ_INPUT (4U)
#define FC_WRITE_MULTIPLE (16U)
#define BULK_TRANSACTIONS (20U)
#define FC16_PAYLOAD_BYTES (100U)
#define TIMEOUT_MARGIN_MS (150U)

static StaticTask_t s_task_tcb;
static StackType_t s_task_stack[MAIN_TASK_STACK_WORDS];

/* ---------------------------------------------------------------------- */
/* Reporting helpers — built on DebugUartDriver.                           */
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

static modbus_frame_t make_request(uint8_t fc, uint16_t len, uint8_t seed)
{
    modbus_frame_t req = {.slave_addr = SLAVE_ADDR, .function_code = fc, .data_len = len};

    for (uint16_t i = 0U; i < len; i++)
    {
        req.data[i] = (uint8_t) (seed + i);
    }
    return req;
}

static void expect_loopback(uint8_t fc, uint16_t len, uint8_t seed, const char *label)
{
    const modbus_frame_t req = make_request(fc, len, seed);
    modbus_frame_t rsp;

    memset(&rsp, 0, sizeof(rsp));
    check(modbus_master_transact(&req, &rsp) == MODBUS_MASTER_ERR_OK,
          label); /* TIMEOUT here usually means the D1-D0 jumper is missing */
    check(rsp.slave_addr == SLAVE_ADDR, "response address differs");
    check(rsp.function_code == fc, "response function code differs");
    check(rsp.data_len == len, "response length differs");
    check(memcmp(rsp.data, req.data, len) == 0, "response payload differs");
}

/* ---------------------------------------------------------------------- */
/* Bring-up task                                                           */
/* ---------------------------------------------------------------------- */

static void bringup_task(void *arg)
{
    modbus_master_stats_t stats;

    (void) arg;

    /* TC-HW-MM-001 */
    check(modbus_master_start() == MODBUS_MASTER_ERR_OK, "TC-HW-MM-001 modbus_master_start");
    check(modbus_master_start() == MODBUS_MASTER_ERR_ALREADY_STARTED,
          "TC-HW-MM-001 second start not rejected");
    bringup_pass("TC-HW-MM-001 init/start; UART4 IRQ priority 6");

    /* TC-HW-MM-002 */
    expect_loopback(FC_READ_INPUT, 4U, 0x10U,
                    "TC-HW-MM-002 transact - is the D1-D0 jumper fitted?");
    bringup_pass("TC-HW-MM-002 FC04 request loops back, payload identical");

    /* TC-HW-MM-003 */
    expect_loopback(FC_WRITE_MULTIPLE, FC16_PAYLOAD_BYTES, 0x20U, "TC-HW-MM-003 transact");
    bringup_pass("TC-HW-MM-003 100-byte payload loops back");

    /* TC-HW-MM-004 */
    expect_loopback(FC_WRITE_MULTIPLE, MODBUS_MAX_DATA_BYTES, 0x30U, "TC-HW-MM-004 transact");
    bringup_pass("TC-HW-MM-004 252-byte payload (256-byte ADU) loops back");

    /* TC-HW-MM-005 */
    for (uint8_t i = 0U; i < BULK_TRANSACTIONS; i++)
    {
        expect_loopback(FC_READ_INPUT, (uint16_t) (4U + i), (uint8_t) (0x40U + i),
                        "TC-HW-MM-005 transact");
    }
    bringup_pass("TC-HW-MM-005 20 consecutive transactions");

    /* TC-HW-MM-006 */
    check(modbus_master_get_stats(&stats) == MODBUS_MASTER_ERR_OK, "TC-HW-MM-006 get_stats");
    check(stats.transactions_ok == (BULK_TRANSACTIONS + 3U), "TC-HW-MM-006 transactions_ok");
    check(stats.transactions_sent == stats.transactions_ok, "TC-HW-MM-006 retries on a clean link");
    check((stats.timeouts == 0U) && (stats.crc_errors == 0U) && (stats.rx_errors == 0U) &&
              (stats.bad_responses == 0U) && (stats.exception_responses == 0U),
          "TC-HW-MM-006 unexpected error counters");
    check(modbus_master_reset_stats() == MODBUS_MASTER_ERR_OK, "TC-HW-MM-006 reset_stats");
    check((modbus_master_get_stats(&stats) == MODBUS_MASTER_ERR_OK) &&
              (stats.transactions_ok == 0U) && (stats.transactions_sent == 0U),
          "TC-HW-MM-006 counters not zeroed");
    bringup_pass("TC-HW-MM-006 counters match; reset_stats() zeroes them");

    /* TC-HW-MM-007: no receiver -> nothing comes back */
    {
        const modbus_frame_t req = make_request(FC_READ_INPUT, 4U, 0x50U);
        modbus_frame_t rsp;
        const uint32_t start = tick_ms();

        UART4->CR1 &= ~USART_CR1_RE;
        check(modbus_master_transact(&req, &rsp) == MODBUS_MASTER_ERR_TIMEOUT,
              "TC-HW-MM-007 expected TIMEOUT");
        const uint32_t elapsed = tick_ms() - start;
        const uint32_t expect = (MAX_RETRIES + 1U) * RESPONSE_TIMEOUT_MS;

        put_u32("timeout transaction ms", elapsed);
        check((elapsed >= expect) && (elapsed < (expect + TIMEOUT_MARGIN_MS)),
              "TC-HW-MM-007 elapsed time outside 4 x 200 ms window");
        check((modbus_master_get_stats(&stats) == MODBUS_MASTER_ERR_OK) &&
                  (stats.timeouts == (MAX_RETRIES + 1U)) &&
                  (stats.transactions_sent == (MAX_RETRIES + 1U)),
              "TC-HW-MM-007 timeout counters");
        bringup_pass("TC-HW-MM-007 no receiver: TIMEOUT after 4 attempts, ~800 ms");
    }

    /* TC-HW-MM-008 */
    UART4->CR1 |= USART_CR1_RE;
    expect_loopback(FC_READ_INPUT, 4U, 0x60U, "TC-HW-MM-008 transact after recovery");
    bringup_pass("TC-HW-MM-008 receiver back: next transaction succeeds");

    /* TC-HW-MM-009 */
    {
        modbus_frame_t rsp;
        modbus_frame_t req = make_request(FC_READ_INPUT, 4U, 0x70U);

        check(modbus_master_transact(NULL, &rsp) == MODBUS_MASTER_ERR_NULL_ARG,
              "TC-HW-MM-009 NULL request");
        check(modbus_master_transact(&req, NULL) == MODBUS_MASTER_ERR_NULL_ARG,
              "TC-HW-MM-009 NULL response");
        req.data_len = MODBUS_MAX_DATA_BYTES + 1U;
        check(modbus_master_transact(&req, &rsp) == MODBUS_MASTER_ERR_INVALID_ARG,
              "TC-HW-MM-009 oversize payload");
        bringup_pass("TC-HW-MM-009 invalid arguments rejected");
    }

    bringup_puts("\r\n===== ALL TESTS PASSED =====\r\n");
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
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

    bringup_puts("\r\n===== ModbusMaster Hardware Bring-up =====\r\n");
    bringup_puts("Board  : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("WIRING : jumper Arduino D1 (PA0) to D0 (PA1) for loopback\r\n");
    bringup_puts("==========================================\r\n\r\n");

    /* The UART ISR calls xTaskNotifyFromISR(), so it must sit at or below the
     * FreeRTOS syscall ceiling (companion MBUART §4 NVIC table: priority 6). */
    NVIC_SetPriority(UART4_IRQn, UART4_IRQ_PRIORITY);

    if ((modbus_uart_init() != MODBUS_UART_OK) ||
        (modbus_master_init(RESPONSE_TIMEOUT_MS, MAX_RETRIES) != MODBUS_MASTER_ERR_OK))
    {
        bringup_fail("init failed");
    }
    modbus_uart_set_tick_source(tick_ms);

    (void) xTaskCreateStatic(bringup_task, "mm_bringup", MAIN_TASK_STACK_WORDS, NULL,
                             MAIN_TASK_PRIORITY, s_task_stack, &s_task_tcb);
    vTaskStartScheduler();

    /* Only reached if the scheduler fails to start. */
    for (;;)
    {
    }
}
