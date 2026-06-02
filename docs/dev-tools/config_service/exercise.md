# Interview Test — ConfigService (25 minutes)

## Brief (read this first — 3 minutes)

ConfigService is the application-layer module that owns the live in-memory
configuration for an IoT field device.  It exposes a read interface
(`IConfigProvider`) consumed by SensorService and AlarmService, and a write
interface (`IConfigManager`) consumed by the CLI and Modbus register map.  All
writes go through a validate → apply → persist pipeline backed by NOR flash via
an `IConfigStore` vtable.

You must implement `config_service_set_param()`: a thread-safe function that
validates a new value for one named parameter, applies it to the in-memory struct
if valid, persists it via the ConfigStore, and fires registered change callbacks
after a successful write.  Constraints: no dynamic memory, BARR-C style,
FreeRTOS mutex for thread safety, no recursive mutex — release the mutex before
calling any callback.

---

## Files given to the candidate

### `config_service_exercise.h`

```c
#ifndef CONFIG_SERVICE_EXERCISE_H
#define CONFIG_SERVICE_EXERCISE_H

#include <stdint.h>
#include <stdbool.h>

/* ── Error codes ── */
typedef enum {
    CS_ERR_OK       = 0,
    CS_ERR_NOT_INIT = 1,
    CS_ERR_NULL_ARG = 2,
    CS_ERR_INVALID  = 3,   /* validation failed */
    CS_ERR_PERSIST  = 4,   /* ConfigStore write failed */
} cs_err_t;

/* ── Parameter identifiers ── */
typedef enum {
    CS_PARAM_POLL_INTERVAL = 0,  /* uint32_t, [100, 60000] */
    CS_PARAM_FILTER_ALPHA  = 1,  /* float,   (0.0, 1.0) exclusive */
    CS_PARAM_MODBUS_ADDR   = 2,  /* uint8_t, [1, 247] */
    CS_PARAM_COUNT,
} cs_param_id_t;

/* ── Live config struct ── */
typedef struct {
    uint32_t poll_interval_ms;
    float    filter_alpha;
    uint8_t  modbus_addr;
} cs_params_t;

/* ── ConfigStore stub (IConfigStore) ── */
typedef struct {
    cs_err_t (*save)(const void *data, uint32_t len);
} ics_store_t;

/* ── Change callback ── */
typedef void (*cs_change_cb_t)(cs_param_id_t param_id);

/* ── Module state (given; do not modify) ── */
typedef struct {
    bool          initialised;
    cs_params_t   params;
    cs_params_t   snapshot;
    bool          snapshot_valid;
    const ics_store_t *store;
    void         *mutex;      /* treat as opaque FreeRTOS SemaphoreHandle_t */
    cs_change_cb_t callbacks[4];
    uint8_t        callback_count;
} cs_state_t;

extern cs_state_t g_cs;

/* ── Stubs already implemented for you ── */
cs_err_t cs_validate(cs_param_id_t id, const void *value, const cs_params_t *cur);
void     cs_apply(cs_params_t *p, cs_param_id_t id, const void *value);
void     mutex_take(void *m);   /* blocks until mutex acquired */
void     mutex_give(void *m);   /* releases mutex */

/**
 * @brief Validate, apply, and persist a single parameter change.
 *
 * On validation failure: return CS_ERR_INVALID; state unchanged.
 * On success: apply to g_cs.params; call g_cs.store->save(); if save fails,
 * return CS_ERR_PERSIST (in-memory already updated).
 * Fire all registered change callbacks AFTER releasing the mutex.
 * Thread-safe: hold mutex for the entire validate + apply + save sequence.
 *
 * @param id    Parameter to change.
 * @param value Pointer to new value (type matches param declaration above).
 * @return cs_err_t
 */
cs_err_t cs_set_param(cs_param_id_t id, const void *value);

#endif /* CONFIG_SERVICE_EXERCISE_H */
```

### `config_service_exercise.c` (partial — candidate fills in `cs_set_param`)

```c
#include "config_service_exercise.h"
#include <string.h>

cs_state_t g_cs;

/* ── Validation (implemented) ── */
cs_err_t cs_validate(cs_param_id_t id, const void *value, const cs_params_t *cur)
{
    switch (id) {
    case CS_PARAM_POLL_INTERVAL: {
        uint32_t v = *(const uint32_t *)value;
        return (v >= 100U && v <= 60000U) ? CS_ERR_OK : CS_ERR_INVALID;
    }
    case CS_PARAM_FILTER_ALPHA: {
        float v = *(const float *)value;
        return (v > 0.0f && v < 1.0f) ? CS_ERR_OK : CS_ERR_INVALID;
    }
    case CS_PARAM_MODBUS_ADDR: {
        uint8_t v = *(const uint8_t *)value;
        return (v >= 1U && v <= 247U) ? CS_ERR_OK : CS_ERR_INVALID;
    }
    default:
        return CS_ERR_INVALID;
    }
}

/* ── Apply (implemented) ── */
void cs_apply(cs_params_t *p, cs_param_id_t id, const void *value)
{
    switch (id) {
    case CS_PARAM_POLL_INTERVAL:
        p->poll_interval_ms = *(const uint32_t *)value; break;
    case CS_PARAM_FILTER_ALPHA:
        p->filter_alpha = *(const float *)value;        break;
    case CS_PARAM_MODBUS_ADDR:
        p->modbus_addr  = *(const uint8_t *)value;      break;
    default:
        break;
    }
}

/* ── Mutex stubs (implemented) ── */
void mutex_take(void *m) { (void)m; /* platform-specific */ }
void mutex_give(void *m) { (void)m; /* platform-specific */ }

/* ── TODO: implement cs_set_param below ── */
cs_err_t cs_set_param(cs_param_id_t id, const void *value)
{
    /* TODO */
    (void)id; (void)value;
    return CS_ERR_NOT_INIT;
}
```

---

## Follow-up questions

**Q1:** Why must the change callbacks be fired *after* releasing the mutex,
rather than while still holding it?

*Model answer:* The callbacks may call `cs_get_params()` or other module
functions that also acquire the mutex.  Calling a callback while holding the
mutex would create a self-deadlock if the callback re-enters this module (the
mutex is non-recursive on FreeRTOS).  Releasing the mutex first eliminates this
hazard while accepting a brief window where another task could change the
parameter — acceptable because callbacks are best-effort notifications, not
transactional guarantees.

**Q2:** The specification says "on ConfigStore failure: return CS_ERR_PERSIST;
in-memory config already updated." Why is the in-memory value *not* rolled back
on a persist failure?

*Model answer:* The in-memory value is always updated first so that the live
system immediately uses the new value — system continuity is more important than
the transactional guarantee of flash persistence.  ConfigStore uses an A/B slot
scheme, so the previous valid slot is still loadable after a power cycle.  On
next reboot, the system applies the last successfully persisted value, which may
be one version behind; the operator would re-apply the intended change.  Rolling
back the in-memory value would keep the system in the old state for no benefit
at runtime and would complicate error handling.

**Q3:** If two tasks call `cs_set_param()` concurrently for different parameters,
how does the mutex prevent a torn write?

*Model answer:* `mutex_take()` blocks any second caller until the first has
completed validate + apply + save and called `mutex_give()`.  Without the mutex,
Task A's `cs_apply()` write to `g_cs.params.poll_interval_ms` and Task B's
write to `g_cs.params.filter_alpha` could interleave: Task A's `save()` would
then capture a half-applied state with Task A's interval and Task B's alpha —
or vice versa.  The mutex serialises the full sequence so that exactly one
version of params is committed to flash.

---

## Model solution

```c
cs_err_t cs_set_param(cs_param_id_t id, const void *value)
{
    if (!g_cs.initialised) { return CS_ERR_NOT_INIT; }
    if (value == NULL)     { return CS_ERR_NULL_ARG;  }

    mutex_take(g_cs.mutex);

    cs_err_t v_err = cs_validate(id, value, &g_cs.params);
    if (v_err != CS_ERR_OK)
    {
        mutex_give(g_cs.mutex);   /* release before returning */
        return v_err;
    }

    cs_apply(&g_cs.params, id, value);

    cs_err_t p_err = g_cs.store->save(&g_cs.params, sizeof(g_cs.params));

    mutex_give(g_cs.mutex);       /* release before callbacks */

    if (p_err != CS_ERR_OK)
    {
        return CS_ERR_PERSIST;
    }

    for (uint8_t i = 0U; i < g_cs.callback_count; i++)
    {
        if (g_cs.callbacks[i] != NULL)
        {
            g_cs.callbacks[i](id);
        }
    }

    return CS_ERR_OK;
}
```

---

## Marking guide

**Must have (pass/fail):**
- Guard `!g_cs.initialised` and `value == NULL` before acquiring the mutex.
- `mutex_take` before any read/write of `g_cs.params`.
- `mutex_give` on the validation-failure path (before returning ERR_INVALID).
- `mutex_give` before firing callbacks (not after).
- Callbacks fired only on `CS_ERR_OK`, not on `CS_ERR_PERSIST`.
- Correct return value on each path (NOT_INIT, NULL_ARG, INVALID, PERSIST, OK).

**Nice to have (differentiates mid from senior):**
- `(void)` cast on `save()` return when used as expression in condition —
  or structured as a variable with explicit check.
- Comments explaining *why* mutex is released before callbacks (deadlock risk).
- Recognising that `g_cs.params` is already updated when `CS_ERR_PERSIST` is
  returned (matches spec: in-memory updated, persist failed).

**Red flags (automatic fail or strong negative signal):**
- Calling callbacks while holding the mutex.
- Rolling back `g_cs.params` on persist failure.
- Missing mutex release on the validation-failure path — the classic bug.
- Ignoring the initialised guard and dereferencing `g_cs.store` without it.
- Using `malloc` or other dynamic allocation anywhere.
