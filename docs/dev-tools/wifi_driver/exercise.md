# Technical Exercise — WifiDriver

## Brief (3 minutes)

The ISM43362 WiFi module replies to every AT command over SPI with a
fixed-size buffer that is **not** null-terminated — it may contain fewer
valid bytes than the buffer's capacity, with unspecified leftover data
after them. Real firmware must classify the reply as success or failure
without ever reading past the bytes actually received.

Implement `wifi_classify_response()`: given a raw response buffer and the
number of valid bytes in it, return whether the module reported success
(`"\r\nOK\r\n"` appears in the valid region), failure (`"\r\nERROR\r\n"`
appears), or neither (truncated / no response yet).

Constraints: no `strstr`/`strlen` on `resp` (it is not a C string), only
the first `resp_len` bytes are meaningful, and the function must not read
or write outside `resp[0 .. resp_len - 1]`.

## Given files

### wifi_exercise.h

```c
#ifndef WIFI_EXERCISE_H
#define WIFI_EXERCISE_H

#include <stddef.h>

typedef enum
{
    WIFI_RESP_OK = 0,        /**< "\r\nOK\r\n" found in the valid region.    */
    WIFI_RESP_ERROR = 1,     /**< "\r\nERROR\r\n" found in the valid region. */
    WIFI_RESP_INCOMPLETE = 2 /**< Neither marker found.                     */
} wifi_resp_class_t;

/**
 * @brief Classify a raw, non-null-terminated AT response buffer.
 *
 * @param[in] resp      Response bytes. May contain embedded NULs; not a C string.
 * @param[in] resp_len  Number of valid bytes in resp (0..N).
 * @return WIFI_RESP_OK, WIFI_RESP_ERROR, or WIFI_RESP_INCOMPLETE.
 */
wifi_resp_class_t wifi_classify_response(const char *resp, size_t resp_len);

#endif /* WIFI_EXERCISE_H */
```

### wifi_exercise.c (partial)

```c
#include "wifi_exercise.h"

wifi_resp_class_t wifi_classify_response(const char *resp, size_t resp_len)
{
    /* TODO: scan the first resp_len bytes of resp for "\r\nERROR\r\n" and
     * "\r\nOK\r\n". Check ERROR first (a response could — in principle —
     * carry echoed text containing "OK" before a genuine error marker).
     * Do not call strlen/strstr on resp: it is not null-terminated. */
}
```

## Questions

**Q1:** Why is `strstr(resp, "\r\nOK\r\n")` unsafe here, even if `resp` happens to be backed by a larger, zero-initialised static buffer?

Answer: `strstr` scans until it finds a `'\0'`, not until `resp_len` bytes have been examined. If the buffer was zero-initialised once and then reused across multiple AT commands (as WifiDriver's `at_buf` is), a shorter response leaves the tail of a *previous, longer* response sitting after the new data — and if that leftover byte sequence happens not to contain an embedded NUL before a stray `"OK"` substring, `strstr` would report a match that has nothing to do with the current transaction.

**Q2:** The scan checks for `"\r\nERROR\r\n"` before `"\r\nOK\r\n"`. Does the order matter, and why?

Answer: For this module's real response grammar it doesn't change correctness (the two markers are mutually exclusive terminators the ISM43362 never emits both of in one reply), but checking the more specific/rarer failure marker first is a defensive habit: if a future firmware revision ever echoes the literal text "OK" inside an otherwise-failed response body, checking ERROR first avoids classifying it as success.

**Q3:** What is the time complexity of a naive substring scan over `resp_len` bytes for each of the two markers, and would you optimise it for this use case?

Answer: O(n·m) worst case (n = resp_len, m = marker length, both tiny and bounded — markers are 6–9 bytes, resp_len is capped at a few hundred). No, it would not be worth optimising: n and m are both small compile-time-bounded constants in this driver, this runs at most a few times per second, and a KMP/Boyer-Moore implementation would add code size and review burden for no measurable benefit on a Cortex-M4 at 80 MHz.

## Model solution

```c
#include "wifi_exercise.h"
#include <string.h>

static int contains(const char *haystack, size_t haystack_len, const char *needle)
{
    const size_t needle_len = strlen(needle);

    if ((needle_len == 0u) || (haystack_len < needle_len))
    {
        return 0;
    }

    for (size_t i = 0u; i <= (haystack_len - needle_len); i++)
    {
        if (memcmp(&haystack[i], needle, needle_len) == 0)
        {
            return 1;
        }
    }
    return 0;
}

wifi_resp_class_t wifi_classify_response(const char *resp, size_t resp_len)
{
    if (contains(resp, resp_len, "\r\nERROR\r\n"))
    {
        return WIFI_RESP_ERROR;
    }
    if (contains(resp, resp_len, "\r\nOK\r\n"))
    {
        return WIFI_RESP_OK;
    }
    return WIFI_RESP_INCOMPLETE;
}
```

## Marking guide

Must have:
- Bounds every comparison by `resp_len`, never by a NUL terminator.
- Uses `memcmp` (or manual byte comparison), not `strcmp`/`strstr`, on `resp`.
- Handles `resp_len` shorter than either marker without reading out of bounds.
- Checks ERROR and OK independently (doesn't assume one implies absence of the other).

Good to have:
- Checks ERROR before OK, with a stated reason.
- Notes the O(n·m) complexity and explicitly justifies not optimising it.
- Mentions the stale-buffer-reuse hazard motivating the whole exercise (see bug-log.md).

Red flags:
- Reaches for `strstr`/`strlen` on `resp` "because it's a string of text".
- Assumes `resp_len` always equals the buffer's full capacity.
- Off-by-one in the loop bound (`i < haystack_len - needle_len` instead of `<=`, or vice versa) — ask the candidate to trace it against `resp_len` exactly equal to the marker length.
