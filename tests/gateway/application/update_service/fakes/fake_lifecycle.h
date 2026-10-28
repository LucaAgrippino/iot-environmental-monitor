/**
 * @file fake_lifecycle.h
 * @brief Fake ILifecycle (UpdateService tests): settable state, recorded events.
 *
 * Including this header makes Ceedling auto-link fake_lifecycle.c.
 */

#ifndef FAKE_LIFECYCLE_H
#define FAKE_LIFECYCLE_H

#include <stdbool.h>
#include <stdint.h>

#include "lifecycle_controller/ilifecycle.h"

#define FL_LOG_MAX (32U)

extern const ilifecycle_t fake_lifecycle_ops;

/** @brief State OPERATIONAL, post_event accepts everything, log cleared. */
void fake_lifecycle_reset(void);

void fake_lifecycle_set_state(lifecycle_state_t state);

/** @brief The next n post_event() calls return false; later ones succeed. */
void fake_lifecycle_refuse_posts(uint32_t n);

/** @brief post_event() returns false for this event type until cleared with FL_ANY_NONE. */
void fake_lifecycle_refuse_type(lifecycle_event_type_t type);
void fake_lifecycle_refuse_none(void);

uint32_t fake_lifecycle_post_calls(void);
uint32_t fake_lifecycle_event_count(void);
lifecycle_event_t fake_lifecycle_event_at(uint32_t index);

/** @brief Number of accepted events of one type. */
uint32_t fake_lifecycle_events_of(lifecycle_event_type_t type);

#endif /* FAKE_LIFECYCLE_H */
