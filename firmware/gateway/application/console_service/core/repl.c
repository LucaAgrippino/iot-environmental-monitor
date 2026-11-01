/**
 * @file repl.c
 * @brief Command output (south panel) and command dispatch.
 */

#include "repl.h"

#include <stdio.h>
#include <string.h>

#include "commands.h"
#include "screen.h"
#include "theme.h"

#define MAX_ARGS (8U)
#define LINE_COPY_MAX (121U)

#ifdef TEST
static scrollback_line_t s_lines[REPL_SCROLLBACK_MAX];
static uint16_t s_count;

uint16_t repl_count(void)
{
    return s_count;
}

const scrollback_line_t *repl_line(uint16_t i)
{
    return (i < s_count) ? &s_lines[i] : NULL;
}
#endif /* TEST */

void repl_clear(void)
{
#ifdef TEST
    (void) memset(s_lines, 0, sizeof(s_lines));
    s_count = 0U;
#endif
    screen_results_clear();
}

void repl_init(void)
{
    repl_clear();
    repl_puts(PAL_MUTED, "ENVMON gateway diagnostic shell - type \"help\" or \"?\"");
}

static size_t prv_utf8_cut(const char *text, size_t max_bytes)
{
    size_t n = strlen(text);

    if (n < max_bytes)
    {
        return n;
    }
    n = max_bytes - 1U;
    while ((n > 0U) && (((uint8_t) text[n] & 0xC0U) == 0x80U))
    {
        n--; /* do not cut inside a multi-byte character */
    }
    return n;
}

void repl_puts(uint8_t fg, const char *text)
{
    char line[REPL_TEXT_MAX];
    const size_t n = prv_utf8_cut(text, REPL_TEXT_MAX);

    (void) memcpy(line, text, n);
    line[n] = '\0';
#ifdef TEST
    if (s_count < REPL_SCROLLBACK_MAX)
    {
        (void) memcpy(s_lines[s_count].text, line, n + 1U);
        s_lines[s_count].fg = fg;
        s_count++;
    }
#endif
    screen_result(fg, line);
}

static void prv_echo(const char *cmd)
{
    char line[REPL_TEXT_MAX];

    (void) snprintf(line, sizeof(line), "envmon> %s", cmd);
    repl_puts(PAL_INK, line);
}

console_service_err_t repl_execute(const char *line)
{
    char copy[LINE_COPY_MAX];
    const char *argv[MAX_ARGS];
    int argc = 0;
    bool too_many = false;

    (void) snprintf(copy, sizeof(copy), "%s", line);

    /* Trim and tokenise in place. */
    char *p = copy;

    while (*p != '\0')
    {
        while ((*p == ' ') || (*p == '\t'))
        {
            *p = '\0';
            p++;
        }
        if (*p == '\0')
        {
            break;
        }
        if ((unsigned) argc < MAX_ARGS)
        {
            argv[argc] = p;
            argc++;
        }
        else
        {
            too_many = true; /* never drop words silently: a description would be cut short */
        }
        while ((*p != '\0') && (*p != ' ') && (*p != '\t'))
        {
            p++;
        }
    }

    if (argc == 0)
    {
        repl_puts(PAL_PROMPT, "envmon>");
        return CONSOLE_SERVICE_ERR_OK;
    }
    prv_echo(line);
    if (too_many)
    {
        repl_puts(PAL_ERR, "[ERR] too many words (max 8)");
        return CONSOLE_SERVICE_ERR_LINE_OVERFLOW;
    }
    return commands_dispatch(argc, argv);
}
