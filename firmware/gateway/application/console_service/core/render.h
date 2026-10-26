/**
 * @file render.h
 * @brief Frame composition: banner, menu rail, panels and footer into the cell grid.
 *
 * Pure function of its inputs: it reads the model, the UI state, the chart history and the REPL
 * scrollback, and writes cells. Nothing is sent to the terminal here (that is termout).
 *
 * @note Companion: docs/lld/application/gw-console-service-lld.md §7, §10.2
 */

#ifndef RENDER_H
#define RENDER_H

#include <stdbool.h>

#include "chart.h"
#include "model.h"
#include "termout.h"
#include "ui_state.h"

/* Box geometry (design handoff 01_LAYOUT_AND_TOKENS.md). */
#define R_BANNER_ROWS (8U)
#define R_MENU_W (30U)
#define R_PANEL_W (57U)
#define R_BOX_ROWS (11U)
#define R_PANEL_COL (31U) /* menu + 1 gap */
#define R_BOX_ROW (9U)    /* banner + gap */
#define R_FOOTER_ROW (20U)

/** @brief Everything one frame depends on. */
typedef struct
{
    const ui_state_t *ui;
    const console_model_t *model;
    const chart_history_t *history;
    bool size_ok;         /**< False when the terminal is smaller than 88x30.        */
    bool rx_overflow;     /**< The receive ring overflowed since the last frame.     */
    const char *fw_check; /**< Result line of the last update check, or NULL.      */
} render_input_t;

/** @brief Clear @p grid and compose one full frame into it. */
void render_compose(cell_grid_t *grid, const render_input_t *in);

/** @brief The footer hint for a focus / screen combination (LLD §7.6). */
const char *render_hint(const ui_state_t *ui);

#endif /* RENDER_H */
