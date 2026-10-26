/**
 * @file input.c
 * @brief Escape-sequence parser for the console.
 */

#include "input.h"

#define ESC_BYTE (0x1BU)
#define FORM_FEED (0x0CU) /* Ctrl-L */
#define CSI_MAX_LEN (12U)
#define SIZE_REPORT_PARAMS (3U)
#define SIZE_REPORT_KIND (8U)

typedef enum
{
    ST_IDLE,
    ST_ESC, /* got ESC, waiting for '[' / 'O' / anything else */
    ST_CSI, /* ESC [ ... collecting parameter bytes           */
    ST_SS3  /* ESC O: application cursor key, one byte follows */
} state_t;

static state_t s_state;
static uint32_t s_esc_at_ms;
static char s_csi[CSI_MAX_LEN];
static uint8_t s_csi_len;
static bool s_stash_valid;
static uint8_t s_stash;

void input_reset(void)
{
    s_state = ST_IDLE;
    s_csi_len = 0U;
    s_stash_valid = false;
}

static bool emit(key_event_t *out, key_kind_t kind, uint8_t ch)
{
    out->kind = kind;
    out->ch = ch;
    out->rows = 0U;
    out->cols = 0U;
    return true;
}

static bool arrow_for(uint8_t final, key_event_t *out)
{
    switch (final)
    {
    case 'A':
        return emit(out, KEY_UP, 0U);
    case 'B':
        return emit(out, KEY_DOWN, 0U);
    case 'C':
        return emit(out, KEY_RIGHT, 0U);
    case 'D':
        return emit(out, KEY_LEFT, 0U);
    default:
        return false;
    }
}

/* Parse "8;rows;cols" out of a CSI parameter string, as sent by the terminal for ESC[18t. */
static bool parse_size_report(key_event_t *out)
{
    uint32_t v[SIZE_REPORT_PARAMS] = {0U, 0U, 0U};
    uint8_t idx = 0U;

    for (uint8_t i = 0U; i < s_csi_len; i++)
    {
        const char c = s_csi[i];

        if ((c >= '0') && (c <= '9'))
        {
            v[idx] = (v[idx] * 10U) + (uint32_t) (c - '0');
            if (v[idx] > 9999U)
            {
                return false;
            }
        }
        else if ((c == ';') && (idx + 1U < SIZE_REPORT_PARAMS))
        {
            idx++;
        }
        else
        {
            return false;
        }
    }
    if ((idx != (SIZE_REPORT_PARAMS - 1U)) || (v[0] != SIZE_REPORT_KIND))
    {
        return false;
    }
    out->kind = KEY_SIZE_REPORT;
    out->ch = 0U;
    out->rows = (uint16_t) v[1];
    out->cols = (uint16_t) v[2];
    return true;
}

static bool plain_byte(uint8_t b, key_event_t *out)
{
    if (b == ESC_BYTE)
    {
        return false; /* handled by the caller: begins a sequence */
    }
    if ((b == '\r') || (b == '\n'))
    {
        return emit(out, KEY_ENTER, 0U);
    }
    if ((b == 0x7FU) || (b == 0x08U))
    {
        return emit(out, KEY_BACKSPACE, 0U);
    }
    if (b == '\t')
    {
        return emit(out, KEY_TAB, 0U);
    }
    if (b == FORM_FEED)
    {
        return emit(out, KEY_REDRAW, 0U);
    }
    if ((b >= '1') && (b <= '7'))
    {
        return emit(out, KEY_DIGIT, (uint8_t) (b - '0'));
    }
    if ((b >= 0x20U) && (b <= 0x7EU))
    {
        return emit(out, KEY_PRINTABLE, b);
    }
    return false; /* Ctrl-C and other control bytes are ignored */
}

bool input_feed(uint8_t byte, uint32_t now_ms, key_event_t *out)
{
    switch (s_state)
    {
    case ST_IDLE:
        if (byte == ESC_BYTE)
        {
            s_state = ST_ESC;
            s_esc_at_ms = now_ms;
            return false;
        }
        return plain_byte(byte, out);

    case ST_ESC:
        if (byte == '[')
        {
            s_state = ST_CSI;
            s_csi_len = 0U;
            return false;
        }
        if (byte == 'O')
        {
            s_state = ST_SS3;
            return false;
        }
        /* ESC was a lone Escape key, and this byte is something else. */
        s_state = ST_IDLE;
        if (byte == ESC_BYTE)
        {
            s_state = ST_ESC; /* ESC ESC: report the first as Escape, start over on the second */
            s_esc_at_ms = now_ms;
        }
        else
        {
            s_stash = byte; /* decoded by input_take_pending() */
            s_stash_valid = true;
        }
        return emit(out, KEY_ESCAPE, 0U);

    case ST_SS3:
        s_state = ST_IDLE;
        return arrow_for(byte, out);

    case ST_CSI:
        if ((byte >= 0x40U) && (byte <= 0x7EU))
        {
            s_state = ST_IDLE;
            if (s_csi_len == 0U)
            {
                return arrow_for(byte, out);
            }
            if (byte == 't')
            {
                return parse_size_report(out);
            }
            if ((byte == '~') && (s_csi_len == 1U))
            {
                if (s_csi[0] == '5')
                {
                    return emit(out, KEY_PGUP, 0U);
                }
                if (s_csi[0] == '6')
                {
                    return emit(out, KEY_PGDN, 0U);
                }
            }
            return false; /* a CSI sequence we do not use (Home, Delete, function keys, ...) */
        }
        if (s_csi_len < CSI_MAX_LEN)
        {
            s_csi[s_csi_len] = (char) byte;
            s_csi_len++;
        }
        else
        {
            s_state = ST_IDLE; /* runaway sequence: abandon it */
        }
        return false;

    default:
        s_state = ST_IDLE;
        return false;
    }
}

bool input_take_pending(key_event_t *out)
{
    if (!s_stash_valid)
    {
        return false;
    }
    s_stash_valid = false;
    return plain_byte(s_stash, out);
}

bool input_poll_timeout(uint32_t now_ms, key_event_t *out)
{
    if ((s_state == ST_ESC) && ((now_ms - s_esc_at_ms) >= INPUT_ESC_TIMEOUT_MS))
    {
        s_state = ST_IDLE;
        return emit(out, KEY_ESCAPE, 0U);
    }
    return false;
}

bool input_esc_pending(void)
{
    return s_state == ST_ESC;
}
