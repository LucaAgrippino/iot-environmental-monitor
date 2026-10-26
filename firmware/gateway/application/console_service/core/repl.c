/**
 * @file repl.c
 * @brief Scrollback ring and command dispatch.
 */

#include "repl.h"

#include <stdio.h>
#include <string.h>

#include "commands.h"
#include "console_service/port/console_mem.h"
#include "theme.h"

#define MAX_ARGS (8U)
#define LINE_COPY_MAX (121U)

CONSOLE_BIG_BUFFER static scrollback_line_t s_lines[REPL_SCROLLBACK_MAX];
static uint16_t s_head; /* next write position */
static uint16_t s_count;

void repl_clear(void)
{
    (void) memset(s_lines, 0, sizeof(s_lines));
    s_head = 0U;
    s_count = 0U;
}

void repl_init(void)
{
    repl_clear();
    repl_puts(PAL_MUTED, "ENVMON gateway diagnostic shell · type \"help\" or \"?\"");
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
    scrollback_line_t *l = &s_lines[s_head];
    const size_t n = prv_utf8_cut(text, REPL_TEXT_MAX);

    (void) memcpy(l->text, text, n);
    l->text[n] = '\0';
    l->fg = fg;
    s_head = (uint16_t) ((s_head + 1U) % REPL_SCROLLBACK_MAX);
    if (s_count < REPL_SCROLLBACK_MAX)
    {
        s_count++;
    }
}

uint16_t repl_count(void)
{
    return s_count;
}

const scrollback_line_t *repl_line(uint16_t i)
{
    if (i >= s_count)
    {
        return NULL;
    }
    const uint16_t oldest =
        (uint16_t) ((s_head + REPL_SCROLLBACK_MAX - s_count) % REPL_SCROLLBACK_MAX);

    return &s_lines[(oldest + i) % REPL_SCROLLBACK_MAX];
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
    return commands_dispatch(argc, argv);
}
