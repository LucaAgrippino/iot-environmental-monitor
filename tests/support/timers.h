/**
 * @file timers.h (mock)
 * @brief Shadow header for FreeRTOS software timers in the test build.
 *
 * All timer types and stubs live in FreeRTOS.h (mock). This file exists so
 * that #include "timers.h" in firmware source files resolves correctly in
 * the host test build. Ceedling adds tests/support to the include path.
 */

#ifndef TIMERS_H
#define TIMERS_H

#include "FreeRTOS.h"

#endif /* TIMERS_H */
