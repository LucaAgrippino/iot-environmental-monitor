# Technical Exercise — CloudPublisher (Gateway)

## Brief (3 minutes)

A mains-powered gateway publishes to a cloud broker. When the connection drops
it must reconnect, but it must not hammer a broker that is down: the delay
between attempts doubles from a floor to a cap, and resets once a connection
succeeds. The countdown is driven by a **1 Hz tick** — the same tick that polls
statistics — so every delay is expressed in whole seconds, and a delay shorter
than two ticks cannot be represented at all.

Implement the per-tick reconnect decision. There is no give-up: an always-on
gateway retries indefinitely, because an operator is not present to restart it.
The subtlety is that a connection *attempt in progress* must keep being ticked
every cycle, while a *failed* attempt must wait out its backoff.

## Given files

### `cp_reconnect_exercise.h`

```c
#ifndef CP_RECONNECT_EXERCISE_H
#define CP_RECONNECT_EXERCISE_H

#include <stdbool.h>
#include <stdint.h>

/** Floor. Must be >= 2 ticks, or a 1 Hz countdown cannot express it. */
#define CP_RECONNECT_BACKOFF_MIN_S (2U)
/** Ceiling. */
#define CP_RECONNECT_BACKOFF_MAX_S (60U)

typedef enum
{
    CP_CONN_IDLE = 0,    /**< Not connected, not attempting. */
    CP_CONN_IN_PROGRESS, /**< A ticked connect is mid-handshake. */
    CP_CONN_ESTABLISHED  /**< Connected. */
} cp_conn_state_t;

typedef enum
{
    CP_STEP_IN_PROGRESS = 0, /**< Handshake continuing; tick again next cycle. */
    CP_STEP_DONE,            /**< Connected. */
    CP_STEP_FAILED           /**< Attempt failed; back off. */
} cp_step_result_t;

typedef struct
{
    cp_conn_state_t state;
    uint32_t backoff_s;    /**< Current delay. 0 before the first failure. */
    uint32_t countdown_s;  /**< Ticks remaining before the next attempt. */
} cp_reconnect_t;

/** Provided: drive one tick of the connect state machine. */
cp_step_result_t mqtt_client_connect_step(void);

/**
 * @brief One 1 Hz tick of the reconnect policy.
 *
 * Called unconditionally every tick, connected or not.
 *
 * @param[in,out] rc  Reconnect state.
 * @return true if a connect step was driven this tick.
 */
bool cp_maybe_reconnect(cp_reconnect_t *rc);

#endif /* CP_RECONNECT_EXERCISE_H */
```

### `cp_reconnect_exercise.c` (partial)

```c
#include "cp_reconnect_exercise.h"

bool cp_maybe_reconnect(cp_reconnect_t *rc)
{
    /* TODO: guard; established -> nothing to do (and reset the backoff). */

    /* TODO: an attempt already in progress ticks EVERY cycle — no backoff. */

    /* TODO: idle -> count down; on zero, attempt. On failure, double and
     *       clamp; on success, reset. */

    return false;
}
```

## Questions

**Q1: Why must the backoff floor be 2 s rather than 1 s?**

Answer: The countdown is decremented once per 1 Hz tick, so 1 s means "retry on
the very next tick" — arithmetically identical to no backoff. The quantisation
of the clock, not the arithmetic, sets the smallest delay that can be expressed;
anything at or below one tick period collapses to zero. This is the real defect
recorded as CP-D13: the backoff was being *calculated* correctly and *consumed*
instantly, so the gateway hammered a downed broker once a second.

**Q2: Why does an attempt already in progress bypass the backoff entirely?**

Answer: Because backoff governs *how often to start* an attempt, not how fast an
attempt runs. `mqtt_client_connect_step()` is a ticked state machine — a TLS
handshake needs many ticks to complete — so applying the backoff to a handshake
in flight would stall it mid-negotiation and guarantee it never finishes. The two
states answer different questions: `IDLE` asks "is it time to try again?",
`IN_PROGRESS` asks "keep going".

**Q3: There is no give-up. Why is unbounded retry safe here, and when would it
not be?**

Answer: Safe because this is a mains-powered always-on gateway with no operator
present, the per-tick blocking is bounded (the ticked connect caps how long one
step takes), and the backlog is a bounded drop-oldest buffer — so retrying
forever consumes no unbounded resource. It would *not* be safe on a
battery-powered node, where each radio attempt has an energy cost and indefinite
retry flattens the battery; nor where each attempt allocates something that is
only released on success. The decision is recorded as CP-D14 precisely because it
depends on those properties rather than being universally correct.

## Model solution

```c
#include "cp_reconnect_exercise.h"

bool cp_maybe_reconnect(cp_reconnect_t *rc)
{
    cp_step_result_t result;

    if (rc == NULL)
    {
        return false;
    }

    /* Connected: nothing to do, and the next outage starts from the floor. */
    if (rc->state == CP_CONN_ESTABLISHED)
    {
        rc->backoff_s = 0U;
        rc->countdown_s = 0U;
        return false;
    }

    /* A handshake in flight is ticked every cycle — backoff does not apply. */
    if (rc->state != CP_CONN_IN_PROGRESS)
    {
        if (rc->countdown_s > 0U)
        {
            rc->countdown_s--;
            return false;
        }
    }

    result = mqtt_client_connect_step();

    switch (result)
    {
        case CP_STEP_DONE:
            rc->state = CP_CONN_ESTABLISHED;
            rc->backoff_s = 0U;
            rc->countdown_s = 0U;
            break;

        case CP_STEP_IN_PROGRESS:
            rc->state = CP_CONN_IN_PROGRESS;
            break;

        case CP_STEP_FAILED:
        default:
            rc->state = CP_CONN_IDLE;
            if (rc->backoff_s == 0U)
            {
                rc->backoff_s = CP_RECONNECT_BACKOFF_MIN_S;
            }
            else
            {
                rc->backoff_s *= 2U;
                if (rc->backoff_s > CP_RECONNECT_BACKOFF_MAX_S)
                {
                    rc->backoff_s = CP_RECONNECT_BACKOFF_MAX_S;
                }
            }
            rc->countdown_s = rc->backoff_s;
            break;
    }

    return true;
}
```

## Marking guide

**Must have:**

- `IN_PROGRESS` drives a step **every** tick, with no countdown check.
- Backoff doubles on failure and is clamped to the maximum.
- Backoff and countdown reset on a successful connect.
- The countdown is decremented **only** in the idle path.
- No give-up branch — retry continues indefinitely.

**Good to have:**

- First failure seeds the floor rather than doubling from zero (doubling zero
  stays zero — a classic trap that yields no backoff at all).
- Clamps before assigning the countdown, so the countdown can never exceed the
  cap.
- Resets the backoff on connect *and* explains why: the next outage should start
  short, not inherit a 60 s delay from the last one.
- Notes that the 2 s floor is a property of the 1 Hz tick, so changing the tick
  rate means revisiting the constant.

**Red flags:**

- Applying the countdown to `IN_PROGRESS`, which stalls handshakes and prevents
  any connection completing.
- `backoff *= 2` with `backoff` starting at 0.
- Clamping after assigning the countdown, letting one overshoot through.
- Adding a maximum-attempts give-up — wrong for this device class, and it
  silently strands the gateway offline until someone power-cycles it.
- Resetting the backoff on `IN_PROGRESS` rather than on `DONE`, which makes a
  broker that accepts TCP but never completes TLS look like success.
