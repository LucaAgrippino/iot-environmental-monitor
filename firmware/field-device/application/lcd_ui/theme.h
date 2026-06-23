/**
 * @file theme.h
 * @brief LcdUi design-token exports — colours, LVGL styles, and layout constants.
 *
 * All colour tokens come from 01_DESIGN_TOKENS.md.
 * All layout constants come from 03_SCREEN_SPECS.md.
 * Fonts: every binding uses &lv_font_montserrat_14 until lv_font_conv is run.
 *
 * @see docs/lcd-ui-design/_reference-v9-claude-design/01_DESIGN_TOKENS.md
 * @see docs/lcd-ui-design/_reference-v9-claude-design/03_SCREEN_SPECS.md
 */

#ifndef THEME_H
#define THEME_H

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

/* ===================================================================== */
/* Colour tokens — 01_DESIGN_TOKENS.md §Colour                          */
/* ===================================================================== */

#define THEME_COL_BG lv_color_hex(0x0D1117U)        /* Canvas background       */
#define THEME_COL_SURF lv_color_hex(0x161B22U)      /* Card surface            */
#define THEME_COL_SURF2 lv_color_hex(0x1B2129U)     /* Elevated surface        */
#define THEME_COL_BORDER lv_color_hex(0x2A2E34U)    /* 1 px structural border  */
#define THEME_COL_INK lv_color_hex(0xE8F2FAU)       /* Primary text            */
#define THEME_COL_MUTED lv_color_hex(0xA0A8B0U)     /* Secondary / label text  */
#define THEME_COL_DIM lv_color_hex(0x6B7280U)       /* Placeholder / disabled  */
#define THEME_COL_OK lv_color_hex(0x6FBF8EU)        /* Success / valid         */
#define THEME_COL_WARN lv_color_hex(0xD4A84CU)      /* Warning                 */
#define THEME_COL_ERR lv_color_hex(0xCC6666U)       /* Error / alarm           */
#define THEME_COL_ACCENT lv_color_hex(0x7BAFD4U)    /* Active / accent         */
#define THEME_COL_HEADER_BG lv_color_hex(0x0A0E14U) /* Tab bar and footer bg   */

/* Tints — pre-blended at 12 % over COL_BG (01_DESIGN_TOKENS.md §Tints) */
#define THEME_COL_OK_TINT lv_color_hex(0x152019U)
#define THEME_COL_WARN_TINT lv_color_hex(0x20211AU)
#define THEME_COL_ERR_TINT lv_color_hex(0x21171AU)
#define THEME_COL_ACCENT_TINT lv_color_hex(0x13202BU)

/* Inactive tab label colour — 03_SCREEN_SPECS.md §Tab bar */
#define THEME_COL_TAB_INACTIVE lv_color_hex(0x888888U)

/* ===================================================================== */
/* Spacing scale — SP_n (01_DESIGN_TOKENS.md §Spacing)                  */
/* ===================================================================== */

#define THEME_SP_1 4
#define THEME_SP_2 6
#define THEME_SP_3 8
#define THEME_SP_4 10
#define THEME_SP_5 12
#define THEME_SP_6 14
#define THEME_SP_7 16 /* card horizontal padding */
#define THEME_SP_8 18 /* card vertical padding   */
#define THEME_SP_9 22
#define THEME_SP_10 24
#define THEME_SP_11 30
#define THEME_SP_12 36

/* ===================================================================== */
/* Layout constants — 03_SCREEN_SPECS.md                                */
/* ===================================================================== */

#define THEME_CANVAS_W 800 /* 03_SCREEN_SPECS.md: fixed 800 × 480 canvas */
#define THEME_CANVAS_H 480

#define THEME_TAB_BAR_H 64    /* 03_SCREEN_SPECS.md §Tab bar   */
#define THEME_HEADER_BAR_H 40 /* 03_SCREEN_SPECS.md §Header bar */
#define THEME_FOOTER_BAR_H 36 /* 03_SCREEN_SPECS.md §Footer bar */

#define THEME_HEADER_BAR_Y (THEME_TAB_BAR_H)                     /* = 64  */
#define THEME_CONTENT_Y (THEME_TAB_BAR_H + THEME_HEADER_BAR_H)   /* = 104 */
#define THEME_FOOTER_BAR_Y (THEME_CANVAS_H - THEME_FOOTER_BAR_H) /* = 444 */
#define THEME_CONTENT_H (THEME_FOOTER_BAR_Y - THEME_CONTENT_Y)   /* = 340 */

/* Tab bar column geometry */
#define THEME_TAB_COL_COUNT 4
#define THEME_TAB_COL_W (THEME_CANVAS_W / THEME_TAB_COL_COUNT) /* = 200 */
#define THEME_TAB_ACCENT_H 3 /* active-tab top accent stripe height, px */

/* Header bar edge inset */
#define THEME_HEADER_INSET 22 /* 03_SCREEN_SPECS.md §Header bar */

/* Sensor card geometry — 03_SCREEN_SPECS.md §Sensor card */
#define THEME_CARD_W 244
#define THEME_CARD_H 280
#define THEME_CARD_STRIPE_W 4 /* left status stripe width */
#define THEME_CARD_GAP 8      /* gap between adjacent cards */

/* Vertical centring within the 340 px content area */
#define THEME_CARD_Y_MARGIN ((THEME_CONTENT_H - THEME_CARD_H) / 2) /* = 30 */

/* Sparkline placeholder — 03_SCREEN_SPECS.md §Sensor card */
#define THEME_SPARKLINE_W 200
#define THEME_SPARKLINE_H 34

/* Corner radius token — P aesthetic: rectangular */
#define THEME_RAD_NONE 0

/* ===================================================================== */
/* Runtime LVGL style objects (initialised by theme_init)               */
/* ===================================================================== */

extern lv_style_t theme_st_screen;       /* Root screen: BG fill, no padding  */
extern lv_style_t theme_st_card;         /* Card container: SURF, 1 px border  */
extern lv_style_t theme_st_tab_bar;      /* Tab bar container                  */
extern lv_style_t theme_st_tab_btn;      /* Inactive tab button                */
extern lv_style_t theme_st_header;       /* Header bar                         */
extern lv_style_t theme_st_footer;       /* Footer bar                         */
extern lv_style_t theme_st_label_ink;    /* Primary text, COL_INK              */
extern lv_style_t theme_st_label_muted;  /* Secondary text, COL_MUTED          */
extern lv_style_t theme_st_label_dim;    /* Placeholder text, COL_DIM          */
extern lv_style_t theme_st_label_ok;     /* Valid state, COL_OK                */
extern lv_style_t theme_st_label_err;    /* Error / alarm state, COL_ERR       */
extern lv_style_t theme_st_label_warn;   /* Warning state, COL_WARN            */
extern lv_style_t theme_st_label_accent; /* Accent / active, COL_ACCENT        */

/* ===================================================================== */
/* Initialisation                                                        */
/* ===================================================================== */

/**
 * @brief Initialise all design-token LVGL styles.
 *
 * Call once before any widget or screen creates objects.
 * Safe to call from any task context; not ISR-safe.
 */
void theme_init(void);

#endif /* THEME_H */
