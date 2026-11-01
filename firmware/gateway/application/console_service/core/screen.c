/**
 * @file screen.c
 * @brief Scroll-region writer for the three-panel console.
 */

#include "screen.h"

#include <stdio.h>
#include <string.h>

#include "console_service/port/console_mem.h"
#include "theme.h"

#define ESC_BYTE (0x1BU)
#define SCRATCH_MAX (704U)
#define PROMPT_LABEL_MAX (16U)
#define CLIP_TAIL_RESERVE (12U) /* room for the closing reset and one escape */

static screen_port_t s_port;
static uint16_t s_rows = SCREEN_ROWS_DEFAULT;
static uint16_t s_cols = SCREEN_COLS_DEFAULT;
static bool s_open;
static uint16_t s_prompt_col = 1U; /* column where the typing cursor sits */
CONSOLE_BIG_BUFFER static char s_scratch[SCRATCH_MAX];
static size_t s_len;

/* ---------------------------------------------------------------------- */
/* Layout                                                                  */
/* ---------------------------------------------------------------------- */

static uint16_t prv_prompt_row(void)
{
    return s_rows;
}

static uint16_t prv_result_bottom(void)
{
    return (uint16_t) (s_rows - 1U);
}

static uint16_t prv_divider_row(void)
{
    return (uint16_t) (s_rows - SCREEN_RESULT_ROWS - 1U);
}

static uint16_t prv_log_top(void)
{
    return (uint16_t) (SCREEN_HEADER_ROWS + 1U);
}

static uint16_t prv_log_bottom(void)
{
    return (uint16_t) (prv_divider_row() - 1U);
}

/* ---------------------------------------------------------------------- */
/* Scratch buffer: one write per operation, so a frame leaves in one piece */
/* ---------------------------------------------------------------------- */

static void prv_lock(void)
{
    if (s_port.lock != NULL)
    {
        s_port.lock();
    }
}

static void prv_unlock(void)
{
    if (s_port.unlock != NULL)
    {
        s_port.unlock();
    }
}

static void prv_begin(void)
{
    s_len = 0U;
}

static void prv_put(const char *text, size_t n)
{
    if ((s_len + n) > sizeof(s_scratch))
    {
        n = sizeof(s_scratch) - s_len; /* never overrun; a cut frame beats a corrupted one */
    }
    (void) memcpy(&s_scratch[s_len], text, n);
    s_len += n;
}

static void prv_puts(const char *text)
{
    prv_put(text, strlen(text));
}

static void prv_printf_move(uint16_t row, uint16_t col)
{
    char tmp[16];
    const int n = snprintf(tmp, sizeof(tmp), "\x1b[%u;%uH", (unsigned) row, (unsigned) col);

    if (n > 0)
    {
        prv_put(tmp, (size_t) n);
    }
}

static void prv_region(uint16_t top, uint16_t bottom)
{
    char tmp[16];
    const int n = snprintf(tmp, sizeof(tmp), "\x1b[%u;%ur", (unsigned) top, (unsigned) bottom);

    if (n > 0)
    {
        prv_put(tmp, (size_t) n);
    }
}

static void prv_cursor_to_prompt(void)
{
    prv_printf_move(prv_prompt_row(), s_prompt_col);
}

static void prv_commit(void)
{
    if ((s_len > 0U) && (s_port.send != NULL))
    {
        (void) s_port.send((const uint8_t *) s_scratch, s_len);
    }
    s_len = 0U;
}

/**
 * Append @p src to the scratch buffer as one line: skip control bytes, keep SGR sequences without
 * counting them, stop after @p max_cols visible columns (a UTF-8 character counts once).
 */
static void prv_put_clipped(const char *src, size_t len, uint16_t max_cols)
{
    uint16_t cols = 0U;
    bool styled = false;
    size_t i = 0U;

    while ((i < len) && (s_len + CLIP_TAIL_RESERVE < sizeof(s_scratch)))
    {
        const uint8_t b = (uint8_t) src[i];

        if (b == ESC_BYTE)
        {
            /* Copy an SGR sequence "ESC [ digits/; m" whole; drop any other escape. */
            size_t j = i + 1U;

            if ((j < len) && (src[j] == '['))
            {
                j++;
                while ((j < len) && (((src[j] >= '0') && (src[j] <= '9')) || (src[j] == ';')))
                {
                    j++;
                }
                if ((j < len) && (src[j] == 'm') && ((j - i + 1U) < CLIP_TAIL_RESERVE * 2U))
                {
                    prv_put(&src[i], j - i + 1U);
                    styled = true;
                }
                i = j + 1U;
            }
            else
            {
                i++;
            }
            continue;
        }
        if (b < 0x20U)
        {
            i++; /* CR, LF, tab and the rest never reach the terminal */
            continue;
        }
        if ((b & 0xC0U) != 0x80U) /* a lead byte or ASCII starts a new column */
        {
            if (cols >= max_cols)
            {
                break;
            }
            cols++;
        }
        prv_put(&src[i], 1U);
        i++;
    }
    if (styled)
    {
        prv_puts(THEME_RESET);
    }
}

/* ---------------------------------------------------------------------- */
/* Pieces of the layout                                                    */
/* ---------------------------------------------------------------------- */

static void prv_rule(uint16_t row, const char *title)
{
    char line[SCREEN_COLS_MAX + 1U];
    const size_t tlen = strlen(title);
    size_t n = 0U;

    prv_printf_move(row, 1U);
    prv_puts(theme_sgr(PAL_MUTED));
    for (; (n < 3U) && (n < s_cols); n++)
    {
        line[n] = '-';
    }
    if (tlen > 0U)
    {
        for (size_t i = 0U; (i < tlen) && (n < (size_t) (s_cols - 1U)); i++, n++)
        {
            line[n] = title[i];
        }
    }
    while (n < s_cols)
    {
        line[n++] = '-';
    }
    prv_put(line, n);
    prv_puts(THEME_RESET);
}

static void prv_scroll_line(uint16_t top, uint16_t bottom, uint8_t pal, const char *src, size_t len)
{
    prv_region(top, bottom);
    prv_printf_move(bottom, 1U);
    prv_puts("\n\r"); /* the bottom margin scrolls the region up one line */
    prv_puts(theme_sgr(pal));
    prv_put_clipped(src, len, (uint16_t) (s_cols - 1U));
    prv_puts(THEME_RESET);
    prv_cursor_to_prompt();
}

/* ---------------------------------------------------------------------- */
/* Public API                                                              */
/* ---------------------------------------------------------------------- */

void screen_init(const screen_port_t *port)
{
    s_port = *port;
    s_rows = SCREEN_ROWS_DEFAULT;
    s_cols = SCREEN_COLS_DEFAULT;
    s_open = false;
    s_prompt_col = 1U;
    s_len = 0U;
}

void screen_set_size(uint16_t rows, uint16_t cols)
{
    prv_lock();
    s_rows = (rows < SCREEN_ROWS_MIN) ? SCREEN_ROWS_MIN : rows;
    s_rows = (s_rows > SCREEN_ROWS_MAX) ? SCREEN_ROWS_MAX : s_rows;
    s_cols = (cols < SCREEN_COLS_MIN) ? SCREEN_COLS_MIN : cols;
    s_cols = (s_cols > SCREEN_COLS_MAX) ? SCREEN_COLS_MAX : s_cols;
    prv_unlock();
}

void screen_size(uint16_t *rows, uint16_t *cols)
{
    *rows = s_rows;
    *cols = s_cols;
}

void screen_open(void)
{
    prv_lock();
    prv_begin();
    prv_puts("\x1b[0m\x1b[r\x1b[2J\x1b[?25h");
    prv_rule((uint16_t) (SCREEN_HEADER_ROWS), "");
    prv_rule(prv_divider_row(), " console - type help ");
    s_prompt_col = 1U;
    prv_cursor_to_prompt();
    prv_commit();
    s_open = true;
    prv_unlock();
}

void screen_close(void)
{
    prv_lock();
    prv_begin();
    prv_puts("\x1b[0m\x1b[r");
    prv_printf_move(prv_prompt_row(), 1U);
    prv_puts("\x1b[2K\r\n");
    prv_commit();
    s_open = false;
    prv_unlock();
}

bool screen_is_open(void)
{
    return s_open;
}

void screen_header(const char *line1, const char *line2)
{
    const char *const lines[2] = {line1, line2};

    prv_lock();
    prv_begin();
    for (uint16_t i = 0U; i < 2U; i++)
    {
        if (lines[i] != NULL)
        {
            prv_printf_move((uint16_t) (i + 1U), 1U);
            prv_puts("\x1b[2K");
            prv_put_clipped(lines[i], strlen(lines[i]), (uint16_t) (s_cols - 1U));
        }
    }
    prv_cursor_to_prompt();
    prv_commit();
    prv_unlock();
}

void screen_log(const char *line, size_t len)
{
    prv_lock();
    prv_begin();
    if (s_open)
    {
        prv_scroll_line(prv_log_top(), prv_log_bottom(), PAL_INK, line, len);
    }
    else
    {
        prv_put_clipped(line, len, SCREEN_COLS_MAX);
        prv_puts("\r\n");
    }
    prv_commit();
    prv_unlock();
}

void screen_result(uint8_t pal, const char *text)
{
    prv_lock();
    prv_begin();
    if (s_open)
    {
        prv_scroll_line((uint16_t) (prv_divider_row() + 1U), prv_result_bottom(), pal, text,
                        strlen(text));
    }
    else
    {
        prv_put_clipped(text, strlen(text), SCREEN_COLS_MAX);
        prv_puts("\r\n");
    }
    prv_commit();
    prv_unlock();
}

void screen_results_clear(void)
{
    prv_lock();
    prv_begin();
    for (uint16_t row = (uint16_t) (prv_divider_row() + 1U); row <= prv_result_bottom(); row++)
    {
        prv_printf_move(row, 1U);
        prv_puts("\x1b[2K");
    }
    prv_cursor_to_prompt();
    prv_commit();
    prv_unlock();
}

void screen_prompt(const char *label, const char *text)
{
    const size_t label_len = strnlen(label, PROMPT_LABEL_MAX);
    const size_t text_len = strlen(text);
    const size_t room = (s_cols > (label_len + 2U)) ? (s_cols - label_len - 2U) : 0U;
    size_t skip = 0U;

    /* A line longer than the row shows its tail, so the cursor stays where the typing happens.
     * Count characters, not bytes: never start in the middle of a UTF-8 sequence. */
    if (text_len > room)
    {
        skip = text_len - room;
        while ((skip < text_len) && (((uint8_t) text[skip] & 0xC0U) == 0x80U))
        {
            skip++;
        }
    }
    prv_lock();
    prv_begin();
    prv_printf_move(prv_prompt_row(), 1U);
    prv_puts("\x1b[2K");
    prv_puts(theme_sgr(PAL_PROMPT));
    prv_put(label, label_len);
    prv_puts(THEME_RESET);
    prv_put(&text[skip], text_len - skip);
    s_prompt_col = (uint16_t) (label_len + (text_len - skip) + 1U);
    prv_commit();
    prv_unlock();
}
