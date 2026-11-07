/**
 * @file ui_format.c
 * @brief Text formatting for the LCD UI (see ui_format.h).
 */

#include "lcd_ui/ui_format.h"

#include <stdio.h>
#include <string.h>

#define SECONDS_PER_MINUTE (60U)
#define SECONDS_PER_HOUR (3600U)
#define SECONDS_PER_DAY (86400U) /* 24 * 3600 */
#define HOURS_PER_DAY (24U)

/* Channel scales: units per whole number (SensorService fixed-point formats). */
#define SCALE_CENTI (100U) /* temperature, humidity: 0.01 units */
#define SCALE_DECI (10U)   /* pressure: 0.1 hPa */

static uint32_t abs_u32(int32_t v)
{
    return (v < 0) ? (uint32_t) (-(int64_t) v) : (uint32_t) v;
}

void ui_fmt_fixed(char *out, uint32_t out_len, int32_t raw, uint32_t divisor, uint32_t decimals)
{
    if ((out == NULL) || (out_len == 0U))
    {
        return;
    }

    /* Scale down to the requested number of decimals, rounding half away from zero. */
    uint32_t shown_div = 1U;
    for (uint32_t i = 0U; i < decimals; i++)
    {
        shown_div *= 10U;
    }
    const uint64_t mag = abs_u32(raw);
    uint64_t scaled;
    if (divisor >= shown_div)
    {
        const uint32_t drop = divisor / shown_div;
        scaled = (mag + (drop / 2U)) / drop;
    }
    else
    {
        scaled = mag * (shown_div / divisor);
    }

    const uint32_t whole = (uint32_t) (scaled / shown_div);
    const uint32_t frac = (uint32_t) (scaled % shown_div);
    const char *sign = ((raw < 0) && (scaled != 0U)) ? UI_MINUS : "";

    if (decimals == 0U)
    {
        (void) snprintf(out, out_len, "%s%lu", sign, (unsigned long) whole);
    }
    else
    {
        (void) snprintf(out, out_len, "%s%lu.%0*lu", sign, (unsigned long) whole, (int) decimals,
                        (unsigned long) frac);
    }
}

void ui_fmt_value(char *out, uint32_t out_len, ui_channel_t ch, int32_t raw)
{
    switch (ch)
    {
    case UI_CH_TEMPERATURE:
    case UI_CH_HUMIDITY:
        ui_fmt_fixed(out, out_len, raw, SCALE_CENTI, 1U);
        break;
    case UI_CH_PRESSURE:
        ui_fmt_fixed(out, out_len, raw, SCALE_DECI, 0U);
        break;
    default:
        if ((out != NULL) && (out_len > 0U))
        {
            out[0] = '\0';
        }
        break;
    }
}

void ui_fmt_delta(char *out, uint32_t out_len, ui_channel_t ch, int32_t delta, uint32_t minutes)
{
    char num[16];

    if ((out == NULL) || (out_len == 0U))
    {
        return;
    }
    if (ch == UI_CH_PRESSURE)
    {
        ui_fmt_fixed(num, sizeof(num), delta, SCALE_DECI, 1U);
    }
    else
    {
        ui_fmt_fixed(num, sizeof(num), delta, SCALE_CENTI, 1U);
    }
    /* A positive change carries an explicit plus sign; zero is shown unsigned. */
    const char *plus = ((delta > 0) && (num[0] != '\0')) ? "+" : "";
    (void) snprintf(out, out_len, "%s%s / %lum", plus, num, (unsigned long) minutes);
}

void ui_fmt_clock(char *out, uint32_t out_len, uint32_t seconds)
{
    if ((out == NULL) || (out_len == 0U))
    {
        return;
    }
    const uint32_t s = seconds % SECONDS_PER_DAY;
    (void) snprintf(out, out_len, "%02lu:%02lu:%02lu", (unsigned long) (s / SECONDS_PER_HOUR),
                    (unsigned long) ((s % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE),
                    (unsigned long) (s % SECONDS_PER_MINUTE));
}

void ui_fmt_uptime(char *out, uint32_t out_len, uint32_t seconds)
{
    if ((out == NULL) || (out_len == 0U))
    {
        return;
    }
    const uint32_t days = seconds / SECONDS_PER_DAY;
    if (days == 0U)
    {
        ui_fmt_clock(out, out_len, seconds);
        return;
    }
    const uint32_t rest = seconds % SECONDS_PER_DAY;
    (void) snprintf(out, out_len, "%lud %02lu:%02lu", (unsigned long) days,
                    (unsigned long) (rest / SECONDS_PER_HOUR),
                    (unsigned long) ((rest % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE));
}

void ui_fmt_ago(char *out, uint32_t out_len, uint32_t seconds)
{
    if ((out == NULL) || (out_len == 0U))
    {
        return;
    }
    const uint32_t days = seconds / SECONDS_PER_DAY;
    const uint32_t hours = (seconds % SECONDS_PER_DAY) / SECONDS_PER_HOUR;
    const uint32_t minutes = (seconds % SECONDS_PER_HOUR) / SECONDS_PER_MINUTE;

    if (days > 0U)
    {
        (void) snprintf(out, out_len, "%lud %luh ago", (unsigned long) days, (unsigned long) hours);
    }
    else if (hours > 0U)
    {
        (void) snprintf(out, out_len, "%luh %lum ago", (unsigned long) hours,
                        (unsigned long) minutes);
    }
    else if (minutes > 0U)
    {
        (void) snprintf(out, out_len, "%lum ago", (unsigned long) minutes);
    }
    else
    {
        (void) snprintf(out, out_len, "just now");
    }
}

#define PARSE_MAX_TENTHS (214748364) /* INT32_MAX / 10: leaves room for the last digit */

bool ui_parse_tenths(const char *text, int32_t *tenths)
{
    if ((text == NULL) || (tenths == NULL))
    {
        return false;
    }
    bool negative = false;
    if (*text == '-')
    {
        negative = true;
        text++;
    }

    int32_t whole = 0;
    uint32_t whole_digits = 0U;
    while ((*text >= '0') && (*text <= '9'))
    {
        if (whole > ((PARSE_MAX_TENTHS - 9) / 10))
        {
            return false;
        }
        whole = (whole * 10) + (int32_t) (*text - '0');
        whole_digits++;
        text++;
    }

    int32_t frac = 0;
    uint32_t frac_digits = 0U;
    if (*text == '.')
    {
        text++;
        if ((*text >= '0') && (*text <= '9'))
        {
            frac = (int32_t) (*text - '0');
            frac_digits = 1U;
            text++;
        }
    }
    if ((*text != '\0') || ((whole_digits == 0U) && (frac_digits == 0U)))
    {
        return false; /* trailing text, a second decimal digit, or no digit at all */
    }
    const int32_t value = (whole * 10) + frac;
    *tenths = negative ? -value : value;
    return true;
}
