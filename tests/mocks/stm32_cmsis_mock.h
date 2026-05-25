#ifndef STM32_CMSIS_MOCK_H
#define STM32_CMSIS_MOCK_H

#include "stm32f469xx.h"

/** Zero all mock peripheral state. Call from setUp() in every test. */
void stm32_cmsis_mock_reset(void);

#endif