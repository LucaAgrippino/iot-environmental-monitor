/**
 * @file fake_lifecycle.c
 * @brief Fake ILifecycle (see fake_lifecycle.h).
 */

#include "fake_lifecycle.h"

#include <stddef.h>

#include "fake_log.h"

#define FL_NO_TYPE (0xFFFFFFFFUL)

static lifecycle_state_t s_state;
static uint32_t s_refuse;
static uint32_t s_refuse_type;
static uint32_t s_post_calls;
static lifecycle_event_t s_events[FL_LOG_MAX];
static uint32_t s_event_count;

static lifecycle_state_t l_get_state(void)
{
    fake_audit_mutex();

    return s_state;
}

static bool l_post_event(lifecycle_event_t event)
{
    static const char *const k_tags[] = {
        [LC_EVENT_OTA_REQUESTED] = "ev_requested", [LC_EVENT_SELF_CHECK_PASS] = "ev_pass",
        [LC_EVENT_SELF_CHECK_FAIL] = "ev_fail",    [LC_EVENT_OTA_RESET_REQUESTED] = "ev_reset",
        [LC_EVENT_OTA_FINISHED] = "ev_finished",
    };

    fake_audit_mutex();
    s_post_calls++;

    if (((uint32_t) event.type < (sizeof(k_tags) / sizeof(k_tags[0]))) &&
        (k_tags[event.type] != NULL))
    {
        fake_trace_add(k_tags[event.type]);
    }

    if (s_refuse > 0U)
    {
        s_refuse--;
        return false;
    }

    if ((uint32_t) event.type == s_refuse_type)
    {
        return false;
    }

    if (s_event_count < FL_LOG_MAX)
    {
        s_events[s_event_count] = event;
        s_event_count++;
    }

    return true;
}

const ilifecycle_t fake_lifecycle_ops = {
    .get_state = l_get_state,
    .post_event = l_post_event,
};

void fake_lifecycle_reset(void)
{
    s_state = LIFECYCLE_STATE_OPERATIONAL;
    s_refuse = 0U;
    s_refuse_type = FL_NO_TYPE;
    s_post_calls = 0U;
    s_event_count = 0U;
}

void fake_lifecycle_set_state(lifecycle_state_t state)
{
    s_state = state;
}

void fake_lifecycle_refuse_posts(uint32_t n)
{
    s_refuse = n;
}

void fake_lifecycle_refuse_type(lifecycle_event_type_t type)
{
    s_refuse_type = (uint32_t) type;
}

void fake_lifecycle_refuse_none(void)
{
    s_refuse_type = FL_NO_TYPE;
}

uint32_t fake_lifecycle_post_calls(void)
{
    return s_post_calls;
}

uint32_t fake_lifecycle_event_count(void)
{
    return s_event_count;
}

lifecycle_event_t fake_lifecycle_event_at(uint32_t index)
{
    return s_events[index];
}

uint32_t fake_lifecycle_events_of(lifecycle_event_type_t type)
{
    uint32_t n = 0U;

    for (uint32_t i = 0U; i < s_event_count; i++)
    {
        if (s_events[i].type == type)
        {
            n++;
        }
    }

    return n;
}
