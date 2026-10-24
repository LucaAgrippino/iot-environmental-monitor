/**
 * @file ui_state.c
 * @brief Navigation, focus model and console line editing.
 */

#include "ui_state.h"

#include <string.h>

#define GRAPH_CHANNELS (3U)

void ui_init(ui_state_t *ui)
{
    (void) memset(ui, 0, sizeof(*ui));
    ui->sel = 0U;
    ui->focus = FOCUS_MENU;
    ui->channel = 0U;
}

screen_t ui_screen(const ui_state_t *ui)
{
    return (screen_t) ui->sel;
}

void ui_clear_input(ui_state_t *ui)
{
    ui->input[0] = '\0';
    ui->input_len = 0U;
}

bool ui_screen_is_interactive(screen_t screen)
{
    /* Only the graph (channel select), the console (command line) and the firmware actions take
     * keys; the other panels are read-only views, so there is nothing to focus. The firmware panel
     * has two actions (check for update, reboot). */
    return (screen == SCR_GRAPH) || (screen == SCR_CONSOLE) || (screen == SCR_FIRMWARE);
}

static ui_action_t prv_menu_key(ui_state_t *ui, const key_event_t *key)
{
    switch (key->kind)
    {
    case KEY_UP:
        ui->sel = (uint8_t) ((ui->sel + UI_MENU_COUNT - 1U) % UI_MENU_COUNT);
        break;
    case KEY_DOWN:
        ui->sel = (uint8_t) ((ui->sel + 1U) % UI_MENU_COUNT);
        break;
    case KEY_DIGIT:
        ui->sel = (uint8_t) (key->ch - 1U);
        break;
    case KEY_ENTER:
    case KEY_TAB:
        if (ui_screen_is_interactive(ui_screen(ui)))
        {
            ui->focus = FOCUS_PANEL;
        }
        break;
    default:
        break;
    }
    return UI_ACT_NONE;
}

static ui_action_t prv_console_key(ui_state_t *ui, const key_event_t *key)
{
    switch (key->kind)
    {
    case KEY_ESCAPE:
        ui->focus = FOCUS_MENU;
        return UI_ACT_NONE;
    case KEY_ENTER:
        return UI_ACT_RUN_LINE;
    case KEY_PGUP:
        ui->scroll =
            (uint16_t) (ui->scroll + UI_SCROLL_STEP); /* the caller clamps to the history */
        return UI_ACT_NONE;
    case KEY_PGDN:
        ui->scroll = (ui->scroll > UI_SCROLL_STEP) ? (uint16_t) (ui->scroll - UI_SCROLL_STEP) : 0U;
        return UI_ACT_NONE;
    case KEY_BACKSPACE:
        if (ui->input_len > 0U)
        {
            ui->input_len--;
            ui->input[ui->input_len] = '\0';
        }
        return UI_ACT_NONE;
    case KEY_DIGIT:
    case KEY_PRINTABLE:
        if ((ui->input_len + 1U) >= UI_INPUT_MAX)
        {
            ui_clear_input(ui);
            return UI_ACT_LINE_OVERFLOW;
        }
        ui->input[ui->input_len] =
            (char) ((key->kind == KEY_DIGIT) ? (char) ('0' + key->ch) : (char) key->ch);
        ui->input_len++;
        ui->input[ui->input_len] = '\0';
        return UI_ACT_NONE;
    default:
        return UI_ACT_NONE;
    }
}

static ui_action_t prv_graph_key(ui_state_t *ui, const key_event_t *key)
{
    switch (key->kind)
    {
    case KEY_ESCAPE:
        ui->focus = FOCUS_MENU;
        break;
    case KEY_UP:
    case KEY_LEFT:
        ui->channel = (uint8_t) ((ui->channel + GRAPH_CHANNELS - 1U) % GRAPH_CHANNELS);
        break;
    case KEY_DOWN:
    case KEY_RIGHT:
        ui->channel = (uint8_t) ((ui->channel + 1U) % GRAPH_CHANNELS);
        break;
    default:
        break;
    }
    return UI_ACT_NONE;
}

static ui_action_t prv_firmware_key(ui_state_t *ui, const key_event_t *key)
{
    switch (key->kind)
    {
    case KEY_ESCAPE:
        ui->focus = FOCUS_MENU;
        break;
    case KEY_UP:
        ui->fw_sel = (uint8_t) ((ui->fw_sel + UI_FW_ACTIONS - 1U) % UI_FW_ACTIONS);
        break;
    case KEY_DOWN:
        ui->fw_sel = (uint8_t) ((ui->fw_sel + 1U) % UI_FW_ACTIONS);
        break;
    case KEY_ENTER:
        return (ui->fw_sel == 0U) ? UI_ACT_FW_CHECK : UI_ACT_FW_REBOOT;
    default:
        break;
    }
    return UI_ACT_NONE;
}

ui_action_t ui_handle_key(ui_state_t *ui, const key_event_t *key)
{
    /* The y/N prompt takes over the keyboard: y / Y confirms, any other key declines. */
    if (ui->confirm_active)
    {
        if ((key->kind == KEY_PRINTABLE) && ((key->ch == 'y') || (key->ch == 'Y')))
        {
            return UI_ACT_CONFIRM_YES;
        }
        return UI_ACT_CONFIRM_NO;
    }

    if (ui->focus == FOCUS_MENU)
    {
        return prv_menu_key(ui, key);
    }
    switch (ui_screen(ui))
    {
    case SCR_CONSOLE:
        return prv_console_key(ui, key);
    case SCR_GRAPH:
        return prv_graph_key(ui, key);
    case SCR_FIRMWARE:
        return prv_firmware_key(ui, key);
    default:
        if (key->kind == KEY_ESCAPE)
        {
            ui->focus = FOCUS_MENU;
        }
        return UI_ACT_NONE;
    }
}
