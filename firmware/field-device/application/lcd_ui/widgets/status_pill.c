/**
 * @file status_pill.c
 * @brief Status pill widget — implementation.
 *
 * @see firmware/field-device/application/lcd_ui/widgets/status_pill.h
 */

#include "lcd_ui/widgets/status_pill.h"
#include "lcd_ui/theme.h"

/* ===================================================================== */
/* Constants                                                            */
/* ===================================================================== */

/* Blink period for PILL_ALARM — visible for 500 ms, dark for 500 ms. */
#define PILL_BLINK_ON_MS 500U
#define PILL_BLINK_OFF_MS 500U

/* ===================================================================== */
/* Internal helpers                                                     */
/* ===================================================================== */

typedef struct
{
    lv_color_t colour;
    const char *text;
} pill_spec_t;

/* Indexed by pill_state_t */
static pill_spec_t k_pill_specs[PILL_STATE_MAX];

/* Animation exec callback — varies object opacity for step-blink */
static void pill_blink_exec_cb(void *var, int32_t val)
{
    lv_obj_t *obj = (lv_obj_t *) var;
    lv_obj_set_style_opa(obj, (lv_opa_t) val, LV_PART_MAIN);
}

static lv_anim_t s_blink_anim; /* one active animation at a time */

/* ===================================================================== */
/* status_pill_create                                                   */
/* ===================================================================== */

lv_obj_t *status_pill_create(lv_obj_t *parent, pill_state_t state)
{
    k_pill_specs[PILL_RUNNING].colour = THEME_COL_OK;
    k_pill_specs[PILL_RUNNING].text = "RUNNING";
    k_pill_specs[PILL_INIT].colour = THEME_COL_DIM;
    k_pill_specs[PILL_INIT].text = "INIT";
    k_pill_specs[PILL_ALARM].colour = THEME_COL_ERR;
    k_pill_specs[PILL_ALARM].text = "ALARM";
    k_pill_specs[PILL_UPDATE].colour = THEME_COL_WARN;
    k_pill_specs[PILL_UPDATE].text = "UPDATE";

    lv_obj_t *pill = lv_obj_create(parent);
    lv_obj_remove_style_all(pill);
    lv_obj_set_style_radius(pill, THEME_RAD_NONE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(pill, THEME_SP_1, LV_PART_MAIN);
    lv_obj_set_style_shadow_width(pill, 0, LV_PART_MAIN);

    lv_obj_t *lbl = lv_label_create(pill);
    lv_obj_remove_style_all(lbl);
    lv_obj_center(lbl);

    /* Store label pointer in the user_data slot of the pill container
     * so status_pill_set_state() can reach it without child traversal. */
    lv_obj_set_user_data(pill, lbl);

    status_pill_set_state(pill, state);
    return pill;
}

/* ===================================================================== */
/* status_pill_set_state                                                */
/* ===================================================================== */

void status_pill_set_state(lv_obj_t *pill_obj, pill_state_t state)
{
    /* Stop any prior blink animation */
    lv_anim_del(pill_obj, pill_blink_exec_cb);

    /* Restore full opacity (in case a prior blink left it transparent) */
    lv_obj_set_style_opa(pill_obj, LV_OPA_COVER, LV_PART_MAIN);

    uint8_t idx = (uint8_t) state;
    if (idx >= (uint8_t) (sizeof(k_pill_specs) / sizeof(k_pill_specs[0])))
    {
        return;
    }

    const pill_spec_t *spec = &k_pill_specs[idx];
    lv_obj_set_style_bg_color(pill_obj, spec->colour, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(pill_obj, LV_OPA_COVER, LV_PART_MAIN);

    lv_obj_t *lbl = (lv_obj_t *) lv_obj_get_user_data(pill_obj);
    if (lbl != NULL)
    {
        lv_obj_set_style_text_color(lbl, THEME_COL_BG, LV_PART_MAIN);
        lv_label_set_text(lbl, spec->text);
    }

    if (state == PILL_ALARM)
    {
        /* Step-blink: 0→255 jump at end, 500 ms on / 500 ms off */
        lv_anim_init(&s_blink_anim);
        lv_anim_set_var(&s_blink_anim, pill_obj);
        lv_anim_set_exec_cb(&s_blink_anim, pill_blink_exec_cb);
        lv_anim_set_values(&s_blink_anim, (int32_t) LV_OPA_COVER, (int32_t) LV_OPA_TRANSP);
        lv_anim_set_time(&s_blink_anim, PILL_BLINK_ON_MS);
        lv_anim_set_playback_time(&s_blink_anim, PILL_BLINK_OFF_MS);
        lv_anim_set_repeat_count(&s_blink_anim, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&s_blink_anim, lv_anim_path_step);
        lv_anim_start(&s_blink_anim);
    }
}
