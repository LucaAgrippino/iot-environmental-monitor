/**
 * @file theme.c
 * @brief Console palette, proximity gradient and SGR writer.
 */

#include "theme.h"

#include <stdio.h>

typedef struct
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
} rgb_t;

/* Design tokens, in palette_token_t order. */
static const rgb_t s_tokens[PAL_TOKEN_COUNT] = {
    [PAL_BG] = {11, 15, 22},        [PAL_BORDER] = {51, 72, 91},
    [PAL_TITLE] = {159, 208, 238},  [PAL_TITLE_ACT] = {207, 235, 255},
    [PAL_LOGO] = {123, 175, 212},   [PAL_MUTED] = {126, 139, 153},
    [PAL_ACCENT] = {123, 175, 212}, [PAL_OK] = {111, 191, 142},
    [PAL_WARN] = {224, 178, 82},    [PAL_ERR] = {204, 102, 102},
    [PAL_NUM] = {255, 212, 121},    [PAL_NUM_SEL] = {255, 233, 176},
    [PAL_INK] = {199, 211, 223},    [PAL_AXIS] = {94, 107, 122},
    [PAL_THR] = {122, 106, 58},     [PAL_SEL_BG] = {123, 175, 212},
    [PAL_SEL_FG] = {8, 18, 26},     [PAL_PROMPT] = {111, 191, 142},
};

static const rgb_t BASE = {123, 175, 212};
static const rgb_t WARN = {224, 178, 82};
static const rgb_t OVER = {204, 102, 102};

#define Q8 (256)
#define GLOW_WARN_END_Q8 (115)   /* glow 0.45 at the threshold                       */
#define GLOW_OVER_RANGE_Q8 (141) /* glow rises a further 0.55 across the red blend  */
#define GLOW_BOLD_Q8 (128)       /* glow > 0.5 earns bold                            */
#define GLOW_NUDGE_MAX (30)      /* brightness nudge at glow 1.0                     */
#define OVER_SPAN_Q8 (154)       /* t runs 1.0 .. 1.6: 0.6 in Q8                     */

static int32_t clamp_i32(int32_t v, int32_t lo, int32_t hi)
{
    return (v < lo) ? lo : ((v > hi) ? hi : v);
}

static uint8_t lerp_u8(uint8_t a, uint8_t b, int32_t num, int32_t den)
{
    return (uint8_t) ((int32_t) a + ((((int32_t) b - (int32_t) a) * num) / den));
}

/* Glow, in Q8, of gradient step g (0 .. PAL_GRAD_STEPS). */
static int32_t grad_glow_q8(uint32_t g)
{
    if (g <= PAL_GRAD_HALF)
    {
        return ((int32_t) g * GLOW_WARN_END_Q8) / (int32_t) PAL_GRAD_HALF;
    }
    return GLOW_WARN_END_Q8 + ((((int32_t) g - (int32_t) PAL_GRAD_HALF) * GLOW_OVER_RANGE_Q8) /
                               (int32_t) PAL_GRAD_HALF);
}

uint8_t theme_prox_index(int32_t t_q8)
{
    uint32_t step;

    if (t_q8 <= 0)
    {
        step = 0U;
    }
    else if (t_q8 <= PROX_ONE)
    {
        step = (uint32_t) (((t_q8 * (int32_t) PAL_GRAD_HALF) + (Q8 / 2)) / Q8);
        if (step == 0U)
        {
            step = 1U; /* any positive proximity is already visibly warmer than BASE */
        }
    }
    else
    {
        const int32_t k_q8 = clamp_i32(((t_q8 - PROX_ONE) * Q8) / OVER_SPAN_Q8, 0, Q8);

        step = PAL_GRAD_HALF + (uint32_t) (((k_q8 * (int32_t) PAL_GRAD_HALF) + (Q8 / 2)) / Q8);
    }
    return (uint8_t) (PAL_GRAD_FIRST + step);
}

bool theme_prox_bold(int32_t t_q8)
{
    return grad_glow_q8((uint32_t) (theme_prox_index(t_q8) - PAL_GRAD_FIRST)) > GLOW_BOLD_Q8;
}

void theme_rgb(uint8_t idx, uint8_t *r, uint8_t *g, uint8_t *b)
{
    rgb_t c;

    if (idx < PAL_TOKEN_COUNT)
    {
        c = s_tokens[idx];
    }
    else if (idx < PAL_COUNT)
    {
        const uint32_t step = (uint32_t) idx - PAL_GRAD_FIRST;
        const int32_t nudge = (grad_glow_q8(step) * GLOW_NUDGE_MAX) / Q8;

        if (step <= PAL_GRAD_HALF)
        {
            c.r = lerp_u8(BASE.r, WARN.r, (int32_t) step, (int32_t) PAL_GRAD_HALF);
            c.g = lerp_u8(BASE.g, WARN.g, (int32_t) step, (int32_t) PAL_GRAD_HALF);
            c.b = lerp_u8(BASE.b, WARN.b, (int32_t) step, (int32_t) PAL_GRAD_HALF);
        }
        else
        {
            const int32_t k = (int32_t) step - (int32_t) PAL_GRAD_HALF;

            c.r = lerp_u8(WARN.r, OVER.r, k, (int32_t) PAL_GRAD_HALF);
            c.g = lerp_u8(WARN.g, OVER.g, k, (int32_t) PAL_GRAD_HALF);
            c.b = lerp_u8(WARN.b, OVER.b, k, (int32_t) PAL_GRAD_HALF);
        }
        c.r = (uint8_t) clamp_i32((int32_t) c.r + nudge, 0, 255);
        c.g = (uint8_t) clamp_i32((int32_t) c.g + nudge, 0, 255);
        c.b = (uint8_t) clamp_i32((int32_t) c.b + nudge, 0, 255);
    }
    else
    {
        c = s_tokens[PAL_INK];
    }
    *r = c.r;
    *g = c.g;
    *b = c.b;
}

#ifdef CONSOLE_256COLOR
/* Nearest entry of the xterm 6x6x6 cube: 16 + 36r + 6g + b. */
static uint8_t cube_index(uint8_t r, uint8_t g, uint8_t b)
{
    const uint8_t rr = (uint8_t) (((uint32_t) r * 5U + 127U) / 255U);
    const uint8_t gg = (uint8_t) (((uint32_t) g * 5U + 127U) / 255U);
    const uint8_t bb = (uint8_t) (((uint32_t) b * 5U + 127U) / 255U);

    return (uint8_t) (16U + (36U * rr) + (6U * gg) + bb);
}
#endif /* CONSOLE_256COLOR */

size_t theme_sgr(char *buf, uint8_t fg, uint8_t bg, bool bold)
{
    uint8_t r;
    uint8_t g;
    uint8_t b;
    int n;

    theme_rgb(fg, &r, &g, &b);
#ifdef CONSOLE_256COLOR
    n = snprintf(buf, THEME_SGR_MAX, "\x1b[0%s;38;5;%u", bold ? ";1" : "",
                 (unsigned) cube_index(r, g, b));
#else
    n = snprintf(buf, THEME_SGR_MAX, "\x1b[0%s;38;2;%u;%u;%u", bold ? ";1" : "", (unsigned) r,
                 (unsigned) g, (unsigned) b);
#endif
    if (bg != PAL_BG)
    {
        theme_rgb(bg, &r, &g, &b);
#ifdef CONSOLE_256COLOR
        n += snprintf(&buf[n], THEME_SGR_MAX - (size_t) n, ";48;5;%u",
                      (unsigned) cube_index(r, g, b));
#else
        n += snprintf(&buf[n], THEME_SGR_MAX - (size_t) n, ";48;2;%u;%u;%u", (unsigned) r,
                      (unsigned) g, (unsigned) b);
#endif
    }
    buf[n] = 'm';
    n++;
    return (size_t) n;
}
