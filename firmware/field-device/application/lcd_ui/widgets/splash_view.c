/**
 * @file splash_view.c
 * @brief Boot splash widget. See splash_view.h.
 */

#include "lcd_ui/widgets/splash_view.h"

#include <stdio.h>
#include <string.h>

#include "lcd_ui/theme.h"

/** Layout of splash.html on the 800 x 480 canvas (the content is centred as a flex column) */
#define SPLASH_LOGO_W 56
#define SPLASH_LOGO_H 52
#define SPLASH_BRAND_GAP 14  /**< logo to text */
#define SPLASH_BRAND_Y 172   /**< top of the brand row */
#define SPLASH_WORDMARK_DY 0 /**< wordmark offset in the brand row */
#define SPLASH_SUB_DY 36     /**< subtitle offset in the brand row */
#define SPLASH_BAR_Y 270     /**< top of the progress track */
#define SPLASH_BAR_H 4
#define SPLASH_BAR_LABEL_DY 14 /**< labels below the track */
#define SPLASH_BADGE_INSET 24  /**< badge from the top and right edges */
#define SPLASH_DOT_D 8
#define SPLASH_FOOTER_INSET 24
#define SPLASH_FOOTER_H 12
#define SPLASH_PCT_MAX 100U
#define SPLASH_PCT_TEXT_W 36 /**< room for "100%" in the 11 px mono face */
#define SPLASH_SUB_TRACK 2   /**< letter spacing of the subtitle, 0.22 em at 11 px */
#define SPLASH_FOOTER_TRACK 1

#define SPLASH_PRODUCT "ENVMON"
#define SPLASH_PRODUCT_SUB "FIELD DEVICE"

static void set_badge(splash_view_t *s, bool ready)
{
    lv_label_set_text(s->badge_lbl, ready ? "READY" : "BOOT");
    lv_obj_set_style_bg_color(s->dot, ready ? THEME_COL_OK : THEME_COL_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_text_color(s->badge_lbl, ready ? THEME_COL_OK : THEME_COL_MUTED, LV_PART_MAIN);
}

#ifndef TEST
/* The logo of the design (a 64 x 64 viewBox) at 56 px: panel outline, rule, handle, dot and trace
 */
static void logo_draw_cb(lv_event_t *e)
{
    lv_obj_t *obj = lv_event_get_target(e);
    lv_draw_ctx_t *ctx = lv_event_get_draw_ctx(e);
    lv_area_t box;

    lv_obj_get_coords(obj, &box);
    const lv_coord_t ox = box.x1;
    const lv_coord_t oy = box.y1;

    lv_draw_line_dsc_t line;
    lv_draw_line_dsc_init(&line);
    line.color = THEME_COL_ACCENT;
    line.width = 2;
    line.round_start = 1U;
    line.round_end = 1U;

    lv_draw_rect_dsc_t rect;
    lv_draw_rect_dsc_init(&rect);
    rect.bg_opa = LV_OPA_TRANSP;
    rect.border_color = THEME_COL_ACCENT;
    rect.border_width = 2;
    rect.radius = 3;
    const lv_area_t panel = {.x1 = (lv_coord_t) (ox + 4),
                             .y1 = (lv_coord_t) (oy + 12),
                             .x2 = (lv_coord_t) (ox + 52),
                             .y2 = (lv_coord_t) (oy + 43)};
    lv_draw_rect(ctx, &rect, &panel);

    const lv_point_t rule[2] = {{(lv_coord_t) (ox + 4), (lv_coord_t) (oy + 23)},
                                {(lv_coord_t) (ox + 52), (lv_coord_t) (oy + 23)}};
    lv_draw_line(ctx, &line, &rule[0], &rule[1]);

    const lv_point_t handle[4] = {{(lv_coord_t) (ox + 12), (lv_coord_t) (oy + 12)},
                                  {(lv_coord_t) (ox + 12), (lv_coord_t) (oy + 5)},
                                  {(lv_coord_t) (ox + 44), (lv_coord_t) (oy + 5)},
                                  {(lv_coord_t) (ox + 44), (lv_coord_t) (oy + 12)}};
    for (uint32_t i = 1U; i < 4U; i++)
    {
        lv_draw_line(ctx, &line, &handle[i - 1U], &handle[i]);
    }

    lv_draw_rect_dsc_t dot;
    lv_draw_rect_dsc_init(&dot);
    dot.bg_color = THEME_COL_ACCENT;
    dot.bg_opa = LV_OPA_COVER;
    dot.border_width = 0;
    dot.radius = LV_RADIUS_CIRCLE;
    const lv_area_t dot_area = {.x1 = (lv_coord_t) (ox + 10),
                                .y1 = (lv_coord_t) (oy + 16),
                                .x2 = (lv_coord_t) (ox + 12),
                                .y2 = (lv_coord_t) (oy + 18)};
    lv_draw_rect(ctx, &dot, &dot_area);

    line.color = THEME_COL_OK;
    const lv_point_t trace[6] = {{(lv_coord_t) (ox + 19), (lv_coord_t) (oy + 33)},
                                 {(lv_coord_t) (ox + 24), (lv_coord_t) (oy + 33)},
                                 {(lv_coord_t) (ox + 27), (lv_coord_t) (oy + 28)},
                                 {(lv_coord_t) (ox + 32), (lv_coord_t) (oy + 38)},
                                 {(lv_coord_t) (ox + 35), (lv_coord_t) (oy + 33)},
                                 {(lv_coord_t) (ox + 42), (lv_coord_t) (oy + 33)}};
    for (uint32_t i = 1U; i < 6U; i++)
    {
        lv_draw_line(ctx, &line, &trace[i - 1U], &trace[i]);
    }
}
#endif

void splash_view_create(splash_view_t *s, lv_obj_t *parent, const char *footer_left,
                        const char *footer_right)
{
    (void) memset(s, 0, sizeof(*s));

    s->root = theme_box_create(parent, 0, 0, THEME_CANVAS_W, THEME_CANVAS_H);
    lv_obj_add_style(s->root, &theme_st_screen, LV_PART_MAIN);

    /* brand row, centred as a block */
    const lv_font_t *word_font = THEME_FONT_TITLE_LG;
    const lv_font_t *sub_font = THEME_FONT_CAPTION_MONO;
    const lv_coord_t word_w = lv_txt_get_width(SPLASH_PRODUCT, (uint32_t) strlen(SPLASH_PRODUCT),
                                               word_font, 0, LV_TEXT_FLAG_NONE);
    const lv_coord_t sub_w =
        lv_txt_get_width(SPLASH_PRODUCT_SUB, (uint32_t) strlen(SPLASH_PRODUCT_SUB), sub_font,
                         SPLASH_SUB_TRACK, LV_TEXT_FLAG_NONE);
    const lv_coord_t text_w = (word_w > sub_w) ? word_w : sub_w;
    const lv_coord_t row_w = (lv_coord_t) (SPLASH_LOGO_W + SPLASH_BRAND_GAP + text_w);
    const lv_coord_t row_x = (lv_coord_t) ((THEME_CANVAS_W - row_w) / 2);
    const lv_coord_t text_x = (lv_coord_t) (row_x + SPLASH_LOGO_W + SPLASH_BRAND_GAP);

    s->logo = theme_box_create(s->root, row_x, SPLASH_BRAND_Y, SPLASH_LOGO_W, SPLASH_LOGO_H);
#ifndef TEST
    lv_obj_add_event_cb(s->logo, logo_draw_cb, LV_EVENT_DRAW_MAIN, NULL);
#endif
    (void) theme_label_create(s->root, &theme_st_txt_title_lg, &theme_st_label_ink, text_x,
                              (lv_coord_t) (SPLASH_BRAND_Y + SPLASH_WORDMARK_DY), SPLASH_PRODUCT);
    lv_obj_t *sub =
        theme_label_create(s->root, &theme_st_txt_caption_mono, &theme_st_label_muted, text_x,
                           (lv_coord_t) (SPLASH_BRAND_Y + SPLASH_SUB_DY), SPLASH_PRODUCT_SUB);
    lv_obj_set_style_text_letter_space(sub, SPLASH_SUB_TRACK, LV_PART_MAIN);

    /* progress bar: 4 px track with a 1 px border, accent fill */
    const lv_coord_t bar_x = (lv_coord_t) ((THEME_CANVAS_W - SPLASH_BAR_W) / 2);
    lv_obj_t *track = theme_box_create(s->root, bar_x, SPLASH_BAR_Y, SPLASH_BAR_W,
                                       (lv_coord_t) (SPLASH_BAR_H + (2 * THEME_BORDER_W)));
    lv_obj_set_style_bg_color(track, THEME_COL_SURF, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(track, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(track, THEME_BORDER_W, LV_PART_MAIN);
    lv_obj_set_style_border_color(track, THEME_COL_BORDER, LV_PART_MAIN);
    s->fill = theme_box_create(track, 0, 0, 0, SPLASH_BAR_H);
    lv_obj_set_style_bg_color(s->fill, THEME_COL_ACCENT, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->fill, LV_OPA_COVER, LV_PART_MAIN);

    const lv_coord_t label_y =
        (lv_coord_t) (SPLASH_BAR_Y + SPLASH_BAR_H + (2 * THEME_BORDER_W) + SPLASH_BAR_LABEL_DY);
    s->step_lbl = theme_label_create(s->root, &theme_st_txt_caption_mono, &theme_st_label_ink,
                                     bar_x, label_y, "");
    s->pct_lbl =
        theme_label_create(s->root, &theme_st_txt_caption_mono, &theme_st_label_muted,
                           (lv_coord_t) (bar_x + SPLASH_BAR_W - SPLASH_PCT_TEXT_W), label_y, "0%");
    lv_obj_set_width(s->pct_lbl, SPLASH_PCT_TEXT_W);
    lv_obj_set_style_text_align(s->pct_lbl, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);

    /* BOOT / READY badge, top right */
    s->badge_lbl = theme_label_create(s->root, &theme_st_txt_caption_mono, &theme_st_label_muted,
                                      (lv_coord_t) (THEME_CANVAS_W - SPLASH_BADGE_INSET - 44),
                                      SPLASH_BADGE_INSET, "BOOT");
    lv_obj_set_width(s->badge_lbl, 44);
    lv_obj_set_style_text_align(s->badge_lbl, LV_TEXT_ALIGN_RIGHT, LV_PART_MAIN);
    s->dot = theme_box_create(
        s->root,
        (lv_coord_t) (THEME_CANVAS_W - SPLASH_BADGE_INSET - 44 - THEME_SP_3 - SPLASH_DOT_D),
        (lv_coord_t) (SPLASH_BADGE_INSET + 3), SPLASH_DOT_D, SPLASH_DOT_D);
    lv_obj_set_style_radius(s->dot, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s->dot, LV_OPA_COVER, LV_PART_MAIN);
    set_badge(s, false);

    /* footer */
    const lv_coord_t foot_y = (lv_coord_t) (THEME_CANVAS_H - SPLASH_FOOTER_INSET - SPLASH_FOOTER_H);
    lv_obj_t *fl = theme_label_create(s->root, &theme_st_txt_pill, &theme_st_label_muted,
                                      SPLASH_FOOTER_INSET, foot_y, footer_left);
    lv_obj_set_style_text_letter_space(fl, SPLASH_FOOTER_TRACK, LV_PART_MAIN);
    lv_obj_t *fr = theme_label_create(s->root, &theme_st_txt_pill, &theme_st_label_muted, 0, foot_y,
                                      footer_right);
    lv_obj_set_style_text_letter_space(fr, SPLASH_FOOTER_TRACK, LV_PART_MAIN);
    const lv_coord_t fr_w =
        lv_txt_get_width(footer_right, (uint32_t) strlen(footer_right), THEME_FONT_PILL,
                         SPLASH_FOOTER_TRACK, LV_TEXT_FLAG_NONE);
    lv_obj_set_x(fr, (lv_coord_t) (THEME_CANVAS_W - SPLASH_FOOTER_INSET - fr_w));

    lv_obj_add_flag(s->root, LV_OBJ_FLAG_HIDDEN);
}

void splash_view_set(splash_view_t *s, uint8_t percent, const char *step)
{
    char text[8];

    if (percent > SPLASH_PCT_MAX)
    {
        percent = (uint8_t) SPLASH_PCT_MAX;
    }
    s->percent = percent;

    lv_obj_set_width(s->fill, (lv_coord_t) (((uint32_t) SPLASH_BAR_W * percent) / SPLASH_PCT_MAX));
    (void) snprintf(text, sizeof(text), "%u%%", (unsigned) percent);
    lv_label_set_text(s->pct_lbl, text);
    if (step != NULL)
    {
        lv_label_set_text(s->step_lbl, step);
    }
    set_badge(s, percent >= SPLASH_PCT_MAX);
}

void splash_view_show(splash_view_t *s, bool visible)
{
    if (visible)
    {
        lv_obj_clear_flag(s->root, LV_OBJ_FLAG_HIDDEN);
    }
    else
    {
        lv_obj_add_flag(s->root, LV_OBJ_FLAG_HIDDEN);
    }
}

bool splash_view_visible(const splash_view_t *s)
{
    return !lv_obj_has_flag(s->root, LV_OBJ_FLAG_HIDDEN);
}
