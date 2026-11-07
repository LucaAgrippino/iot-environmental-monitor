#ifndef SENSOR_CARD_H
#define SENSOR_CARD_H

/**
 * @file sensor_card.h
 * @brief One sensor card of the Sensors screen: stripe, label, hero value, sparkline, status row.
 *
 * 03_SCREEN_SPECS.md "Sensor card" and style-guide.md section 9. The card shows text prepared by
 * the screen (ui_format), so it contains no unit or fixed-point knowledge.
 */

#include <stdbool.h>
#include <stdint.h>

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

/** Marks an invalid sample in the sparkline points (REQ-LD-190: a gap, not an interpolation). */
#define SPARK_GAP ((int16_t) -1)
/** Most points a sparkline holds: the 5 min history at 5 s (REQ-SA-190, REQ-SA-200). */
#define SPARK_MAX_POINTS (60U)

typedef enum
{
    CARD_WAIT = 0,  /**< No data yet: "Waiting for data…". */
    CARD_VALID = 1, /**< Fresh reading. */
    CARD_STALE = 2, /**< Reading overdue: warning stripe, dashed sparkline. */
    CARD_ERROR = 3, /**< Sensor error: last value faded, error stripe. */
    CARD_ALARM = 4  /**< Threshold alarm: value and stripe in the error colour. */
} card_state_t;

typedef struct
{
    lv_obj_t *box;         /**< Card surface. */
    lv_obj_t *stripe;      /**< 4 px left status stripe. */
    lv_obj_t *eyebrow;     /**< "TEMPERATURE · S1". */
    lv_obj_t *value_lbl;   /**< Hero number. */
    lv_obj_t *unit_lbl;    /**< Unit, baseline-aligned next to the number. */
    lv_obj_t *waiting_lbl; /**< "Waiting for data…" in place of the value row. */
    lv_obj_t *spark;       /**< Sparkline drawing area. */
    lv_obj_t *status_lbl;  /**< "● VALID · 12:34:56". */
    lv_obj_t *delta_lbl;   /**< "+0.3 / 5m", right-aligned. */
    card_state_t state;
    lv_coord_t inner_w;                /**< Width available inside the padding. */
    int16_t spark_y[SPARK_MAX_POINTS]; /**< Normalised y per point, 0 = top, or SPARK_GAP. */
    uint32_t spark_n;                  /**< Points in use. */
} sensor_card_t;

/**
 * @brief Build a card.
 *
 * @param[out] card     Card instance (caller-owned, static).
 * @param[in]  parent   Parent (the content panel).
 * @param[in]  x        Left edge in the parent.
 * @param[in]  y        Top edge in the parent.
 * @param[in]  w        Width.
 * @param[in]  h        Height.
 * @param[in]  eyebrow  Label text, UPPERCASE, e.g. "TEMPERATURE · S1".
 * @param[in]  unit     Unit text, e.g. "°C".
 */
void sensor_card_create(sensor_card_t *card, lv_obj_t *parent, lv_coord_t x, lv_coord_t y,
                        lv_coord_t w, lv_coord_t h, const char *eyebrow, const char *unit);

/**
 * @brief Set the look and the texts of the card.
 *
 * @param[in,out] card         Card instance.
 * @param[in]     state        Card state; selects stripe, value and status colours.
 * @param[in]     value        Hero number text (ignored in CARD_WAIT).
 * @param[in]     status_text  Status row text, e.g. "● VALID · 12:34:56".
 * @param[in]     delta_text   Delta text or "—".
 */
void sensor_card_set(sensor_card_t *card, card_state_t state, const char *value,
                     const char *status_text, const char *delta_text);

/**
 * @brief Replace the sparkline.
 *
 * @param[in,out] card  Card instance.
 * @param[in]     y     Normalised y per point (0 top .. THEME_SPARKLINE_H - 1 bottom) or SPARK_GAP.
 * @param[in]     n     Number of points; clamped to SPARK_MAX_POINTS. Fewer than two draws nothing.
 */
void sensor_card_set_spark(sensor_card_t *card, const int16_t *y, uint32_t n);

#endif /* SENSOR_CARD_H */
