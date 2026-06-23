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

#include <stdint.h>

/* --- Idle-task static memory ------------------------------------------- */

static StaticTask_t s_idle_tcb;
static StackType_t s_idle_stack[configMINIMAL_STACK_SIZE] __attribute__((aligned(8)));

static StaticTask_t xTimerTaskTCB;
static StackType_t  uxTimerTaskStack[configTIMER_TASK_STACK_DEPTH] __attribute__((aligned(8)));

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


void vApplicationGetTimerTaskMemory(StaticTask_t **ppxTimerTaskTCBBuffer,
                                     StackType_t  **ppxTimerTaskStackBuffer,
                                     uint32_t *pulTimerTaskStackSize)
{
    *ppxTimerTaskTCBBuffer   = &xTimerTaskTCB;
    *ppxTimerTaskStackBuffer = uxTimerTaskStack;
    *pulTimerTaskStackSize   = configTIMER_TASK_STACK_DEPTH;
}

/* --- Run-time stats timer on Cortex-M4 --------------------------------- */

void debug_timer_dwt_init(void)
{
    /* Enable trace block (DWT lives here) */
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CYCCNT = 0U;
    DWT->CTRL  |= DWT_CTRL_CYCCNTENA_Msk;
}

uint32_t debug_timer_dwt_get_counter(void)
{
    return DWT->CYCCNT;
}
