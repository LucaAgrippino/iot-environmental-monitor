/**
 * @file theme.c
 * @brief Design-token style initialisation for LcdUi.
 *
 * @see firmware/field-device/application/lcd_ui/theme.h
 */

#include "lcd_ui/theme.h"

/* ===================================================================== */
/* File-scope style objects (extern-declared in theme.h)                */
/* ===================================================================== */

lv_style_t theme_st_screen;
lv_style_t theme_st_card;
lv_style_t theme_st_tab_bar;
lv_style_t theme_st_tab_btn;
lv_style_t theme_st_header;
lv_style_t theme_st_footer;
lv_style_t theme_st_label_ink;
lv_style_t theme_st_label_muted;
lv_style_t theme_st_label_dim;
lv_style_t theme_st_label_ok;
lv_style_t theme_st_label_err;
lv_style_t theme_st_label_warn;
lv_style_t theme_st_label_accent;

/* ===================================================================== */
/* theme_init                                                            */
/* ===================================================================== */

void theme_init(void)
{
    /* ── Root screen ──────────────────────────────────────────────────── */
    lv_style_init(&theme_st_screen);
    lv_style_set_bg_color(&theme_st_screen, THEME_COL_BG);
    lv_style_set_bg_opa(&theme_st_screen, LV_OPA_COVER);
    lv_style_set_pad_all(&theme_st_screen, 0);
    lv_style_set_radius(&theme_st_screen, THEME_RAD_NONE);
    lv_style_set_border_width(&theme_st_screen, 0);

    /* ── Card — SURF background, 1 px border, rectangular ─────────────── */
    lv_style_init(&theme_st_card);
    lv_style_set_bg_color(&theme_st_card, THEME_COL_SURF);
    lv_style_set_bg_opa(&theme_st_card, LV_OPA_COVER);
    lv_style_set_border_color(&theme_st_card, THEME_COL_BORDER);
    lv_style_set_border_width(&theme_st_card, 1); /* 01_DESIGN_TOKENS.md: 1 px borders */
    lv_style_set_radius(&theme_st_card, THEME_RAD_NONE);
    lv_style_set_pad_all(&theme_st_card, 0);

    /* ── Tab bar — HEADER_BG fill, bottom border ──────────────────────── */
    lv_style_init(&theme_st_tab_bar);
    lv_style_set_bg_color(&theme_st_tab_bar, THEME_COL_HEADER_BG);
    lv_style_set_bg_opa(&theme_st_tab_bar, LV_OPA_COVER);
    lv_style_set_border_color(&theme_st_tab_bar, THEME_COL_BORDER);
    lv_style_set_border_width(&theme_st_tab_bar, 1);
    lv_style_set_radius(&theme_st_tab_bar, THEME_RAD_NONE);
    lv_style_set_pad_all(&theme_st_tab_bar, 0);

    /* ── Inactive tab button ──────────────────────────────────────────── */
    lv_style_init(&theme_st_tab_btn);
    lv_style_set_bg_color(&theme_st_tab_btn, THEME_COL_HEADER_BG);
    lv_style_set_bg_opa(&theme_st_tab_btn, LV_OPA_COVER);
    lv_style_set_border_width(&theme_st_tab_btn, 0);
    lv_style_set_radius(&theme_st_tab_btn, THEME_RAD_NONE);
    lv_style_set_pad_all(&theme_st_tab_btn, 0);
    /* TODO(fonts): replace FONT_TAB with lv_font_conv output */
    lv_style_set_text_font(&theme_st_tab_btn, &lv_font_montserrat_14);
    lv_style_set_text_color(&theme_st_tab_btn, THEME_COL_TAB_INACTIVE);

    /* ── Header bar — BG fill, bottom border ─────────────────────────── */
    lv_style_init(&theme_st_header);
    lv_style_set_bg_color(&theme_st_header, THEME_COL_BG);
    lv_style_set_bg_opa(&theme_st_header, LV_OPA_COVER);
    lv_style_set_border_color(&theme_st_header, THEME_COL_BORDER);
    lv_style_set_border_width(&theme_st_header, 1);
    lv_style_set_radius(&theme_st_header, THEME_RAD_NONE);
    lv_style_set_pad_all(&theme_st_header, 0);

    /* ── Footer bar — HEADER_BG fill, top border ─────────────────────── */
    lv_style_init(&theme_st_footer);
    lv_style_set_bg_color(&theme_st_footer, THEME_COL_HEADER_BG);
    lv_style_set_bg_opa(&theme_st_footer, LV_OPA_COVER);
    lv_style_set_border_color(&theme_st_footer, THEME_COL_BORDER);
    lv_style_set_border_width(&theme_st_footer, 1);
    lv_style_set_radius(&theme_st_footer, THEME_RAD_NONE);
    lv_style_set_pad_all(&theme_st_footer, 0);

    /* ── Text style: primary (COL_INK) ───────────────────────────────── */
    lv_style_init(&theme_st_label_ink);
    lv_style_set_text_color(&theme_st_label_ink, THEME_COL_INK);
    /* TODO(fonts): replace with FONT_EYEBROW (Sans 10 Bold Upper) when available */
    lv_style_set_text_font(&theme_st_label_ink, &lv_font_montserrat_14);

    /* ── Text style: muted (COL_MUTED) ───────────────────────────────── */
    lv_style_init(&theme_st_label_muted);
    lv_style_set_text_color(&theme_st_label_muted, THEME_COL_MUTED);
    lv_style_set_text_font(&theme_st_label_muted, &lv_font_montserrat_14);

    /* ── Text style: dim placeholder (COL_DIM) ───────────────────────── */
    lv_style_init(&theme_st_label_dim);
    lv_style_set_text_color(&theme_st_label_dim, THEME_COL_DIM);
    lv_style_set_text_font(&theme_st_label_dim, &lv_font_montserrat_14);

    /* ── Text style: valid / OK (COL_OK) ─────────────────────────────── */
    lv_style_init(&theme_st_label_ok);
    lv_style_set_text_color(&theme_st_label_ok, THEME_COL_OK);
    lv_style_set_text_font(&theme_st_label_ok, &lv_font_montserrat_14);

    /* ── Text style: error / alarm (COL_ERR) ─────────────────────────── */
    lv_style_init(&theme_st_label_err);
    lv_style_set_text_color(&theme_st_label_err, THEME_COL_ERR);
    lv_style_set_text_font(&theme_st_label_err, &lv_font_montserrat_14);

    /* ── Text style: warning (COL_WARN) ──────────────────────────────── */
    lv_style_init(&theme_st_label_warn);
    lv_style_set_text_color(&theme_st_label_warn, THEME_COL_WARN);
    lv_style_set_text_font(&theme_st_label_warn, &lv_font_montserrat_14);

    /* ── Text style: accent / active (COL_ACCENT) ────────────────────── */
    lv_style_init(&theme_st_label_accent);
    lv_style_set_text_color(&theme_st_label_accent, THEME_COL_ACCENT);
    lv_style_set_text_font(&theme_st_label_accent, &lv_font_montserrat_14);
}
