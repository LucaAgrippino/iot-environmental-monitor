/**
 * @file test_logger.c
 * @brief Unit tests for Logger Middleware (host build, Unity).
 *
 * Covers TC-LOG-001..022 per docs/lld/companions/logger.md §7.
 *
 * Mocking strategy:
 *   - FreeRTOS    → hand-written stubs in tests/support/FreeRTOS.h + .c
 *   - rtc_get_time / debug_uart_send → stubs defined locally in this TU
 *     (logger.c is the only consumer; isolating the stubs to this file
 *      avoids contaminating other tests' link sets)
 */

#include "unity.h"

/* freertos_mock.h basename matches freertos_mock.c — Ceedling auto-links
 * it the same way stm32_cmsis_mock.h triggers stm32_cmsis_mock.c in the
 * driver tests. No TEST_SOURCE_FILE directive needed or used. */

#include <stdint.h>
#include <stdbool.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "freertos_mock.h"   /* causes Ceedling to auto-link freertos_mock.c */

/* Driver stubs — declares only rtc_get_time and debug_uart_send (plus the
 * types they use). Logger tests provide stub bodies for these inline below.
 * This is intentionally NOT rtc_driver.h / debug_uart_driver.h — including
 * those would make Ceedling auto-link the real driver .c files and collide
 * with the inline stubs. */
#include "driver_stubs.h"

#include "logger.h"

/* ===================================================================== */
/* Local stubs for the Logger's downstream dependencies                  */
/* ===================================================================== */

/* --- rtc_get_time stub --------------------------------------------- */

static rtc_err_t      g_stub_rtc_get_time_return;
static rtc_datetime_t g_stub_rtc_get_time_value;
static uint32_t       g_stub_rtc_get_time_call_count;

rtc_err_t rtc_get_time(rtc_datetime_t *dt)
{
    g_stub_rtc_get_time_call_count++;
    if ((dt != NULL) && (g_stub_rtc_get_time_return == RTC_OK))
    {
        *dt = g_stub_rtc_get_time_value;
    }
    return g_stub_rtc_get_time_return;
}

/* The other rtc_driver functions are no longer referenced — driver_stubs.h
 * doesn't declare them, and the real rtc_driver.c is not linked into this
 * test (which is the whole point of the driver_stubs.h indirection). */

/* --- debug_uart_send stub ------------------------------------------ */

#define STUB_UART_BUF_SIZE  (256U)

static uint8_t  g_stub_uart_buf[STUB_UART_BUF_SIZE + 1U];   /* +1 for trailing NUL */
static uint16_t g_stub_uart_len;
static uint32_t g_stub_uart_call_count;

/* Adjust the return type / OK constant if debug_uart_driver.h declares them
 * differently — LOG-O4 in the companion. */
debug_uart_err_t debug_uart_send(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
	(void)timeout_ms;
    g_stub_uart_call_count++;
    if ((data != NULL) && (len <= STUB_UART_BUF_SIZE))
    {
        (void)memcpy(g_stub_uart_buf, data, len);
        g_stub_uart_buf[len] = '\0';
    }
    g_stub_uart_len = len;
    return DEBUG_UART_OK;
}

/* The other debug_uart functions are no longer referenced — see comment
 * above the rtc stubs. Add stubs here only if logger.c ever calls more
 * than debug_uart_send. */

/* ===================================================================== */
/* Helpers                                                               */
/* ===================================================================== */

static void stub_rtc_set_ok(uint8_t h, uint8_t m, uint8_t s)
{
    g_stub_rtc_get_time_return         = RTC_OK;
    g_stub_rtc_get_time_value.year     = 2026U;
    g_stub_rtc_get_time_value.month    = 5U;
    g_stub_rtc_get_time_value.day      = 28U;
    g_stub_rtc_get_time_value.hour     = h;
    g_stub_rtc_get_time_value.minute   = m;
    g_stub_rtc_get_time_value.second   = s;
}

static void stub_rtc_set_error(void)
{
    g_stub_rtc_get_time_return = RTC_ERR_SYNC_TIMEOUT;
}

static void prime_xQueueReceive_with(const log_entry_t *e)
{
    g_mock_xQueueReceive_return            = pdTRUE;
    g_mock_xQueueReceive_next_item_size    = sizeof(*e);
    (void)memcpy(g_mock_xQueueReceive_next_item, e, sizeof(*e));
}

static void init_logger_at_debug(void)
{
    /* Defaults set initialise-OK with RTC stub ready. */
    g_mock_scheduler_state = taskSCHEDULER_NOT_STARTED;
    TEST_ASSERT_EQUAL(LOGGER_OK, logger_init(LOG_LEVEL_DEBUG));
}

/* ===================================================================== */
/* Unity setUp / tearDown                                                */
/* ===================================================================== */

void setUp(void)
{
    mock_freertos_reset();

    /* RTC stub defaults */
    g_stub_rtc_get_time_return     = RTC_OK;
    g_stub_rtc_get_time_call_count = 0U;
    stub_rtc_set_ok(12U, 34U, 56U);

    /* UART stub state */
    (void)memset(g_stub_uart_buf, 0, sizeof(g_stub_uart_buf));
    g_stub_uart_len        = 0U;
    g_stub_uart_call_count = 0U;

    /* Reset the SUT */
    logger_reset_for_test();

    /* Tell the xQueueSend mock how many bytes to capture (size of an entry). */
    g_mock_xQueueSend_last_item_size = sizeof(log_entry_t);
}

void tearDown(void) { /* nothing */ }

/* ===================================================================== */
/* §A. Init / lifecycle                                                  */
/* ===================================================================== */

void test_TC_LOG_020_init_happy_path_creates_queue_and_task(void)
{
    rtc_err_t err = logger_init(LOG_LEVEL_INFO);
    TEST_ASSERT_EQUAL(LOGGER_OK, err);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xQueueCreateStatic_call_count);
    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xTaskCreateStatic_call_count);
    TEST_ASSERT_NOT_NULL(g_mock_xTaskCreateStatic_last_fn);
}

void test_TC_LOG_015_init_out_of_range_level_returns_invalid_arg(void)
{
    rtc_err_t err = logger_init((log_level_t)99);
    TEST_ASSERT_EQUAL(LOGGER_ERR_INVALID_ARG, err);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueCreateStatic_call_count);
}

void test_TC_LOG_016_set_level_before_init_returns_not_init(void)
{
    rtc_err_t err = logger_set_level(LOG_LEVEL_WARN);
    TEST_ASSERT_EQUAL(LOGGER_ERR_NOT_INIT, err);
}

void test_TC_LOG_010_set_level_out_of_range_returns_invalid_arg(void)
{
    init_logger_at_debug();
    rtc_err_t err = logger_set_level((log_level_t)42);
    TEST_ASSERT_EQUAL(LOGGER_ERR_INVALID_ARG, err);
}

/* ===================================================================== */
/* §B. logger_log — input handling                                       */
/* ===================================================================== */

void test_TC_LOG_008_log_with_null_msg_is_a_no_op(void)
{
    init_logger_at_debug();
    logger_log(LOG_LEVEL_INFO, "Mod", NULL);
    TEST_ASSERT_EQUAL_UINT32(0U, g_stub_uart_call_count);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend_call_count);
}

void test_TC_LOG_009_log_before_init_is_a_no_op(void)
{
    /* No init. */
    logger_log(LOG_LEVEL_INFO, "Mod", "hello");
    TEST_ASSERT_EQUAL_UINT32(0U, g_stub_uart_call_count);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend_call_count);
}

void test_TC_LOG_007_null_module_renders_as_question_mark(void)
{
    init_logger_at_debug();
    logger_log(LOG_LEVEL_INFO, NULL, "hello");
    /* Captured output should contain "[?               ]" (16-wide padding). */
    TEST_ASSERT_NOT_NULL(strstr((char *)g_stub_uart_buf, "[?"));
}

/* ===================================================================== */
/* §C. logger_log — pre-scheduler synchronous path                       */
/* ===================================================================== */

void test_TC_LOG_001_pre_scheduler_info_writes_full_line(void)
{
    init_logger_at_debug();
    stub_rtc_set_ok(12U, 34U, 56U);

    logger_log(LOG_LEVEL_INFO, "MyModule", "Hello");

    TEST_ASSERT_EQUAL_UINT32(1U, g_stub_uart_call_count);
    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend_call_count);
    /* Output contains the level tag, timestamp, module, message, CRLF. */
    TEST_ASSERT_NOT_NULL(strstr((char *)g_stub_uart_buf, "[ INFO]"));
    TEST_ASSERT_NOT_NULL(strstr((char *)g_stub_uart_buf, "[12:34:56]"));
    TEST_ASSERT_NOT_NULL(strstr((char *)g_stub_uart_buf, "[MyModule"));   /* padded */
    TEST_ASSERT_NOT_NULL(strstr((char *)g_stub_uart_buf, "Hello"));
    TEST_ASSERT_EQUAL_STRING("\r\n",
        (const char *)&g_stub_uart_buf[g_stub_uart_len - 2U]);
}

void test_TC_LOG_004_rtc_ok_produces_wallclock_timestamp(void)
{
    init_logger_at_debug();
    stub_rtc_set_ok(7U, 8U, 9U);

    logger_log(LOG_LEVEL_INFO, "M", "x");

    TEST_ASSERT_NOT_NULL(strstr((char *)g_stub_uart_buf, "[07:08:09]"));
}

void test_TC_LOG_005_rtc_error_falls_back_to_uptime_timestamp(void)
{
    init_logger_at_debug();
    stub_rtc_set_error();
    g_mock_tick_count = 12345U;          /* portTICK_PERIOD_MS = 1 → ms = ticks */

    logger_log(LOG_LEVEL_INFO, "M", "x");

    TEST_ASSERT_NOT_NULL(strstr((char *)g_stub_uart_buf, "[T+00012345]"));
}

void test_TC_LOG_011_long_module_truncated_to_width(void)
{
    init_logger_at_debug();
    logger_log(LOG_LEVEL_INFO, "ThisModuleNameIsWayTooLong", "x");
    /* The module field is exactly LOGGER_MODULE_WIDTH chars between [ and ]. */
    const char *p = strchr((char *)g_stub_uart_buf, ']');         /* end of LEVEL */
    p = strchr(p + 1, ']');                                       /* end of TS    */
    const char *open  = strchr(p + 1, '[');
    const char *close = strchr(open + 1, ']');
    TEST_ASSERT_NOT_NULL(open);
    TEST_ASSERT_NOT_NULL(close);
    TEST_ASSERT_EQUAL_INT(LOGGER_MODULE_WIDTH, (int)(close - open - 1));
}

void test_TC_LOG_006_long_message_truncated_in_macro_buffer(void)
{
    init_logger_at_debug();
    /* Pass a message longer than LOGGER_MESSAGE_MAX directly. */
    char big[LOGGER_MESSAGE_MAX + 64U];
    (void)memset(big, 'X', sizeof(big));
    big[sizeof(big) - 1U] = '\0';

    logger_log(LOG_LEVEL_INFO, "M", big);

    /* CRLF must still terminate the line. */
    TEST_ASSERT_EQUAL_STRING("\r\n",
        (const char *)&g_stub_uart_buf[g_stub_uart_len - 2U]);
    /* Line length is bounded by the output buffer. */
    TEST_ASSERT_TRUE(g_stub_uart_len <= 128U);
}

void test_TC_LOG_019_level_tag_padding_each_level(void)
{
    init_logger_at_debug();

    logger_log(LOG_LEVEL_ERROR, "M", "x");
    TEST_ASSERT_NOT_NULL(strstr((char *)g_stub_uart_buf, "[ERROR]"));

    (void)memset(g_stub_uart_buf, 0, sizeof(g_stub_uart_buf));
    logger_log(LOG_LEVEL_WARN, "M", "x");
    TEST_ASSERT_NOT_NULL(strstr((char *)g_stub_uart_buf, "[ WARN]"));

    (void)memset(g_stub_uart_buf, 0, sizeof(g_stub_uart_buf));
    logger_log(LOG_LEVEL_INFO, "M", "x");
    TEST_ASSERT_NOT_NULL(strstr((char *)g_stub_uart_buf, "[ INFO]"));

    (void)memset(g_stub_uart_buf, 0, sizeof(g_stub_uart_buf));
    logger_log(LOG_LEVEL_DEBUG, "M", "x");
    TEST_ASSERT_NOT_NULL(strstr((char *)g_stub_uart_buf, "[DEBUG]"));
}

/* ===================================================================== */
/* §D. logger_log — post-scheduler queue path                            */
/* ===================================================================== */

void test_TC_LOG_014_post_scheduler_nominal_enqueues_not_writes(void)
{
    init_logger_at_debug();
    g_mock_scheduler_state = taskSCHEDULER_RUNNING;
    stub_rtc_set_ok(1U, 2U, 3U);

    logger_log(LOG_LEVEL_INFO, "Mod", "queued");

    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xQueueSend_call_count);
    TEST_ASSERT_EQUAL_UINT32(0U, g_stub_uart_call_count);

    /* The captured queue entry should match what we passed. */
    log_entry_t captured;
    (void)memcpy(&captured, g_mock_xQueueSend_last_item, sizeof(captured));
    TEST_ASSERT_EQUAL(LOG_LEVEL_INFO, captured.level);
    TEST_ASSERT_FALSE(captured.use_uptime);
    TEST_ASSERT_EQUAL_UINT8(1U, captured.ts.wallclock.hour);
    TEST_ASSERT_EQUAL_UINT8(2U, captured.ts.wallclock.minute);
    TEST_ASSERT_EQUAL_UINT8(3U, captured.ts.wallclock.second);
    TEST_ASSERT_EQUAL_STRING("Mod",    captured.module);
    TEST_ASSERT_EQUAL_STRING("queued", captured.message);
}

void test_TC_LOG_002_runtime_filter_below_emits_nothing(void)
{
    init_logger_at_debug();
    g_mock_scheduler_state = taskSCHEDULER_RUNNING;
    TEST_ASSERT_EQUAL(LOGGER_OK, logger_set_level(LOG_LEVEL_WARN));

    logger_log(LOG_LEVEL_INFO, "Mod", "filtered");   /* INFO > WARN → suppressed */

    TEST_ASSERT_EQUAL_UINT32(0U, g_mock_xQueueSend_call_count);
    TEST_ASSERT_EQUAL_UINT32(0U, g_stub_uart_call_count);
}

void test_TC_LOG_003_runtime_filter_at_or_above_emits(void)
{
    init_logger_at_debug();
    g_mock_scheduler_state = taskSCHEDULER_RUNNING;
    TEST_ASSERT_EQUAL(LOGGER_OK, logger_set_level(LOG_LEVEL_WARN));

    logger_log(LOG_LEVEL_ERROR, "Mod", "passed");    /* ERROR ≤ WARN → emitted */

    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xQueueSend_call_count);
}

void test_TC_LOG_012_queue_full_drops_and_increments_counter(void)
{
    init_logger_at_debug();
    g_mock_scheduler_state          = taskSCHEDULER_RUNNING;
    g_mock_xQueueSend_return        = pdFALSE;     /* simulate full queue */

    logger_log(LOG_LEVEL_INFO, "Mod", "lost");

    TEST_ASSERT_EQUAL_UINT32(1U, logger_get_dropped_count());
}

void test_TC_LOG_013_repeated_drops_accumulate(void)
{
    init_logger_at_debug();
    g_mock_scheduler_state          = taskSCHEDULER_RUNNING;
    g_mock_xQueueSend_return        = pdFALSE;

    logger_log(LOG_LEVEL_INFO, "M", "1");
    logger_log(LOG_LEVEL_INFO, "M", "2");
    logger_log(LOG_LEVEL_INFO, "M", "3");

    TEST_ASSERT_EQUAL_UINT32(3U, logger_get_dropped_count());
}

/* ===================================================================== */
/* §E. ISR path                                                          */
/* ===================================================================== */

void test_TC_LOG_017_log_from_isr_enqueues_with_uptime(void)
{
    init_logger_at_debug();
    g_mock_scheduler_state          = taskSCHEDULER_RUNNING;
    g_mock_tick_count_from_isr      = 4242U;

    logger_log_from_isr(LOG_LEVEL_WARN, "Modbus", "frame timeout");

    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xQueueSendFromISR_call_count);

    log_entry_t captured;
    (void)memcpy(&captured, g_mock_xQueueSendFromISR_last_item, sizeof(captured));
    TEST_ASSERT_EQUAL(LOG_LEVEL_WARN, captured.level);
    TEST_ASSERT_TRUE(captured.use_uptime);
    TEST_ASSERT_EQUAL_UINT32(4242U, captured.ts.uptime_ms);
    TEST_ASSERT_EQUAL_STRING("Modbus",        captured.module);
    TEST_ASSERT_EQUAL_STRING("frame timeout", captured.message);
}

/* ===================================================================== */
/* §F. Drain task                                                        */
/* ===================================================================== */

void test_TC_LOG_018_drain_once_dequeues_formats_and_sends(void)
{
    init_logger_at_debug();

    log_entry_t e = {0};
    e.level                 = LOG_LEVEL_INFO;
    e.use_uptime            = false;
    e.ts.wallclock.hour     = 9U;
    e.ts.wallclock.minute   = 0U;
    e.ts.wallclock.second   = 0U;
    (void)strcpy(e.module,  "DrainTest");
    (void)strcpy(e.message, "ready");
    prime_xQueueReceive_with(&e);

    logger_drain_once_for_test();

    TEST_ASSERT_EQUAL_UINT32(1U, g_mock_xQueueReceive_call_count);
    TEST_ASSERT_EQUAL_UINT32(1U, g_stub_uart_call_count);
    TEST_ASSERT_NOT_NULL(strstr((char *)g_stub_uart_buf, "[09:00:00]"));
    TEST_ASSERT_NOT_NULL(strstr((char *)g_stub_uart_buf, "ready"));
}

/* ===================================================================== */
/* §G. ANSI colours                                                      */
/* ===================================================================== */

#if LOGGER_USE_ANSI_COLORS

void test_TC_LOG_021_error_line_starts_with_red_ansi(void)
{
    init_logger_at_debug();
    logger_log(LOG_LEVEL_ERROR, "M", "boom");
    /* Expect leading red, then [ERROR], then reset, then continue. */
    TEST_ASSERT_EQUAL_STRING_LEN("\033[31m[ERROR]\033[0m",
                                 (const char *)g_stub_uart_buf, 16);
}

void test_TC_LOG_022_other_levels_use_correct_ansi(void)
{
    init_logger_at_debug();

    logger_log(LOG_LEVEL_WARN, "M", "x");
    TEST_ASSERT_EQUAL_STRING_LEN("\033[33m[ WARN]\033[0m",
                                 (const char *)g_stub_uart_buf, 16);

    (void)memset(g_stub_uart_buf, 0, sizeof(g_stub_uart_buf));
    logger_log(LOG_LEVEL_INFO, "M", "x");
    TEST_ASSERT_EQUAL_STRING_LEN("\033[36m[ INFO]\033[0m",
                                 (const char *)g_stub_uart_buf, 16);

    (void)memset(g_stub_uart_buf, 0, sizeof(g_stub_uart_buf));
    logger_log(LOG_LEVEL_DEBUG, "M", "x");
    TEST_ASSERT_EQUAL_STRING_LEN("\033[2m[DEBUG]\033[0m",
                                 (const char *)g_stub_uart_buf, 15);
}

#endif /* LOGGER_USE_ANSI_COLORS */
