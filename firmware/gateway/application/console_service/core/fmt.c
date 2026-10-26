/**
 * @file fmt.c
 * @brief Fixed-point text helpers.
 */

#include "fmt.h"

#include <stdio.h>

#define MAX_U32_DIGITS (9U)

static int32_t pow10_u(uint8_t n)
{
    int32_t p = 1;

    for (uint8_t i = 0U; i < n; i++)
    {
        p *= 10;
    }
    return p;
}

static uint8_t digits_of_scale(int32_t scale)
{
    uint8_t d = 0U;

    while (scale > 1)
    {
        scale /= 10;
        d++;
    }
    return d;
}

size_t fmt_fixed(char *buf, size_t size, int32_t value, int32_t scale, uint8_t decimals)
{
    const uint8_t sd = digits_of_scale(scale);
    const uint8_t dec = (decimals > sd) ? sd : decimals;
    const int32_t step = scale / pow10_u(dec); /* source units per displayed unit */
    const int32_t half = step / 2;
    const int64_t mag = (value < 0) ? -(int64_t) value : (int64_t) value;
    const int64_t rounded = (mag + half) / step;
    const int32_t div = pow10_u(dec);
    const int32_t whole = (int32_t) (rounded / div);
    const int32_t frac = (int32_t) (rounded % div);
    const char *sign = ((value < 0) && (rounded != 0)) ? "-" : "";
    int n;

    if (dec == 0U)
    {
        n = snprintf(buf, size, "%s%ld", sign, (long) whole);
    }
    else
    {
        n = snprintf(buf, size, "%s%ld.%0*ld", sign, (long) whole, (int) dec, (long) frac);
    }
    return (n < 0) ? 0U : (size_t) n;
}

bool fmt_parse_fixed(const char *text, int32_t scale, int32_t *out)
{
    const uint8_t max_dec = digits_of_scale(scale);
    bool negative = false;
    int64_t whole = 0;
    int64_t frac = 0;
    uint8_t whole_digits = 0U;
    uint8_t frac_digits = 0U;

    if ((text == NULL) || (out == NULL))
    {
        return false;
    }
    if ((*text == '-') || (*text == '+'))
    {
        negative = (*text == '-');
        text++;
    }
    while ((*text >= '0') && (*text <= '9'))
    {
        whole = (whole * 10) + (*text - '0');
        whole_digits++;
        text++;
        if (whole_digits > MAX_U32_DIGITS)
        {
            return false;
        }
    }
    if (*text == '.')
    {
        text++;
        while ((*text >= '0') && (*text <= '9'))
        {
            frac = (frac * 10) + (*text - '0');
            frac_digits++;
            text++;
            if (frac_digits > max_dec)
            {
                return false;
            }
        }
        if (frac_digits == 0U)
        {
            return false; /* "5." is malformed */
        }
    }
    if ((*text != '\0') || (whole_digits == 0U))
    {
        return false;
    }

    const int64_t total = (whole * scale) + (frac * (scale / pow10_u(frac_digits)));
    const int64_t signed_total = negative ? -total : total;

    if ((signed_total > INT32_MAX) || (signed_total < INT32_MIN))
    {
        return false;
    }
    *out = (int32_t) signed_total;
    return true;
}

bool fmt_parse_u32(const char *text, uint32_t *out)
{
    uint32_t v = 0U;
    uint8_t digits = 0U;

    if ((text == NULL) || (out == NULL))
    {
        return false;
    }
    while ((*text >= '0') && (*text <= '9'))
    {
        v = (v * 10U) + (uint32_t) (*text - '0');
        digits++;
        text++;
        if (digits > MAX_U32_DIGITS)
        {
            return false;
        }
    }
    if ((*text != '\0') || (digits == 0U))
    {
        return false;
    }
    *out = v;
    return true;
}

size_t fmt_cols(const char *text)
{
    size_t n = 0U;

    while (*text != '\0')
    {
        if (((uint8_t) *text & 0xC0U) != 0x80U)
        {
            n++;
        }
        text++;
    }
    return n;
}

size_t fmt_pad(char *dst, size_t size, const char *text, size_t width)
{
    size_t len = 0U;
    const size_t cols = fmt_cols(text);

    while ((text[len] != '\0') && ((len + 1U) < size))
    {
        dst[len] = text[len];
        len++;
    }
    for (size_t i = cols; (i < width) && ((len + 1U) < size); i++)
    {
        dst[len] = ' ';
        len++;
    }
    dst[len] = '\0';
    return len;
}
