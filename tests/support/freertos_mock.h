/**
 * @file freertos_mock.h
 * @brief Include this header in any test that uses FreeRTOS.
 *
 * Including this file causes Ceedling to auto-link freertos_mock.c
 * (basename match). This is the same mechanism used by stm32_cmsis_mock.h
 * in the driver tests — no TEST_SOURCE_FILE directive needed.
 *
 * Place in tests/support/ alongside FreeRTOS.h, task.h, queue.h.
 */

#ifndef FREERTOS_MOCK_H
#define FREERTOS_MOCK_H

/* All types, externs and function prototypes live in FreeRTOS.h.
 * This header exists solely to give Ceedling a basename it can match
 * to freertos_mock.c for auto-link. */
#include "FreeRTOS.h"

#endif /* FREERTOS_MOCK_H */
