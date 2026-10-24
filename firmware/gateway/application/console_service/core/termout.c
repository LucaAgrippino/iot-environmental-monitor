/**
 * @file termout.c
 * @brief Cell grid helpers and the dirty-line escape-sequence writer.
 */

#include "termout.h"

#include <stdio.h>
#include <string.h>

#define TX_BUF_SIZE (192U)
/* An unchanged gap this short is cheaper to re-send than a new cursor move (ESC[r;cH). */
#define MERGE_GAP (6U)
#define CURSOR_MOVE_MAX (16U)

static const char *const s_glyphs[GLYPH_COUNT - GLYPH_ASCII_COUNT] = {
    "─", "│", "┌", "┐", "└", "┘", "├", "┤", "╌", "▁", "▂", "▃", "▄", "▅", "▆", "▇", "█", "●", "▲",
    "■", "►", "▸", "°", "·", "—", "→", "↑", "↓", "←", "╗", "║", "╔", "═", "╝", "╚", "–", "…",
};

/* ASCII table for glyph_utf8(): one-character strings. */
static char s_ascii[GLYPH_ASCII_COUNT][2];
static bool s_ascii_ready;

static void prv_init_ascii(void)
{
    if (!s_ascii_ready)
    {
        for (uint32_t i = 0U; i < GLYPH_ASCII_COUNT; i++)
        {
            s_ascii[i][0] = (char) (0x20U + i);
            s_ascii[i][1] = '\0';
        }
        s_ascii_ready = true;
    }
}

const char *glyph_utf8(uint8_t glyph)
{
    prv_init_ascii();
    if (glyph < GLYPH_ASCII_COUNT)
    {
        return s_ascii[glyph];
    }
    if (glyph < GLYPH_COUNT)
    {
        return s_glyphs[glyph - GLYPH_ASCII_COUNT];
    }
    return NULL;
}

/* Number of bytes in the UTF-8 sequence that starts with @p lead (1 for invalid leads). */
static size_t prv_utf8_len(uint8_t lead)
{
    if (lead < 0x80U)
    {
        return 1U;
    }
    if ((lead & 0xE0U) == 0xC0U)
    {
        return 2U;
    }
    if ((lead & 0xF0U) == 0xE0U)
    {
        return 3U;
    }
    if ((lead & 0xF8U) == 0xF0U)
    {
        return 4U;
    }
    return 1U;
}

static uint8_t prv_glyph_lookup(const char *s, size_t len)
{
    if (len == 1U)
    {
        const uint8_t c = (uint8_t) s[0];

        return ((c >= 0x20U) && (c <= 0x7EU)) ? (uint8_t) (c - 0x20U) : (uint8_t) ('?' - 0x20);
    }
    for (uint32_t i = 0U; i < (GLYPH_COUNT - GLYPH_ASCII_COUNT); i++)
    {
        if ((strlen(s_glyphs[i]) == len) && (memcmp(s_glyphs[i], s, len) == 0))
        {
            return (uint8_t) (GLYPH_ASCII_COUNT + i);
        }
    }
    return (uint8_t) ('?' - 0x20);
}

void grid_clear(cell_grid_t *grid)
{
    (void) memset(grid, 0, sizeof(*grid));
}

void grid_put_glyph(cell_grid_t *grid, uint8_t row, uint8_t col, uint8_t glyph, uint8_t fg,
                    uint8_t bg, bool bold)
{
    if ((row >= GRID_ROWS) || (col >= GRID_COLS))
    {
        return;
    }
    cell_t *c = &grid->cells[row][col];

    c->glyph = glyph;
    c->fg = fg;
    c->bg_attr = (uint8_t) ((bg & CELL_BG_MASK) | (bold ? CELL_BOLD : 0U));
}

size_t grid_puts(cell_grid_t *grid, uint8_t row, uint8_t col, const char *utf8, uint8_t fg,
                 uint8_t bg, bool bold, size_t max_cols)
{
    size_t written = 0U;
    const char *p = utf8;

    while ((*p != '\0') && (written < max_cols) && ((size_t) col + written < GRID_COLS))
    {
        size_t len = prv_utf8_len((uint8_t) *p);

        for (size_t i = 1U; i < len; i++)
        {
            if (p[i] == '\0')
            {
                len = i; /* truncated sequence: stop at the terminator */
                break;
            }
        }
        grid_put_glyph(grid, row, (uint8_t) (col + written), prv_glyph_lookup(p, len), fg, bg,
                       bold);
        written++;
        p += len;
    }
    return written;
}

void grid_fill(cell_grid_t *grid, uint8_t row, uint8_t col, uint8_t glyph, size_t count, uint8_t fg,
               uint8_t bg, bool bold)
{
    for (size_t i = 0U; i < count; i++)
    {
        grid_put_glyph(grid, row, (uint8_t) (col + i), glyph, fg, bg, bold);
    }
}

/* ---------------------------------------------------------------------- */
/* Escape-sequence writer                                                  */
/* ---------------------------------------------------------------------- */

typedef struct
{
    uint8_t buf[TX_BUF_SIZE];
    size_t len;
    size_t total;
    bool failed;
    termout_sink_t sink;
    void *ctx;
} tx_t;

static void tx_flush(tx_t *tx)
{
    if ((tx->len > 0U) && !tx->failed)
    {
        if (!tx->sink(tx->buf, tx->len, tx->ctx))
        {
            tx->failed = true;
        }
    }
    tx->len = 0U;
}

static void tx_put(tx_t *tx, const char *s, size_t n)
{
    if (tx->failed)
    {
        return;
    }
    if ((tx->len + n) > TX_BUF_SIZE)
    {
        tx_flush(tx);
    }
    (void) memcpy(&tx->buf[tx->len], s, n);
    tx->len += n;
    tx->total += n;
}

static bool cell_equal(const cell_t *a, const cell_t *b)
{
    return (a->glyph == b->glyph) && (a->fg == b->fg) && (a->bg_attr == b->bg_attr);
}

/* A blank cell with the default background shows no foreground, so its colour is irrelevant. */
static bool cell_blank(const cell_t *c)
{
    return (c->glyph == 0U) && ((c->bg_attr & CELL_BG_MASK) == PAL_BG);
}

static bool cell_same_look(const cell_t *a, const cell_t *b)
{
    return cell_equal(a, b) || (cell_blank(a) && cell_blank(b));
}

size_t termout_flush(const cell_grid_t *cur, const cell_grid_t *prev, termout_sink_t sink,
                     void *ctx)
{
    tx_t tx;

    tx.len = 0U;
    tx.total = 0U;
    tx.failed = false;
    tx.sink = sink;
    tx.ctx = ctx;

    for (uint32_t row = 0U; row < GRID_ROWS; row++)
    {
        uint32_t col = 0U;

        while (col < GRID_COLS)
        {
            if (cell_same_look(&cur->cells[row][col], &prev->cells[row][col]))
            {
                col++;
                continue;
            }

            /* A span: from the first changed cell to the last one before a long unchanged gap. */
            const uint32_t first = col;
            uint32_t last = col;
            uint32_t scan = col + 1U;

            while (scan < GRID_COLS)
            {
                if (!cell_same_look(&cur->cells[row][scan], &prev->cells[row][scan]))
                {
                    last = scan;
                }
                else if ((scan - last) > MERGE_GAP)
                {
                    break;
                }
                scan++;
            }

            char seq[THEME_SGR_MAX + CURSOR_MOVE_MAX];
            int n = snprintf(seq, sizeof(seq), "\x1b[%u;%uH", (unsigned) (row + 1U),
                             (unsigned) (first + 1U));
            tx_put(&tx, seq, (size_t) n);

            bool have_attr = false;
            uint8_t cur_fg = 0U;
            uint8_t cur_bg = 0U;
            bool cur_bold = false;

            for (uint32_t c = first; c <= last; c++)
            {
                const cell_t *cell = &cur->cells[row][c];
                const uint8_t bg = cell->bg_attr & CELL_BG_MASK;
                const bool bold = (cell->bg_attr & CELL_BOLD) != 0U;
                const bool blank = cell_blank(cell);

                /* A blank cell inherits whatever is active, so it never forces a new SGR. */
                if (!blank &&
                    (!have_attr || (cell->fg != cur_fg) || (bg != cur_bg) || (bold != cur_bold)))
                {
                    const size_t m = theme_sgr(seq, cell->fg, bg, bold);

                    tx_put(&tx, seq, m);
                    cur_fg = cell->fg;
                    cur_bg = bg;
                    cur_bold = bold;
                    have_attr = true;
                }
                else if (blank && have_attr && (cur_bg != PAL_BG))
                {
                    tx_put(&tx, "\x1b[0m", 4U); /* leave a highlight before drawing a gap */
                    have_attr = false;
                }
                const char *g = glyph_utf8(cell->glyph);

                tx_put(&tx, (g != NULL) ? g : " ", (g != NULL) ? strlen(g) : 1U);
            }
            tx_put(&tx, "\x1b[0m", 4U);
            col = last + 1U;
        }
    }
    tx_flush(&tx);
    return tx.total;
}

bool termout_raw(termout_sink_t sink, void *ctx, const char *seq)
{
    return sink((const uint8_t *) seq, strlen(seq), ctx);
}
