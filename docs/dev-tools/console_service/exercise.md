# Technical Exercise — ConsoleService

## Brief (3 minutes)

The ConsoleService provides an operator serial console for the IoT Field Device.
When an operator types a command and presses Enter, the module reads the buffered
line, tokenises it, looks up the first token in a static command table, validates
the argument count, and dispatches to a handler. Handlers that accept user-supplied
values (such as `config set` and `prov set`) also perform domain-specific input
validation and stage the result in a pending struct.

Your task is to implement `console_service_set_polling_interval()` — a stripped-down
function that captures the core of the `config set polling-interval-ms <value>` path:
parse a decimal string, validate the range, and stage the value. You are given the
staging struct, the validation constant, and the error-code enum; you must implement
the body.

## Given files

### console_service_exercise.h

```c
#ifndef CONSOLE_SERVICE_EXERCISE_H
#define CONSOLE_SERVICE_EXERCISE_H

#include <stdint.h>
#include <stdbool.h>

/* Limits from config_service companion: 100 ms .. 60 000 ms */
#define POLL_INTERVAL_MIN_MS (100U)
#define POLL_INTERVAL_MAX_MS (60000U)

typedef enum {
    CS_EX_OK = 0,
    CS_EX_ERR_INVALID_VALUE,  /* non-numeric or out-of-range input */
    CS_EX_ERR_NULL_ARG,       /* NULL pointer passed                */
} cs_ex_err_t;

typedef struct {
    bool     dirty;
    uint32_t polling_interval_ms;
} cs_ex_cfg_pending_t;

/**
 * @brief Parse @p value_str as a decimal integer, validate it is within
 *        [POLL_INTERVAL_MIN_MS, POLL_INTERVAL_MAX_MS], and if valid store
 *        it in @p pending and set @p pending->dirty = true.
 *
 * @param pending      Staging struct to update on success. Must not be NULL.
 * @param value_str    NUL-terminated string from the operator. Must not be NULL.
 * @return CS_EX_OK on success; CS_EX_ERR_NULL_ARG if either pointer is NULL;
 *         CS_EX_ERR_INVALID_VALUE if the string is not a valid decimal integer
 *         or if the parsed value is outside the allowed range.
 *         On CS_EX_ERR_INVALID_VALUE, @p pending must be left unchanged.
 */
cs_ex_err_t console_service_set_polling_interval(cs_ex_cfg_pending_t *pending,
                                                  const char          *value_str);

#endif /* CONSOLE_SERVICE_EXERCISE_H */
```

### console_service_exercise.c (partial)

```c
#include "console_service_exercise.h"
#include <stdlib.h>  /* strtol */
#include <stddef.h>  /* NULL   */

cs_ex_err_t console_service_set_polling_interval(cs_ex_cfg_pending_t *pending,
                                                  const char          *value_str)
{
    /* TODO: implement the body.
     *
     * Hints:
     *  1. Guard for NULL pointers first.
     *  2. Use strtol() with base 10. Detect non-numeric strings by checking
     *     that strtol() consumed the ENTIRE string (i.e. *end == '\0') and
     *     that end != value_str (i.e. at least one digit was parsed).
     *  3. The parsed value must lie in [POLL_INTERVAL_MIN_MS, POLL_INTERVAL_MAX_MS].
     *  4. Only write to *pending on success; on any error leave it unchanged.
     */
    (void) pending;
    (void) value_str;
    return CS_EX_ERR_INVALID_VALUE;
}
```

## Questions

**Q1:** The stub returns `CS_EX_ERR_INVALID_VALUE` unconditionally, yet callers
must distinguish "bad string" from "out-of-range integer". Both map to the same
error code here. Is that an acceptable design? When would you split them into
two separate codes?

**Answer:** It is acceptable here because the console response is human-readable —
the printed error message can be made context-specific regardless of the numeric
error code, and the caller (the command loop) takes the same action either way:
abort and print an error. You would split the codes when downstream code needs to
branch on the specific cause — for example, if a retry-with-correction flow needed
to know whether the value was syntactically wrong (re-prompt with a format hint)
versus merely out of range (re-prompt with the allowed range).

**Q2:** Why does the function signature take `const char *value_str` rather than
accepting a pre-parsed `long` directly?

**Answer:** The function sits at a system boundary — operator input. At system
boundaries the input is untrusted text; parsing and validation should happen at
the same place the input enters the system, keeping the callers free of parsing
concerns. Accepting a pre-parsed `long` would push the `strtol` call into the
command dispatcher, scattering parsing logic away from the domain validation it
belongs with.

**Q3:** What happens if the operator types `"100"` (exactly the minimum) versus
`"99"`? What happens with `" 100"` (leading space)?

**Answer:** `"100"` must be accepted: `strtol` returns 100, `end` points to the
null terminator, and the range check `100 >= POLL_INTERVAL_MIN_MS` passes.
`"99"` must be rejected by the range check even though `strtol` succeeds.
`" 100"` — `strtol` skips the leading space (by specification) and parses 100;
`end` points to the null terminator, `end != value_str`. The model solution
accepts this. In a stricter implementation you might reject leading whitespace
by checking `isspace((unsigned char)value_str[0])` before calling `strtol`,
since the tokeniser in `console_service.c` already strips whitespace between
tokens, making a leading space in the value field an impossible input from a
real operator.

## Model solution

```c
#include "console_service_exercise.h"
#include <stdlib.h>
#include <stddef.h>

cs_ex_err_t console_service_set_polling_interval(cs_ex_cfg_pending_t *pending,
                                                  const char          *value_str)
{
    if ((pending == NULL) || (value_str == NULL))
    {
        return CS_EX_ERR_NULL_ARG;
    }

    char *end = NULL;
    long  parsed = strtol(value_str, &end, 10);

    if ((end == value_str) || (*end != '\0'))
    {
        return CS_EX_ERR_INVALID_VALUE;
    }

    if ((parsed < (long) POLL_INTERVAL_MIN_MS) || (parsed > (long) POLL_INTERVAL_MAX_MS))
    {
        return CS_EX_ERR_INVALID_VALUE;
    }

    pending->polling_interval_ms = (uint32_t) parsed;
    pending->dirty = true;
    return CS_EX_OK;
}
```

## Marking guide

**Must have:**
- NULL-pointer guard returning `CS_EX_ERR_NULL_ARG` before any other logic.
- `strtol` with base 10; both end-pointer checks (`end == value_str` for no
  digits, `*end != '\0'` for trailing garbage).
- Range check against both `POLL_INTERVAL_MIN_MS` and `POLL_INTERVAL_MAX_MS`.
- `pending` struct left **unchanged** on any error return.
- `dirty = true` set **only** on success.

**Good to have:**
- Cast to `long` when comparing against named constants to avoid signed/unsigned
  mismatch warnings (important under `-Wsign-compare`).
- Comment explaining why `strtol` is used instead of `atoi` (no error detection
  in `atoi`).

**Red flags:**
- Using `atoi()` — returns 0 on failure, indistinguishable from a legitimate 0
  (which would be out-of-range here, but the principle matters).
- Checking only `*end != '\0'` without also checking `end != value_str` — an
  empty string `""` causes undefined behaviour: `strtol` sets `*end` to the
  null terminator of the empty string and returns 0, so `*end == '\0'` would
  pass, accepting `""` as `0 ms`.
- Writing to `pending` before the range check — violates the contract that
  `pending` is unchanged on error.
- Missing `dirty = true` — the staging struct would never trigger a commit.
