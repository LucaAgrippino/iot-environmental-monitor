# Technical Exercise — ExtiDriver

## Brief (3 minutes)

The STM32's `SYSCFG_EXTICRx` registers map each of the 16 EXTI lines to a
GPIO port, but the mapping is packed tightly: four registers, each
holding four 4-bit port-selector fields. Line `N`'s field lives in
register `N / 4`, at bit offset `(N % 4) * 4`.

You're given a stripped-down `exti_configure()` with the packing logic
removed. Implement it so it writes exactly the 4-bit field for the
given line, without disturbing the other three fields already packed
into the same 32-bit register. A second driver instance may have
already claimed a different line in the *same* register — your write
must not corrupt it.

## Given files

### exti_exercise.h

```c
#ifndef EXTI_EXERCISE_H
#define EXTI_EXERCISE_H

#include <stdint.h>

typedef enum
{
    EXTI_EX_OK = 0,
    EXTI_EX_INVALID_ARG = 1
} exti_ex_err_t;

/**
 * @brief Map an EXTI line to a GPIO port via SYSCFG_EXTICRx.
 *
 * @param[in] line  EXTI line number (0..15).
 * @param[in] port  Port selector value (0..7). Written into the 4-bit
 *                  field for this line without disturbing the other
 *                  three lines packed into the same register.
 * @return EXTI_EX_OK on success; EXTI_EX_INVALID_ARG if line > 15 or
 *         port > 7.
 */
exti_ex_err_t exti_configure(uint8_t line, uint8_t port);

#endif /* EXTI_EXERCISE_H */
```

### exti_exercise.c (partial)

```c
#include "exti_exercise.h"

#define EXTICR_REG_COUNT   (4u)
#define EXTICR_LINES_PER_REG (4u)
#define EXTICR_FIELD_WIDTH   (4u)
#define EXTICR_FIELD_MASK    (0xFu)
#define EXTI_LINE_MAX        (15u)
#define EXTI_PORT_MAX        (7u)

static uint32_t s_exticr[EXTICR_REG_COUNT];

exti_ex_err_t exti_configure(uint8_t line, uint8_t port)
{
    /* TODO:
     * 1. Validate line and port ranges.
     * 2. Compute which register (0..3) and which bit offset within it.
     * 3. Clear only that 4-bit field, then write the new port value
     *    into it, leaving the other three fields in that register
     *    untouched.
     */
    return EXTI_EX_OK;
}
```

## Questions

Q1: Why must the field be cleared with `&= ~(mask << bit_pos)` before
ORing in the new value, instead of just `|= (port << bit_pos)`?
Answer: OR alone can only set bits, never clear them. If the field
previously held a non-zero port value and the new port value has any
zero bit where the old one had a one, a bare OR leaves that stale bit
set, producing a corrupted, mixed-up port selector that belongs to
neither the old nor the new port. Clearing first guarantees the field
reflects exactly the new value.

Q2: Why is `s_exticr` an array of 4 registers rather than one 16-bit
value indexed by line?
Answer: The real hardware exposes four separate 32-bit registers
(`EXTICR[0..3]`), each covering 4 lines. Modelling it as one array
mirrors the register layout directly, which matters because two
different EXTI lines that happen to fall in the same register (e.g.
lines 0 and 1, both in `EXTICR[0]`) must never have one line's
configuration write corrupt the other's — exactly the bug this
exercise is designed to catch if the masking is wrong.

Q3: This function has no conflict detection — line 1 can be
reconfigured to a different port with no error. What real-world defect
does that allow, and what's the minimal fix?
Answer: Two drivers could each call `exti_configure()` for the same
line during system init, each assuming they own it exclusively; the
second call silently overwrites the first driver's port mapping, and
whichever driver initialised first now silently receives interrupts
for the wrong pin. The minimal fix is a bitmap (`uint16_t`, one bit per
line) checked and set inside `exti_configure()`, returning a distinct
"already configured" error code on a second call for the same line
before any register is touched.

## Model solution

```c
exti_ex_err_t exti_configure(uint8_t line, uint8_t port)
{
    if ((line > EXTI_LINE_MAX) || (port > EXTI_PORT_MAX))
    {
        return EXTI_EX_INVALID_ARG;
    }

    uint8_t reg_idx = line / EXTICR_LINES_PER_REG;
    uint8_t bit_pos = (line % EXTICR_LINES_PER_REG) * EXTICR_FIELD_WIDTH;

    s_exticr[reg_idx] &= ~(EXTICR_FIELD_MASK << bit_pos);
    s_exticr[reg_idx] |= ((uint32_t) port << bit_pos);

    return EXTI_EX_OK;
}
```

## Marking guide

Must have:
- Range validation on both `line` and `port` before touching any state.
- Correct `reg_idx` / `bit_pos` arithmetic (integer division and modulo, not bit-shifting line directly).
- Clear-then-set on the target field; candidate can explain why clearing first is required (Q1).

Good to have:
- Notices unchanged fields in the same register are a testable property and proposes asserting them in a unit test (e.g. configure line 0, then line 1, verify line 0's field is unchanged).
- Uses named constants instead of magic numbers `4`, `0xF`, `15`.

Red flags:
- Uses `|=` without a prior clear ("it happened to work in my test" — ask what happens on a *second* reconfigure of the same line to a different port).
- Indexes `s_exticr[line]` directly (conflates line number with register index — works by accident only for lines 0-3).
- No range validation, or validates only `line` and not `port`.
