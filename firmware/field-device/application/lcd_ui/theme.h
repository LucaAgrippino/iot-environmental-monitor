#ifndef THEME_H
#define THEME_H

/**
 * @file theme.h
 * @brief Design tokens and LVGL styles of the Field Device LCD UI.
 *
 * Every colour, size and font in the UI comes from here, so a screen never carries a magic number.
 * Sources: docs/lcd-ui-design/style-guide.md (tokens, zones, components) and
 * docs/lcd-ui-design/_reference-v9-claude-design/01_DESIGN_TOKENS.md, 02_FONTS_AND_ICONS.md,
 * 03_SCREEN_SPECS.md.
 */

#ifdef TEST
#include "lvgl_stub.h"
#else
#include "lvgl.h"
#endif

#include "graphics_library/fonts/fonts.h"

/* ===================================================================== */
/* Colour tokens — style-guide.md section 2                              */
/* ===================================================================== */

#define THEME_COL_BG lv_color_hex(0x0D1117U)        /**< COL_BG: app background */
#define THEME_COL_SURF lv_color_hex(0x161B22U)      /**< COL_SURF: cards, tiles */
#define THEME_COL_SURF2 lv_color_hex(0x1B2129U)     /**< COL_SURF2: inputs, recessed */
#define THEME_COL_BORDER lv_color_hex(0x2A2E34U)    /**< COL_BORDER: 1 px borders */
#define THEME_COL_INK lv_color_hex(0xE8F2FAU)       /**< COL_INK: primary text */
#define THEME_COL_MUTED lv_color_hex(0xA0A8B0U)     /**< COL_MUTED: secondary text */
#define THEME_COL_DIM lv_color_hex(0x6B7280U)       /**< COL_DIM: disabled, tertiary */
#define THEME_COL_OK lv_color_hex(0x6FBF8EU)        /**< COL_OK: valid, running */
#define THEME_COL_WARN lv_color_hex(0xD4A84CU)      /**< COL_WARN: stale, degraded */
#define THEME_COL_ERR lv_color_hex(0xCC6666U)       /**< COL_ERR: error, alarm */
#define THEME_COL_ACCENT lv_color_hex(0x7BAFD4U)    /**< COL_ACCENT: brand, focus */
#define THEME_COL_HEADER_BG lv_color_hex(0x0A0E14U) /**< COL_HEADER_BG: tab bar, footer */

/** Tints, pre-blended 12 % over COL_BG (style-guide.md section 2) */
#define THEME_COL_OK_TINT lv_color_hex(0x152019U)
#define THEME_COL_WARN_TINT                                                                        \
    lv_color_hex(0x20211AU) /**< Warning tint, the background of a warning element. */
#define THEME_COL_ERR_TINT                                                                         \
    lv_color_hex(0x21171AU) /**< Error tint, the background of an error element. */
#define THEME_COL_ACCENT_TINT                                                                      \
    lv_color_hex(0x13202BU) /**< Accent tint, the background of a highlighted element. */

/** Inactive tab icon and label — style-guide.md section 5 "Tab bar" */
#define THEME_COL_TAB_INACTIVE lv_color_hex(0x888888U)
/** Text on the filled APPLY button — 03_SCREEN_SPECS.md "Config field" footer */
#define THEME_COL_ON_ACCENT lv_color_hex(0x06080BU)

/* ===================================================================== */
/* Spacing scale — style-guide.md section 4                              */
/* ===================================================================== */

#define THEME_SP_1 4   /**< Spacing step 1, in pixels. */
#define THEME_SP_2 6   /**< Spacing step 2, in pixels. */
#define THEME_SP_3 8   /**< Spacing step 3, in pixels. */
#define THEME_SP_4 10  /**< Spacing step 4, in pixels. */
#define THEME_SP_5 12  /**< Spacing step 5, in pixels. */
#define THEME_SP_6 14  /**< Spacing step 6, in pixels. */
#define THEME_SP_7 16  /**< Spacing step 7, in pixels. */
#define THEME_SP_8 18  /**< Spacing step 8, in pixels. */
#define THEME_SP_9 22  /**< Spacing step 9, in pixels. */
#define THEME_SP_10 24 /**< Spacing step 10, in pixels. */
#define THEME_SP_11 30 /**< Spacing step 11, in pixels. */
#define THEME_SP_12 36 /**< Spacing step 12, in pixels. */

/* ===================================================================== */
/* Layout — style-guide.md sections 1, 5, 6                              */
/* ===================================================================== */

#define THEME_CANVAS_W 800                                     /**< canvas, section 1 */
#define THEME_CANVAS_H 480                                     /**< Canvas height, in pixels. */
#define THEME_TAB_BAR_H 64                                     /**< tab bar zone */
#define THEME_HEADER_BAR_H 40                                  /**< header zone */
#define THEME_FOOTER_BAR_H 36                                  /**< footer zone */
#define THEME_HEADER_BAR_Y (THEME_TAB_BAR_H)                   /**< Y position of the header bar. */
#define THEME_CONTENT_Y (THEME_TAB_BAR_H + THEME_HEADER_BAR_H) /**< = 104 */
#define THEME_FOOTER_BAR_Y (THEME_CANVAS_H - THEME_FOOTER_BAR_H) /**< = 444 */
#define THEME_CONTENT_H (THEME_FOOTER_BAR_Y - THEME_CONTENT_Y)   /**< = 340 */

#define THEME_TAB_COL_COUNT 5 /**< SRS v1.2 REQ-LD-000: sensors, trend, status, alarms, config */
#define THEME_TAB_COL_W (THEME_CANVAS_W / THEME_TAB_COL_COUNT) /**< = 160 */
#define THEME_TAB_ACCENT_H 3                                   /**< active tab top stripe */
#define THEME_TAB_ICON_GAP 4  /**< icon to label, 03_SCREEN_SPECS.md "Tab bar" */
#define THEME_HEADER_INSET 22 /**< header and footer edge inset */

#define THEME_BORDER_W 1       /**< all 1 px borders and dividers */
#define THEME_CARD_STRIPE_W 4  /**< sensor card status stripe */
#define THEME_ALARM_STRIPE_W 5 /**< alarm row status stripe */
#define THEME_RAD_NONE 0       /**< P aesthetic is rectangular */
#define THEME_DOT_D 10         /**< status dot diameter */

/** Sensors screen: three cards, 22 px screen padding, 14 px gap (03_SCREEN_SPECS.md "Sensor card")
 */
#define THEME_SCREEN_PAD THEME_SP_9
#define THEME_CARD_GAP THEME_SP_6 /**< Spacing step 6, in pixels. */
/** @brief Width of a card when three cards share a row. */
#define THEME_CARD_W                                                                               \
    ((THEME_CANVAS_W - (2 * THEME_SCREEN_PAD) - (2 * THEME_CARD_GAP)) / 3) /* 242 */
#define THEME_CARD_H (THEME_CONTENT_H - (2 * THEME_SCREEN_PAD))            /**< 296 */
#define THEME_SPARKLINE_H 34 /**< sparkline region height */

/** Runtime icon-font sizes are fixed by the font files (02_FONTS_AND_ICONS.md "Icons") */
#define THEME_ICON_TAB (&font_icons_24)
#define THEME_ICON_MODAL (&font_icons_20) /**< Icon font of the confirmation dialog. */
#define THEME_ICON_FW (&font_icons_32)    /**< Icon font of the firmware tile. */
#define THEME_ICON_HERO (&font_icons_42)  /**< Large icon font of the empty-alarm view. */

/* ===================================================================== */
/* Font tokens — style-guide.md section 3, 02_FONTS_AND_ICONS.md         */
/* ===================================================================== */

#define THEME_FONT_HERO (&font_jb_sb_54)         /**< FONT_HERO: mono 54, sensor value */
#define THEME_FONT_TITLE_LG (&font_inter_sb_30)  /**< Large title font. */
#define THEME_FONT_TITLE (&font_inter_sb_22)     /**< Title font. */
#define THEME_FONT_HEAD (&font_inter_sb_18)      /**< FONT_HEAD: modal title */
#define THEME_FONT_VALUE (&font_jb_sb_18)        /**< FONT_VALUE: status values */
#define THEME_FONT_BODY (&font_inter_md_14)      /**< Body text font. */
#define THEME_FONT_BODY_SB (&font_inter_sb_14)   /**< alarm label, buttons (weight 600) */
#define THEME_FONT_BODY_MONO (&font_jb_md_14)    /**< FONT_BODY_MONO: config input */
#define THEME_FONT_LABEL (&font_inter_md_13)     /**< Label font. */
#define THEME_FONT_VALUE_SM (&font_jb_md_13)     /**< Small value font (monospaced). */
#define THEME_FONT_META (&font_jb_md_12)         /**< FONT_META: header time */
#define THEME_FONT_CAPTION (&font_inter_md_11)   /**< Caption font. */
#define THEME_FONT_CAPTION_MONO (&font_jb_md_11) /**< Caption font (monospaced). */
#define THEME_FONT_TAB (&font_inter_sb_11)       /**< Tab label font. */
#define THEME_FONT_PILL (&font_jb_b_10)          /**< FONT_PILL and FONT_FOOTER */
#define THEME_FONT_EYEBROW (&font_inter_b_10)    /**< Eyebrow (small heading) font. */
#define THEME_FONT_TINY (&font_inter_sb_9)       /**< Smallest font. */

/** Letter spacing in px — 02_FONTS_AND_ICONS.md "Tracking" */
#define THEME_TRACK_HERO (-2)
#define THEME_TRACK_TAB 2     /**< Letter spacing of tab labels, in pixels. */
#define THEME_TRACK_PILL 2    /**< Letter spacing of status pills, in pixels. */
#define THEME_TRACK_EYEBROW 2 /**< Letter spacing of eyebrows, in pixels. */
#define THEME_TRACK_FOOTER 1  /**< Letter spacing of the footer, in pixels. */
#define THEME_TRACK_TINY 2    /**< Letter spacing of the smallest text, in pixels. */

/* ===================================================================== */
/* Runtime LVGL styles (initialised by theme_init)                       */
/* ===================================================================== */

/** Structure */
extern lv_style_t
    theme_st_screen; /**< root screen and content panels: BG fill, no padding, no scroll */
extern lv_style_t theme_st_card; /**< COL_SURF, 1 px border, rectangular */
/** @brief Shared style: tab bar. */
extern lv_style_t theme_st_tab_bar;
/** @brief Shared style: header. */
extern lv_style_t theme_st_header;
/** @brief Shared style: footer. */
extern lv_style_t theme_st_footer;
extern lv_style_t theme_st_plain; /**< transparent, no border, no padding: container for labels */

/** Text: font and tracking only (add one colour style as well) */
extern lv_style_t theme_st_txt_hero;
/** @brief Shared style: txt title lg. */
extern lv_style_t theme_st_txt_title_lg;
/** @brief Shared style: txt title. */
extern lv_style_t theme_st_txt_title;
/** @brief Shared style: txt head. */
extern lv_style_t theme_st_txt_head;
/** @brief Shared style: txt value. */
extern lv_style_t theme_st_txt_value;
/** @brief Shared style: txt body. */
extern lv_style_t theme_st_txt_body;
/** @brief Shared style: txt body sb. */
extern lv_style_t theme_st_txt_body_sb;
/** @brief Shared style: txt body mono. */
extern lv_style_t theme_st_txt_body_mono;
/** @brief Shared style: txt label. */
extern lv_style_t theme_st_txt_label;
/** @brief Shared style: txt value sm. */
extern lv_style_t theme_st_txt_value_sm;
/** @brief Shared style: txt meta. */
extern lv_style_t theme_st_txt_meta;
/** @brief Shared style: txt caption. */
extern lv_style_t theme_st_txt_caption;
/** @brief Shared style: txt caption mono. */
extern lv_style_t theme_st_txt_caption_mono;
/** @brief Shared style: txt tab. */
extern lv_style_t theme_st_txt_tab;
/** @brief Shared style: txt pill. */
extern lv_style_t theme_st_txt_pill;
/** @brief Shared style: txt eyebrow. */
extern lv_style_t theme_st_txt_eyebrow;
/** @brief Shared style: txt tiny. */
extern lv_style_t theme_st_txt_tiny;

/** Text colour only */
extern lv_style_t theme_st_label_ink;
/** @brief Shared style: label muted. */
extern lv_style_t theme_st_label_muted;
/** @brief Shared style: label dim. */
extern lv_style_t theme_st_label_dim;
/** @brief Shared style: label ok. */
extern lv_style_t theme_st_label_ok;
/** @brief Shared style: label err. */
extern lv_style_t theme_st_label_err;
/** @brief Shared style: label warn. */
extern lv_style_t theme_st_label_warn;
/** @brief Shared style: label accent. */
extern lv_style_t theme_st_label_accent;

/**
 * @brief Initialise every style above. Call once after lv_init(), before any widget is built.
 */
void theme_init(void);

/**
 * @brief Create a label with a font style and a colour style, at a position.
 *
 * Every label in the UI is made through this function, so none is ever left unstyled.
 *
 * @param[in] parent  Parent object.
 * @param[in] font    One of the theme_st_txt_* styles.
 * @param[in] colour  One of the theme_st_label_* styles.
 * @param[in] x       Left edge in the parent.
 * @param[in] y       Top edge in the parent.
 * @param[in] text    Initial text (copied).
 * @return The label.
 */
lv_obj_t *theme_label_create(lv_obj_t *parent, lv_style_t *font, lv_style_t *colour, lv_coord_t x,
                             lv_coord_t y, const char *text);

/**
 * @brief Create an empty rectangle with no theme styling at a position and size.
 *
 * @param[in] parent  Parent object.
 * @param[in] x       Left edge in the parent.
 * @param[in] y       Top edge in the parent.
 * @param[in] w       Width.
 * @param[in] h       Height.
 * @return The object (not scrollable, no padding, transparent).
 */
lv_obj_t *theme_box_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w,
                           lv_coord_t h);

#endif /* THEME_H */
