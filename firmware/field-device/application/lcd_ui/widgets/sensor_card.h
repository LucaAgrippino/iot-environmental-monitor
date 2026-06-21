/**
 * @file sensor_card.h
 * @brief Individual sensor display card widget.
 *
 * Each card renders one sensor channel in a 244 × 280 px tile:
 *   ┌──── 4 px stripe (colour-coded by state) ──────────────┐
 *   │  TEMPERATURE         (COL_MUTED eyebrow label)         │
 *   │                                                        │
 *   │  23.4   °C           (hero value + unit)               │
 *   │  ─── Waiting for data ... (hidden when data present)   │
 *   │  ═══════════════════════ (sparkline placeholder)       │
 *   │  ● OK · 12:34:56    —   (status + delta)              │
 *   └────────────────────────────────────────────────────────┘
 *
 * @see docs/lcd-ui-design/_reference-v9-claude-design/03_SCREEN_SPECS.md §Sensor card
 */

#ifndef SENSOR_CARD_H
#define SENSOR_CARD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

/* ===================================================================== */
/* Fixed-point format selector                                           */
/* ===================================================================== */

/**
 * @brief Determines how a raw int32_t sensor value is formatted for display.
 *
 * SENSOR_CARD_FMT_CENTI — value is in units × 100; display shows one
 *   decimal place.  E.g. 2340 → "23.4", −450 → "−4.5".
 *   Used for temperature (0.01 °C) and humidity (0.01 %RH).
 *
 * SENSOR_CARD_FMT_DECI — value is in units × 10; display shows no
 *   decimal place.  E.g. 10132 → "1013".
 *   Used for pressure (0.1 hPa).
 */
typedef enum
{
    SENSOR_CARD_FMT_CENTI = 0, /**< ×100 — one decimal digit displayed */
    SENSOR_CARD_FMT_DECI  = 1, /**< ×10  — integer displayed           */
} sensor_card_fmt_t;

/* ===================================================================== */
/* Aggregate widget handle                                               */
/* ===================================================================== */

/**
 * @brief Sensor card widget state.
 *
 * All lv_obj_t pointers are valid after sensor_card_create() returns.
 * Tests access value_lbl, unit_lbl, status_lbl, and waiting_lbl through
 * the convenience alias pointers in sensor_screen_t.
 */
typedef struct
{
    lv_obj_t         *obj;          /**< Card container (244 × 280 px)     */
    lv_obj_t         *stripe;       /**< 4 px left status stripe           */
    lv_obj_t         *eyebrow_lbl;  /**< "TEMPERATURE" etc. (COL_MUTED)   */
    lv_obj_t         *value_lbl;    /**< Hero numeric value, e.g. "23.4"   */
    lv_obj_t         *unit_lbl;     /**< Unit string, e.g. "C"             */
    lv_obj_t         *waiting_lbl;  /**< "Waiting for data..." overlay     */
    lv_obj_t         *sparkline;    /**< 200 × 34 px placeholder box       */
    lv_obj_t         *status_lbl;   /**< "OK" / "ERROR" / "WAITING"       */
    lv_obj_t         *delta_lbl;    /**< "\xe2\x80\x94" em-dash placeholder */
    sensor_card_fmt_t fmt;          /**< Stored at creation time           */
} sensor_card_t;

/* ===================================================================== */
/* Public API                                                            */
/* ===================================================================== */

/**
 * @brief Create all child widgets inside the card.
 *
 * Positions are absolute within parent (no flex/grid — lv_conf.h GL-D9).
 *
 * @param card          Caller-owned struct to populate.
 * @param parent        Content panel lv_obj_t.
 * @param x             Left edge of card within parent.
 * @param y             Top edge of card within parent.
 * @param w             Card width (normally THEME_CARD_W = 244).
 * @param h             Card height (normally THEME_CARD_H = 280).
 * @param eyebrow_text  Channel label ("TEMPERATURE" etc.).
 * @param unit_text     Unit label ("C", "%", "hPa").
 * @param fmt           Fixed-point format.
 */
void sensor_card_create(sensor_card_t *card, lv_obj_t *parent,
                         lv_coord_t x, lv_coord_t y,
                         lv_coord_t w, lv_coord_t h,
                         const char *eyebrow_text,
                         const char *unit_text,
                         sensor_card_fmt_t fmt);

/**
 * @brief Reset card to "Waiting for data…" state.
 *
 * Hides value / unit / sparkline / status labels; shows waiting overlay;
 * sets stripe to COL_DIM.
 *
 * @param card  Populated card handle.
 */
void sensor_card_show_waiting(sensor_card_t *card);

/**
 * @brief Refresh card with a new sensor reading.
 *
 * Hides the waiting overlay.  Formats value according to card->fmt.
 * Sets status_lbl to "OK" when valid, "ERROR" when not.
 * Sets stripe to COL_OK when valid, COL_ERR when not.
 *
 * @param card   Populated card handle.
 * @param value  Raw fixed-point sensor value.
 * @param valid  true when the sensor reading is reliable.
 */
void sensor_card_update(sensor_card_t *card, int32_t value, bool valid);

#endif /* SENSOR_CARD_H */
