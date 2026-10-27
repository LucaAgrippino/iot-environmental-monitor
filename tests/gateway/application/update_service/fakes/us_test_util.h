/**
 * @file us_test_util.h
 * @brief Shared helpers for the UpdateService unit tests.
 *
 * Builds the world (FreeRTOS mock, fakes, module), creates the instance bound to the fakes,
 * and drives it to the usual starting points. A test that includes this header must also
 * include, directly, every header whose .c it needs linked: update_service.h,
 * update_service_task.h, freertos_mock.h and the fake_* headers it uses (Ceedling links by the
 * test file's own includes only).
 *
 * Including this header makes Ceedling auto-link us_test_util.c.
 */

#ifndef US_TEST_UTIL_H
#define US_TEST_UTIL_H

#include <stdbool.h>
#include <stdint.h>

#include "update_service.h"

/** Job, and the two file lengths of the standard command (running Slot A, so the target is B). */
#define UST_JOB (42UL)
#define UST_SIZE_A (5000UL)
#define UST_SIZE_B (3000UL)

/** The standard Slot-B file is 3000 B: chunks of 1024, 1024 and 952. */
#define UST_LAST_CHUNK (952UL)

extern update_service_t *ust_us;

/** @brief Everything back to power-on: FreeRTOS mock, the fakes, the module (not created). */
void ust_reset_world(void);

/** @brief Create the instance over the fakes. Asserts success. */
update_service_t *ust_create(void);

/** @brief One pass of the task. */
void ust_step(void);

/** @brief Advance the tick counter (1 tick = 1 ms in the mock) and run one pass. */
void ust_advance(uint32_t ms);

/** @brief handle_command() with the standard command, or with explicit values. */
update_service_err_t ust_send_command(void);
update_service_err_t ust_send_command_with(uint32_t job, uint32_t size_a, uint32_t size_b);

/** @brief Reset, create, accept the standard command (AWAITING_START). */
void ust_to_awaiting_start(void);

/** @brief ... then start(): DOWNLOADING with the first chunk requested. */
void ust_to_downloading(void);

/** @brief Deliver the chunk that was requested last (pattern bytes) and run a pass. */
update_service_err_t ust_deliver_requested(void);

/** @brief Deliver every chunk until the download is complete (stops after the last pass). */
void ust_download_all(void);

/** @brief Pattern byte used for chunk contents. */
uint8_t ust_pattern(uint32_t index);

#endif /* US_TEST_UTIL_H */
