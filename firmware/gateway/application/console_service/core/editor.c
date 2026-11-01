/**
 * @file editor.c
 * @brief Line editing for the console prompt.
 */

#include "editor.h"

#include <string.h>

void editor_init(editor_t *ed)
{
    (void) memset(ed, 0, sizeof(*ed));
}

void editor_clear(editor_t *ed)
{
    ed->line[0] = '\0';
    ed->len = 0U;
}

static editor_action_t prv_add(editor_t *ed, char c)
{
    if ((ed->len + 1U) >= EDITOR_LINE_MAX)
    {
        editor_clear(ed);
        return ED_LINE_OVERFLOW;
    }
    ed->line[ed->len] = c;
    ed->len++;
    ed->line[ed->len] = '\0';
    return ED_CHANGED;
}

static editor_action_t prv_answer(const key_event_t *key)
{
    switch (key->kind)
    {
    case KEY_PRINTABLE:
        return ((key->ch == 'y') || (key->ch == 'Y')) ? ED_CONFIRM_YES : ED_CONFIRM_NO;
    case KEY_DIGIT:
    case KEY_ENTER:
    case KEY_ESCAPE:
        return ED_CONFIRM_NO;
    case KEY_REDRAW:
        return ED_REDRAW;
    default:
        return ED_NONE; /* arrows, paging and size reports are not answers */
    }
}

editor_action_t editor_key(editor_t *ed, const key_event_t *key, bool confirm)
{
    if (confirm)
    {
        return prv_answer(key);
    }
    switch (key->kind)
    {
    case KEY_PRINTABLE:
        return prv_add(ed, (char) key->ch);
    case KEY_DIGIT:
        return prv_add(ed, (char) ('0' + key->ch));
    case KEY_BACKSPACE:
        if (ed->len == 0U)
        {
            return ED_NONE;
        }
        ed->len--;
        ed->line[ed->len] = '\0';
        return ED_CHANGED;
    case KEY_ESCAPE:
        if (ed->len == 0U)
        {
            return ED_NONE;
        }
        editor_clear(ed);
        return ED_CHANGED;
    case KEY_UP:
        if (ed->last[0] == '\0')
        {
            return ED_NONE;
        }
        (void) memcpy(ed->line, ed->last, sizeof(ed->line));
        ed->len = (uint8_t) strlen(ed->line);
        return ED_CHANGED;
    case KEY_ENTER:
        if (ed->len > 0U)
        {
            (void) memcpy(ed->last, ed->line, sizeof(ed->last));
        }
        return ED_RUN_LINE;
    case KEY_REDRAW:
        return ED_REDRAW;
    default:
        return ED_NONE;
    }
}
