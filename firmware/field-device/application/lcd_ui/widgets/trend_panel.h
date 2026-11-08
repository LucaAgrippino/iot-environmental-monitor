#ifndef TREND_PANEL_H
#define TREND_PANEL_H

/**
 * @file trend_panel.h
 * @brief One channel of the Trend screen: a 120 px label column and a five-minute line chart.
 *
 * style-guide.md "Trend chart panel": COL_SURF with a 1 px border; the label column is 120 px wide
 * with a 1 px right border; the chart fills the rest of the panel; the time axis reads 0 m .. 5 m
 * in FONT_CAPTION_MONO COL_MUTED; the series is a 1.5 px line in the channel colour (temperature
 * COL_ACCENT, humidity COL_OK, pressure COL_WARN) with a 10 % fill below; invalid samples are a
 * gap, never an interpolation (REQ-LD-190). The data comes straight from ui_history.
 */

#include <stdint.h>

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

#include "lcd_ui/ui_format.h"

#define TREND_AXIS_LABELS 6U  /**< 0 m, 1 m .. 5 m */
#define TREND_LABEL_COL_W 120 /**< Width of the label column, in pixels. */

/** @brief Trend panel widget: a chart of one sensor's history. */
typedef struct
{
    lv_obj_t *box;                     /**< Panel surface. */
    lv_obj_t *name;                    /**< Name of the sensor. */
    lv_obj_t *value;                   /**< Latest value. */
    lv_obj_t *unit;                    /**< Unit text. */
    lv_obj_t *max_lbl;                 /**< Label of the chart maximum. */
    lv_obj_t *min_lbl;                 /**< Label of the chart minimum. */
    lv_obj_t *chart;                   /**< Chart object. */
    lv_obj_t *axis[TREND_AXIS_LABELS]; /**< Time-axis labels. */
    ui_channel_t channel;              /**< Channel shown. */
    lv_color_t colour;                 /**< Line colour. */
    lv_coord_t chart_w;                /**< Chart width, in pixels. */
    lv_coord_t chart_h;                /**< Chart height, in pixels. */
} trend_panel_t;

/**
 * @brief Build one panel.
 *
 * @param[out] p       Panel instance (caller-owned, static).
 * @param[in]  parent  Parent (the content panel).
 * @param[in]  x       Left edge.
 * @param[in]  y       Top edge.
 * @param[in]  w       Width.
 * @param[in]  h       Height.
 * @param[in]  ch      Channel whose history is drawn.
 * @param[in]  name    Name, e.g. "TEMPERATURE".
 * @param[in]  unit    Unit, e.g. "°C".
 * @param[in]  colour  Series colour.
 */
void trend_panel_create(trend_panel_t *p, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h, ui_channel_t ch, const char *name,
                        const char *unit, lv_color_t colour);

/**
 * @brief Set the texts of the label column.
 *
 * @param[in,out] p         Panel instance.
 * @param[in]     value     Current value, or "—" when there is none.
 * @param[in]     max_text  "max 24.1" or empty.
 * @param[in]     min_text  "min 22.8" or empty.
 */
void trend_panel_set_texts(trend_panel_t *p, const char *value, const char *max_text,
                           const char *min_text);

/**
 * @brief Redraw the chart from ui_history (call after a sample was stored).
 *
 * @param p Panel.
 */
void trend_panel_invalidate(trend_panel_t *p);

#endif /* TREND_PANEL_H */
