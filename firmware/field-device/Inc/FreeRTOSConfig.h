/**
 * @file FreeRTOSConfig.h
 * @brief FreeRTOS configuration for the F469 Field Device.
 *
 * Targets fully static allocation in line with project principle P5
 * (bounded resources, no heap). Logger's queue and drain task are
 * created via xQueueCreateStatic / xTaskCreateStatic; the idle task's
 * memory is supplied by the hook in freertos_hooks.c.
 *
 * Tick rate: 1 kHz — matches the assumption baked into Logger's uptime
 * timestamp formatting and into RtcDriver's RTC_MS_TO_TICKS macro.
 *
 * Update co-ordinates: if SYSTEM_HCLK_HZ changes in system_clock.h,
 * update configCPU_CLOCK_HZ to match.
 */

#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/* --- Cortex-M4F port settings ------------------------------------------ */

#define configCPU_CLOCK_HZ (180000000UL)
#define configTICK_RATE_HZ ((TickType_t) 1000)
#define configMINIMAL_STACK_SIZE ((unsigned short) 256)
#define configMAX_PRIORITIES (7)
#define configMAX_TASK_NAME_LEN (16)
#define configUSE_16_BIT_TICKS 0
#define configIDLE_SHOULD_YIELD 1

#define configUSE_PREEMPTION 1
#define configUSE_TIME_SLICING 1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_TICKLESS_IDLE 0

/* --- Memory allocation policy ------------------------------------------ */

#define configSUPPORT_STATIC_ALLOCATION 1
#define configSUPPORT_DYNAMIC_ALLOCATION 0 /* P5: no heap */
#define configTOTAL_HEAP_SIZE 0

/* Officially override the task return address to standard ARM Thread Root (0)
   to natively terminate GDB unwinding without modifying vendor source files. */
#define configTASK_RETURN_ADDRESS    0

/* --- Kernel features --------------------------------------------------- */

#define configUSE_MUTEXES 1
#define configUSE_RECURSIVE_MUTEXES 0
#define configUSE_COUNTING_SEMAPHORES 0
#define configUSE_QUEUE_SETS 0
#define configUSE_TASK_NOTIFICATIONS 1
#define configUSE_STATS_FORMATTING_FUNCTIONS 0

#define configUSE_TIMERS 1
#define configTIMER_TASK_PRIORITY       ( configMAX_PRIORITIES - 1 )
#define configTIMER_QUEUE_LENGTH        10
#define configTIMER_TASK_STACK_DEPTH    256

/* --- Debug features ------------------------------------------------------------- */

#define configUSE_TRACE_FACILITY            1
#define configGENERATE_RUN_TIME_STATS       1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_pcTaskGetName               1

/* --- Hooks ------------------------------------------------------------- */

#define configUSE_IDLE_HOOK 0
#define configUSE_TICK_HOOK 0
#define configUSE_MALLOC_FAILED_HOOK 0
#define configCHECK_FOR_STACK_OVERFLOW 2 /* method 2 — slower but thorough */
#define configUSE_DAEMON_TASK_STARTUP_HOOK 0

/* --- Cortex-M interrupt priority configuration ------------------------- */
/* F469 = Cortex-M4 with 4 priority bits, lowest priority is 15.
 * configMAX_SYSCALL_INTERRUPT_PRIORITY = 5 → ISRs with numerical priority
 * lower than 5 (i.e. higher hardware priority) MUST NOT call FreeRTOS API. */

#define configPRIO_BITS 4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY 0xF
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY 5
#define configKERNEL_INTERRUPT_PRIORITY                                                            \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY                                                       \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* --- API inclusion ----------------------------------------------------- */

#define INCLUDE_vTaskPrioritySet 0
#define INCLUDE_uxTaskPriorityGet 0
#define INCLUDE_vTaskDelete 0
#define INCLUDE_vTaskSuspend 1
#define INCLUDE_vTaskDelayUntil 1
#define INCLUDE_vTaskDelay 1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_xTaskGetCurrentTaskHandle 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTimerPendFunctionCall 0
#define INCLUDE_eTaskGetState 0

/* --- ISR handler name mapping ------------------------------------------ */
/* The CMSIS startup file declares SVC_Handler / PendSV_Handler /
 * SysTick_Handler in the vector table. FreeRTOS provides them under
 * different names; redirect via macros so the linker resolves correctly. */

#define vPortSVCHandler SVC_Handler
#define xPortPendSVHandler PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

/* --- Run-time stats timer ------------------------------------------ */

/*  DWT cycle counter on Cortex-M4 */
extern void     debug_timer_dwt_init(void);          /* one-time init at boot */
extern uint32_t debug_timer_dwt_get_counter(void);   /* returns DWT->CYCCNT  */

#define portCONFIGURE_TIMER_FOR_RUN_TIME_STATS()   debug_timer_dwt_init()
#define portGET_RUN_TIME_COUNTER_VALUE()           debug_timer_dwt_get_counter()
#endif /* FREERTOS_CONFIG_H */
