/**
 * @file logger.c
 * @brief Logger Middleware implementation.
 *
 * Producer-side: capture timestamp + copy module/message into a structured
 * log_entry_t, enqueue (or write synchronously if pre-scheduler).
 * Drain task: dequeue, assemble the on-wire line via format_line(), send.
 *
 * See docs/lld/companions/logger.md v0.3 for the full design.
 */

#include "logger.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"

#include "rtc/rtc_driver.h"
#include "debug-uart/debug_uart_driver.h"

/* ===================================================================== */
/* §1. Configuration                                                     */
/* ===================================================================== */

#define LOGGER_QUEUE_DEPTH (16U)
#define LOGGER_OUT_BUF_MAX (128U)

#ifndef LOGGER_DRAIN_TASK_PRIORITY
#define LOGGER_DRAIN_TASK_PRIORITY (tskIDLE_PRIORITY + 1U)
#endif

#ifndef LOGGER_DRAIN_STACK_WORDS
#define LOGGER_DRAIN_STACK_WORDS (256U)
#endif

/* ANSI escape sequences — used by format_line when colours are enabled. */
#define ANSI_RESET "\033[0m"
#define ANSI_RED "\033[31m"
#define ANSI_YELLOW "\033[33m"
#define ANSI_CYAN "\033[36m"
#define ANSI_DIM "\033[2m"

#define ANSI_BRIGTH_RED "\033[91m"
#define ANSI_BRIGTH_YELLOW "\033[93m"
#define ANSI_BRIGTH_CYAN "\033[96m"
#define ANSI_BRIGTH_DIM "\033[97m"

/* Test-visibility shim: static in production, externally linkable in test
 * builds so the test TU can call internals (format_line, drain helper). */
#ifdef TEST
#define LOGGER_TEST_VISIBLE
#else
#define LOGGER_TEST_VISIBLE static
#endif

/* ===================================================================== */
/* §2. Private types                                                     */
/* ===================================================================== */

/* In test builds log_entry_t is exposed via logger.h under #ifdef TEST so
 * the test TU can inspect what producers enqueue. Don't redefine it here
 * in that case — pick up the definition from the header. */
#ifndef TEST
typedef struct
{
    log_level_t level;
    bool use_uptime;
    union
    {
        struct
        {
            uint8_t hour;
            uint8_t minute;
            uint8_t second;
        } wallclock;
        uint32_t uptime_ms;
    } ts;
    char module[LOGGER_MODULE_WIDTH + 1U];
    char message[LOGGER_MESSAGE_MAX];
} log_entry_t;
#endif

typedef struct
{
    log_level_t level; /**< runtime severity filter */
    bool initialised;
} logger_state_t;

/* ===================================================================== */
/* §3. Private state                                                     */
/* ===================================================================== */

static logger_state_t s_log;
static volatile uint32_t s_dropped;

/* Static FreeRTOS objects — no heap (P5). */
static StaticQueue_t s_queue_ctrl;
static uint8_t s_queue_storage[LOGGER_QUEUE_DEPTH * sizeof(log_entry_t)];
static QueueHandle_t s_queue;

static StaticTask_t s_drain_tcb;
static StackType_t s_drain_stack[LOGGER_DRAIN_STACK_WORDS] __attribute__((aligned(8)));
static TaskHandle_t s_drain_task;

/* ===================================================================== */
/* §4. Forward declarations                                              */
/* ===================================================================== */

static bool level_in_range(log_level_t level);
static void fill_module(char *dst, const char *src);
static void fill_message(char *dst, const char *src);
static void capture_timestamp(log_entry_t *e);
static void drain_task_body(void *arg);

LOGGER_TEST_VISIBLE uint16_t format_line(const log_entry_t *e, char *out, size_t out_size);

/* ===================================================================== */
/* §5. format_line — single source of truth for the on-wire line        */
/* ===================================================================== */

LOGGER_TEST_VISIBLE uint16_t format_line(const log_entry_t *e, char *out, size_t out_size)
{
    static const char *const level_tag[] = {[LOG_LEVEL_ERROR] = "ERROR",
                                            [LOG_LEVEL_WARN] = " WARN",
                                            [LOG_LEVEL_INFO] = " INFO",
                                            [LOG_LEVEL_DEBUG] = "DEBUG"};
#if LOGGER_USE_ANSI_COLORS
    static const char *const level_color[] = {[LOG_LEVEL_ERROR] = ANSI_BRIGTH_RED,
                                              [LOG_LEVEL_WARN] = ANSI_BRIGTH_YELLOW,
                                              [LOG_LEVEL_INFO] = ANSI_BRIGTH_CYAN,
                                              [LOG_LEVEL_DEBUG] = ANSI_BRIGTH_DIM};
#endif

    if ((out == NULL) || (out_size == 0U) || (e == NULL))
    {
        return 0U;
    }

    /* Defensive: clamp the level index for the lookup tables. */
    log_level_t lvl = level_in_range(e->level) ? e->level : LOG_LEVEL_INFO;

    int written = 0;
    int n = 0;

    /* --- Level tag (with ANSI colour wrapping if enabled) ----------- */
#if LOGGER_USE_ANSI_COLORS
    n = snprintf(out, out_size, "%s[%s]%s", level_color[lvl], level_tag[lvl], ANSI_RESET);
#else
    n = snprintf(out, out_size, "[%s]", level_tag[lvl]);
#endif
    if ((n < 0) || ((size_t) n >= out_size))
    {
        out[out_size - 1U] = '\0';
        return (uint16_t) (out_size - 1U);
    }
    written = n;

    /* --- Timestamp ------------------------------------------------- */
    if (e->use_uptime)
    {
        n = snprintf(out + written, out_size - (size_t) written, "[T+%08lu]",
                     (unsigned long) e->ts.uptime_ms);
    }
    else
    {
        n = snprintf(out + written, out_size - (size_t) written, "[%02u:%02u:%02u]",
                     (unsigned) e->ts.wallclock.hour, (unsigned) e->ts.wallclock.minute,
                     (unsigned) e->ts.wallclock.second);
    }
    if ((n < 0) || ((size_t) n >= (out_size - (size_t) written)))
    {
        out[out_size - 1U] = '\0';
        return (uint16_t) (out_size - 1U);
    }
    written += n;

    /* --- Module + space + message + CRLF --------------------------- */
    n = snprintf(out + written, out_size - (size_t) written, "[%-*s] %s\r\n",
                 (int) LOGGER_MODULE_WIDTH, e->module, e->message);
    if ((n < 0) || ((size_t) n >= (out_size - (size_t) written)))
    {
        out[out_size - 1U] = '\0';
        return (uint16_t) (out_size - 1U);
    }
    written += n;

    return (uint16_t) written;
}

/* ===================================================================== */
/* §6. Helpers                                                           */
/* ===================================================================== */

static bool level_in_range(log_level_t level)
{
    return (level >= LOG_LEVEL_ERROR) && (level <= LOG_LEVEL_DEBUG);
}

static void fill_module(char *dst, const char *src)
{
    if (src == NULL)
    {
        dst[0] = '?';
        dst[1] = '\0';
    }
    else
    {
        (void) strncpy(dst, src, LOGGER_MODULE_WIDTH);
        dst[LOGGER_MODULE_WIDTH] = '\0';
    }
}

static void fill_message(char *dst, const char *src)
{
    /* msg NULL is rejected by the caller; this helper is unconditional copy. */
    (void) strncpy(dst, src, LOGGER_MESSAGE_MAX - 1U);
    dst[LOGGER_MESSAGE_MAX - 1U] = '\0';
}

static void capture_timestamp(log_entry_t *e)
{
    rtc_datetime_t dt;
    if (rtc_get_time(&dt) == RTC_OK)
    {
        e->use_uptime = false;
        e->ts.wallclock.hour = dt.hour;
        e->ts.wallclock.minute = dt.minute;
        e->ts.wallclock.second = dt.second;
    }
    else
    {
        e->use_uptime = true;
        e->ts.uptime_ms = (uint32_t) xTaskGetTickCount() * portTICK_PERIOD_MS;
    }
}

/* ===================================================================== */
/* §7. Drain task                                                        */
/* ===================================================================== */

static void drain_task_body(void *arg)
{
    (void) arg;
    log_entry_t entry;
    char out[LOGGER_OUT_BUF_MAX];

    for (;;)
    {
        if (xQueueReceive(s_queue, &entry, portMAX_DELAY) == pdTRUE)
        {
            uint16_t len = format_line(&entry, out, sizeof(out));
            if (DEBUG_UART_OK != debug_uart_send((const uint8_t *) out, len, portMAX_DELAY))
            {
                taskENTER_CRITICAL();
                s_dropped++;
                taskEXIT_CRITICAL();
            }
        }
    }
}

/* ===================================================================== */
/* §8. Public API                                                        */
/* ===================================================================== */

logger_err_t logger_init(log_level_t initial_level)
{
    if (!level_in_range(initial_level))
    {
        return LOGGER_ERR_INVALID_ARG;
    }
    if (s_log.initialised)
    {
        return LOGGER_OK; /* idempotent */
    }

    s_log.level = initial_level;
    s_dropped = 0U;

    s_queue =
        xQueueCreateStatic(LOGGER_QUEUE_DEPTH, sizeof(log_entry_t), s_queue_storage, &s_queue_ctrl);

    s_drain_task = xTaskCreateStatic(drain_task_body, "log_drain", LOGGER_DRAIN_STACK_WORDS, NULL,
                                     LOGGER_DRAIN_TASK_PRIORITY, s_drain_stack, &s_drain_tcb);

    s_log.initialised = true;
    return LOGGER_OK;
}

logger_err_t logger_set_level(log_level_t level)
{
    if (!s_log.initialised)
    {
        return LOGGER_ERR_NOT_INIT;
    }
    if (!level_in_range(level))
    {
        return LOGGER_ERR_INVALID_ARG;
    }
    s_log.level = level;
    return LOGGER_OK;
}

void logger_log(log_level_t level, const char *module, const char *msg)
{
    if (!s_log.initialised)
    {
        return;
    }
    if (!level_in_range(level))
    {
        return;
    }
    if (level > s_log.level)
    {
        return;
    } /* runtime filter */
    if (msg == NULL)
    {
        return;
    }

    log_entry_t e;
    e.level = level;
    capture_timestamp(&e);
    fill_module(e.module, module);
    fill_message(e.message, msg);

    if (xTaskGetSchedulerState() == taskSCHEDULER_NOT_STARTED)
    {
        /* Pre-scheduler: synchronous direct write, single source-of-truth
         * format via format_line() — same line shape as the drain task. */
        char out[LOGGER_OUT_BUF_MAX];
        uint16_t len = format_line(&e, out, sizeof(out));
        if (DEBUG_UART_OK != debug_uart_send((const uint8_t *) out, len, portMAX_DELAY))
        {
            taskENTER_CRITICAL();
            s_dropped++;
            taskEXIT_CRITICAL();
        }
    }
    else if (xQueueSend(s_queue, &e, 0) != pdTRUE)
    {
        taskENTER_CRITICAL();
        s_dropped++;
        taskEXIT_CRITICAL();
    }
}

void logger_log_from_isr(log_level_t level, const char *module, const char *msg)
{
    if (!s_log.initialised)
    {
        return;
    }
    if (!level_in_range(level))
    {
        return;
    }
    if (level > s_log.level)
    {
        return;
    }
    if (msg == NULL)
    {
        return;
    }

    log_entry_t e;
    e.level = level;
    e.use_uptime = true;
    e.ts.uptime_ms = (uint32_t) xTaskGetTickCountFromISR() * portTICK_PERIOD_MS;
    fill_module(e.module, module);
    fill_message(e.message, msg);

    BaseType_t woken = pdFALSE;
    if (xQueueSendFromISR(s_queue, &e, &woken) != pdTRUE)
    {
        UBaseType_t mask = taskENTER_CRITICAL_FROM_ISR();
        s_dropped++;
        taskEXIT_CRITICAL_FROM_ISR(mask);
    }
    portYIELD_FROM_ISR(woken);
}

uint32_t logger_get_dropped_count(void)
{
    return s_dropped;
}

/* ===================================================================== */
/* §9. Test-only hooks                                                   */
/* ===================================================================== */

#ifdef TEST
void logger_reset_for_test(void)
{
    s_log.initialised = false;
    s_log.level = LOG_LEVEL_ERROR;
    s_dropped = 0U;
    s_queue = NULL;
    s_drain_task = NULL;
    (void) memset(&s_queue_ctrl, 0, sizeof(s_queue_ctrl));
    (void) memset(&s_drain_tcb, 0, sizeof(s_drain_tcb));
    (void) memset(s_queue_storage, 0, sizeof(s_queue_storage));
    (void) memset(s_drain_stack, 0, sizeof(s_drain_stack));
}

/**
 * @brief Run one iteration of the drain-task body, then return.
 *
 * Allows the test TU to exercise the dequeue → format → send path
 * without spinning the real FreeRTOS task. The FreeRTOS mock's
 * xQueueReceive must be primed with the entry to deliver.
 */
void logger_drain_once_for_test(void)
{
    log_entry_t entry;
    if (xQueueReceive(s_queue, &entry, portMAX_DELAY) == pdTRUE)
    {
        char out[LOGGER_OUT_BUF_MAX];
        uint16_t len = format_line(&entry, out, sizeof(out));
        if (DEBUG_UART_OK != debug_uart_send((const uint8_t *) out, len, portMAX_DELAY))
        {
            taskENTER_CRITICAL();
            s_dropped++;
            taskEXIT_CRITICAL();
        }
    }
}
#endif
