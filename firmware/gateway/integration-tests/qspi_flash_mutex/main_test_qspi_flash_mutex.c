/**
 * @file main_test_qspi_flash_mutex.c
 * @brief Hardware contention test for the QspiFlashDriver mutex (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default.  To activate, in firmware/gateway/.cproject:
 *   1. Add cloud_publisher (the current harness) to the `excluding=` list of the
 *      `integration-tests` source entry and remove qspi_flash_mutex from it.
 *   2. Point the explicit `integration-tests/<module>` sourcePath entry at
 *      integration-tests/qspi_flash_mutex.
 *   Treat that swap as bench-local; do not commit it.
 *   Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *
 * Validates QSPID-O1: the driver takes its own priority-inheritance mutex on every call,
 * so independent tasks (ConfigStore, CircularFlashLog, FirmwareStore) cannot corrupt each
 * other's QUADSPI transfers. Four worker tasks at two priorities each erase, program and read
 * back their own sectors in a loop while a higher-priority reader keeps re-reading a fixed
 * sector. The tasks preempt one another in the middle of other tasks' operations (an erase
 * takes ~80 ms); without the mutex the shared QUADSPI registers would be overwritten and the
 * read-backs would fail.
 *
 * DESTRUCTIVE only inside QSPI device offsets 0x0060_0000 - 0x0060_4FFF (5 sectors), in the
 * reserved area of flash-partition-layout.md 5.2. ConfigStore, CertStore, the log and the OTA
 * staging area are never touched.
 *
 * Automated test sequence:
 *   TC-HW-QSPIM-001  Workers run to completion: every read-back matches, every erased
 *                    sector reads 0xFF.
 *   TC-HW-QSPIM-002  The reader never sees a corrupted sector while the workers erase.
 *   TC-HW-QSPIM-003  Real contention happened: at one moment several tasks were inside a
 *                    driver call (from their own point of view).
 *   TC-HW-QSPIM-004  Totals: operation counts and elapsed time are logged.
 *
 * Integration checklist:
 *   [ ] Zero data errors and zero reader errors
 *   [ ] At least 2 tasks were simultaneously in a driver call
 *   [ ] Record the elapsed time (serialised, so about the sum of the individual operations)
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "cpu/cpu.h"
#include "cpu/status.h"
#include "debug_uart/debug_uart.h"
#include "gpio/gpio_driver.h"
#include "qspi_flash/qspi_flash.h"
#include "task.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ---------------------------------------------------------------------- */
/* Board constants                                                         */
/* ---------------------------------------------------------------------- */

#define BRINGUP_UART_TX_PORT GPIO_PORT_B
#define BRINGUP_UART_TX_PIN (6U)
#define BRINGUP_UART_RX_PORT GPIO_PORT_B
#define BRINGUP_UART_RX_PIN (7U)
#define BRINGUP_UART_AF (7U)
#define BRINGUP_TX_TIMEOUT_MS (100U)

/* Scratch area: device offsets in the reserved region. Sector 0 is the reader's. */
#define SCRATCH_BASE (0x00600000UL)
#define SECTOR_BYTES (QSPI_FLASH_SECTOR_SIZE_BYTES)
#define PAGE_BYTES (QSPI_FLASH_PAGE_SIZE_BYTES)
#define PAGES_PER_ROUND (4U)

#define WORKER_COUNT (4U)
#define ROUNDS_PER_WORKER (12U)

#define WORKER_STACK_WORDS (512U)
#define WORKER_PRIO_LOW (2U)
#define WORKER_PRIO_HIGH (3U)
#define READER_PRIO (4U)
#define REPORTER_PRIO (1U)
#define REPORTER_POLL_MS (200U)

/* ---------------------------------------------------------------------- */
/* Shared state                                                            */
/* ---------------------------------------------------------------------- */

typedef struct
{
    volatile uint32_t rounds_done;
    volatile uint32_t errors;
    volatile bool finished;
} worker_state_t;

static worker_state_t s_worker[WORKER_COUNT];
static volatile uint32_t s_reader_reads;
static volatile uint32_t s_reader_errors;
static volatile bool s_stop_reader;
static volatile uint32_t s_inflight;
static volatile uint32_t s_inflight_max;
static volatile uint32_t s_calls;

static uint8_t s_reader_expect[PAGE_BYTES];

static uint32_t sector_of(uint32_t index)
{
    return SCRATCH_BASE + (index * SECTOR_BYTES);
}

/* ---------------------------------------------------------------------- */
/* Driver calls, counted: how many tasks are inside the driver at once      */
/* ---------------------------------------------------------------------- */

static void enter_call(void)
{
    const uint32_t now = __atomic_add_fetch(&s_inflight, 1U, __ATOMIC_SEQ_CST);
    uint32_t seen = s_inflight_max;

    while ((now > seen) && !__atomic_compare_exchange_n(&s_inflight_max, &seen, now, false,
                                                        __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST))
    {
    }

    (void) __atomic_add_fetch(&s_calls, 1U, __ATOMIC_SEQ_CST);
}

static void leave_call(void)
{
    (void) __atomic_sub_fetch(&s_inflight, 1U, __ATOMIC_SEQ_CST);
}

static qspi_flash_err_t do_erase(uint32_t addr)
{
    enter_call();
    const qspi_flash_err_t e = qspi_flash_driver->erase_sector(addr);
    leave_call();

    return e;
}

static qspi_flash_err_t do_program(uint32_t addr, const uint8_t *data)
{
    enter_call();
    const qspi_flash_err_t e = qspi_flash_driver->write_page(addr, data, (uint16_t) PAGE_BYTES);
    leave_call();

    return e;
}

static qspi_flash_err_t do_read(uint32_t addr, uint8_t *buf)
{
    enter_call();
    const qspi_flash_err_t e = qspi_flash_driver->read(addr, buf, PAGE_BYTES);
    leave_call();

    return e;
}

/* ---------------------------------------------------------------------- */
/* Tasks                                                                   */
/* ---------------------------------------------------------------------- */

static uint8_t pattern_byte(uint32_t worker, uint32_t round, uint32_t page, uint32_t i)
{
    return (uint8_t) ((worker * 37U) + (round * 11U) + (page * 5U) + i);
}

static void worker_task(void *arg)
{
    const uint32_t w = (uint32_t) (uintptr_t) arg;
    const uint32_t base = sector_of(w + 1U);
    uint8_t wr[PAGE_BYTES];
    uint8_t rd[PAGE_BYTES];

    for (uint32_t round = 0U; round < ROUNDS_PER_WORKER; round++)
    {
        bool ok = (do_erase(base) == QSPI_FLASH_OK);

        /* A freshly erased sector must read 0xFF: nobody else's data leaked in. */
        ok = ok && (do_read(base, rd) == QSPI_FLASH_OK);
        for (uint32_t i = 0U; ok && (i < PAGE_BYTES); i++)
        {
            ok = (rd[i] == 0xFFU);
        }

        for (uint32_t page = 0U; ok && (page < PAGES_PER_ROUND); page++)
        {
            for (uint32_t i = 0U; i < PAGE_BYTES; i++)
            {
                wr[i] = pattern_byte(w, round, page, i);
            }

            ok = (do_program(base + (page * PAGE_BYTES), wr) == QSPI_FLASH_OK);
        }

        for (uint32_t page = 0U; ok && (page < PAGES_PER_ROUND); page++)
        {
            ok = (do_read(base + (page * PAGE_BYTES), rd) == QSPI_FLASH_OK);
            for (uint32_t i = 0U; ok && (i < PAGE_BYTES); i++)
            {
                ok = (rd[i] == pattern_byte(w, round, page, i));
            }
        }

        if (!ok)
        {
            s_worker[w].errors++;
        }

        s_worker[w].rounds_done++;
    }

    s_worker[w].finished = true;
    vTaskDelete(NULL);
}

static void reader_task(void *arg)
{
    (void) arg;
    uint8_t rd[PAGE_BYTES];

    while (!s_stop_reader)
    {
        const bool ok = (do_read(sector_of(0U), rd) == QSPI_FLASH_OK) &&
                        (memcmp(rd, s_reader_expect, PAGE_BYTES) == 0);

        if (!ok)
        {
            s_reader_errors++;
        }

        s_reader_reads++;
        vTaskDelay(pdMS_TO_TICKS(1U));
    }

    vTaskDelete(NULL);
}

/* ---------------------------------------------------------------------- */
/* Reporting                                                               */
/* ---------------------------------------------------------------------- */

static void bringup_puts(const char *s)
{
    (void) debug_uart_send((const uint8_t *) s, strlen(s), BRINGUP_TX_TIMEOUT_MS);
}

static void bringup_line(const char *tag, const char *fmt, uint32_t a, uint32_t b)
{
    char msg[112];
    char full[128];

    (void) snprintf(msg, sizeof(msg), fmt, (unsigned long) a, (unsigned long) b);
    (void) snprintf(full, sizeof(full), "%s %s\r\n", tag, msg);
    bringup_puts(full);
}

static bool all_finished(void)
{
    for (uint32_t w = 0U; w < WORKER_COUNT; w++)
    {
        if (!s_worker[w].finished)
        {
            return false;
        }
    }

    return true;
}

static StaticTask_t s_worker_tcb[WORKER_COUNT];
static StackType_t s_worker_stack[WORKER_COUNT][WORKER_STACK_WORDS] __attribute__((aligned(8)));
static StaticTask_t s_reader_tcb;
static StackType_t s_reader_stack[WORKER_STACK_WORDS] __attribute__((aligned(8)));
static StaticTask_t s_reporter_tcb;
static StackType_t s_reporter_stack[WORKER_STACK_WORDS] __attribute__((aligned(8)));

static void reporter_task(void *arg)
{
    (void) arg;
    while (!all_finished())
    {
        vTaskDelay(pdMS_TO_TICKS(REPORTER_POLL_MS));
    }

    /* The tick counter starts at zero with the scheduler, and this task only gets to run
     * after the workers are done, so its own start time would read as zero. */
    const uint32_t elapsed_ms = (uint32_t) xTaskGetTickCount();

    s_stop_reader = true;
    vTaskDelay(pdMS_TO_TICKS(10U));

    uint32_t data_errors = 0U;
    uint32_t rounds = 0U;

    for (uint32_t w = 0U; w < WORKER_COUNT; w++)
    {
        data_errors += s_worker[w].errors;
        rounds += s_worker[w].rounds_done;
    }

    bringup_line("[INFO]", "workers: %lu rounds done, %lu data errors", rounds, data_errors);
    bringup_line("[INFO]", "reader: %lu reads, %lu errors", s_reader_reads, s_reader_errors);
    bringup_line("[INFO]", "driver calls: %lu, tasks inside the driver at once (max): %lu", s_calls,
                 s_inflight_max);
    bringup_line("[INFO]", "elapsed: %lu ms (%lu rounds)", elapsed_ms, rounds);

    if (data_errors != 0U)
    {
        bringup_puts("[FAIL] TC-HW-QSPIM-001 a worker read back wrong data\r\n");
    }
    else
    {
        bringup_puts(
            "[PASS] TC-HW-QSPIM-001 every read-back matched, every erased sector read 0xFF\r\n");
    }

    if (s_reader_errors != 0U)
    {
        bringup_puts("[FAIL] TC-HW-QSPIM-002 the reader saw a corrupted sector\r\n");
    }
    else
    {
        bringup_puts("[PASS] TC-HW-QSPIM-002 the reader never saw a corrupted sector\r\n");
    }

    if (s_inflight_max < 2U)
    {
        bringup_puts(
            "[FAIL] TC-HW-QSPIM-003 the tasks never overlapped: no contention was tested\r\n");
    }
    else
    {
        bringup_puts("[PASS] TC-HW-QSPIM-003 several tasks were inside the driver at once\r\n");
    }

    bringup_puts((data_errors == 0U) && (s_reader_errors == 0U) && (s_inflight_max >= 2U)
                     ? "\r\n===== ALL TESTS PASSED =====\r\n"
                     : "\r\n===== FAILED =====\r\n");

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/* ---------------------------------------------------------------------- */
/* main                                                                    */
/* ---------------------------------------------------------------------- */

static void halt(const char *why)
{
    bringup_puts("[FAIL] ");
    bringup_puts(why);
    bringup_puts(" - HALTED\r\n");

    for (;;)
    {
    }
}

int main(void)
{
    if (cpu_init() != STATUS_OK)
    {
        for (;;)
        {
            /* Nothing can be reported before the UART exists. */
        }
    }

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

    if ((gpio_init() != GPIO_OK) || (gpio_configure_pin(&uart_tx_config) != GPIO_OK) ||
        (gpio_configure_pin(&uart_rx_config) != GPIO_OK) || (debug_uart_init() != DEBUG_UART_OK))
    {
        for (;;)
        {
            /* No sink to report to. */
        }
    }

    bringup_puts("\r\n===== QspiFlashDriver Mutex Contention Test =====\r\n");
    bringup_puts("Board  : B-L475E-IOT01A  (STM32L475VGTx)\r\n");
    bringup_puts("Region : QSPI device offsets 0x00600000..0x00604FFF (5 sectors)\r\n");
    bringup_puts("WARNING: erases and programs those sectors only\r\n");
    bringup_puts("=================================================\r\n\r\n");

    /* The driver creates its mutex here, before the scheduler starts. */
    if (qspi_flash_init() != QSPI_FLASH_OK)
    {
        halt("qspi_flash_init");
    }

    /* The reader's sector: erased and programmed once, then only read. */
    for (uint32_t i = 0U; i < PAGE_BYTES; i++)
    {
        s_reader_expect[i] = (uint8_t) ((i * 7U) ^ 0xA5U);
    }

    if ((qspi_flash_driver->erase_sector(sector_of(0U)) != QSPI_FLASH_OK) ||
        (qspi_flash_driver->write_page(sector_of(0U), s_reader_expect, (uint16_t) PAGE_BYTES) !=
         QSPI_FLASH_OK))
    {
        halt("could not prepare the reader sector");
    }

    for (uint32_t w = 0U; w < WORKER_COUNT; w++)
    {
        const UBaseType_t prio = ((w % 2U) == 0U) ? WORKER_PRIO_LOW : WORKER_PRIO_HIGH;

        (void) xTaskCreateStatic(worker_task, "qm_work", WORKER_STACK_WORDS, (void *) (uintptr_t) w,
                                 prio, s_worker_stack[w], &s_worker_tcb[w]);
    }

    (void) xTaskCreateStatic(reader_task, "qm_read", WORKER_STACK_WORDS, NULL, READER_PRIO,
                             s_reader_stack, &s_reader_tcb);
    (void) xTaskCreateStatic(reporter_task, "qm_rep", WORKER_STACK_WORDS, NULL, REPORTER_PRIO,
                             s_reporter_stack, &s_reporter_tcb);

    vTaskStartScheduler();

    /* Only reached if the scheduler fails to start. */
    halt("scheduler did not start");

    return 0;
}
