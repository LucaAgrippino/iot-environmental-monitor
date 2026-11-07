/**
 * @file numpad.c
 * @brief Numeric keypad widget (see numpad.h).
 */

#include "lcd_ui/widgets/numpad.h"

#include <stddef.h>
#include <string.h>

#include "lcd_ui/theme.h"

#define NP_COLS 3
#define NP_ROWS 4
#define NP_GAP THEME_SP_2 /* 6 px */
#define NP_DISPLAY_H 42   /* .numpad-display min-height */
#define NP_SIGN_W 44      /* touch target of the +/- toggle */
#define NP_KEY_W ((NUMPAD_W - ((NP_COLS - 1) * NP_GAP)) / NP_COLS)

typedef struct
{
    const char *name;
    const char *label;
    button_kind_t kind;
} key_def_t;

/* Row-major, .numpad-grid order. The backspace key reads "DEL" because the font has no U+232B. */
static const key_def_t k_keys[12] = {
    {"7", "7", BUTTON_KEY}, {"8", "8", BUTTON_KEY}, {"9", "9", BUTTON_KEY},
    {"4", "4", BUTTON_KEY}, {"5", "5", BUTTON_KEY}, {"6", "6", BUTTON_KEY},
    {"1", "1", BUTTON_KEY}, {"2", "2", BUTTON_KEY}, {"3", "3", BUTTON_KEY},
    {".", ".", BUTTON_KEY}, {"0", "0", BUTTON_KEY}, {"DEL", "DEL", BUTTON_KEY_ERR},
};

static numpad_t *s_np;

static void refresh(numpad_t *np)
{
    lv_label_set_text(np->display_text, np->active ? np->buf : "\xE2\x80\x94"); /* — when idle */
}

void numpad_press(numpad_t *np, const char *key)
{
    if (!np->active)
    {
        return;
    }
    if (strcmp(key, "DEL") == 0)
    {
        if (np->len > 0U)
        {
            np->len--;
            np->buf[np->len] = '\0';
        }
    }
    else if (strcmp(key, "SIGN") == 0)
    {
        /* toggle a leading minus, like a calculator's +/- */
        if ((np->len > 0U) && (np->buf[0] == '-'))
        {
            (void) memmove(np->buf, &np->buf[1], np->len);
            np->len--;
        }
        else if (np->len < NUMPAD_TEXT_MAX)
        {
            (void) memmove(&np->buf[1], np->buf, np->len + 1U);
            np->buf[0] = '-';
            np->len++;
        }
    }
    else if ((strcmp(key, ".") == 0) && (strchr(np->buf, '.') != NULL))
    {
        /* one decimal point only */
    }
    else if (np->len < NUMPAD_TEXT_MAX)
    {
        np->buf[np->len] = key[0];
        np->len++;
        np->buf[np->len] = '\0';
    }
    refresh(np);
    if (np->changed != NULL)
    {
        np->changed(np->buf);
    }
}

static void key_event_cb(lv_event_t *e)
{
    const char *name = (const char *) lv_event_get_user_data(e);
    if ((s_np != NULL) && (name != NULL))
    {
        numpad_press(s_np, name);
    }
}

void numpad_create(numpad_t *np, lv_obj_t *parent, lv_coord_t x, lv_coord_t y, lv_coord_t h,
                   numpad_changed_cb_t changed)
{
    s_np = np;
    np->len = 0U;
    np->buf[0] = '\0';
    np->active = false;
    np->invalid = false;
    np->changed = changed;

    np->box = theme_box_create(parent, x, y, NUMPAD_W, h);

    np->display = theme_box_create(np->box, 0, 0, NUMPAD_W, NP_DISPLAY_H);
    lv_obj_set_style_bg_color(np->display, THEME_COL_SURF2, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(np->display, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(np->display, THEME_BORDER_W, LV_PART_MAIN);
    lv_obj_set_style_border_color(np->display, THEME_COL_BORDER, LV_PART_MAIN);
    const lv_font_t *value = THEME_FONT_VALUE;
    np->display_text =
        theme_label_create(np->display, &theme_st_txt_value, &theme_st_label_accent, THEME_SP_5,
                           (lv_coord_t) ((NP_DISPLAY_H - value->line_height) / 2), "");

    button_create(&np->sign, np->display, (lv_coord_t) (NUMPAD_W - NP_SIGN_W - 2), 0, NP_SIGN_W,
                  (lv_coord_t) (NP_DISPLAY_H - (2 * THEME_BORDER_W)), "+/-", BUTTON_BAR);
    button_on_click(&np->sign, key_event_cb, (void *) "SIGN");

    const lv_coord_t grid_y = (lv_coord_t) (NP_DISPLAY_H + NP_GAP);
    const lv_coord_t key_h = (lv_coord_t) ((h - grid_y - ((NP_ROWS - 1) * NP_GAP)) / NP_ROWS);
    for (uint32_t i = 0U; i < 12U; i++)
    {
        const lv_coord_t kx = (lv_coord_t) ((i % NP_COLS) * (NP_KEY_W + NP_GAP));
        const lv_coord_t ky = (lv_coord_t) (grid_y + ((i / NP_COLS) * (key_h + NP_GAP)));
        button_create(&np->key[i], np->box, kx, ky, NP_KEY_W, key_h, k_keys[i].label,
                      k_keys[i].kind);
        button_on_click(&np->key[i], key_event_cb, (void *) k_keys[i].name);
    }
    refresh(np);
}

void numpad_load(numpad_t *np, const char *text)
{
    np->active = (text != NULL);
    np->len = 0U;
    np->buf[0] = '\0';
    if (text != NULL)
    {
        (void) strncpy(np->buf, text, NUMPAD_TEXT_MAX);
        np->buf[NUMPAD_TEXT_MAX] = '\0';
        np->len = (uint32_t) strlen(np->buf);
    }
    numpad_set_invalid(np, false);
    refresh(np);
}

void numpad_set_invalid(numpad_t *np, bool invalid)
{
    np->invalid = invalid;
    lv_obj_set_style_text_color(np->display_text, invalid ? THEME_COL_ERR : THEME_COL_ACCENT,
                                LV_PART_MAIN);
}

const char *numpad_text(const numpad_t *np)
{
    return np->buf;
}
