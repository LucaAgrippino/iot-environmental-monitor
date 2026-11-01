/**
 * @file theme.c
 * @brief Token to ANSI colour mapping.
 */

#include "theme.h"

static const char *const k_sgr[PAL_TOKEN_COUNT] = {
    [PAL_INK] = "\x1b[0m",       [PAL_MUTED] = "\x1b[0;90m", [PAL_OK] = "\x1b[0;32m",
    [PAL_WARN] = "\x1b[0;33m",   [PAL_ERR] = "\x1b[0;91m",   [PAL_NUM] = "\x1b[0;36m",
    [PAL_PROMPT] = "\x1b[1;32m",
};

const char *theme_sgr(uint8_t token)
{
    return (token < (uint8_t) PAL_TOKEN_COUNT) ? k_sgr[token] : k_sgr[PAL_INK];
}
