/**
 * @file termout.h
 * @brief Cell grid, glyph table and the escape-sequence writer with dirty-line diffing.
 *
 * The renderer composes a frame into a cell grid. termout_flush() compares it with the
 * previous frame and emits only what changed: one cursor move per changed span, one SGR per
 * attribute change, and one reset per span (LLD §10.3). A cell is three bytes (glyph index,
 * foreground index, background index + bold bit), which keeps two full grids at about 12 KB
 * (GW-CS-O1).
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §5.3, §10
 */

#ifndef TERMOUT_H
#define TERMOUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "theme.h"

#define GRID_COLS (88U)
#define GRID_ROWS (22U) /* banner 8 + gap 1 + menu/panel 11 + footer 1 + margin 1 */

/** Bold attribute, stored in the top bit of cell_t::bg_attr. */
#define CELL_BOLD (0x80U)
#define CELL_BG_MASK (0x7FU)

/** One character cell. All-zero is a blank cell on the default background. */
typedef struct
{
    uint8_t glyph;   /**< Index into the glyph table (0 = space).  */
    uint8_t fg;      /**< Foreground palette index.                */
    uint8_t bg_attr; /**< Background palette index | CELL_BOLD.    */
} cell_t;

typedef struct
{
    cell_t cells[GRID_ROWS][GRID_COLS];
} cell_grid_t;

/** Glyph indices 0..94 are printable ASCII (0x20 + index); named extras follow. */
enum
{
    GLYPH_ASCII_COUNT = 95,
    GLYPH_H = 95, /* ─ */
    GLYPH_V,      /* │ */
    GLYPH_TL,     /* ┌ */
    GLYPH_TR,     /* ┐ */
    GLYPH_BL,     /* └ */
    GLYPH_BR,     /* ┘ */
    GLYPH_TEE_L,  /* ├ */
    GLYPH_TEE_R,  /* ┤ */
    GLYPH_DASH,   /* ╌ */
    GLYPH_BLK1,   /* ▁ .. █ are GLYPH_BLK1 .. GLYPH_BLK1 + 7 */
    GLYPH_BLK2,
    GLYPH_BLK3,
    GLYPH_BLK4,
    GLYPH_BLK5,
    GLYPH_BLK6,
    GLYPH_BLK7,
    GLYPH_BLK8,
    GLYPH_DOT_FILL, /* ● */
    GLYPH_TRI_UP,   /* ▲ */
    GLYPH_SQUARE,   /* ■ */
    GLYPH_PTR_R,    /* ► */
    GLYPH_CUR_R,    /* ▸ */
    GLYPH_DEGREE,   /* ° */
    GLYPH_MIDDOT,   /* · */
    GLYPH_EMDASH,   /* — */
    GLYPH_ARROW_R,  /* → */
    GLYPH_ARROW_U,  /* ↑ */
    GLYPH_ARROW_D,  /* ↓ */
    GLYPH_ARROW_L,  /* ← */
    GLYPH_LG_TR,    /* ╗ */
    GLYPH_LG_V,     /* ║ */
    GLYPH_LG_TL,    /* ╔ */
    GLYPH_LG_H,     /* ═ */
    GLYPH_LG_BR,    /* ╝ */
    GLYPH_LG_BL,    /* ╚ */
    GLYPH_ENDASH,   /* – */
    GLYPH_ELLIPSIS, /* … */
    GLYPH_COUNT
};

/** Full block, used by the logo and the input cursor. */
#define GLYPH_FULL ((uint8_t) GLYPH_BLK8)

/** Sink that receives the escape sequences; returns false if the write failed. */
typedef bool (*termout_sink_t)(const uint8_t *data, size_t len, void *ctx);

/** @brief Zero every cell of @p grid. */
void grid_clear(cell_grid_t *grid);

/**
 * @brief Write one UTF-8 string into the grid, clipped at @p max_cols columns.
 *
 * Every glyph in the table is one column wide. Unknown code points become '?'.
 *
 * @return Columns written.
 */
size_t grid_puts(cell_grid_t *grid, uint8_t row, uint8_t col, const char *utf8, uint8_t fg,
                 uint8_t bg, bool bold, size_t max_cols);

/** @brief Write one glyph (by index) into a cell; out-of-range coordinates are ignored. */
void grid_put_glyph(cell_grid_t *grid, uint8_t row, uint8_t col, uint8_t glyph, uint8_t fg,
                    uint8_t bg, bool bold);

/** @brief Repeat one glyph @p count times along a row. */
void grid_fill(cell_grid_t *grid, uint8_t row, uint8_t col, uint8_t glyph, size_t count, uint8_t fg,
               uint8_t bg, bool bold);

/** @brief UTF-8 encoding of a glyph index, or NULL if out of range. */
const char *glyph_utf8(uint8_t glyph);

/**
 * @brief Diff @p cur against @p prev and send the minimal escape sequences to @p sink.
 *
 * @param[in] cur   The frame to show.
 * @param[in] prev  What the terminal currently shows.
 * @param[in] sink  Byte sink.
 * @param[in] ctx   Passed to @p sink.
 * @return Bytes emitted; 0 when the grids are identical. If the sink fails, emission stops.
 */
size_t termout_flush(const cell_grid_t *cur, const cell_grid_t *prev, termout_sink_t sink,
                     void *ctx);

/** @brief Send a raw escape string through the sink (init / teardown sequences). */
bool termout_raw(termout_sink_t sink, void *ctx, const char *seq);

#endif /* TERMOUT_H */
