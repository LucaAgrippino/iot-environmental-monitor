/**
 * @file test_logger_main.c
 * @brief Logger integration test on F469 Discovery hardware.
 *
 * End-to-end exercise of the Logger Middleware with the real
 * DebugUartDriver and RtcDriver. Validates:
 *   - The pre-scheduler synchronous-write path (boot diagnostics).
 *   - The post-scheduler queue + drain-task path (periodic logs).
 *   - ANSI colour rendering on a real terminal.
 *   - Wall-clock timestamps from the RTC ticking each second.
 *   - The drop counter under high-rate logging (optional FLOODER task).
 *
 * Activation in CubeIDE:
 *   - Right-click `Src/main.c` → Resource Configurations →
 *     Exclude from Build → All Configurations.
 *   - Add `integration-tests/logger/` to the project source paths
 *     (Project Properties → C/C++ General → Paths and Symbols).
 *   - Build, flash, open PuTTY on the ST-Link VCP at 115200/8N1.
 *
 * Expected output (colours render in PuTTY; shown literally below):
 *   [ INFO][00:00:00][Boot            ] ===== Logger integration test =====
 *   [ INFO][00:00:00][Boot            ] SYSCLK=180 MHz ...
 *   [DEBUG][00:00:00][Boot            ] pre-scheduler path — dim tag
 *   [ WARN][00:00:00][Boot            ] pre-scheduler path — yellow tag
 *   [ERROR][00:00:00][Boot            ] pre-scheduler path — red tag
 *   [ INFO][00:00:00][Boot            ] starting scheduler...
 *   [ INFO][00:00:01][Periodic        ] tick 0 (drops=0)
 *   [DEBUG][00:00:03][Periodic        ] every 3 — dim
 *   ...
 *
 * Set FLOODER_ENABLED = 1 to start a high-rate background task that
 * forces the drop counter to start incrementing — useful for verifying
 * the back-pressure behaviour.
 */

#include "stm32f469xx.h"

#include "FreeRTOS.h"
#include "task.h"

#include "system_clock.h"
#include "rtc/rtc_driver.h"
#include "debug-uart/debug_uart_driver.h"
#include "logger/logger.h"

/* ===================================================================== */
/* Configuration                                                         */
/* ===================================================================== */

#define FLOODER_ENABLED         (0U)        /* set to 1 to exercise drop counter */

#define PERIODIC_STACK_WORDS    (384U)
#define PERIODIC_PRIORITY       (tskIDLE_PRIORITY + 2U)

#define FLOODER_STACK_WORDS     (256U)
#define FLOODER_PRIORITY        (tskIDLE_PRIORITY + 1U)

/* ===================================================================== */
/* Periodic task — exercises all four levels at 1 Hz                     */
/* ===================================================================== */

static StaticTask_t s_periodic_tcb;
static StackType_t  s_periodic_stack[PERIODIC_STACK_WORDS];

static void periodic_task(void *arg)
{
    (void)arg;
    uint32_t i = 0U;
    for (;;)
    {
        LOG_INFO("Periodic", "tick %lu (drops=%lu)",
                 (unsigned long)i,
                 (unsigned long)logger_get_dropped_count());

        if ((i % 5U) == 4U)  { LOG_WARN ("Periodic", "every 5 — yellow"); }
        if ((i % 7U) == 6U)  { LOG_ERROR("Periodic", "every 7 — red");    }
        if ((i % 3U) == 2U)  { LOG_DEBUG("Periodic", "every 3 — dim");    }

        i++;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

/* ===================================================================== */
/* Flooder task — optional, exercises the drop counter                   */
/* ===================================================================== */

#if FLOODER_ENABLED
static StaticTask_t s_flooder_tcb;
static StackType_t  s_flooder_stack[FLOODER_STACK_WORDS];

static void flooder_task(void *arg)
{
    (void)arg;
    uint32_t j = 0U;
    for (;;)
    {
        LOG_DEBUG("Flood", "flood msg %lu", (unsigned long)j);
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
    /* SystemInit() (from startup_stm32f469xx.s) has already run and
     * left the chip at the default HSI 16 MHz. We override below. */

    /* 1. Clock tree → 180 MHz. */
    system_clock_init();

    /* 2. Drivers Logger depends on. Order matters: DebugUart provides
     *    the output sink, Rtc provides the wall-clock timestamp. */
    (void)debug_uart_init();
    (void)rtc_init();

    /* 3. Logger. Creates the queue and drain task statically — they're
     *    valid immediately, but the drain task does not run until the
     *    scheduler starts, so log calls before vTaskStartScheduler()
     *    take the synchronous-write path. */
    (void)logger_init(LOG_LEVEL_DEBUG);

    /* 4. Pre-scheduler diagnostics — exercises the direct-write path. */
    LOG_INFO ("Boot", "===== Logger integration test =====");
    LOG_INFO ("Boot", "SYSCLK=180 MHz PCLK1=45 MHz UART=115200/8N1");
    LOG_DEBUG("Boot", "pre-scheduler path - dim tag");
    LOG_WARN ("Boot", "pre-scheduler path - yellow tag");
    LOG_ERROR("Boot", "pre-scheduler path - red tag");
    LOG_INFO ("Boot", "starting scheduler...");

    /* 5. Spawn tasks. */
    (void)xTaskCreateStatic(periodic_task, "periodic",
                            PERIODIC_STACK_WORDS, NULL,
                            PERIODIC_PRIORITY,
                            s_periodic_stack, &s_periodic_tcb);
#if FLOODER_ENABLED
    (void)xTaskCreateStatic(flooder_task, "flood",
                            FLOODER_STACK_WORDS, NULL,
                            FLOODER_PRIORITY,
                            s_flooder_stack, &s_flooder_tcb);
#endif

    /* 6. Start the scheduler. Does not return under normal operation. */
    vTaskStartScheduler();

    /* 7. Only reached if the scheduler fails to start (very rare —
     *    indicates a fatal RTOS-config error). */
    for (;;) { }
}
