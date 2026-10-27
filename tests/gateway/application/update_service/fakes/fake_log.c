/**
 * @file fake_log.c
 * @brief Logger spy and mutex audit (see fake_log.h).
 */

#include "fake_log.h"

#include <string.h>

#include "FreeRTOS.h"
#include "logger/logger.h"

#define LOG_LEVELS (4U)
#define LOG_KEEP (64U)
#define LOG_TEXT_MAX (96U)

static uint32_t s_level_counts[LOG_LEVELS];
static char s_messages[LOG_KEEP][LOG_TEXT_MAX];
static uint32_t s_message_count;
static uint32_t s_violations;

#define TRACE_MAX (128U)
static const char *s_trace[TRACE_MAX];
static uint32_t s_trace_len;

void logger_log(log_level_t level, const char *module, const char *msg)
{
    (void) module;

    if ((uint32_t) level < LOG_LEVELS)
    {
        s_level_counts[level]++;
    }

    if (s_message_count < LOG_KEEP)
    {
        (void) strncpy(s_messages[s_message_count], msg, LOG_TEXT_MAX - 1U);
        s_messages[s_message_count][LOG_TEXT_MAX - 1U] = '\0';
        s_message_count++;
    }
}

void fake_log_reset(void)
{
    (void) memset(s_level_counts, 0, sizeof(s_level_counts));
    (void) memset(s_messages, 0, sizeof(s_messages));
    s_message_count = 0U;
    s_violations = 0U;
    s_trace_len = 0U;
}

uint32_t fake_log_count(uint32_t level)
{
    return (level < LOG_LEVELS) ? s_level_counts[level] : 0U;
}

bool fake_log_contains(const char *text)
{
    for (uint32_t i = 0U; i < s_message_count; i++)
    {
        if (strstr(s_messages[i], text) != NULL)
        {
            return true;
        }
    }

    return false;
}

void fake_trace_add(const char *tag)
{
    if (s_trace_len < TRACE_MAX)
    {
        s_trace[s_trace_len] = tag;
        s_trace_len++;
    }
}

int32_t fake_trace_find(const char *tag, uint32_t start)
{
    for (uint32_t i = start; i < s_trace_len; i++)
    {
        if (strcmp(s_trace[i], tag) == 0)
        {
            return (int32_t) i;
        }
    }

    return -1;
}

uint32_t fake_trace_count_of(const char *tag)
{
    uint32_t n = 0U;

    for (uint32_t i = 0U; i < s_trace_len; i++)
    {
        if (strcmp(s_trace[i], tag) == 0)
        {
            n++;
        }
    }

    return n;
}

void fake_audit_mutex(void)
{
    if (g_mock_xSemaphoreTake_call_count != g_mock_xSemaphoreGive_call_count)
    {
        s_violations++;
    }
}

uint32_t fake_audit_violations(void)
{
    return s_violations;
}
