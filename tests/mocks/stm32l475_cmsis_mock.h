#ifndef STM32L475_CMSIS_MOCK_H
#define STM32L475_CMSIS_MOCK_H

/* ====================================================================== */
/* L475 CMSIS mock — test-only entry point.                               */
/*                                                                        */
/* Mirrors the role of stm32_cmsis_mock.h for the F469 targets. Gateway  */
/* driver test TUs include this header instead of stm32l475xx.h directly, */
/* giving access to stm32l475_cmsis_mock_reset() in setUp().              */
/* ====================================================================== */

#include "stm32l475xx.h"

/** Zero all L475 mock peripheral state. Call from setUp() in every test. */
void stm32l475_cmsis_mock_reset(void);

#endif /* STM32L475_CMSIS_MOCK_H */
