# Interview Test — ConfigStore (20–25 minutes)

## Brief (read this first — 3 minutes)

The IoT Environmental Monitor Gateway persists its configuration across reboots
as a single opaque byte blob stored in QSPI NOR flash. To survive power-loss
mid-write, the firmware uses an A/B slot scheme: each write targets the *other*
slot, so the previously written slot remains valid if the write is interrupted.
A CRC32/ISO-HDLC field (last 4 bytes of the 32 KB slot) acts as the commit
point — a slot is only accepted if its CRC is valid.

Your task is to implement `config_store_select_active_slot`. Given the header
from each slot, the function must decide which slot holds the current live
configuration. Return its index (0 = slot A, 1 = slot B) in `*active_out`,
and the index of the slot to overwrite on the next save in `*target_out`. If
neither slot is valid, set `*active_out = 0` (active = A, target = B per spec)
and return `CONFIG_STORE_SELECT_NO_VALID_SLOT`. Constraints: no dynamic
allocation, no stdlib beyond `<stdint.h>` and `<stdbool.h>`, BARR-C style.

---

## Files given to the candidate

### `config_store_select_exercise.h`

```c
#ifndef CONFIG_STORE_SELECT_EXERCISE_H
#define CONFIG_STORE_SELECT_EXERCISE_H

#include <stdint.h>
#include <stdbool.h>

#define CONFIG_STORE_MAGIC  0xC0FFEE00UL

/** Result codes returned by config_store_select_active_slot(). */
typedef enum {
    CONFIG_STORE_SELECT_OK           = 0,
    CONFIG_STORE_SELECT_NO_VALID_SLOT = 1,
} config_store_select_err_t;

/**
 * @brief Per-slot header and validity flag provided to the selector.
 *
 * The caller has already verified the CRC of each slot; it sets
 * valid = true only when magic matches AND CRC32 passes.
 */
typedef struct {
    bool     valid;       /**< true if magic + CRC32 check passed. */
    uint32_t seq_number;  /**< Monotonically increasing write counter. */
} cs_slot_descriptor_t;

/**
 * @brief Select the active slot and the target slot for the next write.
 *
 * Rules (from flash-partition-layout.md §6.1):
 *   - active = slot with the higher valid seq_number.
 *   - target = the other slot.
 *   - If only one slot is valid, that slot is active.
 *   - If neither slot is valid, active = 0 (slot A), target = 1 (slot B).
 *     Return CONFIG_STORE_SELECT_NO_VALID_SLOT.
 *
 * @param[in]  slots       Array of exactly two cs_slot_descriptor_t structs
 *                         (index 0 = slot A, index 1 = slot B).
 * @param[out] active_out  Index (0 or 1) of the slot holding live config.
 * @param[out] target_out  Index (0 or 1) of the slot to erase and overwrite.
 * @return CONFIG_STORE_SELECT_OK or CONFIG_STORE_SELECT_NO_VALID_SLOT.
 */
config_store_select_err_t config_store_select_active_slot(
    const cs_slot_descriptor_t *slots,
    uint8_t                    *active_out,
    uint8_t                    *target_out);

#endif /* CONFIG_STORE_SELECT_EXERCISE_H */
```

### `config_store_select_exercise.c` (partial)

```c
#include "config_store_select_exercise.h"

/* Implement the body of config_store_select_active_slot below.
 * Do not use dynamic allocation or any header beyond those already included. */

config_store_select_err_t config_store_select_active_slot(
    const cs_slot_descriptor_t *slots,
    uint8_t                    *active_out,
    uint8_t                    *target_out)
{
    /* TODO: implement per the spec in the header Doxygen comment. */
    (void)slots;
    (void)active_out;
    (void)target_out;
    return CONFIG_STORE_SELECT_NO_VALID_SLOT;
}
```

---

## Follow-up questions

**Q1:** Why does the A/B scheme write the CRC *last*, after the header and data?

*Model answer:* Writing the CRC last makes it the commit point. If power is lost
after the header and data are written but before the CRC, the slot has an
incremented seq_number but a missing or wrong CRC. The load logic therefore
never selects it over the existing valid lower-numbered slot. This guarantees
that a partially written slot never corrupts the live configuration.

**Q2:** The `seq_number` field is `uint32_t`. What happens after 4 294 967 295
write cycles?

*Model answer:* The counter wraps to zero. At one config save per day the wrap
takes over 11 000 years, so no handling is required. A production-quality
implementation might add a comment to document this bound. If wrap-around were
a concern, a 64-bit counter or an explicit reset-on-wrap with a "both slots
contain seq=0, treat slot B as older" convention would be needed.

**Q3:** Two tasks call `config_store_save` simultaneously. What prevents data
corruption in this module?

*Model answer:* `config_store_save` acquires an internal priority-inheritance
mutex (`xSemaphoreCreateMutexStatic`) at the start and releases it at the end.
The second caller blocks until the first completes. Flash erase + write takes
tens of milliseconds, which is an acceptable hold time for the low-priority
maintenance task that calls save. The mutex is created in `config_store_init`
before the scheduler starts, so it is always valid when save is called.

---

## Model solution

```c
config_store_select_err_t config_store_select_active_slot(
    const cs_slot_descriptor_t *slots,
    uint8_t                    *active_out,
    uint8_t                    *target_out)
{
    if ((slots == NULL) || (active_out == NULL) || (target_out == NULL))
    {
        return CONFIG_STORE_SELECT_NO_VALID_SLOT;  /* programming error */
    }

    if (slots[0U].valid && slots[1U].valid)
    {
        /* Both valid: higher seq_number is active. */
        if (slots[1U].seq_number > slots[0U].seq_number)
        {
            *active_out = 1U;
            *target_out = 0U;
        }
        else
        {
            *active_out = 0U;
            *target_out = 1U;
        }
        return CONFIG_STORE_SELECT_OK;
    }

    if (slots[0U].valid)
    {
        *active_out = 0U;
        *target_out = 1U;
        return CONFIG_STORE_SELECT_OK;
    }

    if (slots[1U].valid)
    {
        *active_out = 1U;
        *target_out = 0U;
        return CONFIG_STORE_SELECT_OK;
    }

    /* Neither valid: default per spec (active = A, target = B). */
    *active_out = 0U;
    *target_out = 1U;
    return CONFIG_STORE_SELECT_NO_VALID_SLOT;
}
```

---

## Marking guide

**Must have (pass/fail):**
- Correctly handles both-valid case by comparing `seq_number` (higher wins).
- Correctly handles one-valid case (the single valid slot is active, other is target).
- Correctly handles neither-valid case: sets active=0, target=1, returns
  `NO_VALID_SLOT` (not `OK`).
- No dynamic allocation; no `printf`; BARR-C brace style on all branches.
- `active` and `target` are always set before returning (no uninitialized output).

**Nice to have (differentiates mid from senior):**
- NULL-pointer guard on inputs (with a comment explaining it is a programming error).
- Brief comment explaining WHY higher seq_number wins (monotonicity invariant).
- Explicit `uint8_t` casts on the literal `0U` / `1U` assignments to avoid
  sign-conversion warnings on strict compilers.

**Red flags (automatic fail or strong negative signal):**
- Uses `if (slots[0].seq_number > slots[1].seq_number)` without first checking
  `valid` — a slot with `valid=false` and a large residual seq_number (e.g.
  0xFFFFFFFF from erased flash) would be incorrectly selected.
- Returns `OK` when neither slot is valid.
- Missing the neither-valid case entirely (function returns undefined output).
- Uses `malloc` or a variable-length array.
