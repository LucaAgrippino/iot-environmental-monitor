/**
 * @file test_sdram_driver_main.c
 * @brief Integration test for SdramDriver on STM32F469I-DISCO hardware.
 *
 * Exercises TC-SDRAM-005 (march test), TC-SDRAM-006 (aged-data), and
 * TC-SDRAM-007 (random spot-check) on the full 16 MB external SDRAM at
 * 0xC000_0000.  sdram_init() is called before the scheduler; all three
 * hardware tests run inside a FreeRTOS task after the scheduler starts.
 *
 * Activation in CubeIDE:
 *   1. Exclude Src/main.c from build (Resource Config -> Exclude from Build).
 *   2. Add integration-tests/sdram_driver/ to project source paths.
 *   3. Build, flash, open PuTTY on ST-Link VCP at 115200 / 8N1 for log output.
 *
 * Init ordering (sdram-driver.md §3.6):
 *   system_clock_init() -> debug_uart_init() -> rtc_init() -> logger_init()
 *   -> sdram_init()          [pre-scheduler — must precede vTaskStartScheduler]
 *   -> xTaskCreateStatic()   [create test task]
 *   -> vTaskStartScheduler()
 *   -> sdram_test_task()     [runs TC-SDRAM-005..007]
 *
 * ---
 * Visual checklist — tick each item before declaring the build good:
 *
 *   Pre-scheduler:
 *   [ ] VCP: "[SDRAM] ===== SdramDriver integration test ====="
 *   [ ] VCP: "[SDRAM] sdram_init OK — base=0xC0000000"
 *   [ ] VCP: "[SDRAM] starting scheduler..."
 *
 *   TC-SDRAM-005 — March test (writes and reads all 16 MB twice):
 *   [ ] VCP: "[SDRAM] TC-005 march test starting (16 MB x2 patterns)..."
 *   [ ] VCP: "[SDRAM] TC-005 PASS — 0 errors"
 *   (Failure: "[SDRAM] TC-005 FAIL — N errors" — indicates FMC timing fault)
 *
 *   TC-SDRAM-006 — Aged-data / auto-refresh test:
 *   [ ] VCP: "[SDRAM] TC-006 aged-data test starting (1 MB, 200 ms sleep)..."
 *   [ ] VCP: "[SDRAM] TC-006 PASS — 0 errors"
 *   (Failure: data corrupt after sleep → SDRTR not programmed)
 *
 *   TC-SDRAM-007 — Random spot-check (1 024 addresses):
 *   [ ] VCP: "[SDRAM] TC-007 spot-check starting (1024 random addresses)..."
 *   [ ] VCP: "[SDRAM] TC-007 PASS — 0 errors"
 *
 *   Final:
 *   [ ] VCP: "[SDRAM] all integration tests COMPLETE"
 *   [ ] No VCP freeze or watchdog reset after the final message
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
#include "sdram_driver/sdram_driver.h"

/* ===================================================================== */
/* Configuration                                                         */
/* ===================================================================== */

#define LOG_MODULE                  ("SDRAM")

#define SDRAM_TEST_TASK_STACK_WORDS (1024U)
#define SDRAM_TEST_TASK_PRIORITY    (tskIDLE_PRIORITY + 1U)

#define SDRAM_SIZE_BYTES            (16UL * 1024UL * 1024UL) /* 16 MB    */
#define SDRAM_SIZE_WORDS            (SDRAM_SIZE_BYTES / 4UL) /* 4 M words */

/* Aged-data test region: first 1 MB */
#define AGED_DATA_SIZE_WORDS        (1UL * 1024UL * 1024UL / 4UL)
#define AGED_DATA_SLEEP_MS          (200U)

/* Spot-check test: 1 024 pseudo-random addresses */
#define SPOT_CHECK_COUNT            (1024U)

/* LCG parameters for deterministic pseudo-random address generation */
#define LCG_A                       (1664525UL)
#define LCG_C                       (1013904223UL)
#define LCG_SEED                    (0xDEADBEEFUL)

/* ===================================================================== */
/* Helpers                                                               */
/* ===================================================================== */

static uint32_t lcg_next(uint32_t *state)
{
    *state = (uint32_t)(*state * LCG_A) + LCG_C;
    return *state;
}

/* ===================================================================== */
/* TC-SDRAM-005: March test                                              */
/* ===================================================================== */

static uint32_t run_march_test(volatile uint32_t *base, uint32_t words)
{
    uint32_t errors = 0U;

    /* Pass A: write i ^ 0xA5A5A5A5 to every word. */
    for (uint32_t i = 0U; i < words; i++)
    {
        base[i] = i ^ 0xA5A5A5A5UL;
    }

    /* Pass A: read back and verify. */
    for (uint32_t i = 0U; i < words; i++)
    {
        if (base[i] != (i ^ 0xA5A5A5A5UL))
        {
            errors++;
        }
    }

    /* Pass B: write i ^ 0x5A5A5A5A. */
    for (uint32_t i = 0U; i < words; i++)
    {
        base[i] = i ^ 0x5A5A5A5AUL;
    }

    /* Pass B: read back and verify. */
    for (uint32_t i = 0U; i < words; i++)
    {
        if (base[i] != (i ^ 0x5A5A5A5AUL))
        {
            errors++;
        }
    }

    return errors;
}

/* ===================================================================== */
/* TC-SDRAM-006: Aged-data / auto-refresh test                          */
/* ===================================================================== */

static uint32_t run_aged_data_test(volatile uint32_t *base, uint32_t words)
{
    uint32_t errors = 0U;

    /* Write fixed pattern across the first 1 MB. */
    for (uint32_t i = 0U; i < words; i++)
    {
        base[i] = 0xCAFEBABEUL ^ i;
    }

    /* Sleep 200 ms — SDRAM must retain data via auto-refresh. */
    vTaskDelay(pdMS_TO_TICKS(AGED_DATA_SLEEP_MS));

    /* Read back and verify. */
    for (uint32_t i = 0U; i < words; i++)
    {
        if (base[i] != (0xCAFEBABEUL ^ i))
        {
            errors++;
        }
    }

    return errors;
}

/* ===================================================================== */
/* TC-SDRAM-007: Random spot-check                                      */
/* ===================================================================== */

static uint32_t run_spot_check(volatile uint32_t *base, uint32_t words)
{
    uint32_t errors = 0U;
    uint32_t lcg    = LCG_SEED;
    static uint32_t addrs[SPOT_CHECK_COUNT];
    static uint32_t patterns[SPOT_CHECK_COUNT];

    /* Generate random addresses and patterns, then write. */
    for (uint32_t i = 0U; i < SPOT_CHECK_COUNT; i++)
    {
    	uint32_t addr = lcg_next(&lcg) % words;
        addrs[i]    = addr;
        uint32_t patt = lcg_next(&lcg);
        patterns[i] = patt;
        base[addrs[i]] = patterns[i];
    }

    /* Read back in a separate pass (different cache/prefetch state). */
    for (uint32_t i = 0U; i < SPOT_CHECK_COUNT; i++)
    {
        if (base[addrs[i]] != patterns[i])
        {
            errors++;
        }
    }

    return errors;
}

/* ===================================================================== */
/* Test task                                                             */
/* ===================================================================== */

static void sdram_test_task(void *arg)
{
    (void) arg;

    volatile uint32_t *const sdram = (volatile uint32_t *) sdram_get_base_addr();
    uint32_t errors;

    /* TC-SDRAM-005 */
    LOG_INFO(LOG_MODULE, "TC-005 march test starting (16 MB x2 patterns)...");
    errors = run_march_test(sdram, SDRAM_SIZE_WORDS);
    if (errors == 0U)
    {
        LOG_INFO(LOG_MODULE, "TC-005 PASS -- 0 errors");
    }
    else
    {
        LOG_ERROR(LOG_MODULE, "TC-005 FAIL -- %lu errors", (unsigned long) errors);
    }

    /* TC-SDRAM-006 */
    LOG_INFO(LOG_MODULE, "TC-006 aged-data test starting (1 MB, 200 ms sleep)...");
    errors = run_aged_data_test(sdram, AGED_DATA_SIZE_WORDS);
    if (errors == 0U)
    {
        LOG_INFO(LOG_MODULE, "TC-006 PASS -- 0 errors");
    }
    else
    {
        LOG_ERROR(LOG_MODULE, "TC-006 FAIL -- %lu errors", (unsigned long) errors);
    }

    /* TC-SDRAM-007 */
    LOG_INFO(LOG_MODULE, "TC-007 spot-check starting (1024 random addresses)...");
    errors = run_spot_check(sdram, SDRAM_SIZE_WORDS);
    if (errors == 0U)
    {
        LOG_INFO(LOG_MODULE, "TC-007 PASS -- 0 errors");
    }
    else
    {
        LOG_ERROR(LOG_MODULE, "TC-007 FAIL -- %lu errors", (unsigned long) errors);
    }

    LOG_INFO(LOG_MODULE, "all integration tests COMPLETE");

    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000U));
    }
}

/* ===================================================================== */
/* Task static storage                                                   */
/* ===================================================================== */

static StaticTask_t s_sdram_test_tcb;
static StackType_t  s_sdram_test_stack[SDRAM_TEST_TASK_STACK_WORDS] __attribute__((aligned(8)));

/* ===================================================================== */
/* Entry point                                                           */
/* ===================================================================== */

int main(void)
{
    system_clock_init();

    (void) debug_uart_init();
    (void) rtc_init();
    (void) logger_init(LOG_LEVEL_DEBUG);

    LOG_INFO(LOG_MODULE, "===== SdramDriver integration test =====");

    /* sdram_init() must run before the scheduler starts. */
    sdram_err_t sdram_err = sdram_init();
    if (sdram_err != SDRAM_ERR_OK)
    {
        LOG_ERROR(LOG_MODULE, "sdram_init FAILED err=%u -- halting",
                  (unsigned) sdram_err);
        for (;;)
        {
        }
    }

    LOG_INFO(LOG_MODULE, "sdram_init OK -- base=0x%08lX",
             (unsigned long) sdram_get_base_addr());

    (void) xTaskCreateStatic(sdram_test_task,
                             "SdramTest",
                             SDRAM_TEST_TASK_STACK_WORDS,
                             NULL,
                             SDRAM_TEST_TASK_PRIORITY,
                             s_sdram_test_stack,
                             &s_sdram_test_tcb);

    LOG_INFO(LOG_MODULE, "starting scheduler...");
    vTaskStartScheduler();

    for (;;)
    {
    }
}
