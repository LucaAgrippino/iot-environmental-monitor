# Interview Test — SensorService Pipeline (25 minutes)

## Brief (3 minutes)

The IoT Environmental Monitor Field Device acquires temperature, humidity, and
pressure from simulated sensor drivers. A SensorService module sits between the
raw driver readings and the rest of the system. It owns a five-step processing
pipeline executed on every acquisition cycle: (1) call the driver, (2) stamp a
timestamp, (3) range-validate the raw value, (4) clamp to the valid range, and
(5) apply a low-pass IIR filter. The filtered value and a validity flag are
stored in a snapshot that other components read.

Implement the core processing step for a single sensor reading. You are given
the function signature, the types, and a partial implementation stub. You must
fill in the pipeline body. Constraints: no dynamic allocation, BARR-C coding
standard (braces on every control body, fixed-width types, `const` correctness),
no `printf`, no recursion.

---

## Files given to the candidate

### `sensor_pipeline_exercise.h`

```c
#ifndef SENSOR_PIPELINE_EXERCISE_H
#define SENSOR_PIPELINE_EXERCISE_H

#include <stdint.h>
#include <stdbool.h>

/** Engineering-unit reading produced by the pipeline for one sensor. */
typedef struct
{
    float    value;   /**< Filtered value in engineering units. */
    bool     valid;   /**< true if driver read succeeded AND value is in range. */
    uint32_t epoch;   /**< Acquisition timestamp (Unix seconds or uptime). */
} reading_t;

/**
 * @brief Process one raw sensor value through the five-step pipeline.
 *
 * Steps:
 *   1. If driver_ok is false → mark reading invalid; return immediately
 *      (steps 2–5 skipped).
 *   2. Stamp reading->epoch = timestamp.
 *   3. Range-validate: if raw_value < range_min || raw_value > range_max
 *      → reading->valid = false.
 *   4. Clamp: raw_value = clamp(raw_value, range_min, range_max).
 *   5. IIR low-pass filter:
 *        filtered = alpha * clamped + (1 - alpha) * (*prev_filtered)
 *        *prev_filtered = filtered
 *        reading->value = filtered
 *
 * @param reading       Output reading to populate.  Must not be NULL.
 * @param raw_value     Value from the driver (engineering units).
 * @param driver_ok     true if the driver call succeeded.
 * @param range_min     Lower bound of valid range.
 * @param range_max     Upper bound of valid range.
 * @param alpha         IIR coefficient in (0, 1).
 * @param prev_filtered Persistent filter state; updated in place. Must not be NULL.
 * @param timestamp     Epoch seconds to stamp on the reading.
 */
void process_sensor(reading_t *reading,
                    float      raw_value,
                    bool       driver_ok,
                    float      range_min,
                    float      range_max,
                    float      alpha,
                    float     *prev_filtered,
                    uint32_t   timestamp);

#endif /* SENSOR_PIPELINE_EXERCISE_H */
```

### `sensor_pipeline_exercise.c` (partial)

```c
#include "sensor_pipeline_exercise.h"

void process_sensor(reading_t *reading,
                    float      raw_value,
                    bool       driver_ok,
                    float      range_min,
                    float      range_max,
                    float      alpha,
                    float     *prev_filtered,
                    uint32_t   timestamp)
{
    /* TODO: implement the five-step pipeline described in the header. */
}
```

---

## Follow-up questions

**Q1:** Why is the IIR filter applied to the *clamped* value instead of the
raw value? What would go wrong if you used `raw_value` directly in step 5?

*Model answer:* Applying the filter to the unclamped raw value would allow a
transient out-of-range spike (e.g., a sensor glitch reading 200°C when the
limit is 85°C) to "poison" the filter state — `prev_filtered` would jump
drastically and take many cycles to recover, producing a prolonged period of
wrong filtered output. Clamping first limits the filter state to a physically
plausible range; an out-of-range spike at most drives the filter toward the
boundary value, which converges back quickly once normal readings resume.
The validity flag (`valid = false`) separately signals to consumers that the
reading was anomalous, while the filter continues to run on the clamped input
for state continuity.

**Q2:** This function marks `reading->valid = false` on both driver failure
(step 1) and range violation (step 3). Are these two "invalid" cases treated
differently — and why?

*Model answer:* Yes. On driver failure (step 1) the function returns
immediately, skipping steps 2–5. The epoch is not updated, `prev_filtered` is
not advanced, and `reading->value` retains whatever it held from the previous
cycle (the last good filtered value). On range violation (step 3) the pipeline
continues: the epoch is stamped, the value is clamped and passed through the
filter, and `reading->value` gets the new filtered output. The reasoning is
that a driver failure means no new data arrived at all — so there is nothing to
timestamp or filter. A range violation means data did arrive (the sensor is
alive) but the reported value is outside the credible window — the filter
should still advance so it tracks the boundary rather than stagnating.

**Q3:** The IIR coefficient `alpha` is described as being in the open interval
(0, 1) exclusive. What goes wrong at the boundary values `alpha = 0` and
`alpha = 1`, and how would you validate this parameter at module init time?

*Model answer:* With `alpha = 0`, the term `alpha * clamped` is always zero,
so `filtered = prev_filtered` every cycle — the output never changes regardless
of input (the filter ignores new data entirely). With `alpha = 1`, `filtered =
clamped` every cycle — the filter is a pass-through and provides no smoothing
at all, defeating its purpose. Validation at init: `if (alpha <= 0.0f ||
alpha >= 1.0f) { /* reject, use default */ }`. Note the use of `>=` (not `>`)
for the upper bound — accepting `alpha = 1.0` is an easy off-by-one mistake
that a careless boundary check would make. Empirically, a value of 0.1 gives
a time constant of approximately 1/alpha sample periods, so at 10 Hz that is
~1 second, which is a reasonable default for environmental sensors.

---

## Model solution

```c
#include "sensor_pipeline_exercise.h"

void process_sensor(reading_t *reading,
                    float      raw_value,
                    bool       driver_ok,
                    float      range_min,
                    float      range_max,
                    float      alpha,
                    float     *prev_filtered,
                    uint32_t   timestamp)
{
    /* Step 1: driver failure — mark invalid and return (steps 2–5 skipped). */
    if (!driver_ok)
    {
        reading->valid = false;
        return;
    }

    /* Step 2: timestamp. */
    reading->epoch = timestamp;

    /* Step 3: range validate. */
    if (raw_value < range_min || raw_value > range_max)
    {
        reading->valid = false;
    }
    else
    {
        reading->valid = true;
    }

    /* Step 4: clamp. */
    float clamped = raw_value;
    if (clamped < range_min) { clamped = range_min; }
    if (clamped > range_max) { clamped = range_max; }

    /* Step 5: IIR low-pass filter applied to the clamped value. */
    float filtered = alpha * clamped + (1.0f - alpha) * (*prev_filtered);
    *prev_filtered  = filtered;
    reading->value  = filtered;
}
```

---

## Marking guide

**Must have (pass/fail):**
- Step 1 returns early on `!driver_ok` without modifying epoch or filter state.
- Step 3 sets `valid = false` on out-of-range but does NOT return early.
- Step 4 clamps before step 5 (filter receives clamped, not raw value).
- Filter formula is `alpha * clamped + (1 - alpha) * *prev_filtered`; both
  `*prev_filtered` update and `reading->value` assignment present.
- Braces on every `if`/`else` body.

**Nice to have (differentiates mid from senior):**
- Stamps timestamp on range-violating readings (i.e., step 2 is before step 3).
- Sets `reading->valid = true` explicitly in the else branch rather than
  relying on a prior memset (defensive init).
- Uses `(1.0f - alpha)` rather than a precomputed variable — compiler handles
  constant folding; no need to allocate a local.

**Red flags (automatic fail or strong negative signal):**
- Returns early after range validation (skipping the filter update).
- Filters `raw_value` instead of `clamped`.
- Does not update `*prev_filtered` (filter state not persisted across calls).
- Uses dynamic allocation (`malloc`/`calloc`).
- Missing braces on single-line `if` bodies.
- Uses `printf` for any output.
