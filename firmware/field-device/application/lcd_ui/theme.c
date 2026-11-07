/**
 * @file theme.c
 * @brief Design-token styles of the Field Device LCD UI (see theme.h for the sources).
 */

#include "lcd_ui/theme.h"

lv_style_t theme_st_screen;
lv_style_t theme_st_card;
lv_style_t theme_st_tab_bar;
lv_style_t theme_st_header;
lv_style_t theme_st_footer;
lv_style_t theme_st_plain;
lv_style_t theme_st_txt_hero;
lv_style_t theme_st_txt_title_lg;
lv_style_t theme_st_txt_title;
lv_style_t theme_st_txt_head;
lv_style_t theme_st_txt_value;
lv_style_t theme_st_txt_body;
lv_style_t theme_st_txt_body_sb;
lv_style_t theme_st_txt_body_mono;
lv_style_t theme_st_txt_label;
lv_style_t theme_st_txt_value_sm;
lv_style_t theme_st_txt_meta;
lv_style_t theme_st_txt_caption;
lv_style_t theme_st_txt_caption_mono;
lv_style_t theme_st_txt_tab;
lv_style_t theme_st_txt_pill;
lv_style_t theme_st_txt_eyebrow;
lv_style_t theme_st_txt_tiny;
lv_style_t theme_st_label_ink;
lv_style_t theme_st_label_muted;
lv_style_t theme_st_label_dim;
lv_style_t theme_st_label_ok;
lv_style_t theme_st_label_err;
lv_style_t theme_st_label_warn;
lv_style_t theme_st_label_accent;

/** A text style: font and tracking only. */
static void init_text(lv_style_t *st, const lv_font_t *font, lv_coord_t tracking)
{
    lv_style_init(st);
    lv_style_set_text_font(st, font);
    lv_style_set_text_letter_space(st, tracking);
}

/** A text colour style. */
static void init_colour(lv_style_t *st, lv_color_t colour)
{
    lv_style_init(st);
    lv_style_set_text_color(st, colour);
}

/** A flat filled rectangle with an optional 1 px border (the P aesthetic has no radius). */
static void init_surface(lv_style_t *st, lv_color_t fill, lv_color_t border, lv_coord_t border_w)
{
    lv_style_init(st);
    lv_style_set_bg_color(st, fill);
    lv_style_set_bg_opa(st, LV_OPA_COVER);
    lv_style_set_border_color(st, border);
    lv_style_set_border_width(st, border_w);
    lv_style_set_radius(st, THEME_RAD_NONE);
    lv_style_set_pad_all(st, 0);
}

void theme_init(void)
{
    init_surface(&theme_st_screen, THEME_COL_BG, THEME_COL_BG, 0);
    init_surface(&theme_st_card, THEME_COL_SURF, THEME_COL_BORDER, THEME_BORDER_W);
    init_surface(&theme_st_tab_bar, THEME_COL_HEADER_BG, THEME_COL_BORDER, 0);
    init_surface(&theme_st_header, THEME_COL_BG, THEME_COL_BORDER, 0);
    init_surface(&theme_st_footer, THEME_COL_HEADER_BG, THEME_COL_BORDER, 0);

    lv_style_init(&theme_st_plain);
    lv_style_set_bg_opa(&theme_st_plain, LV_OPA_TRANSP);
    lv_style_set_border_width(&theme_st_plain, 0);
    lv_style_set_radius(&theme_st_plain, THEME_RAD_NONE);
    lv_style_set_pad_all(&theme_st_plain, 0);

    init_text(&theme_st_txt_hero, THEME_FONT_HERO, THEME_TRACK_HERO);
    init_text(&theme_st_txt_title_lg, THEME_FONT_TITLE_LG, -1);
    init_text(&theme_st_txt_title, THEME_FONT_TITLE, 0);
    init_text(&theme_st_txt_head, THEME_FONT_HEAD, 0);
    init_text(&theme_st_txt_value, THEME_FONT_VALUE, 0);
    init_text(&theme_st_txt_body, THEME_FONT_BODY, 0);
    init_text(&theme_st_txt_body_sb, THEME_FONT_BODY_SB, 0);
    init_text(&theme_st_txt_body_mono, THEME_FONT_BODY_MONO, 0);
    init_text(&theme_st_txt_label, THEME_FONT_LABEL, 0);
    init_text(&theme_st_txt_value_sm, THEME_FONT_VALUE_SM, 0);
    init_text(&theme_st_txt_meta, THEME_FONT_META, 0);
    init_text(&theme_st_txt_caption, THEME_FONT_CAPTION, 0);
    init_text(&theme_st_txt_caption_mono, THEME_FONT_CAPTION_MONO, 0);
    init_text(&theme_st_txt_tab, THEME_FONT_TAB, THEME_TRACK_TAB);
    init_text(&theme_st_txt_pill, THEME_FONT_PILL, THEME_TRACK_PILL);
    init_text(&theme_st_txt_eyebrow, THEME_FONT_EYEBROW, THEME_TRACK_EYEBROW);
    init_text(&theme_st_txt_tiny, THEME_FONT_TINY, THEME_TRACK_TINY);

    init_colour(&theme_st_label_ink, THEME_COL_INK);
    init_colour(&theme_st_label_muted, THEME_COL_MUTED);
    init_colour(&theme_st_label_dim, THEME_COL_DIM);
    init_colour(&theme_st_label_ok, THEME_COL_OK);
    init_colour(&theme_st_label_err, THEME_COL_ERR);
    init_colour(&theme_st_label_warn, THEME_COL_WARN);
    init_colour(&theme_st_label_accent, THEME_COL_ACCENT);
}

lv_obj_t *theme_label_create(lv_obj_t *parent, lv_style_t *font, lv_style_t *colour, lv_coord_t x,
                             lv_coord_t y, const char *text)
{
    lv_obj_t *label = lv_label_create(parent);
    lv_obj_remove_style_all(label);
    lv_obj_add_style(label, font, LV_PART_MAIN);
    lv_obj_add_style(label, colour, LV_PART_MAIN);
    lv_obj_set_pos(label, x, y);
    lv_label_set_text(label, text);
    return label;
}

lv_obj_t *theme_box_create(lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t w, lv_coord_t h)
{
    lv_obj_t *box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_add_style(box, &theme_st_plain, LV_PART_MAIN);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    return box;
}
