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

#define configCPU_CLOCK_HZ                       (180000000UL)
#define configTICK_RATE_HZ                       ((TickType_t)1000)
#define configMINIMAL_STACK_SIZE                 ((unsigned short)128)
#define configMAX_PRIORITIES                     (7)
#define configMAX_TASK_NAME_LEN                  (16)
#define configUSE_16_BIT_TICKS                   0
#define configIDLE_SHOULD_YIELD                  1

#define configUSE_PREEMPTION                     1
#define configUSE_TIME_SLICING                   1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION  0
#define configUSE_TICKLESS_IDLE                  0

/* --- Memory allocation policy ------------------------------------------ */

#define configSUPPORT_STATIC_ALLOCATION          1
#define configSUPPORT_DYNAMIC_ALLOCATION         0   /* P5: no heap */
#define configTOTAL_HEAP_SIZE                    0

/* --- Kernel features --------------------------------------------------- */

#define configUSE_MUTEXES                        1
#define configUSE_RECURSIVE_MUTEXES              0
#define configUSE_COUNTING_SEMAPHORES            0
#define configUSE_QUEUE_SETS                     0
#define configUSE_TIMERS                         0   /* no software-timer task */
#define configUSE_TASK_NOTIFICATIONS             1
#define configUSE_TRACE_FACILITY                 0
#define configUSE_STATS_FORMATTING_FUNCTIONS     0
#define configGENERATE_RUN_TIME_STATS            0

/* --- Hooks ------------------------------------------------------------- */

#define configUSE_IDLE_HOOK                      0
#define configUSE_TICK_HOOK                      0
#define configUSE_MALLOC_FAILED_HOOK             0
#define configCHECK_FOR_STACK_OVERFLOW           2   /* method 2 — slower but thorough */
#define configUSE_DAEMON_TASK_STARTUP_HOOK       0

/* --- Cortex-M interrupt priority configuration ------------------------- */
/* F469 = Cortex-M4 with 4 priority bits, lowest priority is 15.
 * configMAX_SYSCALL_INTERRUPT_PRIORITY = 5 → ISRs with numerical priority
 * lower than 5 (i.e. higher hardware priority) MUST NOT call FreeRTOS API. */

#define configPRIO_BITS                          4
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY      0xF
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY   5
#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8 - configPRIO_BITS))

/* --- API inclusion ----------------------------------------------------- */

#define INCLUDE_vTaskPrioritySet                 0
#define INCLUDE_uxTaskPriorityGet                0
#define INCLUDE_vTaskDelete                      0
#define INCLUDE_vTaskSuspend                     1
#define INCLUDE_vTaskDelayUntil                  1
#define INCLUDE_vTaskDelay                       1
#define INCLUDE_xTaskGetSchedulerState           1
#define INCLUDE_xTaskGetCurrentTaskHandle        1
#define INCLUDE_uxTaskGetStackHighWaterMark      1
#define INCLUDE_xTimerPendFunctionCall           0
#define INCLUDE_eTaskGetState                    0

/* --- ISR handler name mapping ------------------------------------------ */
/* The CMSIS startup file declares SVC_Handler / PendSV_Handler /
 * SysTick_Handler in the vector table. FreeRTOS provides them under
 * different names; redirect via macros so the linker resolves correctly. */

#define vPortSVCHandler                          SVC_Handler
#define xPortPendSVHandler                       PendSV_Handler
#define xPortSysTickHandler                      SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
