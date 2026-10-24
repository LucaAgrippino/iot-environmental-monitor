/**
 * @file console_service.c
 * @brief ConsoleService: lifecycle, input path, render loop and task.
 */

#include "console_service.h"

#include <string.h>

#include "FreeRTOS.h"
#include "core/chart.h"
#include "core/commands.h"
#include "core/input.h"
#include "core/model.h"
#include "core/render.h"
#include "core/repl.h"
#include "core/ui_state.h"
#include "port/console_port.h"
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
#define NOTIFY_BLINK (1UL << 2)

#define RING_SIZE (256U) /* power of two: masked-index arithmetic */
#define RING_MASK (RING_SIZE - 1U)
#define TX_TIMEOUT_MS (200U)
#define DATA_TICK_MS (1000U)
#define BLINK_MS (500U)
#define SIZE_QUERY_EVERY_TICKS (5U)
#define MIN_ROWS (30U)
#define MIN_COLS (88U)
#define REBOOT_SETTLE_MS (100U)

#define SEQ_CLEAR "\x1b[2J\x1b[H"
#define SEQ_ENTER "\x1b[?1049h\x1b[?25l\x1b[2J\x1b[H"
#define SEQ_QUERY_SIZE "\x1b[18t"
#define SEQ_LEAVE "\x1b[?25h\x1b[?1049l\x1b[0m"

static console_deps_t s_deps;
static bool s_initialised;
static bool s_started;

static ui_state_t s_ui;
static console_model_t s_model;
static chart_history_t s_history;
static cell_grid_t s_grid_cur;
static cell_grid_t s_grid_prev;

static volatile uint8_t s_ring[RING_SIZE];
static volatile uint16_t s_ring_head; /* written by the ISR */
static uint16_t s_ring_tail;          /* read by the task   */
static volatile bool s_ring_overflow;

static bool s_size_ok = true;
static bool s_redraw;    /* repaint everything on the next frame            */
static bool s_size_seen; /* a terminal has answered the size query          */
static uint16_t s_term_rows;
static uint16_t s_term_cols;
static uint8_t s_tick_count;
static int16_t s_last_sample[CHART_CHANNELS];

static TaskHandle_t s_task;
static TimerHandle_t s_tick_timer;
static TimerHandle_t s_blink_timer;
static StaticTimer_t s_tick_timer_buf;
static StaticTimer_t s_blink_timer_buf;

static console_service_err_t prv_run_once(void);
static const iconsole_service_t s_vtable = {.run_once = prv_run_once};
const iconsole_service_t *const console_service = &s_vtable;

/* ---------------------------------------------------------------------- */
/* Output                                                                  */
/* ---------------------------------------------------------------------- */

static bool prv_sink(const uint8_t *data, size_t len, void *ctx)
{
    (void) ctx;
    return s_deps.uart->send(data, len, TX_TIMEOUT_MS) == DEBUG_UART_OK;
}

/* ---------------------------------------------------------------------- */
/* Receive path                                                            */
/* ---------------------------------------------------------------------- */

static void prv_rx_isr(uint8_t byte, void *ctx)
{
    (void) ctx;
    const uint16_t next = (uint16_t) ((s_ring_head + 1U) & RING_MASK);

    if (next == s_ring_tail)
    {
        s_ring_overflow = true; /* full: drop the byte, tell the operator on the next frame */
    }
    else
    {
        s_ring[s_ring_head] = byte;
        s_ring_head = next;
    }
    if (s_task != NULL)
    {
        BaseType_t woken = pdFALSE;

        (void) xTaskNotifyFromISR(s_task, NOTIFY_RX, eSetBits, &woken);
        portYIELD_FROM_ISR(woken);
    }
}

static void prv_tick_cb(TimerHandle_t t)
{
    (void) t;
    if (s_task != NULL)
    {
        (void) xTaskNotify(s_task, NOTIFY_DATA_TICK, eSetBits);
    }
}

static void prv_blink_cb(TimerHandle_t t)
{
    (void) t;
    if (s_task != NULL)
    {
        (void) xTaskNotify(s_task, NOTIFY_BLINK, eSetBits);
    }
}

/* ---------------------------------------------------------------------- */
/* Key handling                                                            */
/* ---------------------------------------------------------------------- */

static void prv_run_line(void)
{
    char line[UI_INPUT_MAX];

    (void) memcpy(line, s_ui.input, sizeof(line));
    ui_clear_input(&s_ui);
    (void) repl_execute(line);
}

static void prv_apply_action(ui_action_t act)
{
    switch (act)
    {
    case UI_ACT_RUN_LINE:
        prv_run_line();
        break;
    case UI_ACT_LINE_OVERFLOW:
        repl_puts(PAL_ERR, "[ERR] line overflow: input cleared");
        break;
    case UI_ACT_CONFIRM_YES:
        (void) commands_confirm_answer(true);
        break;
    case UI_ACT_CONFIRM_NO:
        (void) commands_confirm_answer(false);
        break;
    default:
        break;
    }
}

static void prv_handle_key(const key_event_t *key)
{
    if (key->kind == KEY_REDRAW)
    {
        s_redraw = true;
        return;
    }
    if (key->kind == KEY_SIZE_REPORT)
    {
        s_size_ok = (key->rows >= MIN_ROWS) && (key->cols >= MIN_COLS);
        /* The first answer proves a terminal is attached, and a changed size means the old
         * picture is wrong: repaint once in either case. */
        if (!s_size_seen || (key->rows != s_term_rows) || (key->cols != s_term_cols))
        {
            s_redraw = true;
        }
        s_size_seen = true;
        s_term_rows = key->rows;
        s_term_cols = key->cols;
        return;
    }
    prv_apply_action(ui_handle_key(&s_ui, key));
    s_ui.confirm_active = commands_confirm_active();
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
/* Data tick                                                               */
/* ---------------------------------------------------------------------- */

static void prv_update_history(void)
{
    int16_t sample[CHART_CHANNELS];

    for (uint8_t ch = 0U; ch < CHART_CHANNELS; ch++)
    {
        int32_t v;

        if (model_value(&s_model, ch, &v))
        {
            s_last_sample[ch] = (int16_t) v;
        }
        sample[ch] = s_last_sample[ch]; /* a failed read repeats the last good value */
    }
    chart_history_push(&s_history, sample);
}

static void prv_data_tick(void)
{
    prv_update_history();
    if (commands_tick() == CONSOLE_SERVICE_ERR_TIMEOUT)
    {
        s_ui.confirm_active = false;
    }
    s_tick_count++;
    if ((s_tick_count % SIZE_QUERY_EVERY_TICKS) == 0U)
    {
        (void) termout_raw(prv_sink, NULL, SEQ_QUERY_SIZE);
    }
}

/* ---------------------------------------------------------------------- */
/* One cycle                                                               */
/* ---------------------------------------------------------------------- */

static void prv_start(void)
{
    (void) termout_raw(prv_sink, NULL, SEQ_ENTER);
    (void) termout_raw(prv_sink, NULL, SEQ_QUERY_SIZE);
    s_started = true;
}

static void prv_render(void)
{
    render_input_t in;

    in.ui = &s_ui;
    in.model = &s_model;
    in.history = &s_history;
    in.size_ok = s_size_ok;
    in.rx_overflow = s_ring_overflow;
    s_ring_overflow = false;
    render_compose(&s_grid_cur, &in);
    if (s_redraw)
    {
        /* Whatever the terminal shows is unknown: wipe it and send the frame from blank. */
        (void) termout_raw(prv_sink, NULL, SEQ_CLEAR);
        grid_clear(&s_grid_prev);
        s_redraw = false;
    }
    (void) termout_flush(&s_grid_cur, &s_grid_prev, prv_sink, NULL);
    (void) memcpy(&s_grid_prev, &s_grid_cur, sizeof(s_grid_prev));
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
        prv_data_tick();
    }
    if ((bits & NOTIFY_BLINK) != 0U)
    {
        s_ui.cursor_on = !s_ui.cursor_on;
    }
    s_ui.confirm_active = commands_confirm_active();

    model_refresh(&s_deps, &s_model); /* a command may have changed what is shown */
    prv_render();

    /* Slow work goes after the frame, so the screen is never stuck behind a flash write. */
    if (commands_take_persist_due())
    {
        (void) s_deps.cfg_write->persist_pending(s_deps.cfg_write->ctx);
    }
    if (commands_take_reboot_request())
    {
        (void) termout_raw(prv_sink, NULL, SEQ_LEAVE);
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
    s_size_ok = true;
    s_redraw = false;
    s_size_seen = false;
    s_term_rows = 0U;
    s_term_cols = 0U;
    s_tick_count = 0U;
    s_ring_head = 0U;
    s_ring_tail = 0U;
    s_ring_overflow = false;
    (void) memset(s_last_sample, 0, sizeof(s_last_sample));
    (void) memset(&s_model, 0, sizeof(s_model));
    grid_clear(&s_grid_cur);
    grid_clear(&s_grid_prev);
    ui_init(&s_ui);
    input_reset();
    chart_history_reset(&s_history);
    repl_init();
    commands_init(&s_deps);

    s_tick_timer = xTimerCreateStatic("con_tick", pdMS_TO_TICKS(DATA_TICK_MS), pdTRUE, NULL,
                                      prv_tick_cb, &s_tick_timer_buf);
    s_blink_timer = xTimerCreateStatic("con_blink", pdMS_TO_TICKS(BLINK_MS), pdTRUE, NULL,
                                       prv_blink_cb, &s_blink_timer_buf);

    if (s_deps.uart->attach_rx_byte(prv_rx_isr, NULL) != DEBUG_UART_OK)
    {
        LOG_ERROR(MODULE_TAG, "cannot attach the UART receive path");
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
    (void) xTimerStart(s_blink_timer, 0U);

    (void) prv_cycle(NOTIFY_DATA_TICK); /* first frame without waiting for a tick */
    for (;;)
    {
        (void) console_service->run_once();
    }
}

#ifdef TEST
void console_service_reset_for_test(void)
{
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

const cell_grid_t *console_service_grid_for_test(void)
{
    return &s_grid_cur;
}
#endif /* TEST */
