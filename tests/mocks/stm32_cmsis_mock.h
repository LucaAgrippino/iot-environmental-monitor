#ifndef STM32_CMSIS_MOCK_H
#define STM32_CMSIS_MOCK_H

/* ====================================================================== */
/* CMSIS mock — test-only entry point.                                    */
/*                                                                        */
/* Driver code includes "stm32f469xx.h" directly; that header defines     */
/* the mock peripheral types, instances, and bit constants. This header  */
/* adds the test-only helper(s) on top, so test files include this one   */
/* instead of the bare CMSIS mock.                                       */
/* ====================================================================== */

#include "stm32f469xx.h"

/** Zero all mock peripheral state. Call from setUp() in every test. */
void stm32_cmsis_mock_reset(void);

#endif /* STM32_CMSIS_MOCK_H */
