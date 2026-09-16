# Technical Exercise — WifiTask (Gateway)

## Brief (3 minutes)

A single FreeRTOS task owns a WiFi module and serialises access to it. Callers
post a request to its queue and block until it notifies them back. One caller
is a TLS stack, which cannot afford to block: it wants "give me whatever bytes
have arrived, or tell me nothing has" and will retry. So the task also keeps a
per-socket background receive slot that it fills opportunistically between
requests.

Implement the pickup half of that slot. TLS is a **byte stream**: mbedTLS
reads a 5-byte record header, then the body, as separate calls, and each call
must resume where the previous one stopped. The buffer must not be recycled
for a new background attempt while bytes remain unconsumed. Getting either
wrong produces a handshake failure that no host test will reproduce.

## Given files

### `wifitask_exercise.h`

```c
#ifndef WIFITASK_EXERCISE_H
#define WIFITASK_EXERCISE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WIFITASK_SCRATCH_SIZE (1500U)

typedef enum
{
    RECV_SLOT_IDLE = 0,  /**< Nothing armed; may be armed. */
    RECV_SLOT_ARMED,     /**< Background poll requested. */
    RECV_SLOT_OK,        /**< Payload present, wholly or partly unconsumed. */
    RECV_SLOT_NONE,      /**< Poll completed with no data. */
    RECV_SLOT_ERROR      /**< Poll failed. */
} recv_slot_state_t;

typedef struct
{
    recv_slot_state_t state;
    uint8_t scratch[WIFITASK_SCRATCH_SIZE];
    size_t scratch_len; /**< Bytes the poll delivered. */
    size_t scratch_off; /**< Bytes already handed to the caller. */
} recv_slot_t;

typedef enum
{
    WIFITASK_RECV_READY = 0, /**< out_len bytes copied. */
    WIFITASK_RECV_PENDING,   /**< Armed, nothing yet — caller should retry. */
    WIFITASK_RECV_NONE,      /**< Poll finished with no data. */
    WIFITASK_RECV_ERROR      /**< Poll failed. */
} wifitask_recv_result_t;

/**
 * @brief Take up to buf_len bytes from an already-polled receive slot.
 *
 * Resumes from the slot's consumption cursor. The slot returns to IDLE — and
 * so becomes re-armable — only once every delivered byte has been handed over.
 *
 * @param[in,out] slot     The socket's background receive slot.
 * @param[out]    buf      Destination.
 * @param[in]     buf_len  Capacity of buf.
 * @param[out]    out_len  Bytes actually copied (0 unless READY).
 * @return READY / PENDING / NONE / ERROR.
 */
wifitask_recv_result_t wifitask_slot_pickup(recv_slot_t *slot, uint8_t *buf,
                                            size_t buf_len, size_t *out_len);

#endif /* WIFITASK_EXERCISE_H */
```

### `wifitask_exercise.c` (partial)

```c
#include "wifitask_exercise.h"
#include <string.h>

wifitask_recv_result_t wifitask_slot_pickup(recv_slot_t *slot, uint8_t *buf,
                                            size_t buf_len, size_t *out_len)
{
    /* TODO: NULL guards; out_len must be defined on every path. */

    /* TODO: map the non-OK states. Which of them may clear the slot? */

    /* TODO: copy from the cursor, advance it, and only return the slot to
     *       IDLE when it is fully drained. */

    return WIFITASK_RECV_ERROR;
}
```

## Questions

**Q1: Why must the slot stay out of `IDLE` until `scratch_off == scratch_len`,
rather than being freed on the first pickup?**

Answer: `IDLE` is what permits a re-arm, and a re-arm lets a background poll
overwrite `scratch`. If the slot is freed after the first pickup, a new attempt
can land on top of a buffer the caller has only partly read — mbedTLS takes the
5-byte header, the slot is recycled, and the body it asks for next is gone or
replaced by unrelated bytes. Draining fully before re-arming is what makes the
slot a stream rather than a mailbox. Dropped arms are harmless: the owner sends
one on every call, so the next attempt re-arms anyway.

**Q2: `NONE` and `ERROR` both mean "no data". Should they be treated
identically?**

Answer: No, for two reasons. They differ for the caller — `NONE` maps to
`MBEDTLS_ERR_SSL_WANT_READ` (retry, perfectly normal) while `ERROR` maps to
`MBEDTLS_ERR_SSL_TIMEOUT` (abort the handshake) — so collapsing them turns an
idle poll into a fatal error. They also differ in lifetime: an `ERROR` that is
not cleared on socket close survives into the *next* connection, which is
exactly the real defect WIFITASK-T26 pins. Both should return the slot to
`IDLE` here so a fresh arm can happen, but they must not return the same code.

**Q3: A test arms the slot, runs one step, picks up once, and asserts the
bytes match. What class of bug does that test structurally fail to catch?**

Answer: Anything that only manifests across *multiple* pickups or *multiple*
arm cycles. With a single read, a missing consumption cursor is invisible —
offset 0 and offset `scratch_off` are the same thing on the first call.
Likewise a slot wrongly freed early is harmless when nothing re-arms
afterwards. This is precisely how the real cursor bug reached hardware with a
fully green suite; the regression tests added afterwards deliberately assert
*consecutive* pickups and a refused re-arm.

## Model solution

```c
#include "wifitask_exercise.h"
#include <string.h>

wifitask_recv_result_t wifitask_slot_pickup(recv_slot_t *slot, uint8_t *buf,
                                            size_t buf_len, size_t *out_len)
{
    size_t remaining;
    size_t copy;

    if ((slot == NULL) || (buf == NULL) || (out_len == NULL))
    {
        return WIFITASK_RECV_ERROR;
    }

    *out_len = 0U;

    switch (slot->state)
    {
        case RECV_SLOT_IDLE:
        case RECV_SLOT_ARMED:
            return WIFITASK_RECV_PENDING;

        case RECV_SLOT_NONE:
            slot->state = RECV_SLOT_IDLE; /* Re-armable. */
            return WIFITASK_RECV_NONE;

        case RECV_SLOT_ERROR:
            slot->state = RECV_SLOT_IDLE; /* Cleared, or it poisons the next socket. */
            return WIFITASK_RECV_ERROR;

        case RECV_SLOT_OK:
        default:
            break;
    }

    if (buf_len == 0U)
    {
        return WIFITASK_RECV_PENDING; /* Nothing asked for; slot untouched. */
    }

    remaining = slot->scratch_len - slot->scratch_off;
    copy = (remaining < buf_len) ? remaining : buf_len;

    (void) memcpy(buf, &slot->scratch[slot->scratch_off], copy);
    slot->scratch_off += copy;
    *out_len = copy;

    if (slot->scratch_off >= slot->scratch_len)
    {
        slot->scratch_len = 0U;
        slot->scratch_off = 0U;
        slot->state = RECV_SLOT_IDLE; /* Drained — re-arm permitted. */
    }

    return WIFITASK_RECV_READY;
}
```

## Marking guide

**Must have:**

- Copies from `scratch_off`, not from 0.
- Advances `scratch_off` by the number of bytes copied.
- Returns to `IDLE` **only** when fully drained.
- `*out_len` written on every return path, including the error paths.
- `NONE` and `ERROR` return distinct codes.

**Good to have:**

- Clears `ERROR` so it cannot leak into the next connection, and can explain
  why that matters (stale error after a socket reopen).
- Resets `scratch_len` / `scratch_off` together when draining.
- Handles `buf_len == 0` without corrupting the cursor.
- Notes that the caller-visible contract is "retry until READY", so PENDING
  must be cheap and side-effect-free.
- Observes that pickup and the background publish must not interleave — in the
  real driver this runs in a critical section.

**Red flags:**

- `memcpy(buf, slot->scratch, ...)` — the original defect.
- Setting `state = IDLE` unconditionally at the end.
- Treating `NONE` as `ERROR`, which aborts a handshake that was merely idle.
- Returning `READY` with `*out_len == 0`, which spins the caller forever.
- Reading `scratch_len - scratch_off` without having established `OK` state
  first — the subtraction underflows on a slot that was never filled.
