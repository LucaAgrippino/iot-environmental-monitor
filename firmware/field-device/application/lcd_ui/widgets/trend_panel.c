/**
 * @file trend_panel.c
 * @brief Trend panel widget (see trend_panel.h).
 */

#include "lcd_ui/widgets/trend_panel.h"

#include <string.h>

#include "lcd_ui/theme.h"
#include "lcd_ui/ui_history.h"

#define PLOT_TOP 8         /**< space above the plot inside the chart */
#define PLOT_BOTTOM 22     /**< space below: the time-axis labels */
#define PLOT_MARGIN_PCT 10 /**< headroom above and below the data, per cent of the span */
#define FILL_OPA 26        /**< 10 % of 255, style-guide.md "Area fill" */
#define GRID_LINES 3       /**< horizontal grid lines in the plot */
#define LINE_W 2           /**< 1.5 px in the design, rounded up for the pixel grid */
#define LABEL_X                                                                                    \
    THEME_SP_4 /**< label column inner padding, 10 px: the name column is 120 px wide              \
                */

#ifndef TEST
/** Map a raw value to a y pixel inside the chart for the current autoscale. */
static lv_coord_t y_of(int32_t v, int32_t lo, int32_t span, lv_coord_t plot_h)
{
    return (lv_coord_t) (PLOT_TOP + plot_h - (lv_coord_t) (((int64_t) (v - lo) * plot_h) / span));
}

static void chart_draw_cb(lv_event_t *e)
{
    trend_panel_t *p = (trend_panel_t *) lv_event_get_user_data(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t box;
    int32_t lo = 0;
    int32_t hi = 0;
    bool have = false;

    lv_obj_get_coords(p->chart, &box);
    const lv_coord_t w = (lv_coord_t) (box.x2 - box.x1);
    const lv_coord_t plot_h = (lv_coord_t) (p->chart_h - PLOT_TOP - PLOT_BOTTOM);
    const uint32_t n = ui_history_count();

    /* grid: faint horizontal lines */
    lv_draw_line_dsc_t grid;
    lv_draw_line_dsc_init(&grid);
    grid.color = THEME_COL_BORDER;
    grid.width = THEME_BORDER_W;
    for (uint32_t g = 0U; g < GRID_LINES; g++)
    {
        const lv_coord_t gy =
            (lv_coord_t) (box.y1 + PLOT_TOP + ((plot_h * (lv_coord_t) g) / (GRID_LINES - 1U)));
        lv_point_t a = {.x = box.x1, .y = gy};
        lv_point_t b = {.x = box.x2, .y = gy};
        lv_draw_line(ctx, &grid, &a, &b);
    }

    for (uint32_t i = 0U; i < n; i++)
    {
        int32_t v;
        if (ui_history_get(p->channel, i, &v))
        {
            if (!have || (v < lo))
            {
                lo = v;
            }
            if (!have || (v > hi))
            {
                hi = v;
            }
            have = true;
        }
    }
    if (!have)
    {
        return;
    }
    /* autoscale with headroom; a flat series still gets a visible span */
    int32_t span = hi - lo;
    if (span < 1)
    {
        span = 1;
    }
    lo -= (span * PLOT_MARGIN_PCT) / 100;
    span += 2 * ((span * PLOT_MARGIN_PCT) / 100);
    if (span < 2)
    {
        span = 2;
    }

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = p->colour;
    line.width = LINE_W;
    line.round_start = 1U;
    line.round_end = 1U;

    lv_draw_rect_dsc_t fill;
    lv_draw_rect_dsc_init(&fill);
    fill.bg_color = p->colour;
    fill.bg_opa = FILL_OPA;
    fill.border_width = 0;

    lv_draw_rect_dsc_t gap;
    lv_draw_rect_dsc_init(&gap);
    gap.bg_color = THEME_COL_SURF2;
    gap.bg_opa = LV_OPA_COVER;
    gap.border_width = 0;

    const uint32_t slots = UI_HISTORY_LEN - 1U;
    lv_point_t last = {.x = 0, .y = 0};
    bool have_last = false;

    for (uint32_t i = 0U; i < n; i++)
    {
        const lv_coord_t x = (lv_coord_t) (box.x1 + (((uint32_t) w * i) / slots));
        int32_t v;
        if (!ui_history_get(p->channel, i, &v))
        {
            /* an invalid sample: shade its column and break the line (REQ-LD-190) */
            const lv_coord_t x2 = (lv_coord_t) (box.x1 + (((uint32_t) w * (i + 1U)) / slots));
            lv_area_t col = {.x1 = x,
                             .y1 = (lv_coord_t) (box.y1 + PLOT_TOP),
                             .x2 = x2,
                             .y2 = (lv_coord_t) (box.y1 + PLOT_TOP + plot_h)};
            lv_draw_rect(ctx, &gap, &col);
            have_last = false;
            continue;
        }
        lv_point_t pt = {.x = x, .y = (lv_coord_t) (box.y1 + y_of(v, lo, span, plot_h))};
        if (have_last)
        {
            for (lv_coord_t cx = last.x; cx < pt.x; cx++) /* pt.x is painted by the next segment */
            {
                const lv_coord_t dx = (lv_coord_t) (pt.x - last.x);
                const lv_coord_t cy =
                    (dx == 0) ? last.y
                              : (lv_coord_t) (last.y + (((pt.y - last.y) * (cx - last.x)) / dx));
                lv_area_t col = {
                    .x1 = cx, .y1 = cy, .x2 = cx, .y2 = (lv_coord_t) (box.y1 + PLOT_TOP + plot_h)};
                lv_draw_rect(ctx, &fill, &col);
            }
            lv_draw_line(ctx, &line, &last, &pt);
        }
        last = pt;
        have_last = true;
    }

    /* while the buffer is filling: a dotted line at the current level out to the right edge */
    if (have_last && (n < UI_HISTORY_LEN))
    {
        lv_draw_line_dsc_t dotted;
        lv_draw_line_dsc_init(&dotted);
        dotted.color = THEME_COL_DIM;
        dotted.width = THEME_BORDER_W;
        dotted.dash_width = 2;
        dotted.dash_gap = 4;
        lv_point_t end = {.x = box.x2, .y = last.y};
        lv_draw_line(ctx, &dotted, &last, &end);
    }
}
#endif /* TEST */

void trend_panel_create(trend_panel_t *p, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h, ui_channel_t ch, const char *name,
                        const char *unit, lv_color_t colour)
{
    static const char *const k_axis[TREND_AXIS_LABELS] = {"0 m", "1 m", "2 m", "3 m", "4 m", "5 m"};
    const lv_font_t *cap = THEME_FONT_CAPTION_MONO;

    p->channel = ch;
    p->colour = colour;
    p->chart_w = (lv_coord_t) (w - TREND_LABEL_COL_W - THEME_BORDER_W);
    p->chart_h = h;

    p->box = theme_box_create(parent, x, y, w, h);
    lv_obj_add_style(p->box, &theme_st_card, LV_PART_MAIN);

    /* label column: name, value, unit, max, min, with a 1 px divider on its right */
    p->name =
        theme_label_create(p->box, &theme_st_txt_eyebrow, &theme_st_label_muted, LABEL_X, 7, name);
    p->value =
        theme_label_create(p->box, &theme_st_txt_value, &theme_st_label_ink, LABEL_X, 19, "");
    lv_obj_set_style_text_color(p->value, colour, LV_PART_MAIN);
    p->unit = theme_label_create(p->box, &theme_st_txt_caption_mono, &theme_st_label_muted, LABEL_X,
                                 43, unit);
    p->max_lbl = theme_label_create(p->box, &theme_st_txt_caption_mono, &theme_st_label_dim,
                                    LABEL_X, (lv_coord_t) (h - (2 * cap->line_height) - 4), "");
    p->min_lbl = theme_label_create(p->box, &theme_st_txt_caption_mono, &theme_st_label_dim,
                                    LABEL_X, (lv_coord_t) (h - cap->line_height - 4), "");
    lv_obj_t *divider = theme_box_create(p->box, TREND_LABEL_COL_W, 0, THEME_BORDER_W, h);
    lv_obj_set_style_bg_color(divider, THEME_COL_BORDER, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(divider, LV_OPA_COVER, LV_PART_MAIN);

    p->chart = theme_box_create(p->box, (lv_coord_t) (TREND_LABEL_COL_W + THEME_BORDER_W), 0,
                                p->chart_w, h);
#ifndef TEST
    lv_obj_add_event_cb(p->chart, chart_draw_cb, LV_EVENT_DRAW_MAIN, p);
#endif

    /* time axis: 0 m .. 5 m spread over the chart; the labels sit below the plot */
    for (uint32_t i = 0U; i < TREND_AXIS_LABELS; i++)
    {
        const lv_coord_t tw =
            lv_txt_get_width(k_axis[i], (uint32_t) strlen(k_axis[i]), cap, 0, LV_TEXT_FLAG_NONE);
        lv_coord_t lx =
            (lv_coord_t) ((p->chart_w * (lv_coord_t) i) / (lv_coord_t) (TREND_AXIS_LABELS - 1U));
        if (i == (TREND_AXIS_LABELS - 1U))
        {
            lx = (lv_coord_t) (lx - tw); /* keep the last label inside the chart */
        }
        else if (i > 0U)
        {
            lx = (lv_coord_t) (lx - (tw / 2));
        }
        p->axis[i] = theme_label_create(p->chart, &theme_st_txt_caption_mono, &theme_st_label_dim,
                                        lx, (lv_coord_t) (h - cap->line_height - 4), k_axis[i]);
    }
}

void trend_panel_set_texts(trend_panel_t *p, const char *value, const char *max_text,
                           const char *min_text)
{
    lv_label_set_text(p->value, value);
    lv_label_set_text(p->max_lbl, max_text);
    lv_label_set_text(p->min_lbl, min_text);
}

void trend_panel_invalidate(trend_panel_t *p)
{
    lv_obj_invalidate(p->chart);
}
