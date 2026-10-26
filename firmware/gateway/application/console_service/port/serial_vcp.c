/**
 * @file serial_vcp.c
 * @brief Gateway platform hooks: unique ID, reset and time base.
 *
 * Byte I/O itself goes through IDebugUart (ST-Link virtual COM port, USART1).
 */

#include "console_port.h"

#include "FreeRTOS.h"
#include "stm32l475xx.h"
#include "task.h"

#define UID_BASE_ADDR (0x1FFF7590UL)
#define UID_WORDS (3U)

void console_port_uid(uint32_t uid[3])
{
    const volatile uint32_t *src = (const volatile uint32_t *) UID_BASE_ADDR;

    for (uint32_t i = 0U; i < UID_WORDS; i++)
    {
        uid[i] = src[i];
    }
}

void console_port_reset(void)
{
    NVIC_SystemReset();
}

uint32_t console_port_millis(void)
{
    return (uint32_t) (xTaskGetTickCount() * portTICK_PERIOD_MS);
}
