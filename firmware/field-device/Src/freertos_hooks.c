/**
 * @file freertos_hooks.c
 * @brief FreeRTOS application hooks required by FreeRTOSConfig.h.
 *
 * Under fully static allocation (configSUPPORT_STATIC_ALLOCATION = 1,
 * configSUPPORT_DYNAMIC_ALLOCATION = 0), the application must provide
 * the idle task's stack/TCB memory via vApplicationGetIdleTaskMemory().
 * The timer task hook is omitted because configUSE_TIMERS = 0.
 *
 * configCHECK_FOR_STACK_OVERFLOW = 2 requires
 * vApplicationStackOverflowHook(); this implementation traps in a
 * tight loop so a debugger can inspect the offending task name.
 */

#include "FreeRTOS.h"
#include "task.h"

#include "stm32f469xx.h" /* for __disable_irq() */

/* --- Idle-task static memory ------------------------------------------- */

static StaticTask_t s_idle_tcb;
static StackType_t s_idle_stack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer,
                                   StackType_t **ppxIdleTaskStackBuffer,
                                   uint32_t *pulIdleTaskStackSize)
{
    *ppxIdleTaskTCBBuffer = &s_idle_tcb;
    *ppxIdleTaskStackBuffer = s_idle_stack;
    *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
}

/* --- Stack overflow trap ----------------------------------------------- */

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void) xTask;
    (void) pcTaskName;
    /* A stack overflow is unrecoverable. Disable interrupts and spin so a
     * debugger lands here with pcTaskName visible in the inspector. */
    __disable_irq();
    for (;;)
    {
    }
}
