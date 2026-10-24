/**
 * @file test_render.c
 * @brief Unity tests for frame composition and the dirty-line writer (companion §22.5).
 */

#include <stdio.h>
#include <string.h>

#include "chart.h"
#include "commands.h"
#include "console_spies.h"
#include "fmt.h"
#include "model.h"
#include "render.h"
#include "repl.h"
#include "termout.h"
#include "theme.h"
#include "ui_state.h"
#include "unity.h"

static ui_state_t s_ui;
static console_model_t s_model;
static chart_history_t s_hist;
static cell_grid_t s_grid;
static console_deps_t s_deps;

void setUp(void)
{
    spy_reset();
    s_deps = spy_deps();
    commands_init(&s_deps);
    repl_init();
    ui_init(&s_ui);
    chart_history_reset(&s_hist);
    grid_clear(&s_grid);
}

void tearDown(void)
{
}

static void compose(bool size_ok, bool overflow)
{
    render_input_t in;

    model_refresh(&s_deps, &s_model);
    in.ui = &s_ui;
    in.model = &s_model;
    in.history = &s_hist;
    in.size_ok = size_ok;
    in.rx_overflow = overflow;
    render_compose(&s_grid, &in);
}

/* Row of the grid as UTF-8 text. */
static void row_text(uint8_t row, char *out, size_t size)
{
    size_t n = 0U;

    for (uint32_t c = 0U; c < GRID_COLS; c++)
    {
        const char *g = glyph_utf8(s_grid.cells[row][c].glyph);

        for (; (g != NULL) && (*g != '\0') && ((n + 1U) < size); g++)
        {
            out[n++] = *g;
        }
    }
    out[n] = '\0';
}

static bool row_has(uint8_t row, const char *needle)
{
    char buf[512];

    row_text(row, buf, sizeof(buf));
    return strstr(buf, needle) != NULL;
}

typedef struct
{
    uint8_t bytes[4096];
    size_t len;
} capture_t;

static bool cap_sink(const uint8_t *d, size_t n, void *ctx)
{
    capture_t *c = (capture_t *) ctx;

    memcpy(&c->bytes[c->len], d, n);
    c->len += n;
    return true;
}

/* ---------------------------------------------------------------------- */
/* Frame composition                                                       */
/* ---------------------------------------------------------------------- */

void test_banner_shows_the_logo_title_and_live_details(void)
{
    compose(true, false);

    TEST_ASSERT_EQUAL_UINT8(GLYPH_TL, s_grid.cells[0][0].glyph);
    TEST_ASSERT_EQUAL_UINT8(GLYPH_BR, s_grid.cells[7][GRID_COLS - 1U].glyph);
    TEST_ASSERT_EQUAL_UINT8(PAL_LOGO, s_grid.cells[1][1].fg);
    TEST_ASSERT_TRUE(row_has(1U, "ENVIRONMENTAL GATEWAY"));
    TEST_ASSERT_TRUE(row_has(5U, "uptime 3d 14:22"));
    TEST_ASSERT_TRUE(row_has(5U, "0 alarms"));
    TEST_ASSERT_TRUE(row_has(6U, "4 / 4 sensors online"));
}

/* TC-GCS-083 */
void test_banner_alarm_count_turns_red_when_an_alarm_is_active(void)
{
    g_spy_alarm_states[SENSOR_ID_TEMPERATURE] = ALARM_STATE_ACTIVE_HIGH;
    compose(true, false);

    TEST_ASSERT_TRUE(row_has(5U, "1 alarms"));
    char buf[512];
    row_text(5U, buf, sizeof(buf));
    const char *p = strstr(buf, "1 alarms");
    TEST_ASSERT_NOT_NULL(p);
    /* Column of the '1': count code points before it. */
    uint8_t col = 0U;
    for (const char *q = buf; q < p; q++)
    {
        if (((uint8_t) *q & 0xC0U) != 0x80U)
        {
            col++;
        }
    }
    TEST_ASSERT_EQUAL_UINT8(PAL_ERR, s_grid.cells[5][col].fg);
}

void test_connectivity_dots_follow_the_health_flags(void)
{
    g_spy_health.cloud_connected = false;
    compose(true, false);
    char buf[512];

    row_text(4U, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "wifi ● "));
    /* Find the mqtt dot: it must be red, the wifi dot green. */
    uint8_t wifi_col = 0U;
    uint8_t mqtt_col = 0U;
    for (uint8_t c = 0U; c < GRID_COLS; c++)
    {
        if (s_grid.cells[4][c].glyph == GLYPH_DOT_FILL)
        {
            (wifi_col == 0U) ? (wifi_col = c) : ((mqtt_col == 0U) ? (mqtt_col = c) : (void) 0);
        }
    }
    TEST_ASSERT_EQUAL_UINT8(PAL_OK, s_grid.cells[4][wifi_col].fg);
    TEST_ASSERT_EQUAL_UINT8(PAL_ERR, s_grid.cells[4][mqtt_col].fg);
}

void test_menu_lists_all_seven_items(void)
{
    compose(true, false);

    TEST_ASSERT_TRUE(row_has(9U, "MAIN MENU"));
    TEST_ASSERT_TRUE(row_has(11U, "Live sensor readings"));
    TEST_ASSERT_TRUE(row_has(12U, "System status"));
    TEST_ASSERT_TRUE(row_has(13U, "Active alarms"));
    TEST_ASSERT_TRUE(row_has(14U, "Thresholds"));
    TEST_ASSERT_TRUE(row_has(15U, "Network"));
    TEST_ASSERT_TRUE(row_has(16U, "Console"));
    TEST_ASSERT_TRUE(row_has(17U, "Firmware · Reboot"));
    TEST_ASSERT_EQUAL_UINT8(GLYPH_PTR_R, s_grid.cells[14][27].glyph); /* deeper-menu marker */
}

/* TC-GCS-082 */
void test_selected_menu_item_has_the_highlight_and_cursor(void)
{
    s_ui.sel = 2U;
    compose(true, false);

    TEST_ASSERT_EQUAL_UINT8(GLYPH_CUR_R, s_grid.cells[13][4].glyph);
    TEST_ASSERT_EQUAL_UINT8(PAL_SEL_BG, s_grid.cells[13][6].bg_attr & CELL_BG_MASK);
    TEST_ASSERT_EQUAL_UINT8(PAL_SEL_BG, s_grid.cells[13][28].bg_attr & CELL_BG_MASK); /* to the inner edge */
    TEST_ASSERT_EQUAL_UINT8(PAL_BG, s_grid.cells[12][6].bg_attr & CELL_BG_MASK);
    TEST_ASSERT_EQUAL_UINT8(PAL_NUM_SEL, s_grid.cells[13][2].fg);
}

/* TC-GCS-080 */
void test_graph_panel_title_is_dim_without_focus(void)
{
    compose(true, false);

    TEST_ASSERT_TRUE(row_has(9U, "LIVE · TEMPERATURE  °C"));
    TEST_ASSERT_EQUAL_UINT8(PAL_TITLE, s_grid.cells[9][35].fg);
    TEST_ASSERT_EQUAL_UINT8(0U, s_grid.cells[9][35].bg_attr & CELL_BOLD);
}

/* TC-GCS-081 */
void test_graph_panel_title_brightens_with_focus(void)
{
    s_ui.focus = FOCUS_PANEL;
    compose(true, false);

    TEST_ASSERT_EQUAL_UINT8(PAL_TITLE_ACT, s_grid.cells[9][35].fg);
    TEST_ASSERT_NOT_EQUAL(0U, s_grid.cells[9][35].bg_attr & CELL_BOLD);
}

void test_graph_panel_shows_axis_value_limit_and_status(void)
{
    int16_t v[CHART_CHANNELS] = {2200, 5000, 10130};

    for (uint32_t i = 0U; i < 30U; i++)
    {
        chart_history_push(&s_hist, v);
    }
    compose(true, false);

    TEST_ASSERT_TRUE(row_has(16U, "└"));
    TEST_ASSERT_TRUE(row_has(17U, "-90s"));
    TEST_ASSERT_TRUE(row_has(17U, "now"));
    TEST_ASSERT_TRUE(row_has(18U, "now 22.0 °C"));
    TEST_ASSERT_TRUE(row_has(18U, "HI 40.0"));
    TEST_ASSERT_TRUE(row_has(18U, "● ok"));
}

void test_graph_value_line_goes_red_past_the_limit(void)
{
    spy_set_reading(SENSOR_ID_TEMPERATURE, 4300, true);
    int16_t v[CHART_CHANNELS] = {4300, 5000, 10130};

    for (uint32_t i = 0U; i < 30U; i++)
    {
        chart_history_push(&s_hist, v);
    }
    compose(true, false);

    TEST_ASSERT_TRUE(row_has(18U, "■ alarm"));
    TEST_ASSERT_TRUE(row_has(18U, "43.0 °C"));
}

void test_graph_shows_stale_when_the_reading_is_invalid(void)
{
    spy_set_reading(SENSOR_ID_PRESSURE, 0, false);
    s_ui.channel = 2U;
    compose(true, false);

    TEST_ASSERT_TRUE(row_has(9U, "PRESSURE"));
    TEST_ASSERT_TRUE(row_has(18U, "▲ stale"));
}

void test_status_alarms_config_network_and_firmware_panels_render(void)
{
    s_ui.sel = 1U;
    compose(true, false);
    TEST_ASSERT_TRUE(row_has(9U, "SYSTEM STATUS"));
    TEST_ASSERT_TRUE(row_has(12U, "● connected  RSSI -42 dBm"));
    TEST_ASSERT_TRUE(row_has(16U, "3d 14:22:05"));

    s_ui.sel = 2U;
    compose(true, false);
    TEST_ASSERT_TRUE(row_has(12U, "No active alarms"));

    g_spy_alarm_states[SENSOR_ID_HUMIDITY] = ALARM_STATE_ACTIVE_LOW;
    spy_set_reading(SENSOR_ID_HUMIDITY, 1500, true);
    compose(true, false);
    TEST_ASSERT_TRUE(row_has(11U, "HUMIDITY"));
    TEST_ASSERT_TRUE(row_has(11U, "LOW"));

    s_ui.sel = 3U;
    compose(true, false);
    TEST_ASSERT_TRUE(row_has(9U, "THRESHOLDS"));
    TEST_ASSERT_TRUE(row_has(11U, "HI 40.0"));
    TEST_ASSERT_TRUE(row_has(15U, "set t.hi 24"));

    s_ui.sel = 4U;
    compose(true, false);
    TEST_ASSERT_TRUE(row_has(9U, "NETWORK"));
    TEST_ASSERT_TRUE(row_has(11U, "-42 dBm"));
    TEST_ASSERT_TRUE(row_has(12U, "(not set):8883"));

    s_ui.sel = 6U;
    compose(true, false);
    TEST_ASSERT_TRUE(row_has(9U, "FIRMWARE · REBOOT"));
    TEST_ASSERT_TRUE(row_has(13U, "STM32L475VGT6"));
}

void test_console_panel_shows_the_tail_prompt_and_blinking_cursor(void)
{
    s_ui.sel = 5U;
    s_ui.focus = FOCUS_PANEL;
    s_ui.cursor_on = true;
    (void) snprintf(s_ui.input, sizeof(s_ui.input), "sens");
    s_ui.input_len = 4U;
    (void) repl_execute("uptime");
    compose(true, false);

    TEST_ASSERT_TRUE(row_has(9U, "CONSOLE"));
    TEST_ASSERT_TRUE(row_has(18U, "envmon> sens"));
    TEST_ASSERT_TRUE(row_has(17U, "uptime 3d 14:22:05"));
    char buf[512];
    row_text(18U, buf, sizeof(buf));
    TEST_ASSERT_NOT_NULL(strstr(buf, "█"));

    s_ui.cursor_on = false;
    compose(true, false);
    row_text(18U, buf, sizeof(buf));
    TEST_ASSERT_NULL(strstr(buf, "█"));
}

void test_console_panel_shows_the_confirm_prompt_instead_of_the_input_line(void)
{
    s_ui.sel = 5U;
    s_ui.focus = FOCUS_PANEL;
    s_ui.confirm_active = true;
    compose(true, false);

    TEST_ASSERT_TRUE(row_has(18U, "Apply? [y/N]:"));
    TEST_ASSERT_FALSE(row_has(18U, "envmon>"));
}

void test_a_long_input_line_keeps_its_tail_visible(void)
{
    s_ui.sel = 5U;
    s_ui.focus = FOCUS_PANEL;
    memset(s_ui.input, 'a', 100U);
    s_ui.input[100] = '\0';
    memcpy(&s_ui.input[96], "END", 4U);
    s_ui.input_len = 99U;
    compose(true, false);

    TEST_ASSERT_TRUE(row_has(18U, "END"));
}

/* TC-GCS-084 */
void test_footer_hint_matches_focus_and_screen(void)
{
    compose(true, false);
    TEST_ASSERT_TRUE(row_has(20U, "↑↓ select · Enter / Tab → panel · 1–7 jump"));
    TEST_ASSERT_TRUE(row_has(20U, "focus MENU · 115200 8N1"));

    s_ui.focus = FOCUS_PANEL;
    compose(true, false);
    TEST_ASSERT_TRUE(row_has(20U, "↑↓ change channel · Esc → menu"));
    TEST_ASSERT_TRUE(row_has(20U, "focus PANEL"));

    s_ui.sel = 5U;
    compose(true, false);
    TEST_ASSERT_TRUE(row_has(20U, "type a command · Enter run · Esc → menu"));

    s_ui.sel = 1U;
    compose(true, false);
    TEST_ASSERT_EQUAL_STRING("Esc → menu", render_hint(&s_ui));
}

void test_rx_overflow_replaces_the_hint_in_the_footer(void)
{
    compose(true, true);

    TEST_ASSERT_TRUE(row_has(20U, "[WARN] RX overflow"));
    TEST_ASSERT_FALSE(row_has(20U, "select"));
}

void test_a_small_terminal_gets_only_the_resize_notice(void)
{
    compose(false, false);

    TEST_ASSERT_TRUE(row_has(10U, "Resize the terminal to at least 88x30"));
    TEST_ASSERT_FALSE(row_has(0U, "┌"));
    TEST_ASSERT_FALSE(row_has(9U, "MAIN MENU"));
}

void test_every_row_stays_inside_the_grid_and_boxes_close(void)
{
    for (uint8_t sel = 0U; sel < UI_MENU_COUNT; sel++)
    {
        s_ui.sel = sel;
        compose(true, false);
        TEST_ASSERT_EQUAL_UINT8(GLYPH_V, s_grid.cells[12][0].glyph);
        TEST_ASSERT_EQUAL_UINT8(GLYPH_V, s_grid.cells[12][GRID_COLS - 1U].glyph);
        TEST_ASSERT_EQUAL_UINT8(GLYPH_BL, s_grid.cells[19][0].glyph);
        TEST_ASSERT_EQUAL_UINT8(GLYPH_BR, s_grid.cells[19][GRID_COLS - 1U].glyph);
        TEST_ASSERT_EQUAL_UINT8(GLYPH_V, s_grid.cells[12][30].glyph == GLYPH_V ? GLYPH_V : s_grid.cells[12][29].glyph);
    }
}

/* ---------------------------------------------------------------------- */
/* Dirty-line writer                                                       */
/* ---------------------------------------------------------------------- */

/* TC-GCS-085 */
void test_flush_of_identical_grids_emits_nothing(void)
{
    static capture_t cap;
    static cell_grid_t a;

    cap.len = 0U;
    compose(true, false);
    a = s_grid;

    TEST_ASSERT_EQUAL_size_t(0U, termout_flush(&s_grid, &a, cap_sink, &cap));
    TEST_ASSERT_EQUAL_size_t(0U, cap.len);
}

/* TC-GCS-086 */
void test_flush_of_one_changed_cell_emits_move_colour_glyph_reset(void)
{
    static capture_t cap;
    static cell_grid_t a;
    static cell_grid_t b;

    cap.len = 0U;
    grid_clear(&a);
    b = a;
    grid_put_glyph(&b, 2U, 4U, (uint8_t) ('Z' - 0x20), PAL_OK, PAL_BG, false);

    const size_t n = termout_flush(&b, &a, cap_sink, &cap);

    TEST_ASSERT_EQUAL_size_t(cap.len, n);
    cap.bytes[cap.len] = 0U;
    TEST_ASSERT_EQUAL_STRING("\x1b[3;5H\x1b[0;38;2;111;191;142mZ\x1b[0m", (char *) cap.bytes);
}

/* TC-GCS-087 */
void test_a_run_of_equal_colour_cells_shares_one_sgr(void)
{
    static capture_t cap;
    static cell_grid_t a;
    static cell_grid_t b;

    cap.len = 0U;
    grid_clear(&a);
    b = a;
    (void) grid_puts(&b, 0U, 0U, "ABCDE", PAL_WARN, PAL_BG, false, 5U);

    (void) termout_flush(&b, &a, cap_sink, &cap);
    cap.bytes[cap.len] = 0U;
    size_t sgr = 0U;

    for (size_t i = 0U; i + 1U < cap.len; i++)
    {
        if ((cap.bytes[i] == 0x1BU) && (cap.bytes[i + 1U] == '[') && (cap.bytes[i + 2U] == '0') && (cap.bytes[i + 3U] == ';'))
        {
            sgr++;
        }
    }
    TEST_ASSERT_EQUAL_size_t(1U, sgr);
    TEST_ASSERT_NOT_NULL(strstr((char *) cap.bytes, "ABCDE"));
}

void test_a_colour_change_inside_a_span_adds_one_more_sgr_and_bold_is_emitted(void)
{
    static capture_t cap;
    static cell_grid_t a;
    static cell_grid_t b;

    cap.len = 0U;
    grid_clear(&a);
    b = a;
    (void) grid_puts(&b, 0U, 0U, "AB", PAL_OK, PAL_BG, false, 2U);
    (void) grid_puts(&b, 0U, 2U, "CD", PAL_ERR, PAL_BG, true, 2U);

    (void) termout_flush(&b, &a, cap_sink, &cap);
    cap.bytes[cap.len] = 0U;

    TEST_ASSERT_NOT_NULL(strstr((char *) cap.bytes, "\x1b[0;1;38;2;204;102;102m"));
    TEST_ASSERT_NOT_NULL(strstr((char *) cap.bytes, "AB\x1b[0;1;38"));
}

void test_two_distant_changes_use_two_cursor_moves(void)
{
    static capture_t cap;
    static cell_grid_t a;
    static cell_grid_t b;

    cap.len = 0U;
    grid_clear(&a);
    b = a;
    grid_put_glyph(&b, 0U, 1U, 1U, PAL_OK, PAL_BG, false);
    grid_put_glyph(&b, 0U, 60U, 1U, PAL_OK, PAL_BG, false);

    (void) termout_flush(&b, &a, cap_sink, &cap);
    cap.bytes[cap.len] = 0U;

    TEST_ASSERT_NOT_NULL(strstr((char *) cap.bytes, "\x1b[1;2H"));
    TEST_ASSERT_NOT_NULL(strstr((char *) cap.bytes, "\x1b[1;61H"));
}

void test_a_cell_cleared_back_to_blank_is_erased_with_a_space(void)
{
    static capture_t cap;
    static cell_grid_t a;
    static cell_grid_t b;

    cap.len = 0U;
    grid_clear(&b);
    a = b;
    grid_put_glyph(&a, 1U, 1U, 5U, PAL_OK, PAL_BG, false);

    (void) termout_flush(&b, &a, cap_sink, &cap);
    cap.bytes[cap.len] = 0U;

    TEST_ASSERT_NOT_NULL(strstr((char *) cap.bytes, "\x1b[2;2H "));
}

static bool failing_sink(const uint8_t *d, size_t n, void *ctx)
{
    (void) d;
    (void) n;
    (*(uint32_t *) ctx)++;
    return false;
}

void test_a_failing_sink_stops_the_emission(void)
{
    static cell_grid_t a;
    uint32_t calls = 0U;

    grid_clear(&a);
    compose(true, false);
    (void) termout_flush(&s_grid, &a, failing_sink, &calls);

    TEST_ASSERT_EQUAL_UINT32(1U, calls);
}

void test_glyph_table_maps_utf8_in_both_directions(void)
{
    cell_grid_t g;

    grid_clear(&g);
    TEST_ASSERT_EQUAL_size_t(5U, grid_puts(&g, 0U, 0U, "a█°é▲", PAL_INK, PAL_BG, false, 10U));
    TEST_ASSERT_EQUAL_UINT8('a' - 0x20, g.cells[0][0].glyph);
    TEST_ASSERT_EQUAL_UINT8(GLYPH_BLK8, g.cells[0][1].glyph);
    TEST_ASSERT_EQUAL_UINT8(GLYPH_DEGREE, g.cells[0][2].glyph);
    TEST_ASSERT_EQUAL_UINT8('?' - 0x20, g.cells[0][3].glyph); /* unknown code point */
    TEST_ASSERT_EQUAL_UINT8(GLYPH_TRI_UP, g.cells[0][4].glyph);
    TEST_ASSERT_EQUAL_STRING("█", glyph_utf8(GLYPH_BLK8));
    TEST_ASSERT_NULL(glyph_utf8(255U));
}

void test_grid_puts_clips_and_ignores_out_of_range_cells(void)
{
    cell_grid_t g;

    grid_clear(&g);
    TEST_ASSERT_EQUAL_size_t(3U, grid_puts(&g, 0U, 0U, "abcdef", PAL_INK, PAL_BG, false, 3U));
    TEST_ASSERT_EQUAL_size_t(2U, grid_puts(&g, 0U, GRID_COLS - 2U, "abcdef", PAL_INK, PAL_BG, false, 10U));
    grid_put_glyph(&g, GRID_ROWS, 0U, 1U, PAL_INK, PAL_BG, false); /* must not crash */
    grid_put_glyph(&g, 0U, GRID_COLS, 1U, PAL_INK, PAL_BG, false);
    TEST_ASSERT_EQUAL_UINT8(0U, g.cells[0][3].glyph);
}
