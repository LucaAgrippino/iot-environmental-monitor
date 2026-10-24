/**
 * @file render.c
 * @brief Composes the cockpit screen into a cell grid.
 */

#include "render.h"

#include <stdio.h>
#include <string.h>

#include "firmware_version.h"
#include "fmt.h"
#include "repl.h"

#define BANNER_INNER (86U)
#define MENU_INNER (28U)
#define PANEL_INNER (55U)
#define LOGO_W (57U)
#define LOGO_GAP (3U)

#define CHART_GUTTER (7U)    /* label 4 + space + axis + space */
#define AXIS_COL (5U)        /* offset of the axis glyph inside the panel interior */
#define HINT_RIGHT_COL (60U) /* the hint is clipped here, leaving room for the focus text */
#define MIN_COLS_NOTICE_ROW (10U)

static const char *const s_logo[6] = {
    "███████╗███╗   ██╗██╗   ██╗███╗   ███╗ ██████╗ ███╗   ██╗",
    "██╔════╝████╗  ██║██║   ██║████╗ ████║██╔═══██╗████╗  ██║",
    "█████╗  ██╔██╗ ██║██║   ██║██╔████╔██║██║   ██║██╔██╗ ██║",
    "██╔══╝  ██║╚██╗██║╚██╗ ██╔╝██║╚██╔╝██║██║   ██║██║╚██╗██║",
    "███████╗██║ ╚████║ ╚████╔╝ ██║ ╚═╝ ██║╚██████╔╝██║ ╚████║",
    "╚══════╝╚═╝  ╚═══╝  ╚═══╝  ╚═╝     ╚═╝ ╚═════╝ ╚═╝  ╚═══╝",
};

static const struct
{
    const char *label;
    bool deeper; /* shows the ► marker: only for items that open something to work in */
} s_menu[UI_MENU_COUNT] = {
    {"Live sensor readings", true}, {"System status", false}, {"Active alarms", false},
    {"Thresholds", false},          {"Network", false},       {"Console", true},
    {"Firmware · Reboot", true},
};

/* ---------------------------------------------------------------------- */
/* Drawing helpers                                                         */
/* ---------------------------------------------------------------------- */

/* Write @p text at (*col) in @p fg and advance the column, clipped at @p limit (exclusive). */
static void prv_seg(cell_grid_t *g, uint8_t row, uint8_t *col, uint8_t limit, const char *text,
                    uint8_t fg)
{
    if (*col < limit)
    {
        *col = (uint8_t) (*col + grid_puts(g, row, *col, text, fg, PAL_BG, false,
                                           (size_t) (limit - *col)));
    }
}

static void prv_box_top(cell_grid_t *g, uint8_t row, uint8_t col, uint8_t width, const char *title,
                        bool active)
{
    uint8_t c = col;

    grid_put_glyph(g, row, c, (uint8_t) GLYPH_TL, PAL_BORDER, PAL_BG, false);
    if (title == NULL)
    {
        grid_fill(g, row, (uint8_t) (col + 1U), (uint8_t) GLYPH_H, (size_t) (width - 2U),
                  PAL_BORDER, PAL_BG, false);
    }
    else
    {
        c = (uint8_t) (col + 1U);
        c = (uint8_t) (c + grid_puts(g, row, c, "─[ ", PAL_BORDER, PAL_BG, false, 3U));
        c = (uint8_t) (c + grid_puts(g, row, c, title, active ? PAL_TITLE_ACT : PAL_TITLE, PAL_BG,
                                     active, 40U));
        c = (uint8_t) (c + grid_puts(g, row, c, " ]", PAL_BORDER, PAL_BG, false, 2U));
        if (c < (col + width - 1U))
        {
            grid_fill(g, row, c, (uint8_t) GLYPH_H, (size_t) ((col + width - 1U) - c), PAL_BORDER,
                      PAL_BG, false);
        }
    }
    grid_put_glyph(g, row, (uint8_t) (col + width - 1U), (uint8_t) GLYPH_TR, PAL_BORDER, PAL_BG,
                   false);
}

static void prv_box_bottom(cell_grid_t *g, uint8_t row, uint8_t col, uint8_t width)
{
    grid_put_glyph(g, row, col, (uint8_t) GLYPH_BL, PAL_BORDER, PAL_BG, false);
    grid_fill(g, row, (uint8_t) (col + 1U), (uint8_t) GLYPH_H, (size_t) (width - 2U), PAL_BORDER,
              PAL_BG, false);
    grid_put_glyph(g, row, (uint8_t) (col + width - 1U), (uint8_t) GLYPH_BR, PAL_BORDER, PAL_BG,
                   false);
}

static void prv_box_sides(cell_grid_t *g, uint8_t row, uint8_t col, uint8_t width)
{
    grid_put_glyph(g, row, col, (uint8_t) GLYPH_V, PAL_BORDER, PAL_BG, false);
    grid_put_glyph(g, row, (uint8_t) (col + width - 1U), (uint8_t) GLYPH_V, PAL_BORDER, PAL_BG,
                   false);
}

/* ---------------------------------------------------------------------- */
/* Banner                                                                  */
/* ---------------------------------------------------------------------- */

static void prv_banner(cell_grid_t *g, const console_model_t *m)
{
    char buf[40];
    char up[24];

    prv_box_top(g, 0U, 0U, (uint8_t) GRID_COLS, NULL, false);
    for (uint8_t r = 1U; r <= 6U; r++)
    {
        prv_box_sides(g, r, 0U, (uint8_t) GRID_COLS);
        (void) grid_puts(g, r, 1U, s_logo[r - 1U], PAL_LOGO, PAL_BG, false, LOGO_W);
    }
    prv_box_bottom(g, 7U, 0U, (uint8_t) GRID_COLS);

    const uint8_t x0 = (uint8_t) (1U + LOGO_W + LOGO_GAP);
    const uint8_t limit = (uint8_t) (GRID_COLS - 1U);
    uint8_t c;

    c = x0;
    prv_seg(g, 1U, &c, limit, "ENVIRONMENTAL GATEWAY", PAL_MUTED);
    c = x0;
    prv_seg(g, 2U, &c, limit, "─────────────────────", PAL_BORDER);

    c = x0;
    (void) snprintf(buf, sizeof(buf), "%u.%u.%u", (unsigned) FW_VERSION_MAJOR,
                    (unsigned) FW_VERSION_MINOR, (unsigned) FW_VERSION_PATCH);
    prv_seg(g, 3U, &c, limit, "fw ", PAL_MUTED);
    prv_seg(g, 3U, &c, limit, buf, PAL_ACCENT);
    prv_seg(g, 3U, &c, limit, " · STM32L475", PAL_MUTED);

    const bool wifi = m->health.wifi_rssi_dbm < 0;

    c = x0;
    prv_seg(g, 4U, &c, limit, "wifi ", PAL_MUTED);
    prv_seg(g, 4U, &c, limit, "●", wifi ? PAL_OK : PAL_ERR);
    prv_seg(g, 4U, &c, limit, "  mqtt ", PAL_MUTED);
    prv_seg(g, 4U, &c, limit, "●", m->health.cloud_connected ? PAL_OK : PAL_ERR);
    prv_seg(g, 4U, &c, limit, "  modbus ", PAL_MUTED);
    prv_seg(g, 4U, &c, limit, "●", m->health.modbus_link_online ? PAL_OK : PAL_ERR);

    const uint8_t alarms = model_alarm_count(m);

    c = x0;
    model_uptime_short(m, up, sizeof(up));
    prv_seg(g, 5U, &c, limit, "uptime ", PAL_MUTED);
    prv_seg(g, 5U, &c, limit, up, PAL_ACCENT);
    prv_seg(g, 5U, &c, limit, " · ", PAL_MUTED);
    (void) snprintf(buf, sizeof(buf), "%u alarms", (unsigned) alarms);
    prv_seg(g, 5U, &c, limit, buf, (alarms > 0U) ? PAL_ERR : PAL_OK);

    c = x0;
    (void) snprintf(buf, sizeof(buf), "%u / 4 sensors online", (unsigned) model_sensors_online(m));
    prv_seg(g, 6U, &c, limit, "● ", PAL_OK);
    prv_seg(g, 6U, &c, limit, buf, PAL_MUTED);
}

/* ---------------------------------------------------------------------- */
/* Menu rail                                                               */
/* ---------------------------------------------------------------------- */

static void prv_menu(cell_grid_t *g, const ui_state_t *ui)
{
    prv_box_top(g, R_BOX_ROW, 0U, (uint8_t) R_MENU_W, "MAIN MENU", false);
    prv_box_sides(g, R_BOX_ROW + 1U, 0U, (uint8_t) R_MENU_W);
    for (uint8_t i = 0U; i < UI_MENU_COUNT; i++)
    {
        const uint8_t row = (uint8_t) (R_BOX_ROW + 2U + i);
        const bool sel = (i == ui->sel);
        uint8_t c = 1U;
        char num[8];

        prv_box_sides(g, row, 0U, (uint8_t) R_MENU_W);
        (void) snprintf(num, sizeof(num), " %u ", (unsigned) (i + 1U));
        prv_seg(g, row, &c, (uint8_t) (R_MENU_W - 1U), num, sel ? PAL_NUM_SEL : PAL_NUM);
        prv_seg(g, row, &c, (uint8_t) (R_MENU_W - 1U), sel ? "▸ " : "  ", PAL_ACCENT);
        if (sel)
        {
            char label[40];

            (void) fmt_pad(label, sizeof(label), s_menu[i].label, MENU_INNER - 5U);
            (void) grid_puts(g, row, c, label, PAL_SEL_FG, PAL_SEL_BG, false, MENU_INNER - 5U);
        }
        else
        {
            prv_seg(g, row, &c, (uint8_t) (R_MENU_W - 1U), s_menu[i].label, PAL_INK);
            if (s_menu[i].deeper)
            {
                (void) grid_puts(g, row, (uint8_t) (R_MENU_W - 3U), "►", PAL_ACCENT, PAL_BG, false,
                                 1U);
            }
        }
    }
    prv_box_sides(g, R_BOX_ROW + 9U, 0U, (uint8_t) R_MENU_W);
    prv_box_bottom(g, R_BOX_ROW + 10U, 0U, (uint8_t) R_MENU_W);
}

/* ---------------------------------------------------------------------- */
/* Panels                                                                  */
/* ---------------------------------------------------------------------- */

static void prv_panel_frame(cell_grid_t *g, const char *title, bool active)
{
    prv_box_top(g, R_BOX_ROW, R_PANEL_COL, (uint8_t) R_PANEL_W, title, active);
    for (uint8_t r = 1U; r < (R_BOX_ROWS - 1U); r++)
    {
        prv_box_sides(g, (uint8_t) (R_BOX_ROW + r), R_PANEL_COL, (uint8_t) R_PANEL_W);
    }
    prv_box_bottom(g, R_BOX_ROW + R_BOX_ROWS - 1U, R_PANEL_COL, (uint8_t) R_PANEL_W);
}

/* Start column of a panel's interior text. */
static uint8_t prv_in_col(void)
{
    return (uint8_t) (R_PANEL_COL + 1U);
}

/* One interior row (1..9) of a text panel: label (INK) then value, both clipped to the box. */
static void prv_kv(cell_grid_t *g, uint8_t line, const char *label, const char *value, uint8_t vfg)
{
    const uint8_t row = (uint8_t) (R_BOX_ROW + line);
    const uint8_t limit = (uint8_t) (R_PANEL_COL + R_PANEL_W - 1U);
    uint8_t c = prv_in_col();

    prv_seg(g, row, &c, limit, label, PAL_INK);
    prv_seg(g, row, &c, limit, value, vfg);
}

static void prv_heading(cell_grid_t *g, uint8_t line, const char *text)
{
    uint8_t c = prv_in_col();

    prv_seg(g, (uint8_t) (R_BOX_ROW + line), &c, (uint8_t) (R_PANEL_COL + R_PANEL_W - 1U), text,
            PAL_MUTED);
}

static void prv_value_with_unit(char *buf, size_t size, const console_model_t *m, uint8_t ch,
                                bool *valid)
{
    int32_t v = 0;
    const channel_info_t *info = model_channel(ch);

    *valid = model_value(m, ch, &v);
    if (*valid)
    {
        char num[16];

        (void) fmt_fixed(num, sizeof(num), v, info->scale, info->decimals);
        (void) snprintf(buf, size, "%s %s", num, info->unit);
    }
    else
    {
        (void) snprintf(buf, size, "—");
    }
}

static const char *prv_chip_text(chart_status_t st)
{
    switch (st)
    {
    case CHART_ST_STALE:
        return "▲ stale";
    case CHART_ST_ALARM:
        return "■ alarm";
    case CHART_ST_NEAR:
        return "▲ near";
    default:
        return "● ok";
    }
}

static uint8_t prv_chip_fg(chart_status_t st)
{
    return (st == CHART_ST_ALARM) ? PAL_ERR : ((st == CHART_ST_OK) ? PAL_OK : PAL_WARN);
}

static void prv_graph(cell_grid_t *g, const render_input_t *in)
{
    const uint8_t ch = in->ui->channel;
    const channel_info_t *info = model_channel(ch);
    const chart_limits_t lim = model_limits(in->model, ch);
    const bool active = (in->ui->focus == FOCUS_PANEL);
    char title[48];
    int32_t data[CHART_COLS];

    (void) snprintf(title, sizeof(title), "LIVE · %s  %s", info->label, info->unit);
    prv_panel_frame(g, title, active);

    chart_resample(in->history, ch, data);
    const uint8_t base_col = (uint8_t) (prv_in_col() + CHART_GUTTER);
    const chart_range_t range = chart_render(g, (uint8_t) (R_BOX_ROW + 1U), base_col, data, &lim);

    /* Y axis: labels on the top and bottom rows, ticks every other row. */
    static const uint8_t axis[CHART_ROWS] = {GLYPH_TEE_R, GLYPH_V,     GLYPH_TEE_R,
                                             GLYPH_V,     GLYPH_TEE_R, GLYPH_V};

    for (uint8_t r = 0U; r < CHART_ROWS; r++)
    {
        const uint8_t row = (uint8_t) (R_BOX_ROW + 1U + r);

        grid_put_glyph(g, row, (uint8_t) (prv_in_col() + AXIS_COL), axis[r], PAL_AXIS, PAL_BG,
                       false);
    }
    char lab[12];
    char padded[12];

    (void) fmt_fixed(lab, sizeof(lab), range.hi, info->scale, 0U);
    (void) snprintf(padded, sizeof(padded), "%4s", lab);
    (void) grid_puts(g, R_BOX_ROW + 1U, prv_in_col(), padded, PAL_AXIS, PAL_BG, false, 4U);
    (void) fmt_fixed(lab, sizeof(lab), range.lo, info->scale, 0U);
    (void) snprintf(padded, sizeof(padded), "%4s", lab);
    (void) grid_puts(g, R_BOX_ROW + CHART_ROWS, prv_in_col(), padded, PAL_AXIS, PAL_BG, false, 4U);

    /* Baseline and time axis. */
    const uint8_t base_row = (uint8_t) (R_BOX_ROW + 1U + CHART_ROWS);

    grid_put_glyph(g, base_row, (uint8_t) (prv_in_col() + AXIS_COL), (uint8_t) GLYPH_BL, PAL_AXIS,
                   PAL_BG, false);
    grid_fill(g, base_row, (uint8_t) (prv_in_col() + AXIS_COL + 1U), (uint8_t) GLYPH_H,
              CHART_COLS + 1U, PAL_AXIS, PAL_BG, false);
    (void) grid_puts(g, (uint8_t) (base_row + 1U), base_col, "-90s", PAL_AXIS, PAL_BG, false, 4U);
    (void) grid_puts(g, (uint8_t) (base_row + 1U), (uint8_t) (base_col + 15U), "-60s", PAL_AXIS,
                     PAL_BG, false, 4U);
    (void) grid_puts(g, (uint8_t) (base_row + 1U), (uint8_t) (base_col + 30U), "-30s", PAL_AXIS,
                     PAL_BG, false, 4U);
    (void) grid_puts(g, (uint8_t) (base_row + 1U), (uint8_t) (base_col + 43U), "now", PAL_AXIS,
                     PAL_BG, false, 3U);

    /* Value line: latest value (heated by proximity), limit and status chip. */
    const uint8_t vrow = (uint8_t) (base_row + 2U);
    const uint8_t limit = (uint8_t) (R_PANEL_COL + R_PANEL_W - 1U);
    uint8_t c = prv_in_col();
    char val[24];
    bool valid;
    int32_t latest = 0;

    prv_value_with_unit(val, sizeof(val), in->model, ch, &valid);
    (void) model_value(in->model, ch, &latest);
    const int32_t t = valid ? chart_proximity_q8(latest, &lim) : 0;
    const chart_status_t st = chart_status(t, !valid);

    prv_seg(g, vrow, &c, limit, "  now ", PAL_MUTED);
    if (c < limit)
    {
        c = (uint8_t) (c + grid_puts(g, vrow, c, val, theme_prox_index(t), PAL_BG,
                                     theme_prox_bold(t), (size_t) (limit - c)));
    }
    /* Show the limit the latest value is closest to. */
    const bool show_hi = lim.has_hi && (!lim.has_lo || ((lim.hi - latest) <= (latest - lim.lo)));
    char lim_txt[24];
    char num[16];

    (void) fmt_fixed(num, sizeof(num), show_hi ? lim.hi : lim.lo, info->scale, 1U);
    (void) snprintf(lim_txt, sizeof(lim_txt), "%s %s", show_hi ? "HI" : "LO", num);
    prv_seg(g, vrow, &c, limit, "   ", PAL_BG);
    prv_seg(g, vrow, &c, limit, lim_txt, PAL_WARN);
    prv_seg(g, vrow, &c, limit, "   ", PAL_BG);
    prv_seg(g, vrow, &c, limit, prv_chip_text(st), prv_chip_fg(st));
}

static void prv_status(cell_grid_t *g, const render_input_t *in, bool active)
{
    const console_model_t *m = in->model;
    char buf[40];
    char up[24];

    prv_panel_frame(g, "SYSTEM STATUS", active);
    model_uptime_long(m, up, sizeof(up));
    prv_heading(g, 2U, "  CONNECTIVITY");
    (void) snprintf(buf, sizeof(buf), "%s  RSSI %ld dBm",
                    (m->health.wifi_rssi_dbm < 0) ? "● connected" : "● down",
                    (long) m->health.wifi_rssi_dbm);
    prv_kv(g, 3U, "  WiFi         ", buf, (m->health.wifi_rssi_dbm < 0) ? PAL_OK : PAL_ERR);
    (void) snprintf(buf, sizeof(buf), "%s  %lu sent / %lu failed",
                    m->health.cloud_connected ? "● connected" : "● down",
                    (unsigned long) m->health.mqtt_publishes_sent,
                    (unsigned long) m->health.mqtt_publishes_failed);
    prv_kv(g, 4U, "  MQTT         ", buf, m->health.cloud_connected ? PAL_OK : PAL_ERR);
    (void) snprintf(buf, sizeof(buf), "%s  %lu ok / %lu timeouts",
                    m->health.modbus_link_online ? "● online" : "● offline",
                    (unsigned long) m->health.modbus_transactions_ok,
                    (unsigned long) m->health.modbus_timeouts);
    prv_kv(g, 5U, "  Modbus       ", buf, m->health.modbus_link_online ? PAL_OK : PAL_WARN);
    prv_heading(g, 6U, "  SYSTEM");
    prv_kv(g, 7U, "  Uptime       ", up, PAL_NUM);
    (void) snprintf(buf, sizeof(buf), "%u / 4 online", (unsigned) model_sensors_online(m));
    prv_kv(g, 8U, "  Sensors      ", buf, PAL_NUM);
    (void) snprintf(buf, sizeof(buf), "%lu entries", (unsigned long) m->health.buffer_entry_count);
    prv_kv(g, 9U, "  S&F buffer   ", buf, PAL_NUM);
}

static void prv_alarms(cell_grid_t *g, const render_input_t *in, bool active)
{
    const console_model_t *m = in->model;
    uint8_t line = 2U;

    prv_panel_frame(g, "ACTIVE ALARMS", active);
    for (uint8_t ch = 0U; ch < CHART_CHANNELS; ch++)
    {
        const alarm_state_t st = m->alarm_state[model_channel(ch)->id];

        if (st == ALARM_STATE_CLEAR)
        {
            continue;
        }
        const chart_limits_t lim = model_limits(m, ch);
        const channel_info_t *info = model_channel(ch);
        char v[16];
        char t[16];
        char buf[48];
        int32_t val = 0;

        (void) model_value(m, ch, &val);
        (void) fmt_fixed(v, sizeof(v), val, info->scale, info->decimals);
        (void) fmt_fixed(t, sizeof(t), (st == ALARM_STATE_ACTIVE_HIGH) ? lim.hi : lim.lo,
                         info->scale, info->decimals);
        (void) snprintf(buf, sizeof(buf), "%s  %s / %s %s",
                        (st == ALARM_STATE_ACTIVE_HIGH) ? "HIGH" : "LOW ", v, t, info->unit);
        prv_kv(g, line, "  ", info->label, PAL_INK);
        {
            uint8_t c = (uint8_t) (prv_in_col() + 15U);

            prv_seg(g, (uint8_t) (R_BOX_ROW + line), &c, (uint8_t) (R_PANEL_COL + R_PANEL_W - 1U),
                    buf, PAL_ERR);
        }
        line++;
    }
    if (line == 2U)
    {
        prv_kv(g, 3U, "     ", "", PAL_INK);
        {
            uint8_t c = (uint8_t) (prv_in_col() + 5U);
            const uint8_t limit = (uint8_t) (R_PANEL_COL + R_PANEL_W - 1U);

            prv_seg(g, R_BOX_ROW + 3U, &c, limit, "● ", PAL_OK);
            prv_seg(g, R_BOX_ROW + 3U, &c, limit, "No active alarms", PAL_INK);
        }
        prv_kv(g, 4U, "     ", "All channels within thresholds.", PAL_MUTED);
    }
}

static void prv_config(cell_grid_t *g, const render_input_t *in, bool active)
{
    const console_model_t *m = in->model;

    prv_panel_frame(g, "THRESHOLDS", active);
    for (uint8_t ch = 0U; ch < CHART_CHANNELS; ch++)
    {
        const chart_limits_t lim = model_limits(m, ch);
        const channel_info_t *info = model_channel(ch);
        char hi[16];
        char lo[16];
        char hi_pad[16];
        char name[12];
        const uint8_t row = (uint8_t) (R_BOX_ROW + 2U + ch);
        uint8_t c = prv_in_col();
        const uint8_t limit = (uint8_t) (R_PANEL_COL + R_PANEL_W - 1U);

        (void) fmt_fixed(hi, sizeof(hi), lim.hi, info->scale, 1U);
        (void) fmt_fixed(lo, sizeof(lo), lim.lo, info->scale, 1U);
        (void) fmt_pad(hi_pad, sizeof(hi_pad), hi, 9U);
        (void) fmt_pad(name, sizeof(name), info->name, 8U);
        prv_seg(g, row, &c, limit, "  ", PAL_BG);
        prv_seg(g, row, &c, limit, name, PAL_INK);
        prv_seg(g, row, &c, limit, "HI ", PAL_MUTED);
        prv_seg(g, row, &c, limit, hi_pad, PAL_NUM);
        prv_seg(g, row, &c, limit, "LO ", PAL_MUTED);
        prv_seg(g, row, &c, limit, lo, PAL_NUM);
        prv_seg(g, row, &c, limit, " ", PAL_BG);
        prv_seg(g, row, &c, limit, info->unit, PAL_MUTED);
    }
    {
        uint8_t c = prv_in_col();
        const uint8_t limit = (uint8_t) (R_PANEL_COL + R_PANEL_W - 1U);

        prv_seg(g, R_BOX_ROW + 6U, &c, limit, "  edit via console: ", PAL_MUTED);
        prv_seg(g, R_BOX_ROW + 6U, &c, limit, "set t.hi 24", PAL_ACCENT);
    }
}

static void prv_network(cell_grid_t *g, const render_input_t *in, bool active)
{
    const console_model_t *m = in->model;
    char buf[48];

    prv_panel_frame(g, "NETWORK", active);
    (void) snprintf(buf, sizeof(buf), "%ld dBm", (long) m->health.wifi_rssi_dbm);
    prv_kv(g, 2U, "  WiFi RSSI    ", buf, (m->health.wifi_rssi_dbm < 0) ? PAL_NUM : PAL_ERR);
    if (m->cfg != NULL)
    {
        (void) snprintf(buf, sizeof(buf), "%s:%u",
                        (m->cfg->mqtt_broker[0] != '\0') ? m->cfg->mqtt_broker : "(not set)",
                        (unsigned) m->cfg->mqtt_port);
        prv_kv(g, 3U, "  MQTT broker  ", buf, PAL_NUM);
    }
    prv_kv(g, 4U, "  MQTT state   ", m->health.cloud_connected ? "● connected" : "● down",
           m->health.cloud_connected ? PAL_OK : PAL_ERR);
    prv_kv(g, 5U, "  Modbus       ", m->health.modbus_link_online ? "● online" : "● offline",
           m->health.modbus_link_online ? PAL_OK : PAL_WARN);
    (void) snprintf(buf, sizeof(buf), "%lu reconnects",
                    (unsigned long) m->health.mqtt_reconnect_count);
    prv_kv(g, 6U, "  Reconnects   ", buf, PAL_NUM);
    (void) snprintf(buf, sizeof(buf), "%lu entries", (unsigned long) m->health.buffer_entry_count);
    prv_kv(g, 7U, "  S&F buffer   ", buf, PAL_NUM);
    prv_kv(g, 8U, "  SSID / IP    ", "not reported yet", PAL_MUTED);
}

static void prv_action(cell_grid_t *g, uint8_t line, const char *text, bool selected)
{
    const uint8_t row = (uint8_t) (R_BOX_ROW + line);
    const uint8_t limit = (uint8_t) (R_PANEL_COL + R_PANEL_W - 1U);
    uint8_t c = (uint8_t) (prv_in_col() + 2U);
    char label[40];

    (void) snprintf(label, sizeof(label), " %-24s", text);
    if (selected)
    {
        prv_seg(g, row, &c, limit, "▸", PAL_ACCENT);
        (void) grid_puts(g, row, c, label, PAL_SEL_FG, PAL_SEL_BG, false, (size_t) (limit - c));
    }
    else
    {
        c = (uint8_t) (c + 1U);
        prv_seg(g, row, &c, limit, label, PAL_INK);
    }
}

static void prv_firmware(cell_grid_t *g, const render_input_t *in, bool active)
{
    char buf[48];
    const bool sel_on = active && !in->ui->confirm_active;
    const char *check = in->fw_check;

    prv_panel_frame(g, "FIRMWARE · REBOOT", active);
    (void) snprintf(buf, sizeof(buf), "%s", FW_VERSION_STRING);
    prv_kv(g, 2U, "  installed    ", buf, PAL_NUM);
    (void) snprintf(buf, sizeof(buf), "%s %s", FW_BUILD_DATE, FW_BUILD_TIME);
    prv_kv(g, 3U, "  built        ", buf, PAL_MUTED);
    prv_kv(g, 4U, "  mcu          ", "STM32L475VGT6", PAL_NUM);
    prv_action(g, 6U, "Check for update", sel_on && (in->ui->fw_sel == 0U));
    prv_action(g, 7U, "Reboot gateway", sel_on && (in->ui->fw_sel == 1U));
    if (in->ui->confirm_active && in->ui->confirm_reboot)
    {
        prv_kv(g, 9U, "  ", "Reboot the gateway now? [y/N]", PAL_WARN);
    }
    else if (check != NULL)
    {
        prv_kv(g, 9U, "  update       ", check, PAL_ERR);
    }
}

static void prv_console(cell_grid_t *g, const render_input_t *in)
{
    const ui_state_t *ui = in->ui;
    const bool active = (ui->focus == FOCUS_PANEL);
    const uint8_t limit = (uint8_t) (R_PANEL_COL + R_PANEL_W - 1U);
    const uint16_t total_all = repl_count();
    const uint16_t scroll = (ui->scroll < total_all) ? ui->scroll : total_all;
    const uint16_t total = (uint16_t) (total_all - scroll); /* newest line of the window */
    const uint16_t shown = (total < REPL_VISIBLE_LINES) ? total : (uint16_t) REPL_VISIBLE_LINES;

    prv_panel_frame(g, "CONSOLE", active);

    /* Newest REPL_VISIBLE_LINES lines, bottom-aligned above the input line. */
    for (uint16_t i = 0U; i < shown; i++)
    {
        const scrollback_line_t *l = repl_line((uint16_t) (total - shown + i));
        const uint8_t row = (uint8_t) (R_BOX_ROW + 1U + (REPL_VISIBLE_LINES - shown) + i);
        uint8_t c = (uint8_t) (prv_in_col() + 1U); /* one space of padding inside the box */

        if (l != NULL)
        {
            prv_seg(g, row, &c, limit, l->text, l->fg);
        }
    }

    /* Live input line, or the y/N prompt. */
    const uint8_t irow = (uint8_t) (R_BOX_ROW + 1U + REPL_VISIBLE_LINES);
    uint8_t c = (uint8_t) (prv_in_col() + 1U);

    if (ui->confirm_active)
    {
        prv_seg(g, irow, &c, limit, "Apply? [y/N]: ", PAL_WARN);
    }
    else
    {
        prv_seg(g, irow, &c, limit, "envmon", PAL_PROMPT);
        prv_seg(g, irow, &c, limit, "> ", PAL_MUTED);
        /* Keep the tail of a long line visible. */
        const size_t room = (size_t) (limit - c) - 1U;
        const size_t len = ui->input_len;
        const char *text = (len > room) ? &ui->input[len - room] : ui->input;

        prv_seg(g, irow, &c, limit, text, PAL_INK);
    }
    if (active && ui->cursor_on && (c < limit))
    {
        grid_put_glyph(g, irow, c, GLYPH_FULL, PAL_ACCENT, PAL_BG, false);
    }
    if (scroll > 0U)
    {
        char more[16];
        uint8_t mc;

        (void) snprintf(more, sizeof(more), "↓ %u more", (unsigned) scroll);
        mc = (uint8_t) (limit - fmt_cols(more) - 1U);
        prv_seg(g, irow, &mc, limit, more, PAL_WARN);
    }
}

/* ---------------------------------------------------------------------- */
/* Footer                                                                  */
/* ---------------------------------------------------------------------- */

const char *render_hint(const ui_state_t *ui)
{
    if (ui->confirm_active)
    {
        return "y confirms · any other key declines";
    }
    if (ui->focus == FOCUS_MENU)
    {
        return ui_screen_is_interactive(ui_screen(ui))
                   ? "↑↓ select · Enter / Tab → panel · 1–7 jump"
                   : "↑↓ select · 1–7 jump";
    }
    switch (ui_screen(ui))
    {
    case SCR_CONSOLE:
        return "type a command · Enter run · PgUp/PgDn scroll · Esc → menu";
    case SCR_GRAPH:
        return "↑↓ change channel · Esc → menu";
    case SCR_FIRMWARE:
        return "↑↓ select · Enter run · Esc → menu";
    default:
        return "Esc → menu";
    }
}

static void prv_footer(cell_grid_t *g, const render_input_t *in)
{
    char right[32];
    uint8_t c = 0U;

    if (in->rx_overflow)
    {
        prv_seg(g, R_FOOTER_ROW, &c, HINT_RIGHT_COL, "[WARN] RX overflow", PAL_WARN);
    }
    else
    {
        prv_seg(g, R_FOOTER_ROW, &c, HINT_RIGHT_COL, render_hint(in->ui), PAL_MUTED);
    }
    (void) snprintf(right, sizeof(right), "focus %s · %u 8N1",
                    (in->ui->focus == FOCUS_MENU) ? "MENU" : "PANEL", (unsigned) CONSOLE_LINK_BAUD);
    c = (uint8_t) (GRID_COLS - fmt_cols(right)); /* right-aligned to the last column */
    prv_seg(g, R_FOOTER_ROW, &c, (uint8_t) GRID_COLS, right, PAL_MUTED);
}

/* ---------------------------------------------------------------------- */
/* Entry point                                                             */
/* ---------------------------------------------------------------------- */

void render_compose(cell_grid_t *grid, const render_input_t *in)
{
    grid_clear(grid);

    if (!in->size_ok)
    {
        const char *msg = "Resize the terminal to at least 88x30";
        const uint8_t col = (uint8_t) ((GRID_COLS - fmt_cols(msg)) / 2U);

        (void) grid_puts(grid, MIN_COLS_NOTICE_ROW, col, msg, PAL_WARN, PAL_BG, true, GRID_COLS);
        return;
    }

    prv_banner(grid, in->model);
    prv_menu(grid, in->ui);

    const bool active = (in->ui->focus == FOCUS_PANEL);

    switch (ui_screen(in->ui))
    {
    case SCR_GRAPH:
        prv_graph(grid, in);
        break;
    case SCR_STATUS:
        prv_status(grid, in, active);
        break;
    case SCR_ALARMS:
        prv_alarms(grid, in, active);
        break;
    case SCR_CONFIG:
        prv_config(grid, in, active);
        break;
    case SCR_NETWORK:
        prv_network(grid, in, active);
        break;
    case SCR_CONSOLE:
        prv_console(grid, in);
        break;
    default:
        prv_firmware(grid, in, active);
        break;
    }
    prv_footer(grid, in);
}
