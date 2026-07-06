/**
 * @file main_test_logger.c
 * @brief Hardware bring-up test for Logger (B-L475E-IOT01A, Gateway).
 *
 * NOT compiled by default. To activate:
 *   1. Copy this file into Src/ (or add integration-tests/logger/ as a
 *      source path in CubeIDE).
 *   2. In CubeIDE, right-click Src/main.c -> Properties ->
 *      C/C++ Build -> check "Exclude resource from build".
 *   3. Connect a serial terminal to PB6 (TX) at 115 200 8N1.
 *   4. Flash and run.
 *
 * Hardware outputs:
 *   USART1 TX  PB6  115 200 8N1  Human-readable log lines
 *   LD2 green  PA5                Lit solid once init succeeds
 *
 * End-to-end exercise of the Logger Middleware with the real
 * DebugUartDriver and RtcDriver (companion: docs/lld/middleware/logger.md).
 * Validates:
 *   - The pre-scheduler synchronous-write path (boot diagnostics).
 *   - The post-scheduler queue + drain-task path (periodic logs).
 *   - ANSI colour rendering on a real terminal.
 *   - Wall-clock timestamps from the RTC ticking each second.
 *   - The drop counter under high-rate logging (optional FLOODER task).
 *
 * Expected output (colours render in PuTTY; shown literally below):
 *   [ INFO][00:00:00][Boot            ] ===== Logger integration test (GW) =====
 *   [ INFO][00:00:00][Boot            ] SYSCLK=80 MHz UART=115200/8N1
 *   [DEBUG][00:00:00][Boot            ] pre-scheduler path — dim tag
 *   [ WARN][00:00:00][Boot            ] pre-scheduler path — yellow tag
 *   [ERROR][00:00:00][Boot            ] pre-scheduler path — red tag
 *   [ INFO][00:00:00][Boot            ] starting scheduler...
 *   [ INFO][00:00:01][Periodic        ] tick 0 (drops=0)
 *   [DEBUG][00:00:03][Periodic        ] every 3 — dim
 *   ...
 *
 * Set FLOODER_ENABLED to 1 to start a high-rate background task that
 * forces the drop counter to start incrementing — useful for verifying
 * the back-pressure behaviour.
 *
 * If cpu_init(), debug_uart_init(), or rtc_init() fail, Logger cannot be
 * trusted as the reporting channel yet — the board halts with a fast LED
 * blink instead of attempting to log the failure (same convention as the
 * other Gateway bring-up tests, e.g. integration-tests/rtc/main_test_rtc.c).
 */

#include <stdint.h>

#include "FreeRTOS.h"
#include "task.h"

#include "cpu/cpu.h"
#include "gpio/gpio_driver.h"
#include "debug_uart/debug_uart.h"
#include "rtc/rtc.h"
#include "logger/logger.h"
#include "cpu/status.h"

#ifdef STM32L475xx
#include "stm32l475xx.h"
#endif

/* ===================================================================== */
/* Configuration                                                         */
/* ===================================================================== */

#define FLOODER_ENABLED (0U) /* set to 1 to exercise drop counter */

#define PERIODIC_STACK_WORDS (384U)
#define PERIODIC_PRIORITY (tskIDLE_PRIORITY + 2U)

#define FLOODER_STACK_WORDS (256U)
#define FLOODER_PRIORITY (tskIDLE_PRIORITY + 1U)

#define BRINGUP_LED_PIN (5U)

/* ===================================================================== */
/* Boot-failure halt — Logger is not yet trusted, so this bypasses it     */
/* ===================================================================== */

static void bringup_raw_spin(uint32_t n)
{
    volatile uint32_t c = n;
    while (c > 0U)
    {
        --c;
    }
}

static void bringup_halt(void)
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

/* ===================================================================== */
/* Periodic task — exercises all four levels at 1 Hz                     */
/* ===================================================================== */

static StaticTask_t s_periodic_tcb;
static StackType_t s_periodic_stack[PERIODIC_STACK_WORDS];

static void periodic_task(void *arg)
{
    (void) arg;
    uint32_t i = 0U;
    for (;;)
    {
        LOG_INFO("Periodic", "tick %lu (drops=%lu)", (unsigned long) i,
                 (unsigned long) logger_get_dropped_count());

        if ((i % 5U) == 4U)
        {
            LOG_WARN("Periodic", "every 5 — yellow");
        }
        if ((i % 7U) == 6U)
        {
            LOG_ERROR("Periodic", "every 7 — red");
        }
        if ((i % 3U) == 2U)
        {
            LOG_DEBUG("Periodic", "every 3 — dim");
        }

        i++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ===================================================================== */
/* Flooder task — optional, exercises the drop counter                   */
/* ===================================================================== */

#if FLOODER_ENABLED
static StaticTask_t s_flooder_tcb;
static StackType_t s_flooder_stack[FLOODER_STACK_WORDS];

static void flooder_task(void *arg)
{
    (void) arg;
    uint32_t j = 0U;
    for (;;)
    {
        LOG_DEBUG("Flood", "flood msg %lu", (unsigned long) j);
        j++;
        /* No delay — produce as fast as possible to exercise queue full. */
    }
}
#endif

/* ===================================================================== */
/* Entry point                                                           */
/* ===================================================================== */

int main(void)
{
    /* 1. Clock tree -> 80 MHz, DWT, fault handlers, LSE for the RTC. */
    if (cpu_init() != STATUS_OK)
    {
        bringup_halt();
    }

    /* 2. Drivers Logger depends on. Order matters: DebugUart provides
     *    the output sink, Rtc provides the wall-clock timestamp. */
    if (debug_uart_init() != DEBUG_UART_OK)
    {
        bringup_halt();
    }
    if (rtc_init() != RTC_OK)
    {
        bringup_halt();
    }

    /* 3. Logger. Creates the queue and drain task statically — they're
     *    valid immediately, but the drain task does not run until the
     *    scheduler starts, so log calls before vTaskStartScheduler()
     *    take the synchronous-write path. */
    (void) logger_init(LOG_LEVEL_DEBUG);

    /* 4. Pre-scheduler diagnostics — exercises the direct-write path. */
    LOG_INFO("Boot", "===== Logger integration test (GW) =====");
    LOG_INFO("Boot", "SYSCLK=%lu MHz UART=115200/8N1", (unsigned long) (cpu_get_sysclk_hz() / 1000000U));
    LOG_DEBUG("Boot", "pre-scheduler path - dim tag");
    LOG_WARN("Boot", "pre-scheduler path - yellow tag");
    LOG_ERROR("Boot", "pre-scheduler path - red tag");
    LOG_INFO("Boot", "starting scheduler...");

    /* 5. Spawn tasks. */
    (void) xTaskCreateStatic(periodic_task, "periodic", PERIODIC_STACK_WORDS, NULL,
                             PERIODIC_PRIORITY, s_periodic_stack, &s_periodic_tcb);
#if FLOODER_ENABLED
    (void) xTaskCreateStatic(flooder_task, "flood", FLOODER_STACK_WORDS, NULL, FLOODER_PRIORITY,
                             s_flooder_stack, &s_flooder_tcb);
#endif

    /* 6. Start the scheduler. Does not return under normal operation. */
    vTaskStartScheduler();

    /* 7. Only reached if the scheduler fails to start (very rare —
     *    indicates a fatal RTOS-config error). */
    for (;;)
    {
    }
}
