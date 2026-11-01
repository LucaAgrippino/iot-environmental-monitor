/**
 * @file console_service.c
 * @brief ConsoleService: lifecycle, input path, render loop and task.
 */

#include "console_service.h"

#include <string.h>

#include "FreeRTOS.h"
#include "core/commands.h"
#include "core/editor.h"
#include "core/header.h"
#include "core/input.h"
#include "core/model.h"
#include "core/repl.h"
#include "core/screen.h"
#include "core/theme.h"
#include "port/console_mem.h"
#include "port/console_port.h"
#include "semphr.h"
#include "task.h"
#include "timers.h"

#ifdef TEST
#define LOG_ERROR(m, f, ...) ((void) 0)
#else
#include "logger/logger.h"
#endif /* TEST */

#define MODULE_TAG "CON"

/* Notification bits for ConsoleTask. */
#define NOTIFY_RX (1UL << 0)
#define NOTIFY_DATA_TICK (1UL << 1)

#define RING_SIZE (256U) /* power of two: masked-index arithmetic */
#define RING_MASK (RING_SIZE - 1U)
#define TX_TIMEOUT_MS (200U)
#define DATA_TICK_MS (1000U)
#define HEADER_LINE_MAX (256U)

#define PROMPT_NORMAL "envmon> "
#define PROMPT_CONFIRM "[y/N] "

static console_deps_t s_deps;
static bool s_initialised;
static bool s_started;

CONSOLE_BIG_BUFFER static console_model_t s_model;
CONSOLE_BIG_BUFFER static editor_t s_editor;

static volatile uint8_t s_ring[RING_SIZE];
static volatile uint16_t s_ring_head; /* written by the ISR */
static uint16_t s_ring_tail;          /* read by the task   */
static volatile bool s_ring_overflow;

static bool s_redraw; /* repaint the whole layout on the next cycle */

static TaskHandle_t s_task;
static TimerHandle_t s_tick_timer;
static StaticTimer_t s_tick_timer_buf;
static SemaphoreHandle_t s_out_lock;
static StaticSemaphore_t s_out_lock_buf;

static console_service_err_t prv_run_once(void);
static const iconsole_service_t s_vtable = {.run_once = prv_run_once};
const iconsole_service_t *const console_service = &s_vtable;

/* ---------------------------------------------------------------------- */
/* Output                                                                  */
/* ---------------------------------------------------------------------- */

static bool prv_send(const uint8_t *data, size_t len)
{
    return s_deps.uart->send(data, len, TX_TIMEOUT_MS) == DEBUG_UART_OK;
}

/* One lock for the whole screen: the Logger task and ConsoleTask both draw, and an escape
 * sequence cut in two by the other writer would corrupt the layout. */
static void prv_out_lock(void)
{
    (void) xSemaphoreTake(s_out_lock, portMAX_DELAY);
}

static void prv_out_unlock(void)
{
    (void) xSemaphoreGive(s_out_lock);
}

/* Registered with the Logger: every log line lands in the centre panel. */
static bool prv_log_sink(const uint8_t *data, uint16_t len)
{
    screen_log((const char *) data, len);
    return true;
}

static void prv_draw_prompt(void)
{
    screen_prompt(commands_confirm_active() ? PROMPT_CONFIRM : PROMPT_NORMAL, s_editor.line);
}

static void prv_draw_header(void)
{
    char l1[HEADER_LINE_MAX];
    char l2[HEADER_LINE_MAX];

    header_compose(&s_model, l1, sizeof(l1), l2, sizeof(l2));
    screen_header(l1, l2);
}

/* ---------------------------------------------------------------------- */
/* Receive path                                                            */
/* ---------------------------------------------------------------------- */

static void prv_rx_isr(uint8_t byte, void *ctx)
{
    (void) ctx;
    const uint16_t head = s_ring_head;
    const uint16_t next = (uint16_t) ((head + 1U) & RING_MASK);

    if (next == s_ring_tail)
    {
        s_ring_overflow = true; /* full: drop the byte, tell the operator on the next frame */
        return;
    }
    s_ring[head] = byte;
    s_ring_head = next;

    /* At 921600 baud a byte arrives every 12 us, so the ISR has to be short: wake the task only
     * for the first byte of a burst. The task drains the whole ring each time it runs. */
    if ((s_task != NULL) && (head == s_ring_tail))
    {
        BaseType_t woken = pdFALSE;

        (void) xTaskNotifyFromISR(s_task, NOTIFY_RX, eSetBits, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

/* Bytes are waiting in the receive ring. */
static bool prv_input_waiting(void)
{
    return s_ring_tail != s_ring_head;
}

static void prv_tick_cb(TimerHandle_t t)
{
    (void) t;
    if (s_task != NULL)
    {
        (void) xTaskNotify(s_task, NOTIFY_DATA_TICK, eSetBits);
    }
}

/* ---------------------------------------------------------------------- */
/* Key handling                                                            */
/* ---------------------------------------------------------------------- */
/* Key handling                                                            */
/* ---------------------------------------------------------------------- */

static void prv_run_line(void)
{
    char line[EDITOR_LINE_MAX];

    (void) memcpy(line, s_editor.line, sizeof(line));
    editor_clear(&s_editor);
    prv_draw_prompt(); /* the typed line leaves the prompt before its answer appears */
    (void) repl_execute(line);
}

static void prv_apply_action(editor_action_t act)
{
    switch (act)
    {
    case ED_RUN_LINE:
        prv_run_line();
        break;
    case ED_LINE_OVERFLOW:
        repl_puts(PAL_ERR, "[ERR] line overflow: input cleared");
        break;
    case ED_CONFIRM_YES:
        (void) commands_confirm_answer(true);
        break;
    case ED_CONFIRM_NO:
        (void) commands_confirm_answer(false);
        break;
    case ED_REDRAW:
        s_redraw = true;
        break;
    default:
        break;
    }
    if (act != ED_NONE)
    {
        prv_draw_prompt();
    }
}

static void prv_handle_key(const key_event_t *key)
{
    if (key->kind == KEY_SIZE_REPORT)
    {
        return; /* the layout size is fixed (the `term` command changes it) */
    }
    prv_apply_action(editor_key(&s_editor, key, commands_confirm_active()));
}

static void prv_drain_input(void)
{
    key_event_t key;
    const uint32_t now = console_port_millis();

    while (s_ring_tail != s_ring_head)
    {
        const uint8_t b = s_ring[s_ring_tail];

        s_ring_tail = (uint16_t) ((s_ring_tail + 1U) & RING_MASK);
        if (input_feed(b, now, &key))
        {
            prv_handle_key(&key);
            if (input_take_pending(&key)) /* a key that followed an Escape in the same burst */
            {
                prv_handle_key(&key);
            }
        }
    }
    if (input_poll_timeout(now, &key))
    {
        prv_handle_key(&key);
    }
}

/* ---------------------------------------------------------------------- */
/* One cycle                                                               */
/* ---------------------------------------------------------------------- */

static void prv_paint_all(void)
{
    screen_open();
    prv_draw_header();
    prv_draw_prompt();
    s_redraw = false;
}

static void prv_start(void)
{
    const screen_port_t port = {.send = prv_send, .lock = prv_out_lock, .unlock = prv_out_unlock};

    screen_init(&port);
    model_refresh(&s_deps, &s_model);
    prv_paint_all();
    repl_init();
#ifndef TEST
    logger_set_sink(prv_log_sink);
#endif
    s_started = true;
}

static console_service_err_t prv_cycle(uint32_t bits)
{
    if (!s_initialised)
    {
        return CONSOLE_SERVICE_ERR_NOT_INIT;
    }
    if (!s_started)
    {
        prv_start();
    }

    model_refresh(&s_deps, &s_model);
    prv_drain_input();
    if ((bits & NOTIFY_DATA_TICK) != 0U)
    {
        if (commands_tick() == CONSOLE_SERVICE_ERR_TIMEOUT)
        {
            prv_draw_prompt();
        }
        model_refresh(&s_deps, &s_model);
        prv_draw_header();
    }
    if (s_ring_overflow)
    {
        s_ring_overflow = false;
        repl_puts(PAL_WARN, "[WARN] input too fast: some keys were dropped");
    }
    if (commands_take_redraw_request())
    {
        s_redraw = true;
    }
    if (s_redraw)
    {
        prv_paint_all();
    }

    /* Slow work goes after the screen is current, so it never waits behind a flash write. */
    if (commands_take_persist_due())
    {
        (void) s_deps.cfg_write->persist_pending(s_deps.cfg_write->ctx);
    }
    if (commands_take_reboot_request())
    {
        screen_close();
        console_port_reset();
    }
    return CONSOLE_SERVICE_ERR_OK;
}

static console_service_err_t prv_run_once(void)
{
    uint32_t bits = 0U;
    const TickType_t wait =
        input_esc_pending() ? pdMS_TO_TICKS(INPUT_ESC_TIMEOUT_MS) : portMAX_DELAY;

    if (!s_initialised)
    {
        return CONSOLE_SERVICE_ERR_NOT_INIT;
    }
    (void) xTaskNotifyWait(0U, UINT32_MAX, &bits, wait);
    return prv_cycle(bits);
}

/* ---------------------------------------------------------------------- */
/* Public API                                                              */
/* ---------------------------------------------------------------------- */

console_service_err_t console_service_init(const console_deps_t *deps)
{
    if ((deps == NULL) || (deps->uart == NULL) || (deps->sensors == NULL) ||
        (deps->alarms == NULL) || (deps->cfg_read == NULL) || (deps->cfg_write == NULL) ||
        (deps->health == NULL) || (deps->prof_read == NULL))
    {
        return CONSOLE_SERVICE_ERR_NULL_ARG;
    }

    s_deps = *deps;
    s_started = false;
    s_redraw = false;
    s_ring_head = 0U;
    s_ring_tail = 0U;
    s_ring_overflow = false;
    (void) memset(&s_model, 0, sizeof(s_model));
    editor_init(&s_editor);
    input_reset();
    commands_init(&s_deps);

    s_out_lock = xSemaphoreCreateMutexStatic(&s_out_lock_buf);
    s_tick_timer = xTimerCreateStatic("con_tick", pdMS_TO_TICKS(DATA_TICK_MS), pdTRUE, NULL,
                                      prv_tick_cb, &s_tick_timer_buf);

    if (s_deps.uart->attach_rx_byte(prv_rx_isr, NULL) != DEBUG_UART_OK)
    {
        LOG_ERROR(MODULE_TAG, "cannot attach the UART receive path");
        return CONSOLE_SERVICE_ERR_APPLY_FAILED;
    }
    if ((s_deps.uart->set_baud != NULL) &&
        (s_deps.uart->set_baud(CONSOLE_LINK_BAUD) != DEBUG_UART_OK))
    {
        LOG_ERROR(MODULE_TAG, "cannot set the console baud rate");
        return CONSOLE_SERVICE_ERR_APPLY_FAILED;
    }
    s_initialised = true;
    return CONSOLE_SERVICE_ERR_OK;
}

void console_task_body(void *arg)
{
    (void) arg;
    s_task = xTaskGetCurrentTaskHandle();
    (void) xTimerStart(s_tick_timer, 0U);

    (void) prv_cycle(NOTIFY_DATA_TICK); /* first frame without waiting for a tick */
    for (;;)
    {
        (void) console_service->run_once();
    }
}

#ifdef TEST
void console_service_reset_for_test(void)
{
    const screen_port_t none = {.send = NULL, .lock = NULL, .unlock = NULL};

    screen_init(&none); /* a previous test's port must not outlive its spies */
    s_initialised = false;
    s_started = false;
    s_task = NULL;
    (void) memset(&s_deps, 0, sizeof(s_deps));
}

console_service_err_t console_service_cycle_for_test(uint32_t notify_bits)
{
    return prv_cycle(notify_bits);
}

void console_service_rx_for_test(const uint8_t *bytes, uint32_t len)
{
    for (uint32_t i = 0U; i < len; i++)
    {
        prv_rx_isr(bytes[i], NULL);
    }
}
#endif /* TEST */
