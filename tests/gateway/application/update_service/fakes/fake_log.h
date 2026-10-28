/**
 * @file fake_log.h
 * @brief Logger spy and mutex-balance audit for the UpdateService tests.
 *
 * Defines logger_log() so that the LOG_* macros of the code under test link without the real
 * Logger, and counts messages per level. The audit records, at every call out of UpdateService
 * into a fake, whether the instance mutex was free (every take matched by a give): the proof of
 * TC-US-080, that the mutex is never held across a FirmwareStore, transport or lifecycle call.
 *
 * Including this header makes Ceedling auto-link fake_log.c.
 */

#ifndef FAKE_LOG_H
#define FAKE_LOG_H

#include <stdbool.h>
#include <stdint.h>

/** @brief Clear counters and messages. */
void fake_log_reset(void);

/** @brief Messages logged at a level (0 = ERROR, 1 = WARN, 2 = INFO, 3 = DEBUG). */
uint32_t fake_log_count(uint32_t level);

/** @brief True if any message logged so far contains the text. */
bool fake_log_contains(const char *text);

/** @brief Append a tag to the shared call trace (every fake does, in call order). */
void fake_trace_add(const char *tag);

/** @brief Index of the first trace entry equal to tag at or after start; -1 if none. */
int32_t fake_trace_find(const char *tag, uint32_t start);

/** @brief Number of times a tag appears in the trace. */
uint32_t fake_trace_count_of(const char *tag);

/** @brief Called by the fakes on entry: note whether the mutex is held right now. */
void fake_audit_mutex(void);

/** @brief Calls out of UpdateService made while the mutex was held. */
uint32_t fake_audit_violations(void);

#endif /* FAKE_LOG_H */
